#!/usr/bin/env python3
"""Compile production C routines with host fakes; no ESP-IDF or board required.

Extraction keeps the controller, PCM loop and USB RX callback under test identical
with the firmware. These tests do not emulate USB host timing or dual-I2S DMA.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
APP = (ROOT / 'main/usb_audio.c').read_text()
UAC = (ROOT / 'components/usb_device_uac/usb_device_uac.c').read_text()


def function(source, signature):
    start = source.index(signature)
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
'''

controller = COMMON + r'''
#define CONFIG_UAC_SAMPLE_RATE 48000
#define UAC_BYTES_PER_PAIR 6
''' + APP[APP.index('#define USB_AUDIO_RINGBUF_SIZE'):APP.index('static RingbufHandle_t')] + r'''
typedef unsigned UBaseType_t;
static void *s_ringbuf = (void *)1;
typedef struct { struct { float drift_kp, drift_ki, drift_target_fill, drift_max_ppm; } system; } dsp_config_t;
static dsp_config_t config = {{25, .5, .5, 200}};
static int64_t clock_us;
static float fill_ms;
static int command_ppm;
static int64_t esp_timer_get_time(void) { return clock_us; }
static const dsp_config_t *dsp_param_get_active(void) { return &config; }
static unsigned xRingbufferGetCurFreeSize(void *ring) {
    return USB_AUDIO_RINGBUF_SIZE - (unsigned)lroundf(fill_ms * USB_AUDIO_BYTES_PER_MS);
}
static void uac_device_set_feedback_ppm(int32_t ppm) { command_ppm = ppm; }
''' + APP[APP.index('#define DRIFT_TIMER_INTERVAL_US'):APP.index('/* DSP telemetry */')] + function(APP, 'static void drift_compensation_cb(') + r'''
static void tick(bool active) {
    clock_us += 100000;
    if (active) s_last_usb_audio_us = clock_us;
    drift_compensation_cb(NULL);
    assert(isfinite(s_drift_correction_ppm));
    assert(abs(command_ppm) <= 200);
}
int main(int argc, char **argv) {
    float mismatch = strtof(argv[1], NULL);
    fill_ms = 24;
    s_playing = true;
    /* Thirty minutes, with 1 ms measurement jitter and host ppm quantization. */
    double physical_fill = fill_ms;
    for (int i = 0; i < 18000; i++) {
        fill_ms = physical_fill + sinf(i * 1.7f);
        float old_ppm = s_drift_correction_ppm;
        tick(true);
        assert(fabsf(s_drift_correction_ppm - old_ppm) <= 2.001f);
        physical_fill += (command_ppm - mismatch) * 0.0001;
        assert(physical_fill > 12 && physical_fill < 36);
    }
    assert(fabs(physical_fill - 24) < 1.5);
    assert(fabs(command_ppm - mismatch) < 10);
    /* Pausing/priming must not wind up the integral, even with an empty ring. */
    s_playing = false;
    fill_ms = 0;
    float integral = s_feedback_integral_ppm;
    for (int i = 0; i < 600; i++) tick(false);
    assert(integral == s_feedback_integral_ppm);
    for (int i = 0; i < 50; i++) tick(true);
    assert(integral == s_feedback_integral_ppm);
    /* Resume at the cushion; old ASRC settings must fall back to safe gains. */
    config.system.drift_kp = .3f;
    config.system.drift_ki = .02f;
    config.system.drift_max_ppm = 1400;
    s_playing = true;
    fill_ms = 24;
    tick(true);
    assert(fabsf(s_drift_filtered_fill * USB_AUDIO_RINGBUF_SIZE /
                 USB_AUDIO_BYTES_PER_MS - 24) < .01);
    /* Sustained saturation must not integrate further into the command rail. */
    fill_ms = 100;
    for (int i = 0; i < 250; i++) tick(true);
    assert(command_ppm == -200);
    integral = s_feedback_integral_ppm;
    for (int i = 0; i < 100; i++) tick(true);
    assert(integral == s_feedback_integral_ppm);
    printf("PASS: 30 min, drift %+.0f ppm, pause/resume, saturation\n", mismatch);
}
'''

rx = COMMON + r'''
#define SPEAK_CHANNEL_NUM 2
#define ESP_OK 0
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
static struct {
    uint8_t spk_resolution;
    uint8_t spk_buf[1024];
    struct { esp_err_t (*output_cb)(uint8_t *, size_t, void *); void *cb_ctx; } user_cfg;
} device, *s_uac_device = &device;
static uint8_t fifo[4096], received[8192];
static size_t fifo_size, received_size;
static size_t tud_audio_available(void) { return fifo_size; }
static int tud_audio_read(void *buf, size_t size) {
    assert(size <= fifo_size && size <= sizeof(device.spk_buf));
    memcpy(buf, fifo, size);
    fifo_size -= size;
    memmove(fifo, fifo + size, fifo_size);
    return size;
}
static esp_err_t receive(uint8_t *buf, size_t size, void *ctx) {
    assert(size % (device.spk_resolution / 8 * 2) == 0);
    memcpy(received + received_size, buf, size);
    received_size += size;
    return ESP_OK;
}
''' + function(UAC, 'bool tud_audio_rx_done_post_read_cb(') + function(UAC, 'static uint32_t feedback_word_16_16(') + r'''
int main(void) {
    device.user_cfg.output_cb = receive;
    for (unsigned bits = 16; bits <= 24; bits += 8) {
        device.spk_resolution = bits;
        unsigned frame = bits / 8 * 2;
        received_size = fifo_size = 0;
        /* 49-frame packet must be delivered in full, not capped to 48. */
        for (unsigned packet = 0; packet < 4; packet++) {
            fifo_size = 49 * frame;
            memset(fifo, packet + 1, fifo_size);
            tud_audio_rx_done_post_read_cb(0, fifo_size, 0, 0, 1);
            assert(fifo_size == 0);
        }
        assert(received_size == 4 * 49 * frame);
        for (size_t i = 0; i < received_size; i++)
            assert(received[i] == i / (49 * frame) + 1);
        /* A fragmented frame stays in FIFO until its remaining bytes arrive. */
        fifo_size = frame - 1;
        size_t before = received_size;
        tud_audio_rx_done_post_read_cb(0, 0, 0, 0, 1);
        assert(received_size == before && fifo_size == frame - 1);
        fifo_size++;
        tud_audio_rx_done_post_read_cb(0, 0, 0, 0, 1);
        assert(received_size == before + frame && fifo_size == 0);
        /* Backlog larger than scratch capacity is drained in aligned chunks. */
        fifo_size = frame * 400;
        memset(fifo, 7, fifo_size);
        before = received_size;
        while (fifo_size) tud_audio_rx_done_post_read_cb(0, 0, 0, 0, 1);
        assert(received_size == before + frame * 400);
    }
    for (unsigned fps = 1000; fps <= 8000; fps *= 8) {
        unsigned nominal = (uint64_t)48000 * 65536 / fps;
        assert(feedback_word_16_16(48000, fps, 0) == nominal);
        assert(feedback_word_16_16(48000, fps, 200) > nominal);
        assert(feedback_word_16_16(48000, fps, -200) < nominal);
    }
    puts("PASS: USB extra frames, buffer ownership, partial frames, backlog, FS/HS feedback");
}
'''

pcm = COMMON + r'''
#define CONFIG_UAC_BIT_DEPTH 24
''' + APP[APP.index('#if CONFIG_UAC_BIT_DEPTH'):APP.index('/**\n * Soft saturation')] + function(APP, 'static inline float soft_clip(') + r'''
typedef int dsp_config_t;
static float expected_l, expected_r;
static size_t processed;
static void dsp_pipeline_process(const dsp_config_t *cfg, float l, float r, float out[4]) {
    /* A constant negative/positive pair detects startup zeros, swapped channels,
     * interpolation and sign extension errors. */
    assert(l == expected_l && r == expected_r);
    processed++;
    out[0] = out[2] = l;
    out[1] = out[3] = r;
}
static bool s_usb_mute;
static float s_usb_volume = 1;
static size_t process(const uint8_t *in, size_t item_size, int32_t *out0, int32_t *out1) {
    const dsp_config_t *cfg = NULL;
''' + APP[APP.index('        float uv ='):APP.index('        /* DSP telemetry */', APP.index('        float uv ='))] + r'''
    return out_bytes;
}
int main(void) {
    uint8_t input[96 * 6];
    int32_t out0[192], out1[192];
    expected_l = -.25f; expected_r = .25f;
    for (int i = 0; i < 96; i++) {
        memcpy(input + i * 6, (uint8_t[]){0, 0, 0xe0, 0, 0, 0x20}, 6);
    }
    /* Simulate repeated bounded dequeues, including short chunks at ring wrap. */
    for (int block = 0; block < 10000; block++) {
        size_t frames = block % 96 + 1;
        processed = 0;
        assert(process(input, frames * 6, out0, out1) == frames * 8);
        assert(processed == frames);
        assert(memcmp(out0, out1, frames * 8) == 0);
    }
    puts("PASS: 10,000 PCM blocks, one output frame per input, signed stereo decode");
}
'''

with tempfile.TemporaryDirectory(prefix='dq-feedback-test-') as temp:
    for name, code in [('controller', controller), ('rx', rx), ('pcm', pcm)]:
        path = Path(temp) / name
        path.with_suffix('.c').write_text(code)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2',
                        str(path.with_suffix('.c')), '-lm', '-o', str(path)], check=True)
        args = ['-180', '-100', '0', '100', '180'] if name == 'controller' else [None]
        for arg in args:
            subprocess.run([str(path)] + ([arg] if arg is not None else []), check=True)
