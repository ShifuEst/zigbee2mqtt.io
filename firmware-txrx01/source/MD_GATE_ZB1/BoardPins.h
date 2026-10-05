#pragma once
#include <stdint.h>
// MD-GATE-ZB1: same protocol, new physical relay pins.
// TXRX-01 candidate. Hardware acceptance is still required.
// Pins after S1/S2 in the same row are sensor inputs GPIO0/GPIO1.
#if !defined(MD_TXRX_HARDWARE_VALIDATED) && !defined(MD_TXRX_BENCH_ONLY)
#error "TX/RX socket wiring requires verified 3.3V interface and boot/reset/flash pulse suppression. See LOE_ENNE.txt."
#endif
#ifdef MD_TXRX_BENCH_ONLY
#warning "BENCH ONLY: hardware is NOT validated. Disconnect the relay base and gate before flashing."
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
