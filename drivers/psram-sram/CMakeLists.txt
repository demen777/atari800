# SRAM-backed stand-in for drivers/psram.
#
# The Waveshare RP2350-PiZero ships with the PSRAM pad unpopulated, but RP2350B
# has 520K of SRAM where RP2040 had 264K. The emulator only ever puts ~34K in
# PSRAM (the RAM shadowed under the XL/XE OS, under cartridges and the ANTIC
# self-test bank), so that region fits in SRAM with room to spare and the
# read8psram/write8psram call sites in memory.c / statesav.c stay untouched.

add_library(psram-sram INTERFACE)

target_sources(psram-sram INTERFACE
	${CMAKE_CURRENT_LIST_DIR}/psram_sram.c
)

target_include_directories(psram-sram INTERFACE
  ${CMAKE_CURRENT_LIST_DIR}
)
