/**
 * File: blink.h
 * Author: Diego Parrilla Santamaría
 * Date: November 2024
 * Copyright: 2024 - GOODDATA LABS SL
 * Description: Header file for the blinking functions
 */

#ifndef BLINK_H
#define BLINK_H

#include "constants.h"
#include "debug.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"

#ifdef CYW43_WL_GPIO_LED_PIN
#include "network.h"
#include "pico/cyw43_arch.h"
#endif

// Morse code
#define DOT_DURATION_MS 150
#define DASH_DURATION_MS 450
#define SYMBOL_GAP_MS 150
#define CHARACTER_GAP_MS 700

typedef struct {
  char character;
  const char *morse;
} MorseCode;

/**
 * @brief Turns off the LED.
 *
 * This function turns off the LED by setting the appropriate GPIO pin to low.
 * It checks if the CYW43_WL_GPIO_LED_PIN is defined and uses it if available.
 * Otherwise, it defaults to using the PICO_DEFAULT_LED_PIN.
 */
void blink_off();

/**
 * @brief Turns on the LED.
 *
 * This function turns on the LED by setting the appropriate GPIO pin high.
 * It checks if the `CYW43_WL_GPIO_LED_PIN` is defined and uses it to set the
 * GPIO pin. If not defined, it defaults to using `PICO_DEFAULT_LED_PIN`.
 */
void blink_on();

/**
 * @brief Toggles the blink state and updates the blink status accordingly.
 *
 * This function toggles the global variable `blink_state` between true and
 * false. If `blink_state` is true, it calls the `blink_on()` function to turn
 * on the blink. If `blink_state` is false, it calls the `blink_off()` function
 * to turn off the blink.
 */
void blink_toogle();

#endif  // BLINK_H
