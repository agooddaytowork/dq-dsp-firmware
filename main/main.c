/**
 * ESP32-S3 Audio DSP Processor with USB Audio + Serial Control
 *
 * Audio flows:  Mac/iPhone → USB Audio Class → ringbuffer → DSP → dual I2S TX
 * Control:      Web App → Web Serial → UART bridge → msg_handler → dsp_param_apply()
 *
 * Single transport: serial only. BLE removed for simplicity.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"

#include "dsp_config.h"
#include "dsp_param_update.h"
#include "i2s_audio.h"
#include "usb_audio.h"
#include "serial_server.h"

static const char *TAG = "dsp_main";

extern void dsp_pipeline_init(void);

/* -----------------------------------------------------------------------
 * Initial configuration (loaded from flash or defaults)
 * ----------------------------------------------------------------------- */

static dsp_config_t initial_config __attribute__((aligned(4)));

/* NVS save debounce timer */
static esp_timer_handle_t nvs_save_timer = NULL;
#define NVS_SAVE_DEBOUNCE_MS 2000

/* -----------------------------------------------------------------------
 * NVS Config Load/Save (shared with msg_handler via extern)
 * ----------------------------------------------------------------------- */

static bool load_config_from_nvs(dsp_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("dsp", NVS_READONLY, &handle);
    if (err != ESP_OK) return false;

    size_t size = sizeof(dsp_config_t);
    err = nvs_get_blob(handle, "config", config, &size);
    nvs_close(handle);

    if (err != ESP_OK) return false;

    if (config->header.magic != DSP_MAGIC) {
        ESP_LOGW(TAG, "Invalid config magic: 0x%08lx", (unsigned long)config->header.magic);
        return false;
    }

    if (config->header.version != DSP_VERSION) {
        ESP_LOGW(TAG, "Config version mismatch: %d vs %d", config->header.version, DSP_VERSION);
        return false;
    }

    ESP_LOGI(TAG, "Loaded config preset %d, sample rate %lu Hz",
             config->header.preset_index, (unsigned long)config->header.sample_rate);
    return true;
}

static void nvs_save_timer_cb(void *arg)
{
    (void)arg;
    const dsp_config_t *cfg = dsp_param_get_active();
    if (!cfg) return;

    nvs_handle_t handle;
    esp_err_t err = nvs_open("dsp", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed for save: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(handle, "config", cfg, sizeof(dsp_config_t));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Config saved to NVS (%d bytes)", (int)sizeof(dsp_config_t));
    } else {
        ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
    }
}

void save_config_to_nvs_debounced(void)
{
    if (nvs_save_timer) {
        esp_timer_stop(nvs_save_timer);
        esp_timer_start_once(nvs_save_timer, NVS_SAVE_DEBOUNCE_MS * 1000);
    }
}

void save_config_to_nvs_immediate(void)
{
    if (nvs_save_timer) {
        esp_timer_stop(nvs_save_timer);
    }
    nvs_save_timer_cb(NULL);
}

/* -----------------------------------------------------------------------
 * Application Entry Point
 * ----------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 Audio DSP + USB Audio + Serial starting...");
    esp_err_t err;

    /* 1. Initialize NVS */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* 2. Load or initialize DSP configuration */
    if (!load_config_from_nvs(&initial_config)) {
        ESP_LOGI(TAG, "No valid config found, using defaults");
        memset(&initial_config, 0, sizeof(initial_config));
        initial_config.header.magic = DSP_MAGIC;
        initial_config.header.version = DSP_VERSION;
        initial_config.header.sample_rate = CONFIG_UAC_SAMPLE_RATE;
        initial_config.global.master_volume = 1.0f;
        initial_config.global.sample_rate = CONFIG_UAC_SAMPLE_RATE;

        for (int i = 0; i < DSP_NUM_INPUTS; i++) {
            initial_config.inputs[i].gain = 1.0f;
            for (int b = 0; b < DSP_MAX_PEQ_BANDS; b++) {
                initial_config.inputs[i].eq_bands[b] = (biquad_coeffs_t){1,0,0,0,0};
            }
        }
        for (int o = 0; o < DSP_NUM_OUTPUTS; o++) {
            initial_config.outputs[o].gain = 1.0f;
            for (int b = 0; b < DSP_MAX_PEQ_BANDS; b++) {
                initial_config.outputs[o].eq_bands[b] = (biquad_coeffs_t){1,0,0,0,0};
            }
            for (int s = 0; s < DSP_MAX_XO_STAGES; s++) {
                initial_config.outputs[o].hp_stages[s] = (biquad_coeffs_t){1,0,0,0,0};
                initial_config.outputs[o].lp_stages[s] = (biquad_coeffs_t){1,0,0,0,0};
            }
        }

        /* Default routing: stereo on both DACs
         * Input 0 (L) -> Out 0 (I2S0 L) + Out 2 (I2S1 L)
         * Input 1 (R) -> Out 1 (I2S0 R) + Out 3 (I2S1 R) */
        initial_config.routing[0][0].enabled = 1;
        initial_config.routing[0][0].gain = 1.0f;
        initial_config.routing[1][1].enabled = 1;
        initial_config.routing[1][1].gain = 1.0f;
        initial_config.routing[0][2].enabled = 1;
        initial_config.routing[0][2].gain = 1.0f;
        initial_config.routing[1][3].enabled = 1;
        initial_config.routing[1][3].gain = 1.0f;

        /* Adaptive UAC feedback defaults; preserve the legacy wire layout. */
        initial_config.system.drift_kp = 25.0f;
        initial_config.system.drift_ki = 0.50f;
        initial_config.system.drift_target_fill = 24.0f * (CONFIG_UAC_SAMPLE_RATE / 1000)
                                                 * 2 * (CONFIG_UAC_BIT_DEPTH / 8) / (192 * 1024);
        initial_config.system.drift_max_ppm = 200.0f;
    }

    /* Force the sample rate fields to match the firmware's compile-time
     * UAC rate, even if NVS held an older value. The USB UAC component
     * only advertises CONFIG_UAC_SAMPLE_RATE — keeping the DSP config in
     * sync prevents biquads being calculated at the wrong rate. */
    if (initial_config.global.sample_rate != CONFIG_UAC_SAMPLE_RATE) {
        ESP_LOGW(TAG, "NVS sample_rate %lu differs from UAC %d — forcing %d",
                 (unsigned long)initial_config.global.sample_rate,
                 CONFIG_UAC_SAMPLE_RATE, CONFIG_UAC_SAMPLE_RATE);
        initial_config.global.sample_rate = CONFIG_UAC_SAMPLE_RATE;
        initial_config.header.sample_rate = CONFIG_UAC_SAMPLE_RATE;
    }

    ESP_LOGI(TAG, "sizeof(dsp_config_t) = %d bytes", (int)sizeof(dsp_config_t));

    /* 3. Initialize parameter update engine */
    err = dsp_param_init(&initial_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init param update engine: %d", err);
        return;
    }

    /* 3a. NVS save debounce timer */
    const esp_timer_create_args_t nvs_timer_args = {
        .callback = nvs_save_timer_cb,
        .name = "nvs_save",
    };
    err = esp_timer_create(&nvs_timer_args, &nvs_save_timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create NVS save timer: %s", esp_err_to_name(err));
    }

    /* 4. Initialize DSP pipeline state */
    dsp_pipeline_init();

    /* 5. Initialize serial server (UART transport for web UI) */
    err = serial_server_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Serial server init failed: %s", esp_err_to_name(err));
    } else {
        serial_server_start();
    }

    /* 6. Initialize dual I2S TX outputs */
    uint32_t sr = initial_config.global.sample_rate;
    ESP_LOGI(TAG, "Initializing dual I2S TX at %lu Hz", (unsigned long)sr);
    err = i2s_audio_init_dual_output(sr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S: %s", esp_err_to_name(err));
        return;
    }

    /* 7. Initialize and start USB Audio */
    err = usb_audio_init(sr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB audio init failed: %s", esp_err_to_name(err));
        return;
    }
    usb_audio_start();

    ESP_LOGI(TAG, "DSP processor running (USB Audio + Serial control)");

    serial_server_attach_logs();
}
