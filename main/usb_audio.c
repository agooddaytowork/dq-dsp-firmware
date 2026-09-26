/**
 * USB Audio Class (UAC) Device Driver for ESP32-S3
 *
 * Uses the espressif/usb_device_uac component to present the board
 * as a USB speaker to the host.  Audio data flows:
 *
 *   Host (Mac/iPhone) → USB UAC output_cb → ring buffer
 *     → audio task (Core 1) → DSP pipeline → dual I2S TX
 *
 * Architecture mirrors bt_app_core.c from the ESP32 firmware:
 *   - Ring buffer decouples USB isochronous timing from DSP processing
 *   - Audio task pinned to Core 1 for real-time performance
 *   - DSP telemetry posted to msg_handler for transport layer
 */

#include "usb_audio.h"
#include "dsp_config.h"
#include "dsp_param_update.h"
#include "msg_handler.h"
#include "i2s_audio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include "esp_timer.h"
#include <math.h>
#include "usb_device_uac.h"
#include "sdkconfig.h"

extern void dsp_pipeline_process(const dsp_config_t *cfg, float in_l, float in_r, float out[4]);
extern void dsp_pipeline_reset_filter_states(void);

/* -----------------------------------------------------------------------
 * Sample format helpers — gated on CONFIG_UAC_BIT_DEPTH (Kconfig).
 *
 * Input (USB):  little-endian PCM, 2 bytes/sample (16) or 3 bytes/sample (24).
 * Output (I2S): always int32 MSB-aligned for the 32-bit-slot DAC frame.
 *               PCM5102A latches the upper 24 bits.
 * ----------------------------------------------------------------------- */
#if CONFIG_UAC_BIT_DEPTH == 24
#define UAC_BYTES_PER_SAMPLE 3
static inline float decode_in(const uint8_t *p)
{
    /* Sign-extend from bit 23 into int32, then normalize. */
    int32_t s = ((int32_t)p[0]) | ((int32_t)p[1] << 8) | ((int32_t)(int8_t)p[2] << 16);
    return (float)s / 8388608.0f;  /* 2^23 */
}
#else
#define UAC_BYTES_PER_SAMPLE 2
static inline float decode_in(const uint8_t *p)
{
    int16_t s = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    return (float)s / 32768.0f;
}
#endif
#define UAC_BYTES_PER_PAIR (UAC_BYTES_PER_SAMPLE * 2)

/**
 * Soft saturation curve — replaces hard clipping at the int16 boundary.
 *
 * Linear (transparent) for |x| ≤ threshold, then smoothly asymptotic to ±1.0.
 * The knee is C1-continuous (derivative = 1 at the threshold), so the
 * transition into saturation is inaudible on transients yet eliminates the
 * harsh odd harmonics that hard clipping injects into the ear-sensitive
 * 2–5 kHz region — a major source of long-listen fatigue.
 *
 *   f(x) = x                                              for |x| ≤ t
 *   f(x) = sign(x) * (1 - (1-t)² / (|x| - 2t + 1))        for |x| > t
 *
 * With t = 0.85 (≈ −1.4 dBFS) the linear region preserves all clean signal
 * dynamics; only true overshoots from EQ boosts or hot masters get shaped.
 * No transcendentals — single division per sample, hot-path friendly.
 */
static inline float soft_clip(float x)
{
    const float t = 0.85f;
    float ax = fabsf(x);
    if (ax <= t) return x;
    float sign = (x < 0.0f) ? -1.0f : 1.0f;
    return sign * (1.0f - (1.0f - t) * (1.0f - t) / (ax - 2.0f * t + 1.0f));
}

static const char *TAG = "usb_audio";

/* -----------------------------------------------------------------------
 * Ring buffer and DMA buffers
 * ----------------------------------------------------------------------- */

/* Keep wrap boundaries and every read/write aligned to stereo PCM frames. */
#define USB_AUDIO_RINGBUF_SIZE  (192 * 1024)
#define USB_AUDIO_DMA_BUF_SIZE  (16 * 1024)
#define USB_AUDIO_BYTES_PER_MS  ((CONFIG_UAC_SAMPLE_RATE / 1000) * UAC_BYTES_PER_PAIR)
#define USB_AUDIO_CUSHION_BYTES (24 * USB_AUDIO_BYTES_PER_MS)
#define USB_AUDIO_REBUFFER_BYTES (12 * USB_AUDIO_BYTES_PER_MS)
#define USB_AUDIO_PI_TARGET_BYTES USB_AUDIO_CUSHION_BYTES
#define USB_AUDIO_BLOCK_FRAMES  (2 * CONFIG_UAC_SAMPLE_RATE / 1000)
#define USB_AUDIO_BLOCK_BYTES   (USB_AUDIO_BLOCK_FRAMES * 8)
#define USB_IDLE_THRESHOLD_US  100000

_Static_assert(USB_AUDIO_RINGBUF_SIZE % UAC_BYTES_PER_PAIR == 0,
               "ring wrap must preserve stereo frame alignment");
_Static_assert(USB_AUDIO_BLOCK_BYTES <= USB_AUDIO_DMA_BUF_SIZE,
               "every input block must fit both I2S output buffers");

static volatile bool s_playing = false;
static int64_t s_last_usb_audio_us = 0; /* accessed atomically across cores */

static RingbufHandle_t s_ringbuf = NULL;
static uint8_t *s_buf_i2s0 = NULL;
static uint8_t *s_buf_i2s1 = NULL;
static TaskHandle_t s_audio_task_handle = NULL;

/* USB host volume/mute (applied in audio task) */
static volatile float s_usb_volume = 1.0f;
static volatile bool  s_usb_mute = false;

/* Overflow tracking (for diagnostics, read by BLE timer on Core 0) */
static volatile uint32_t s_overflow_count = 0;

/* -----------------------------------------------------------------------
 * USB/I2S clock recovery
 *
 * Audio remains sample-exact 1:1 inside the ESP32. A slow PI controller asks
 * the host for fractionally more/fewer samples per USB frame so the physical
 * I2S clock owns the rate and the 24 ms app-ring cushion stays centered.
 * ----------------------------------------------------------------------- */

#define DRIFT_TIMER_INTERVAL_US  100000   /* 100ms */

/* Legacy wire-field names are retained for config compatibility:
 *   drift_kp = feedback proportional gain, ppm per millisecond
 *   drift_ki = feedback integral gain, ppm per millisecond-second */
#define DRIFT_KP_DEFAULT         25.0f
#define DRIFT_KI_DEFAULT         0.50f
#define DRIFT_TARGET_DEFAULT     ((float)USB_AUDIO_PI_TARGET_BYTES / (float)USB_AUDIO_RINGBUF_SIZE)
#define DRIFT_MAX_PPM_DEFAULT    200.0f
#define DRIFT_MAX_PPM_HARD       200.0f
#define DRIFT_FILL_LPF_ALPHA     0.10f    /* ~1 s at the 100 ms timer rate */
#define DRIFT_DEADBAND_MS        0.75f
#define DRIFT_SLEW_PPM_PER_TICK  2.0f     /* host command changes at <=20 ppm/s */

static float s_feedback_integral_ppm = 0.0f;
static float s_drift_filtered_fill = DRIFT_TARGET_DEFAULT;
static bool s_drift_filter_valid = false;
static int64_t s_feedback_prev_us = 0;
static volatile float s_drift_fill_pct = 0.0f;        /* last buffer fill % (for telemetry) */
static volatile float s_drift_correction_ppm = 0.0f;  /* UAC feedback offset (telemetry) */

/* DSP telemetry */
static uint32_t s_dsp_min_us = UINT32_MAX;
static uint32_t s_dsp_max_us = 0;
static uint64_t s_dsp_sum_us = 0;
static uint32_t s_dsp_block_count = 0;
static int64_t  s_dsp_last_report_us = 0;
#define DSP_TELEMETRY_INTERVAL_US  1000000

/* -----------------------------------------------------------------------
 * UAC Callbacks
 * ----------------------------------------------------------------------- */

/**
 * Called by the UAC stack when the host sends audio data (speaker output).
 * Must be non-blocking — just push into the ring buffer.
 */
static esp_err_t uac_output_cb(uint8_t *buf, size_t len, void *arg)
{
    if (!s_ringbuf || len == 0) return ESP_OK;

    __atomic_store_n(&s_last_usb_audio_us, esp_timer_get_time(), __ATOMIC_RELAXED);
    /* The UAC component only dispatches whole stereo frames. */
    if (len % UAC_BYTES_PER_PAIR != 0) return ESP_ERR_INVALID_SIZE;

    /* Only drop at 90% full — USB feedback handles normal drift.
     * This is a safety net, not the primary drift mechanism. */
    UBaseType_t free_bytes = xRingbufferGetCurFreeSize(s_ringbuf);
    if (free_bytes < USB_AUDIO_RINGBUF_SIZE / 10) {
        s_overflow_count++;
        return ESP_OK;
    }

    if (xRingbufferSend(s_ringbuf, buf, len, pdMS_TO_TICKS(0)) != pdTRUE) {
        s_overflow_count++;
    }
    return ESP_OK;
}

static void uac_set_mute_cb(uint32_t mute, void *arg)
{
    ESP_LOGI(TAG, "USB host set mute: %lu", (unsigned long)mute);
    s_usb_mute = (mute != 0);
}

static void uac_set_volume_cb(uint32_t volume, void *arg)
{
    ESP_LOGI(TAG, "USB host set volume: %lu", (unsigned long)volume);
    /* Convert 0-100 to logarithmic gain curve matching perceived loudness.
     * Map 0→silence, 100→0dB (unity). Uses 60dB range. */
    if (volume == 0) {
        s_usb_volume = 0.0f;
    } else {
        float db = -60.0f * (1.0f - (float)volume / 100.0f);
        s_usb_volume = powf(10.0f, db / 20.0f);
    }
}

/* -----------------------------------------------------------------------
 * Audio Processing Task (Core 1)
 *
 * Same architecture as bt_i2s_task_handler in the ESP32 firmware:
 * reads PCM from ring buffer, runs DSP pipeline, writes to dual I2S.
 * ----------------------------------------------------------------------- */

static void usb_audio_task(void *arg)
{
    ESP_LOGI(TAG, "USB audio DSP task started on core %d", xPortGetCoreID());

    bool buffering = true;
    for (;;) {
        size_t used = USB_AUDIO_RINGBUF_SIZE - xRingbufferGetCurFreeSize(s_ringbuf);
        if (!buffering && used < USB_AUDIO_REBUFFER_BYTES) {
            buffering = true;
        }
        if (buffering) {
            s_playing = false;
            if (used < USB_AUDIO_CUSHION_BYTES) {
                /* Pace priming with the physical output clock. Filling DMA
                 * with silence prevents it from swallowing the ring cushion
                 * in an initial burst when playback starts. */
                memset(s_buf_i2s0, 0, USB_AUDIO_BLOCK_BYTES);
                memset(s_buf_i2s1, 0, USB_AUDIO_BLOCK_BYTES);
                i2s_audio_write_dual(s_buf_i2s0, s_buf_i2s1, USB_AUDIO_BLOCK_BYTES);
                continue;
            }
            buffering = false;
            s_playing = true;
        }

        size_t item_size = 0;
        uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(
            s_ringbuf, &item_size, pdMS_TO_TICKS(50),
            USB_AUDIO_BLOCK_FRAMES * UAC_BYTES_PER_PAIR);
        if (data == NULL || item_size == 0) {
            buffering = true;
            s_playing = false;
            continue;
        }

        /* If Core 0 is recalculating coefficients, output silence.
         * Silence size mirrors the input duration but in I2S output units
         * (8 bytes/pair: stereo int32). */
        if (dsp_param_is_recalculating()) {
            size_t silence_pairs = item_size / UAC_BYTES_PER_PAIR;
            size_t silence_bytes = silence_pairs * 8;
            if (silence_bytes > USB_AUDIO_DMA_BUF_SIZE) silence_bytes = USB_AUDIO_DMA_BUF_SIZE;
            memset(s_buf_i2s0, 0, silence_bytes);
            memset(s_buf_i2s1, 0, silence_bytes);
            i2s_audio_write_dual(s_buf_i2s0, s_buf_i2s1, silence_bytes);
            vRingbufferReturnItem(s_ringbuf, (void *)data);
            continue;
        }

        /* DSP parameter update: commit + reset filter states. */
        if (dsp_param_poll_update()) {
            dsp_param_commit();
            dsp_pipeline_reset_filter_states();
        }

        const dsp_config_t *cfg = dsp_param_get_active();

        const uint8_t *in = data;
        int32_t *out0 = (int32_t *)s_buf_i2s0;
        int32_t *out1 = (int32_t *)s_buf_i2s1;

        int64_t t0 = esp_timer_get_time();

        /* Exactly one DSP/output frame per USB stereo frame. The sole rate
         * controller adjusts host packet cadence through the feedback EP. */
        float uv = s_usb_mute ? 0.0f : s_usb_volume;
        size_t num_pairs = item_size / UAC_BYTES_PER_PAIR;
        size_t out_bytes = 0;

        for (size_t in_idx = 0; in_idx < num_pairs; in_idx++) {
            const uint8_t *p = in + in_idx * UAC_BYTES_PER_PAIR;
            float sample_l = decode_in(p);
            float sample_r = decode_in(p + UAC_BYTES_PER_SAMPLE);

            float dsp_out[4];
            dsp_pipeline_process(cfg, sample_l, sample_r, dsp_out);

            int32_t s24[4];
            for (int ch = 0; ch < 4; ch++) {
                float s = soft_clip(dsp_out[ch]) * uv;
                s24[ch] = (int32_t)(s * 8388607.0f);
            }

            *out0++ = s24[0] << 8;
            *out0++ = s24[1] << 8;
            *out1++ = s24[2] << 8;
            *out1++ = s24[3] << 8;
            out_bytes += 8;
        }

        /* DSP telemetry */
        int64_t elapsed_us = esp_timer_get_time() - t0;
        uint32_t elapsed = (uint32_t)elapsed_us;
        if (elapsed < s_dsp_min_us) s_dsp_min_us = elapsed;
        if (elapsed > s_dsp_max_us) s_dsp_max_us = elapsed;
        s_dsp_sum_us += elapsed;
        s_dsp_block_count++;

        int64_t now = esp_timer_get_time();
        if (now - s_dsp_last_report_us >= DSP_TELEMETRY_INTERVAL_US) {
            if (s_dsp_block_count > 0) {
                dsp_telemetry_t stats = {
                    .dsp_min_us = s_dsp_min_us,
                    .dsp_max_us = s_dsp_max_us,
                    .dsp_avg_us = (uint32_t)(s_dsp_sum_us / s_dsp_block_count),
                    .blocks_processed = s_dsp_block_count,
                    .buffer_fill_pct = (uint8_t)s_drift_fill_pct,
                    .correction_ppm = s_drift_correction_ppm,
                };
                msg_handler_post_telemetry(&stats);
            }

            s_dsp_min_us = UINT32_MAX;
            s_dsp_max_us = 0;
            s_dsp_sum_us = 0;
            s_dsp_block_count = 0;
            s_dsp_last_report_us = now;
        }

        if (out_bytes > 0) {
            i2s_audio_write_dual(s_buf_i2s0, s_buf_i2s1, out_bytes);
        }

        vRingbufferReturnItem(s_ringbuf, (void *)data);
    }
}

/* -----------------------------------------------------------------------
 * Adaptive UAC feedback controller (Core 0, 100 ms).
 *
 * Positive feedback PPM asks the USB host for more samples. Therefore a low
 * ring (negative error) produces a positive command, and a high ring produces
 * a negative command. The integral learns the fixed USB↔I2S clock offset;
 * the proportional term rejects short fill excursions. Audio samples remain
 * strictly 1:1 inside the ESP32.
 * ----------------------------------------------------------------------- */
static void drift_compensation_cb(void *arg)
{
    (void)arg;
    if (!s_ringbuf) return;

    int64_t now = esp_timer_get_time();
    int64_t last_usb_us = __atomic_load_n(&s_last_usb_audio_us, __ATOMIC_RELAXED);
    bool usb_active = last_usb_us > 0 && (now - last_usb_us) < USB_IDLE_THRESHOLD_US;


    const dsp_config_t *cfg = dsp_param_get_active();
    float kp = (cfg->system.drift_kp >= 5.0f &&
                cfg->system.drift_kp <= 60.0f)
             ? cfg->system.drift_kp : DRIFT_KP_DEFAULT;
    float ki = (cfg->system.drift_ki >= 0.05f &&
                cfg->system.drift_ki <= 2.0f)
             ? cfg->system.drift_ki : DRIFT_KI_DEFAULT;
    float max_ppm = cfg->system.drift_max_ppm > 0.0f
                  ? cfg->system.drift_max_ppm : DRIFT_MAX_PPM_DEFAULT;
    if (max_ppm < 20.0f) max_ppm = 20.0f;
    if (max_ppm > DRIFT_MAX_PPM_HARD) max_ppm = DRIFT_MAX_PPM_HARD;

    UBaseType_t free = xRingbufferGetCurFreeSize(s_ringbuf);
    float fill = 1.0f - (float)free / (float)USB_AUDIO_RINGBUF_SIZE;

    static bool was_playing = false;
    bool playback_start = s_playing && !was_playing;
    was_playing = s_playing;
    if (!s_drift_filter_valid || playback_start) {
        s_drift_filtered_fill = fill;
        s_drift_filter_valid = true;
        s_feedback_prev_us = now;
    } else if (s_playing && usb_active) {
        s_drift_filtered_fill +=
            (fill - s_drift_filtered_fill) * DRIFT_FILL_LPF_ALPHA;
    }

    float target = (float)USB_AUDIO_PI_TARGET_BYTES /
                   (float)USB_AUDIO_RINGBUF_SIZE;
    float error_ms = (s_drift_filtered_fill - target) *
                     (float)USB_AUDIO_RINGBUF_SIZE /
                     (float)USB_AUDIO_BYTES_PER_MS;
    float effective_error_ms = 0.0f;
    if (error_ms > DRIFT_DEADBAND_MS) {
        effective_error_ms = error_ms - DRIFT_DEADBAND_MS;
    } else if (error_ms < -DRIFT_DEADBAND_MS) {
        effective_error_ms = error_ms + DRIFT_DEADBAND_MS;
    }

    float dt_s = s_feedback_prev_us > 0
               ? (float)(now - s_feedback_prev_us) / 1000000.0f : 0.0f;
    s_feedback_prev_us = now;
    if (dt_s < 0.0f || dt_s > 0.5f) dt_s = 0.0f;

    if (s_playing && usb_active) {
        float integral_step = -ki * effective_error_ms * dt_s;
        float candidate = s_feedback_integral_ppm + integral_step;
        if (candidate >  max_ppm) candidate =  max_ppm;
        if (candidate < -max_ppm) candidate = -max_ppm;

        /* Conditional integration prevents wind-up while the combined P+I
         * request is already pushing farther into either command rail. */
        float candidate_request = candidate - kp * effective_error_ms;
        bool pushes_high_rail = candidate_request > max_ppm &&
                                integral_step > 0.0f;
        bool pushes_low_rail = candidate_request < -max_ppm &&
                               integral_step < 0.0f;
        if (!pushes_high_rail && !pushes_low_rail) {
            s_feedback_integral_ppm = candidate;
        }
    }

    float requested = s_feedback_integral_ppm;
    if (s_playing && usb_active) {
        requested += -kp * effective_error_ms;
    }
    if (requested >  max_ppm) requested =  max_ppm;
    if (requested < -max_ppm) requested = -max_ppm;

    float delta = requested - s_drift_correction_ppm;
    if (delta >  DRIFT_SLEW_PPM_PER_TICK) delta =  DRIFT_SLEW_PPM_PER_TICK;
    if (delta < -DRIFT_SLEW_PPM_PER_TICK) delta = -DRIFT_SLEW_PPM_PER_TICK;
    float feedback_ppm = s_drift_correction_ppm + delta;

    uac_device_set_feedback_ppm((int32_t)lroundf(feedback_ppm));

    /* No local rate conversion: preserve every host PCM sample exactly once. */
    s_drift_fill_pct = fill * 100.0f;
    s_drift_correction_ppm = feedback_ppm;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

esp_err_t usb_audio_init(uint32_t sample_rate)
{
    /* Allocate ring buffer in internal RAM for fast USB callback access */
    s_ringbuf = xRingbufferCreate(USB_AUDIO_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!s_ringbuf) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        return ESP_ERR_NO_MEM;
    }

    /* Allocate DMA-capable I2S output buffers */
    s_buf_i2s0 = heap_caps_malloc(USB_AUDIO_DMA_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_buf_i2s1 = heap_caps_malloc(USB_AUDIO_DMA_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_buf_i2s0 || !s_buf_i2s1) {
        ESP_LOGE(TAG, "Failed to allocate I2S DMA buffers");
        return ESP_ERR_NO_MEM;
    }

    /* TEMP REVERT: composite UAC + WebUSB Vendor descriptor was breaking
     * macOS audio HAL binding. Going back to UAC-only (component owns
     * tinyusb + descriptor) so audio works again. The composite path will
     * come back once the descriptor layout is verified against actual
     * macOS enumeration logs. */
    uac_device_config_t uac_cfg = {
        .output_cb     = uac_output_cb,
        .input_cb      = NULL,
        .set_mute_cb   = uac_set_mute_cb,
        .set_volume_cb = uac_set_volume_cb,
        .cb_ctx        = NULL,
    };

    esp_err_t err = uac_device_init(&uac_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UAC device init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Start the sole clock-recovery loop: adaptive USB feedback (100 ms) */
    const esp_timer_create_args_t drift_timer_args = {
        .callback = drift_compensation_cb,
        .name = "uac_feedback",
    };
    esp_timer_handle_t drift_timer;
    ESP_ERROR_CHECK(esp_timer_create(&drift_timer_args, &drift_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(drift_timer, DRIFT_TIMER_INTERVAL_US));

    ESP_LOGI(TAG, "USB Audio initialized (stereo, %d-bit, %lu Hz, adaptive UAC feedback)",
             CONFIG_UAC_BIT_DEPTH, (unsigned long)sample_rate);
    return ESP_OK;
}

void usb_audio_start(void)
{
    /* Launch audio processing task on Core 1 */
    xTaskCreatePinnedToCore(
        usb_audio_task,
        "UsbAudioT",
        4096,
        NULL,
        configMAX_PRIORITIES - 3,
        &s_audio_task_handle,
        1   /* Core 1 for real-time audio */
    );

    ESP_LOGI(TAG, "USB audio streaming started");
}

void usb_audio_stop(void)
{
    if (s_audio_task_handle) {
        vTaskDelete(s_audio_task_handle);
        s_audio_task_handle = NULL;
    }

    if (s_ringbuf) {
        vRingbufferDelete(s_ringbuf);
        s_ringbuf = NULL;
    }

    if (s_buf_i2s0) {
        heap_caps_free(s_buf_i2s0);
        s_buf_i2s0 = NULL;
    }
    if (s_buf_i2s1) {
        heap_caps_free(s_buf_i2s1);
        s_buf_i2s1 = NULL;
    }

    ESP_LOGI(TAG, "USB audio stopped");
}
