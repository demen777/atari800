/* SRAM-backed stand-in for the PIO PSRAM driver's public API.
 *
 * Deliberately keeps the original header name so that #include "psram_spi.h"
 * in src/memory.c and src/main.cpp needs no change. Only the symbols the
 * emulator actually uses are provided; the PIO-level psram_spi_* API of the
 * real driver is gone, because there is no SPI device behind this.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Size of the emulated PSRAM window. Must cover every offset memory.c derives
 * from under_atarixl_os_base (16K) + under_cart809F (8K) + under_cartA0BF (8K)
 * + antic_bank_under_selftest (2K) = 34K. Rounded up to 40K for headroom. */
#define PSRAM_SRAM_SIZE (40u * 1024u)

void init_psram(void);
void psram_cleanup(void);
void write8psram(uint32_t addr32, uint8_t v);
void write16psram(uint32_t addr32, uint16_t v);
uint8_t read8psram(uint32_t addr32);
uint16_t read16psram(uint32_t addr32);

#ifdef __cplusplus
}
#endif
