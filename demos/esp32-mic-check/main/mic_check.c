/* INMP441 wiring check for ESP32-S3.
 *
 * Wiring: VDD 3V3, GND GND, SCK GPIO 4, WS GPIO 5, SD GPIO 6, L/R GND.
 *
 * Reads both I2S slots (the INMP441 sends 24-bit samples in 32-bit slots and
 * needs 64 clocks per sample) and prints, four times a second, the level of
 * each slot and a verdict. The RGB LED on GPIO 48 shows the level: off when
 * silent, green while you speak, red when nothing arrives. */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#define SAMPLE_RATE 16000
#define PIN_SCK GPIO_NUM_4
#define PIN_WS GPIO_NUM_5
#define PIN_SD GPIO_NUM_6
#define PIN_LED GPIO_NUM_48
#define WINDOW_FRAMES (SAMPLE_RATE / 4) /* 250 ms */
#define CHUNK_FRAMES 256

typedef struct {
  double sum, sum_sq;
  int32_t peak, first;
  int zeros, same;
} slot_stats;

static i2s_chan_handle_t rx;
static led_strip_handle_t led;

static void i2s_start(void) {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.dma_desc_num = 6;
  chan.dma_frame_num = 240;
  ESP_ERROR_CHECK(i2s_new_channel(&chan, NULL, &rx));
  i2s_std_config_t std = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = PIN_SCK, .ws = PIN_WS,
                   .dout = I2S_GPIO_UNUSED, .din = PIN_SD},
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std));
  ESP_ERROR_CHECK(i2s_channel_enable(rx));
}

static void led_start(void) {
  led_strip_config_t strip = {.strip_gpio_num = PIN_LED, .max_leds = 1};
  led_strip_rmt_config_t rmt = {.resolution_hz = 10 * 1000 * 1000};
  ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip, &rmt, &led));
  led_strip_clear(led);
}

static void led_set(uint8_t r, uint8_t g, uint8_t b) {
  led_strip_set_pixel(led, 0, r, g, b);
  led_strip_refresh(led);
}

/* 24-bit sample, full scale 2^23. */
static double dbfs(double rms) { return rms > 0 ? 20.0 * log10(rms / 8388608.0) : -200.0; }

static void add(slot_stats *s, int32_t raw, int index) {
  int32_t v = raw >> 8; /* data sits in the top 24 bits */
  if (index == 0) s->first = v;
  s->sum += v;
  s->sum_sq += (double)v * v;
  int32_t mag = v < 0 ? -v : v;
  if (mag > s->peak) s->peak = mag;
  if (v == 0) s->zeros++;
  if (v == s->first) s->same++;
}

typedef enum { NO_DATA, STUCK, NEAR_SILENT, SIGNAL } slot_state;
static const char *const state_names[] = {"NO DATA", "STUCK", "NEAR SILENT", "SIGNAL"};

static slot_state verdict(const slot_stats *s, int n, double rms_db) {
  if (s->zeros == n) return NO_DATA;
  if (s->same == n) return STUCK;
  if (rms_db < -95.0) return NEAR_SILENT;
  return SIGNAL;
}

void app_main(void) {
  static int32_t buf[CHUNK_FRAMES * 2];
  led_start();
  i2s_start();
  printf("\nmic_check: INMP441 on SCK=%d WS=%d SD=%d, %d Hz, 32-bit stereo slots\n",
         PIN_SCK, PIN_WS, PIN_SD, SAMPLE_RATE);
  vTaskDelay(pdMS_TO_TICKS(150)); /* INMP441 start-up time */
  for (int report = 0;; report++) {
    slot_stats left = {0}, right = {0};
    int frames = 0;
    while (frames < WINDOW_FRAMES) {
      size_t bytes = 0;
      ESP_ERROR_CHECK(i2s_channel_read(rx, buf, sizeof buf, &bytes, portMAX_DELAY));
      int got = (int)(bytes / (2 * sizeof(int32_t)));
      for (int i = 0; i < got; i++) {
        add(&left, buf[2 * i], frames + i);
        add(&right, buf[2 * i + 1], frames + i);
      }
      frames += got;
    }
    double l_mean = left.sum / frames, r_mean = right.sum / frames;
    double l_db = dbfs(sqrt(left.sum_sq / frames - l_mean * l_mean));
    double r_db = dbfs(sqrt(right.sum_sq / frames - r_mean * r_mean));
    slot_state lv = verdict(&left, frames, l_db), rv = verdict(&right, frames, r_db);
    const char *advice = "OK: microphone on the left slot (L/R to GND)";
    if (lv != SIGNAL && rv == SIGNAL)
      advice = "L/R is high: tie L/R to GND";
    else if (lv != SIGNAL)
      advice = "no audio: check VDD=3V3, GND, SD=GPIO6, SCK=GPIO4, WS=GPIO5";
    printf("[%4d] L %-11s rms %6.1f dBFS peak %6.1f dBFS dc %8.0f | R %-11s rms %6.1f dBFS | %s\n",
           report, state_names[lv], l_db, dbfs(left.peak), l_mean, state_names[rv], r_db, advice);
    if (lv != SIGNAL) {
      led_set(40, 0, 0);
    } else {
      /* -70 dBFS (quiet room) .. -20 dBFS (loud speech) -> LED brightness */
      double level = (l_db + 70.0) / 50.0;
      level = level < 0 ? 0 : level > 1 ? 1 : level;
      led_set(0, (uint8_t)(level * 80), 0);
    }
  }
}
