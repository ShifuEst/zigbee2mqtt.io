#pragma once
#include <stdint.h>
// TXRX01 USB-only firmware; electrical commissioning is still required.
#ifndef MD_TXRX_QUIET_BOOT
#error "Build using the supplied ESP-IDF project with quiet boot settings"
#endif
#if !CONFIG_BOOTLOADER_LOG_LEVEL_NONE || !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#error "Bootloader must be quiet and IDF console must use native USB"
#endif
#if !defined(ARDUINO_USB_CDC_ON_BOOT) || !ARDUINO_USB_CDC_ON_BOOT
#error "Serial must use native USB CDC; UART0 TX is now relay S1."
#endif
#if !defined(CONFIG_IDF_TARGET_ESP32H2)
#error "This pin mapping is for ESP32-H2 only."
#endif
static_assert(MD_RELAY_ACTIVE_HIGH == 0, "This relay board is active LOW");
constexpr uint8_t RELAY_PIN = 24;
constexpr uint8_t WALK_PIN = 23;
constexpr uint8_t CLOSED_PIN = 0;
constexpr uint8_t OPEN_PIN = 1;
static_assert(RELAY_PIN != WALK_PIN && CLOSED_PIN != OPEN_PIN);
static_assert(CLOSED_PIN != RELAY_PIN && CLOSED_PIN != WALK_PIN);
static_assert(OPEN_PIN != RELAY_PIN && OPEN_PIN != WALK_PIN);
// Native USB CDC prevents application UART output only.
// It does NOT silence the ROM bootloader or establish electrical safety.
