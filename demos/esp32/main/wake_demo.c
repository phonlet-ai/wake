/* ESP32-S3 wake-word demo: INMP441 I2S microphone -> phonlet_wake.
 *
 * Board: ESP32-S3-N16R8 (16 MB flash, 8 MB octal PSRAM), RGB LED on GPIO 48.
 * Wiring: VDD 3V3, GND GND, SCK GPIO 4, WS GPIO 5, SD GPIO 6, L/R GND.
 * Run demos/esp32-mic-check first to prove the microphone path.
 *
 * Copy resources/models/en-1/model.pwake and an enrollment array (for example
 * resources/keywords/en.json) to main/model.pwake and main/keywords.json
 * before building (see README). The LED glows dim blue while listening and
 * flashes orange on each detection; the console prints the phrase and, every
 * five seconds, the compute time per 32 ms frame. */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wake.h"
#include "s3_engine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

extern const uint8_t model_start[] asm("_binary_model_pwake_start");
extern const uint8_t model_end[] asm("_binary_model_pwake_end");
extern const uint8_t keywords_start[] asm("_binary_keywords_json_start");

#define SAMPLE_RATE 16000
#define FRAME_SAMPLES 512
#define PIN_SCK GPIO_NUM_4
#define PIN_WS GPIO_NUM_5
#define PIN_SD GPIO_NUM_6
#define PIN_LED GPIO_NUM_48
#define SENSITIVITY 0.5f
/* INMP441 samples are 24-bit in the top of a 32-bit slot. Shifting right by
 * GAIN_SHIFT gives PCM16 with 2^(16 - GAIN_SHIFT) gain over plain truncation
 * (16 = no gain, 15 = +6 dB). Measured with demos/esp32-mic-check on
 * 2026-10-30: speech near the board -25 dBFS rms, peaks -12 dBFS, so no gain. */
#define GAIN_SHIFT 16
#define FLASH_US (400 * 1000)

static i2s_chan_handle_t rx;
static led_strip_handle_t led;

static void i2s_start(void) {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.dma_desc_num = 8;
  chan.dma_frame_num = 256; /* 128 ms of buffering absorbs a slow frame */
  ESP_ERROR_CHECK(i2s_new_channel(&chan, NULL, &rx));
  i2s_std_config_t std = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      /* 32-bit slots: the INMP441 needs 64 bit clocks per sample. */
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
      .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = PIN_SCK, .ws = PIN_WS,
                   .dout = I2S_GPIO_UNUSED, .din = PIN_SD},
  };
  std.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT; /* L/R tied to GND */
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std));
  ESP_ERROR_CHECK(i2s_channel_enable(rx));
}

static void led_start(void) {
  led_strip_config_t strip = {.strip_gpio_num = PIN_LED, .max_leds = 1};
  led_strip_rmt_config_t rmt = {.resolution_hz = 10 * 1000 * 1000};
  ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip, &rmt, &led));
}

static void led_set(uint8_t r, uint8_t g, uint8_t b) {
  led_strip_set_pixel(led, 0, r, g, b);
  led_strip_refresh(led);
}

static int16_t to_pcm16(int32_t slot) {
  int32_t v = slot >> GAIN_SHIFT;
  return (int16_t)(v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v);
}

static void wake_task(void *arg) {
  static int32_t raw[FRAME_SAMPLES];
  static int16_t pcm[FRAME_SAMPLES];
  pw_wake *wake = NULL;
  pw_status status = esp_wake_begin(model_start, (int32_t)(model_end - model_start),
                                    (const char *)keywords_start, SENSITIVITY, &wake);
  if (status != PW_OK) {
    printf("wake init failed: %d (%s)\n", status, phonlet_wake_error());
    led_set(60, 0, 0);
    vTaskDelete(NULL);
  }
  printf("listening: %d Hz, %d-sample frames, gain shift %d\n", SAMPLE_RATE, FRAME_SAMPLES,
         GAIN_SHIFT);
  printf("memory free: internal %u KB, PSRAM %u KB\n",
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  led_set(0, 0, 8);
  int64_t flash_until = 0, busy_us = 0, report_at = esp_timer_get_time() + 5000000;
  int frames = 0, worst_us = 0;
  double loudest = 0; /* highest per-buffer RMS in the report window, PCM16 units */
  for (;;) {
    size_t bytes = 0;
    ESP_ERROR_CHECK(i2s_channel_read(rx, raw, sizeof raw, &bytes, portMAX_DELAY));
    int samples = (int)(bytes / sizeof(int32_t));
    double energy = 0;
    for (int i = 0; i < samples; i++) {
      pcm[i] = to_pcm16(raw[i]);
      energy += (double)pcm[i] * pcm[i];
    }
    if (samples && sqrt(energy / samples) > loudest) loudest = sqrt(energy / samples);

    int64_t start = esp_timer_get_time();
    int32_t fired = -1;
    status = esp_wake_feed(wake, pcm, samples, &fired);
    int64_t now = esp_timer_get_time();
    if (status != PW_OK) {
      printf("wake process failed: %d (%s)\n", status, phonlet_wake_error());
      continue;
    }
    busy_us += now - start;
    worst_us = (int)(now - start) > worst_us ? (int)(now - start) : worst_us;
    frames++;

    if (fired >= 0) {
      printf(">>> detected \"%s\"\n", esp_wake_keyword_name(fired));
      led_set(90, 20, 0); /* brand orange */
      flash_until = now + FLASH_US;
    } else if (flash_until && now > flash_until) {
      led_set(0, 0, 8);
      flash_until = 0;
    }
    if (now > report_at) {
      printf("compute: %.2f ms average, %.2f ms worst per 32 ms frame (%.0f%% of one core); "
             "loudest audio %.1f dBFS\n",
             busy_us / 1000.0 / frames, worst_us / 1000.0, 100.0 * busy_us / frames / 32000.0,
             loudest > 0 ? 20.0 * log10(loudest / 32768.0) : -120.0);
      loudest = 0;
      uint32_t kernel = 0, total = 0, profiled = 0;
      esp_wake_s3_profile(&kernel, &total, &profiled);
      size_t internal = 0, external = 0;
      esp_wake_s3_memory(&internal, &external);
      if (profiled)
        printf("  acoustic: %.2f ms per frame, of which integer kernels %.2f ms; weights %u KB "
               "internal, %u KB PSRAM\n",
               total / 240000.0 / profiled, kernel / 240000.0 / profiled,
               (unsigned)(internal / 1024), (unsigned)(external / 1024));
      busy_us = 0;
      worst_us = 0;
      frames = 0;
      report_at = now + 5000000;
    }
  }
}

void app_main(void) {
  led_start();
  i2s_start();
  /* The engine parses JSON and builds the session on this task: give it room. */
  xTaskCreatePinnedToCore(wake_task, "wake", 32 * 1024, NULL, 5, NULL, 1);
}
