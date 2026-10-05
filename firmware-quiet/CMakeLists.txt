cmake_minimum_required(VERSION 3.16)
add_compile_definitions(ARDUINO_USB_CDC_ON_BOOT=1 ARDUINO_USB_MODE=1 ZIGBEE_MODE_ZCZR=1 MD_RELAY_ACTIVE_HIGH=0 MD_TXRX_QUIET_BOOT=1)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(md_gate_h2)
