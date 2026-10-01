cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350 CACHE STRING "Platform")

include(pico_sdk_import.cmake)

project(sampleplayer C CXX ASM)

pico_sdk_init()

add_executable(sampleplayer
    src/main.c
    src/audio_i2s.c
    src/mixer.c
    src/encoder.c
    src/midi.c
    src/ui.c
    src/settings.c
    src/settings_flash.c
    src/ssd1306.c
    src/oled_i2c.c
)

pico_generate_pio_header(sampleplayer ${CMAKE_CURRENT_LIST_DIR}/src/audio_i2s.pio)

target_include_directories(sampleplayer PRIVATE src)

target_link_libraries(sampleplayer
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_i2c
    hardware_uart
    hardware_flash
    pico_flash
    hardware_sync
)

# Debug console on USB CDC; UART0 stays free (GP0/GP1) but is disabled as a
# stdio target, so it does not get in the way of future instrumentation.
pico_enable_stdio_usb(sampleplayer 1)
pico_enable_stdio_uart(sampleplayer 0)

pico_add_extra_outputs(sampleplayer)

# Which panel this build is for: 1U/20HP (0.91" 128x32) or 3U/6HP (0.96"
# 128x64). Fixed per format, picked at compile time, not read at runtime.
set(MURDRUM_PANEL "1U" CACHE STRING "Panel format: 1U (128x32) or 6HP (128x64)")
set_property(CACHE MURDRUM_PANEL PROPERTY STRINGS 1U 6HP)
if(MURDRUM_PANEL STREQUAL "6HP")
    target_compile_definitions(sampleplayer PRIVATE OLED_PANEL_128X64)
elseif(NOT MURDRUM_PANEL STREQUAL "1U")
    message(FATAL_ERROR "MURDRUM_PANEL must be 1U or 6HP, got '${MURDRUM_PANEL}'")
endif()

# Which way the encoder turns. The EC11's outer pair is not marked and is not
# consistent between makers, so a knob that decodes CW as CCW is a part, not a
# wiring fault - see pins.h. REV swaps the two quadrature channels handed to
# the decoder; nothing else in the firmware changes.
set(MURDRUM_ENC "STD" CACHE STRING "Encoder direction: STD or REV")
set_property(CACHE MURDRUM_ENC PROPERTY STRINGS STD REV)
if(MURDRUM_ENC STREQUAL "REV")
    target_compile_definitions(sampleplayer PRIVATE ENCODER_REVERSED)
elseif(NOT MURDRUM_ENC STREQUAL "STD")
    message(FATAL_ERROR "MURDRUM_ENC must be STD or REV, got '${MURDRUM_ENC}'")
endif()

# Build identifier shown on the ABOUT page: the git short hash (+ "-dirty"),
# computed by build.sh so it is never stale (see version.h) and passed
# through here rather than run from CMake, since build.sh already
# reconfigures on every invocation anyway.
set(FIRMWARE_BUILD "unknown" CACHE STRING "Build identifier, set by build.sh")
target_compile_definitions(sampleplayer PRIVATE FIRMWARE_BUILD="${FIRMWARE_BUILD}")

# Must stay in sync with SAMPLE_LIB_FLASH_OFFSET in src/sample_lib.h.
set(SAMPLE_LIB_FLASH_OFFSET 0x80000)

add_custom_command(TARGET sampleplayer POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DBIN=$<TARGET_FILE_DIR:sampleplayer>/sampleplayer.bin
        -DLIMIT=${SAMPLE_LIB_FLASH_OFFSET}
        -P ${CMAKE_CURRENT_LIST_DIR}/tools/check_firmware_size.cmake
    VERBATIM)
