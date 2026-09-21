#include <Arduino.h>
#include "RelayLevels.h"
#include "Zigbee.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include <Preferences.h>
#include <math.h>
#include "esp_system.h"

#ifndef ZIGBEE_MODE_ZCZR
#error "Build with Zigbee ZCZR and zigbee_zczr partition scheme"
#endif

// Keep the contact type and explicit prototype before Arduino-generated prototypes.
struct Contact {
  uint8_t pin;
  bool candidate = false, stable = false;
  uint32_t changed = 0;
};
bool sampleContact(Contact &contact);

// LOW-only package: requires 3.3V-compatible relay inputs.
// Each output needs its own 10k pull-up to 3.3V. Never pull either GPIO to 5V.
constexpr uint8_t RELAY_PIN = 10, WALK_PIN = 11, CLOSED_PIN = 13, OPEN_PIN = 14;
constexpr uint32_t DEFAULT_PULSE_MS = 300, MIN_PULSE_MS = 0, MAX_PULSE_MS = 1000, DEBOUNCE_MS = 40;
uint32_t pulseMs = DEFAULT_PULSE_MS;
Preferences settings;
class PulseDurationEndpoint : public ZigbeeAnalog {
public:
  PulseDurationEndpoint(uint8_t endpoint) : ZigbeeAnalog(endpoint) {}
  bool updateValue(float value) {
    return setClusterAttribute(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_OUTPUT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
      ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_PRESENT_VALUE_ID, &value, false) == ESP_ZB_ZCL_STATUS_SUCCESS;
  }
};
PulseDurationEndpoint durationSetting(4);
class ContactEndpoint : public ZigbeeBinary {
public:
  ContactEndpoint(uint8_t endpoint) : ZigbeeBinary(endpoint) {}
  bool reportValue() {
    esp_zb_zcl_report_attr_cmd_t command = {};
    command.address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT;
    command.zcl_basic_cmd.src_endpoint = _endpoint;
    command.clusterID = ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT;
    command.attributeID = ESP_ZB_ZCL_ATTR_BINARY_INPUT_PRESENT_VALUE_ID;
    command.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
    command.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
    return reportClusterAttribute(&command);
  }
};
volatile bool durationPending = false;
volatile float requestedDuration = DEFAULT_PULSE_MS;
ZigbeePowerOutlet relay(1), walkRelay(5);
void resetRelays() { relay.setState(false); walkRelay.setState(false); }
ContactEndpoint closedSensor(2), openSensor(3);
rmt_channel_handle_t pulseChannels[2] = {nullptr, nullptr};
uint8_t activeRelay = 0;
volatile uint8_t requestedRelay = 0;
rmt_encoder_handle_t pulseEncoder = nullptr;
rmt_symbol_word_t pulseSymbols[8];
portMUX_TYPE pulseMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool requestPulse = false, pulseActive = false, pulseDone = false;
volatile bool acceptCommands = false;
volatile bool clearIdleState = false;
volatile uint32_t pulseElapsedUs = 0, pulseStartedUs = 0;
bool stackReady = false;
uint32_t connectedSince = 0;
bool connectionObserved = false;
constexpr uint32_t JOIN_GUARD_MS = 10000;

Contact closedContact{CLOSED_PIN}, openContact{OPEN_PIN};

bool endPulse(rmt_channel_handle_t, const rmt_tx_done_event_data_t *, void *) {
  // RMT has already driven the pin to its selected OFF level in hardware, before this callback.
  portENTER_CRITICAL_ISR(&pulseMux);
  pulseElapsedUs = micros() - pulseStartedUs;
  pulseActive = false;
  pulseDone = true;
  portEXIT_CRITICAL_ISR(&pulseMux);
  return false;
}

bool sendWaveform(bool active, uint32_t durationMs) {
  // Zero is never sent to RMT: it would be an end marker, not a pulse.
  if (durationMs == 0 || durationMs > MAX_PULSE_MS) return false;
  const uint32_t ticks = durationMs * 100;
  const uint32_t base = ticks / 16, remainder = ticks % 16;
  for (uint32_t i = 0; i < 8; ++i) {
    // Distribute remaining 10us ticks for exact integer-millisecond totals.
    pulseSymbols[i].duration0 = base + (2 * i < remainder);
    pulseSymbols[i].duration1 = base + (2 * i + 1 < remainder);
    pulseSymbols[i].level0 = relayLevelFor(RELAY_ACTIVE_HIGH, active);
    pulseSymbols[i].level1 = relayLevelFor(RELAY_ACTIVE_HIGH, active);
  }
  rmt_transmit_config_t config = {};
  config.loop_count = 0;
  config.flags.eot_level = RELAY_OFF_LEVEL;
  pulseStartedUs = micros();
  return rmt_transmit(pulseChannels[activeRelay], pulseEncoder, pulseSymbols, sizeof(pulseSymbols), &config) == ESP_OK;
}

bool validDuration(float value) {
  return isfinite(value) && value >= MIN_PULSE_MS && value <= MAX_PULSE_MS &&
    value == floorf(value);
}

void onDurationWrite(float value) {
  // The stack callback never writes flash or activates the relay.
  portENTER_CRITICAL(&pulseMux);
  requestedDuration = value;
  durationPending = true;
  portEXIT_CRITICAL(&pulseMux);
}

void publishDuration() {
  // Update the attribute without invoking the network-write callback.
  durationSetting.updateValue(static_cast<float>(pulseMs));
  // Duration is read explicitly by the coordinator; no manual report here.
}

// Only an explicit network ON requests a pulse. OFF never activates the pin.
// No queue of future pulses; repeated ONs during a pulse are discarded.
void requestRelayCommand(bool state, uint8_t index) {
  if (!state) return;
  portENTER_CRITICAL(&pulseMux);
  if (acceptCommands && digitalRead(BOOT_PIN) != LOW && !pulseActive && !pulseDone && !requestPulse) {
    requestedRelay = index;
    requestPulse = true;
    acceptCommands = false;
  } else clearIdleState = true;
  portEXIT_CRITICAL(&pulseMux);
}

void onRelayCommand(bool state) { requestRelayCommand(state, 0); }
void onWalkCommand(bool state) { requestRelayCommand(state, 1); }

void failSafe(const char *reason) {
  // Retain the physical output while disconnecting RMT; never reset the pad to input.
  // If a pulse is active, let its bounded hardware waveform finish first.
  if (pulseActive) delay(MAX_PULSE_MS + 10);
  for (int i = 0; i < 2; ++i) {
    const gpio_num_t pin = i == 0 ? GPIO_NUM_10 : GPIO_NUM_11;
    gpio_hold_en(pin);
    if (pulseChannels[i]) rmt_disable(pulseChannels[i]);
    gpio_set_level(pin, RELAY_OFF_LEVEL);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
  }
  Serial.printf("FATAL: %s; relay OFF\n", reason);
  while (true) delay(1000);
}

bool sampleContact(Contact &contact) {
  const bool value = digitalRead(contact.pin) == LOW;
  if (value != contact.candidate) {
    contact.candidate = value;
    contact.changed = millis();
  }
  if (contact.stable != contact.candidate && millis() - contact.changed >= DEBOUNCE_MS) {
    contact.stable = contact.candidate;
    return true;
  }
  return false;
}

void reportContacts() {
  closedSensor.setBinaryInput(closedContact.stable);
  openSensor.setBinaryInput(openContact.stable);
  if (Zigbee.connected()) {
    closedSensor.reportValue();
    openSensor.reportValue();
  }
}

void setup() {
  // Set output latch before enabling output. Never restore a saved relay state.
  for (const gpio_num_t pin : {GPIO_NUM_10, GPIO_NUM_11}) {
    gpio_set_level(pin, RELAY_OFF_LEVEL);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    if (RELAY_ACTIVE_HIGH) { gpio_pullup_dis(pin); gpio_pulldown_en(pin); }
    else { gpio_pulldown_dis(pin); gpio_pullup_en(pin); }
    gpio_hold_dis(pin);
    gpio_hold_en(pin);
  }
  pinMode(CLOSED_PIN, INPUT_PULLUP);
  pinMode(OPEN_PIN, INPUT_PULLUP);
  pinMode(BOOT_PIN, INPUT_PULLUP);
  Serial.begin(115200);
  // Diagnostics must never wait for an absent USB host reader.
  Serial.setTxTimeoutMs(10);
  delay(1200);
  Serial.println("\nMakeDIY MD-GATE-ZB1 firmware 1.7.0-rc2");
  Serial.println("ESP32-H2 / Zigbee ROUTER / ZCZR 4MB / no Wi-Fi");
  Serial.printf("RESET reason=%d\n", (int)esp_reset_reason());
  if (!settings.begin("md-gate", false)) failSafe("settings storage");
  pulseMs = settings.getUInt("pulse_ms", DEFAULT_PULSE_MS);
  if (!validDuration(static_cast<float>(pulseMs))) pulseMs = DEFAULT_PULSE_MS;
  Serial.printf("GPIO10 OFF=%u ON=%u; pulse_ms=%lu (0..1000 step 1; zero disables pulse); GPIO11=WALK GPIO13=CLOSED GPIO14=OPEN\n", RELAY_OFF_LEVEL, RELAY_ON_LEVEL, (unsigned long)pulseMs);
  Serial.printf("BOOT GPIO%d hold 3 seconds: factory reset; short press does nothing\n", BOOT_PIN);

  rmt_copy_encoder_config_t encoderConfig = {};
  if (rmt_new_copy_encoder(&encoderConfig, &pulseEncoder) != ESP_OK) failSafe("RMT encoder");
  for (uint8_t i = 0; i < 2; ++i) {
    const gpio_num_t pin = i == 0 ? GPIO_NUM_10 : GPIO_NUM_11;
    rmt_tx_channel_config_t config = {};
    config.gpio_num = pin;
    config.flags.invert_out = false;
    config.flags.init_level = RELAY_OFF_LEVEL;
    config.clk_src = RMT_CLK_SRC_DEFAULT;
    config.resolution_hz = 100000;
    config.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
    config.trans_queue_depth = 1;
    rmt_tx_event_callbacks_t callbacks = {};
    callbacks.on_trans_done = endPulse;
    if (rmt_new_tx_channel(&config, &pulseChannels[i]) != ESP_OK ||
        rmt_tx_register_event_callbacks(pulseChannels[i], &callbacks, nullptr) != ESP_OK ||
        rmt_enable(pulseChannels[i]) != ESP_OK) failSafe("RMT channel setup");
    activeRelay = i;
    pulseDone = false;
    if (!sendWaveform(false, 500)) failSafe("RMT self-test start");
    uint32_t testStarted = millis();
    while (!pulseDone && millis() - testStarted < 750) delay(1);
    if (!pulseDone || pulseElapsedUs < 495000 || pulseElapsedUs > 510000) failSafe("500ms timer self-test");
    Serial.printf("RMT SELFTEST PASS GPIO%d held OFF elapsed_us=%lu\n", pin, (unsigned long)pulseElapsedUs);
    pulseDone = false;
    gpio_hold_dis(pin);
  }
  activeRelay = 0;

  relay.setManufacturerAndModel("MakeDIY", "MD-GATE-ZB1");
  closedSensor.setManufacturerAndModel("MakeDIY", "MD-GATE-ZB1");
  openSensor.setManufacturerAndModel("MakeDIY", "MD-GATE-ZB1");
  durationSetting.setManufacturerAndModel("MakeDIY", "MD-GATE-ZB1");
  walkRelay.setManufacturerAndModel("MakeDIY", "MD-GATE-ZB1");
  relay.setVersion(21);
  walkRelay.setVersion(21);
  if (!durationSetting.addAnalogOutput()) failSafe("analog output cluster");
  durationSetting.setAnalogOutputDescription("Pulse duration (ms)");
  durationSetting.setAnalogOutputMinMax(MIN_PULSE_MS, MAX_PULSE_MS);
  durationSetting.setAnalogOutputResolution(1);
  durationSetting.onAnalogOutputChange(onDurationWrite);
  relay.onPowerOutletChange(onRelayCommand);
  walkRelay.onPowerOutletChange(onWalkCommand);
  if (!closedSensor.addBinaryInput() || !openSensor.addBinaryInput()) failSafe("input clusters");
  closedSensor.setBinaryInputDescription("CLOSED limit contact");
  openSensor.setBinaryInputDescription("OPEN limit contact (optional)");
  if (!Zigbee.addEndpoint(&relay) || !Zigbee.addEndpoint(&closedSensor) || !Zigbee.addEndpoint(&openSensor) || !Zigbee.addEndpoint(&durationSetting) || !Zigbee.addEndpoint(&walkRelay)) {
    failSafe("endpoint registration");
  }
  Zigbee.setTimeout(10000);
  Serial.println("Starting Zigbee router; awaiting permitted network...");
  if (!Zigbee.begin(ZIGBEE_ROUTER)) failSafe("Zigbee start");
  stackReady = true;
  resetRelays();
  closedContact.candidate = closedContact.stable = digitalRead(CLOSED_PIN) == LOW;
  openContact.candidate = openContact.stable = digitalRead(OPEN_PIN) == LOW;
  closedSensor.setBinaryInput(closedContact.stable);
  openSensor.setBinaryInput(openContact.stable);
  publishDuration();
  Serial.println("Zigbee stack READY; endpoints 1=OnOff 2=CLOSED 3=OPEN 4=pulse_duration_ms 5=WALK");
}

void loop() {
  static uint32_t buttonSince = 0, lastStatus = 0;
  static bool buttonHeld = false, wasConnected = false;
  const uint32_t now = millis();
  const bool connected = stackReady && Zigbee.connected();
  if (connected && !connectionObserved) connectedSince = now;
  connectionObserved = connected;
  const bool joinGuardPassed = connected && (now - connectedSince >= JOIN_GUARD_MS);

  if (digitalRead(BOOT_PIN) == LOW) {
    if (!buttonHeld) { buttonHeld = true; buttonSince = now; }
    portENTER_CRITICAL(&pulseMux);
    acceptCommands = false;
    requestPulse = false;
    portEXIT_CRITICAL(&pulseMux);
    if (now - buttonSince >= 3000) {
      portENTER_CRITICAL(&pulseMux);
      acceptCommands = false;
      requestPulse = false;
      portEXIT_CRITICAL(&pulseMux);
      // The last possible pulse ended at least two seconds ago. Keep RMT attached
      // and latch the selected OFF level without releasing GPIO10 to input.
      gpio_hold_en(GPIO_NUM_10);
      gpio_hold_en(GPIO_NUM_11);
      Serial.println("FACTORY RESET: relay OFF; erasing Zigbee network and restarting");
      settings.clear();
      // Reset after button release; never restart while BOOT is held.
      while (digitalRead(BOOT_PIN) == LOW) delay(10);
      delay(50);
      Zigbee.factoryReset();
      while (true) delay(1000);
    }
  } else buttonHeld = false;

  bool start = false, finished = false;
  portENTER_CRITICAL(&pulseMux);
  if (!joinGuardPassed || buttonHeld) requestPulse = false;
  if (requestPulse && joinGuardPassed && !buttonHeld) {
    requestPulse = false;
    activeRelay = requestedRelay;
    pulseActive = true;
    start = true;
  }
  if (pulseDone) { pulseDone = false; finished = true; }
  portEXIT_CRITICAL(&pulseMux);
  if (start) {
    if (pulseMs == 0) {
      // Do not transmit any waveform or briefly activate GPIO10.
      portENTER_CRITICAL(&pulseMux);
      pulseActive = false;
      portEXIT_CRITICAL(&pulseMux);
      resetRelays();
      Serial.println("PULSE DISABLED: duration=0; both relays remain OFF");
    } else {
      if (!sendWaveform(true, pulseMs)) failSafe("RMT pulse start");
      Serial.printf("PULSE START GPIO%d %lums\n", activeRelay == 0 ? 10 : 11, (unsigned long)pulseMs);
    }
  }
  if (finished) {
    resetRelays();
    Serial.printf("PULSE END both relays idle elapsed_us=%lu\n", (unsigned long)pulseElapsedUs);

  }
  // Do not run potentially blocking Zigbee reporting while the pulse is active.
  // RMT switches the output OFF in hardware, independently of this loop/ISR.
  if (!pulseActive) {
    bool updateDuration = false;
    float newDuration = 0;
    portENTER_CRITICAL(&pulseMux);
    if (durationPending && !requestPulse && !pulseDone) {
      updateDuration = true;
      newDuration = requestedDuration;
      durationPending = false;
      acceptCommands = false;
    }
    portEXIT_CRITICAL(&pulseMux);
    if (updateDuration) {
      if (validDuration(newDuration) && newDuration != pulseMs) {
        if (settings.putUInt("pulse_ms", static_cast<uint32_t>(newDuration)) != sizeof(uint32_t)) failSafe("saving pulse duration");
        pulseMs = static_cast<uint32_t>(newDuration);
        Serial.printf("CONFIG SAVED pulse_ms=%lu; relay unchanged\n", (unsigned long)pulseMs);
        publishDuration();
      } else {
        if (!validDuration(newDuration)) Serial.println("CONFIG REJECTED: duration must be an integer from 0 to 1000ms");
        publishDuration();
      }
    }
    bool clearState = false;
    portENTER_CRITICAL(&pulseMux);
    if (clearIdleState && !requestPulse && !pulseDone) {
      clearState = true;
      clearIdleState = false;
    }
    portEXIT_CRITICAL(&pulseMux);
    if (clearState) resetRelays();
    bool changed = sampleContact(closedContact);
    changed = sampleContact(openContact) || changed;
    // Only report real contact changes. Initial values are read during configure.
    if (changed) reportContacts();
    if (connected && !wasConnected) { Serial.println("ZIGBEE JOINED: router connected"); publishDuration(); }
    if (!connected && wasConnected) Serial.println("ZIGBEE DISCONNECTED: no relay action");
    wasConnected = connected;
    portENTER_CRITICAL(&pulseMux);
    // No post-pulse cooldown: either channel is ready again after completion.
    acceptCommands = joinGuardPassed && !buttonHeld && !pulseDone && !requestPulse;
    portEXIT_CRITICAL(&pulseMux);
  }
  if (now - lastStatus >= 5000) {
    lastStatus = now;
    Serial.printf("STATUS uptime=%lus joined=%d relay=%d closed=%d open=%d heap=%u reset_reason=%d\n",
      (unsigned long)(now / 1000), connected, pulseActive, closedContact.stable, openContact.stable, ESP.getFreeHeap(), (int)esp_reset_reason());
  }
  delay(5);
}


