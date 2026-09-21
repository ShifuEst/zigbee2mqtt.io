#pragma once
#include <stdint.h>

// Selected when building/flashing; never changed by pairing or remote commands.
#ifndef MD_RELAY_ACTIVE_HIGH
#error "Select MD_RELAY_ACTIVE_HIGH=0 (LOW relay) or 1 (HIGH relay)"
#endif
static_assert(MD_RELAY_ACTIVE_HIGH == 0 || MD_RELAY_ACTIVE_HIGH == 1,
              "Relay polarity must be 0 or 1");
constexpr uint8_t relayLevelFor(bool activeHigh, bool energized) {
  return activeHigh == energized ? 1 : 0;
}
constexpr bool RELAY_ACTIVE_HIGH = MD_RELAY_ACTIVE_HIGH != 0;
constexpr uint8_t RELAY_OFF_LEVEL = relayLevelFor(RELAY_ACTIVE_HIGH, false);
constexpr uint8_t RELAY_ON_LEVEL = relayLevelFor(RELAY_ACTIVE_HIGH, true);
static_assert(relayLevelFor(false, false) == 1 && relayLevelFor(false, true) == 0);
static_assert(relayLevelFor(true, false) == 0 && relayLevelFor(true, true) == 1);
