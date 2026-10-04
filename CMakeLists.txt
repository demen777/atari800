# == DO NEVER EDIT THE NEXT LINES for Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.22)

# Target: Waveshare RP2350-PiZero (RP2350B, 520K SRAM, 16M flash, DVI on GPIO32-39).
# Must be set before pico_sdk_import so the SDK pre-load picks the rp2350/Cortex-M33 path.
set(PICO_PLATFORM rp2350 CACHE STRING "Pico platform")
set(PICO_BOARD waveshare_rp2350_pizero CACHE STRING "Target board")

include(pico_sdk_import.cmake)

project(pico-atari C CXX ASM)
pico_sdk_init()

option(VGA "Enable VGA" OFF)
option(TFT "Enable TFT display" OFF)
option(ILI9341 "Enable TFT ILI9341 display" OFF)
option(TV "Enable composite TV output" OFF)
option(USB_LOG "Send printf/Log_print to the USB CDC port (bring-up diagnostics)" OFF)
option(KBD_USB "USB HID keyboard on the native USB port (host mode)" OFF)
option(SD_LOG "Append printf/Log_print to \\atari.log on the SD card (MNGR_DEBUG)" OFF)
set(SYS_CLOCK_KHZ 252000 CACHE STRING "System clock in kHz; 252000 gives an exact DVI pixel clock")
option(FLASH_SIZE "Target Flash Size" 16384)

if(NOT FLASH_SIZE)
set(FLASH_SIZE 16384)
endif ()

SET(BUILD_NAME "${PROJECT_NAME}-${FLASH_SIZE}K")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -ffast-math -feliminate-unused-debug-types -ffunction-sections -fdata-sections -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}  -ffast-math -feliminate-unused-debug-types -ffunction-sections -fdata-sections -O2")

set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${OUTPUT_DIR}")

set(FAMILY rp2350)
set(BOARD pico_sdk)

add_subdirectory(drivers/ps2)
IF (KBD_USB)
    add_subdirectory(drivers/usbkbd)
ENDIF ()
add_subdirectory(drivers/fatfs)
add_subdirectory(drivers/sdcard)
add_subdirectory(drivers/nespad)
add_subdirectory(drivers/psram-sram)

add_subdirectory(drivers/vga-nextgen)
add_subdirectory(drivers/st7789)
add_subdirectory(drivers/hdmi)
add_subdirectory(drivers/tv)
add_subdirectory(drivers/graphics)

# INCLUDE FILES THAT SHOULD BE COMPILED:
file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c"
#        "drivers/usb/*.c"
)

message(STATUS "Add source files:")
foreach (SRC_FILE IN LISTS SRC)
    message(STATUS "${SRC_FILE}")
endforeach ()
message(STATUS "")

add_executable(${PROJECT_NAME} ${SRC})
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Core 0's stack must not live in SCRATCH_Y: see linker_overrides/ for why.
pico_add_linker_script_override_path(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/linker_overrides GLOB_FILES)

pico_set_program_name(${PROJECT_NAME} "pico-atari")
pico_set_program_version(${PROJECT_NAME} "test")

target_link_libraries(${PROJECT_NAME} PRIVATE
        ps2
        sdcard
        psram-sram
        fatfs
        nespad

        graphics

        pico_multicore
        pico_runtime
        pico_stdlib

        hardware_dma
        hardware_pio
        hardware_i2c
        hardware_interp
        hardware_timer
        hardware_clocks
        hardware_pwm
        hardware_flash

)

#family_configure_device_example(${PROJECT_NAME} noos)
target_link_options(${PROJECT_NAME} PRIVATE -Xlinker --print-memory-usage)
# Do NOT put src/ or drivers/usb on this list ahead of the SDK: both used to
# carry a USB *host* tusb_config.h (CFG_TUH_ENABLED, no CFG_TUD_*) which
# shadowed pico_stdio_usb's own tusb_config.h via -I beating -isystem. The
# result was a TinyUSB build with no device CDC: no serial port ever appeared,
# picotool could not see the board without BOOT, and stdio_init_all() blocked
# forever waiting for a CDC connection that could never happen.
target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
target_compile_options(${PROJECT_NAME} PUBLIC
        -Wno-error=suggest-attribute=format
        -Wno-error=cast-qual
        -Wno-error=unused-parameter
        -Wno-error=conversion
        -Wno-error=format=
        -Wno-error=sign-compare
        -Wno-error=missing-field-initializers
        -Wno-error=switch
        -Wno-error=implicit-fallthrough=
        -Wno-error=stringop-truncation
        -Wno-error=restrict
        # Demoted back to warnings: default-errors as of GCC 14/15, all
        # pre-existing in the vendored upstream tree.
        -Wno-error=implicit-function-declaration
        -Wno-error=incompatible-pointer-types
        -Wno-error=int-conversion
        -Wno-error=implicit-int
        -w
)
endif ()

target_compile_definitions(${PROJECT_NAME} PRIVATE
        PACKAGE_VERSION="3.2.0"

        PICO_FLASH_SIZE_BYTES=16777216

        # Core 0's stack is relocated to the top of main RAM by the scripts in
        # linker_overrides/ (32 KB there); this only sizes core 1's, which runs
        # the shallow render_core and its DMA handler.
        PICO_CORE1_STACK_SIZE=4096

        # ---- Fixed by the RP2350-PiZero PCB: do not change ----
        # On-board DVI connector, 8 pins from GPIO32: the three TMDS data pairs
        # are GPIO32-37 and the clock pair is GPIO38/39. HSTX is hard-wired to
        # GPIO12-19 on RP2350 and cannot reach this connector, so video is PIO
        # TMDS (drivers/hdmi).
        #
        # Note this is the OPPOSITE order from the driver's usual contiguous
        # layout (clk at base, data at base+2). pico-spec's hdmi.h special-cases
        # the Zero boards as data=base, clk=base+6, and this board follows that,
        # so the two bases are set explicitly here rather than derived from
        # HDMI_BASE_PIN.
        HDMI_PIN_DATA_BASE=32
        HDMI_PIN_CLK_BASE=38

        # This board has a real DVI connector, so the pairs are wired straight
        # through and the channel order is BGR. The Murmulator's resistor
        # network needed both of these inverted; pico-spec likewise uses 0/0
        # for its Zero boards and 1/1 for everything else.
        HDMI_PIN_invert_diffpairs=0
        HDMI_PIN_RGB_notBGR=0

        # On-board microSD slot, hardware SPI1 (CS is a plain GPIO, not SPI1 CSn).
        SDCARD_SPI_BUS=spi1
        SDCARD_PIN_SPI0_SCK=30
        SDCARD_PIN_SPI0_MOSI=31
        SDCARD_PIN_SPI0_MISO=40
        SDCARD_PIN_SPI0_CS=43

        # ---- Wired by you on the 40-pin header ----
        # Taken from the ZERO2 target of DnCraptor/pico-spec rather than guessed,
        # so they match what that firmware expects on this board. Still unverified
        # *here*: nobody has run this port's PS/2 or NES code on real wiring.
        KBD_CLOCK_PIN=2
        KBD_DATA_PIN=3

        NES_GPIO_CLK=4
        NES_GPIO_LAT=5
        NES_GPIO_DATA=7

        # util_Wii_Joy.c references these unconditionally even though nothing
        # calls Init_Wii_Joystick(); parked on free I2C1 pins, feature off
        # (USE_WII intentionally not defined).
        WII_SDA_PIN=18
        WII_SCL_PIN=19

        LOAD_WAV_PIO=17
        PWM_PIN0=10
        PWM_PIN1=11
        BEEPER_PIN=12

        # PSRAM pad is unpopulated on a stock board: read8psram/write8psram are
        # backed by an SRAM array (drivers/psram-sram). PSRAM stays defined so
        # memory.c / statesav.c keep their banking paths.
        PSRAM
        STEREO_SOUND=1
        CONSOLE_SOUND=1
        SERIO_SOUND=1
        VOICEBOX=1
        SOUND_THIN_API=1

        #MNGR_DEBUG=1
        #DEBUG2=1

        HAVE_OPENDIR=1
)


# Flash divider is left at the board header's default of 2 (126 MHz at 252 MHz
# sys clock). pico-spec runs this board the same way; the RP2040 Murmulator
# build needed PICO_FLASH_SPI_CLKDIV=4 but RP2350 does not.
target_compile_definitions(${PROJECT_NAME} PRIVATE FLASH_SIZE=${FLASH_SIZE})

target_compile_definitions(${PROJECT_NAME} PRIVATE SYS_CLOCK_KHZ=${SYS_CLOCK_KHZ})
IF (NOT SYS_CLOCK_KHZ EQUAL 252000)
    message(WARNING "SYS_CLOCK_KHZ=${SYS_CLOCK_KHZ} will skew DVI timing; 252000 is the only exact setting")
    SET(BUILD_NAME "${BUILD_NAME}-${SYS_CLOCK_KHZ}kHz")
ENDIF ()

# Host mode owns the native USB controller, so there is no CDC device left to
# print to. Rather than silently dropping the log - which cost a lot of time
# earlier in this port - refuse the combination outright.
IF (KBD_USB AND USB_LOG)
    message(FATAL_ERROR
            "KBD_USB and USB_LOG are mutually exclusive: USB host mode removes "
            "the CDC port that USB_LOG prints to. Use -DSD_LOG=ON instead.")
ENDIF ()

IF (KBD_USB)
    target_link_libraries(${PROJECT_NAME} PRIVATE usbkbd)
    target_compile_definitions(${PROJECT_NAME} PRIVATE KBD_USB)
    pico_enable_stdio_usb(${PROJECT_NAME} 0)
    SET(BUILD_NAME "${BUILD_NAME}-usbkbd")
ENDIF ()

IF (USB_LOG)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
            USB_LOG
            # Wait indefinitely for a terminal to open the CDC port, otherwise
            # every boot message is discarded before a human can connect.
            PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=-1
    )
    SET(BUILD_NAME "${BUILD_NAME}-usblog")
ENDIF ()
IF (SD_LOG)
    target_compile_definitions(${PROJECT_NAME} PRIVATE MNGR_DEBUG=1)
    SET(BUILD_NAME "${BUILD_NAME}-sdlog")
ENDIF ()

IF (TFT)
    target_link_libraries(${PROJECT_NAME} PRIVATE st7789)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TFT)
    SET(BUILD_NAME "${BUILD_NAME}-TFT")
    IF (ILI9341)
        SET(BUILD_NAME "${BUILD_NAME}-ILI9341")
        target_compile_definitions(${PROJECT_NAME} PRIVATE ILI9341)
    ELSE ()
        SET(BUILD_NAME "${BUILD_NAME}-ST7789")
    ENDIF ()
ELSEIF (VGA)
    # Not wired on this board: VGA needs 8 contiguous GPIOs and the PiZero
    # header has no suitable run. Kept only for reference.
    message(WARNING "VGA is not supported on RP2350-PiZero (no 8 contiguous header GPIOs)")
    target_compile_definitions(${PROJECT_NAME} PRIVATE VGA)
    target_link_libraries(${PROJECT_NAME} PRIVATE vga-nextgen)
    SET(BUILD_NAME "${BUILD_NAME}-VGA")
ELSEIF (TV)
    message(WARNING "TV out is not wired on RP2350-PiZero")
    target_compile_definitions(${PROJECT_NAME} PRIVATE TV)
    target_link_libraries(${PROJECT_NAME} PRIVATE tv)
    SET(BUILD_NAME "${BUILD_NAME}-TV")
ELSE ()
    # Default: the board's own DVI connector
    target_link_libraries(${PROJECT_NAME} PRIVATE hdmi)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HDMI)
    SET(BUILD_NAME "${BUILD_NAME}-DVI")
ENDIF ()

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${BUILD_NAME}")

pico_enable_stdio_uart(${PROJECT_NAME} 0)
pico_enable_stdio_usb(${PROJECT_NAME} 1)

#pico_set_binary_type(${PROJECT_NAME} no_flash)
#pico_set_binary_type(${PROJECT_NAME} copy_to_ram)

pico_add_extra_outputs(${PROJECT_NAME})
