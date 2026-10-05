MD-GATE-ZB1 / ESP32-H2 / TXRX01 — BENCH CANDIDATE

Pins: S1=GPIO24 (UART TX); S2=GPIO23 (UART RX);
CLOSED dry contact=GPIO0; OPEN dry contact=GPIO1; contacts close to GND.
Native USB CDC; Zigbee router; 4 MiB flash; active LOW relay outputs.

This CI profile deliberately uses MD_TXRX_BENCH_ONLY, not a hardware-validation
claim. It compiles the functional candidate for an isolated bench. Only flash
with the H2 removed from the relay base and all gate connections removed.
It does not suppress UART ROM output before the application starts.
Do not connect this candidate directly to the gate relay base.

Known hardware questions must be resolved before commissioning:
1. Manufacturer schematic connects S1/S2 pull-ups R12/R6 and U4 gates to VCC.
   When VCC is 5V this is not a verified 3.3V MCU interface.
2. GPIO24 is UART0 TX. ROM/bootloader/reset/flashing activity can produce
   unintended low pulses before firmware can control the output.
   Application USB CDC selection does not eliminate that activity.
3. Measure startup, reset, brownout and flash behavior with the gate disconnected.

No eFuses are modified. No hardware test has been performed by this build.
MD_TXRX_HARDWARE_VALIDATED remains required for a commissioned build without
the bench profile. Do not define it without recorded hardware evidence.

Manufacturer schematic:
https://github.com/nulllaborg/esp32-c3-relay-module/blob/main/2_relay_module_sch.pdf
