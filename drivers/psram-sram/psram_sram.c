#include "psram_spi.h"

#include <string.h>

/* The backing store. Out-of-range accesses are clamped rather than left to
 * corrupt neighbouring statics: memory.c derives offsets from bank sizes that
 * change with MEMORY_ram_size, so a bad offset is a plausible regression and
 * silently scribbling over SRAM would be far harder to diagnose than a
 * visibly-wrong byte. */
static uint8_t psram_store[PSRAM_SRAM_SIZE];

void init_psram(void) {
    memset(psram_store, 0, sizeof psram_store);
}

void psram_cleanup(void) {
    /* nothing to release */
}

void write8psram(uint32_t addr32, uint8_t v) {
    if (addr32 >= PSRAM_SRAM_SIZE) return;
    psram_store[addr32] = v;
}

uint8_t read8psram(uint32_t addr32) {
    if (addr32 >= PSRAM_SRAM_SIZE) return 0xff;
    return psram_store[addr32];
}

void write16psram(uint32_t addr32, uint16_t v) {
    if (addr32 + 1 >= PSRAM_SRAM_SIZE) return;
    psram_store[addr32] = (uint8_t)v;
    psram_store[addr32 + 1] = (uint8_t)(v >> 8);
}

uint16_t read16psram(uint32_t addr32) {
    if (addr32 + 1 >= PSRAM_SRAM_SIZE) return 0xffff;
    return (uint16_t)psram_store[addr32] | ((uint16_t)psram_store[addr32 + 1] << 8);
}





