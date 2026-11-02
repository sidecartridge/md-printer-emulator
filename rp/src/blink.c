#include "blink.h"

static bool blinkState = false;
static absolute_time_t blinkTime = {0};

#if defined(CYW43_WL_GPIO_LED_PIN)
// The Pico W's LED is on its Wi-Fi chip: the chip's driver (poll mode, no
// network) starts at the LED's first use.
static bool chipReady = false;

static bool blink_initChip(void) {
  if (!chipReady) {
    int res = cyw43_arch_init();
    if (res != 0) {
      DPRINTF("Failed to initialize CYW43: %d\n", res);
      return false;
    }
    chipReady = true;
  }
  return true;
}
#endif

void blink_on() {
#if defined(CYW43_WL_GPIO_LED_PIN)
  if (blink_initChip()) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
  }
#else
  gpio_put(PICO_DEFAULT_LED_PIN, 1);
#endif
}

void blink_off() {
#if defined(CYW43_WL_GPIO_LED_PIN)
  if (blink_initChip()) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
  }
#else
  gpio_put(PICO_DEFAULT_LED_PIN, 0);
#endif
}

void blink_poll() {
#if defined(CYW43_WL_GPIO_LED_PIN)
  if (chipReady) {
    cyw43_arch_poll();
  }
#endif
}

void blink_toogle() {
  // Blink when timeout
  if ((absolute_time_diff_us(get_absolute_time(), blinkTime) < 0)) {
    blinkState = !blinkState;
    blinkTime = make_timeout_time_ms(CHARACTER_GAP_MS);
    if (blinkState) {
      blink_on();
    } else {
      blink_off();
    }
  }
}
