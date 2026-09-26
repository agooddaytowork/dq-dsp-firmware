/*
 * Dual I2S TX Output Driver (ESP32-S3)
 *
 * Identical to the ESP32 version — GPIO pins come from Kconfig.projbuild
 * which has S3-specific defaults (4/5/6 for I2S0, 16/17/18 for I2S1).
 */

#include "i2s_audio.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "i2s_audio";

static i2s_chan_handle_t i2s_tx_chan0 = NULL;
static i2s_chan_handle_t i2s_tx_chan1 = NULL;

esp_err_t i2s_audio_init_dual_output(uint32_t sample_rate)
{
    esp_err_t err;

    /* ---------- I2S0 -- left speaker ----------
     *
     * auto_clear=true outputs silence on underrun instead of replaying an old
     * DMA descriptor. USB feedback follows this fixed physical output clock;
     * the audio task primes a ring cushion before playback.
     *
     * Clock source: ESP32-S3 only exposes XTAL (40 MHz) and PLL_F160M for I2S
     * (no APLL on S3 I2S unit). Default PLL_F160M divides to 96 kHz × 64fs =
     * 6.144 MHz with fractional divider; PCM5102A's PLL has enough capture
     * range to lock. */
    i2s_chan_config_t chan0_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan0_cfg.dma_desc_num = 12;
    chan0_cfg.dma_frame_num = 240;
    chan0_cfg.auto_clear = true;

    err = i2s_new_channel(&chan0_cfg, &i2s_tx_chan0, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S0 TX channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std0_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_EXAMPLE_I2S0_BCK_PIN,
            .ws   = CONFIG_EXAMPLE_I2S0_LRCK_PIN,
            .dout = CONFIG_EXAMPLE_I2S0_DATA_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std0_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_chan0, &std0_cfg));

    /* ---------- I2S1 -- right speaker ---------- */
    i2s_chan_config_t chan1_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan1_cfg.dma_desc_num = 12;
    chan1_cfg.dma_frame_num = 240;
    chan1_cfg.auto_clear = true;

    err = i2s_new_channel(&chan1_cfg, &i2s_tx_chan1, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S1 TX channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std1_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_EXAMPLE_I2S1_BCK_PIN,
            .ws   = CONFIG_EXAMPLE_I2S1_LRCK_PIN,
            .dout = CONFIG_EXAMPLE_I2S1_DATA_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std1_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_chan1, &std1_cfg));

    /* Enable both channels back-to-back for phase-coherent start */
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_chan0));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_chan1));

    ESP_LOGI(TAG, "Dual I2S TX initialized at %lu Hz (S3 GPIOs)", (unsigned long)sample_rate);
    return ESP_OK;
}

void i2s_audio_write_dual(const uint8_t *buf_i2s0, const uint8_t *buf_i2s1, size_t size)
{
    size_t written0 = 0, written1 = 0;
    i2s_channel_write(i2s_tx_chan0, buf_i2s0, size, &written0, portMAX_DELAY);
    i2s_channel_write(i2s_tx_chan1, buf_i2s1, size, &written1, portMAX_DELAY);
}

void i2s_audio_reconfig_sample_rate(int sample_rate)
{
    /* Disable → reconfig → enable. This causes a brief gap (~1ms).
     * USB clock recovery never calls this: it adjusts host feedback instead. */
    i2s_channel_disable(i2s_tx_chan0);
    i2s_channel_disable(i2s_tx_chan1);

    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    i2s_channel_reconfig_std_clock(i2s_tx_chan0, &clk);
    i2s_channel_reconfig_std_clock(i2s_tx_chan1, &clk);

    i2s_channel_enable(i2s_tx_chan0);
    i2s_channel_enable(i2s_tx_chan1);
}

void i2s_audio_deinit(void)
{
    if (i2s_tx_chan0) {
        i2s_channel_disable(i2s_tx_chan0);
        i2s_del_channel(i2s_tx_chan0);
        i2s_tx_chan0 = NULL;
    }
    if (i2s_tx_chan1) {
        i2s_channel_disable(i2s_tx_chan1);
        i2s_del_channel(i2s_tx_chan1);
        i2s_tx_chan1 = NULL;
    }
}
