#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "inttypes.h"
#include "stdbool.h"

#include "hardware/pio.h"

#define PIO_VIDEO pio0
#define PIO_VIDEO_ADDR pio0
#define VIDEO_DMA_IRQ (DMA_IRQ_0)

/* The TMDS data run (6 pins = 3 differential pairs) and the clock pair (2 pins)
 * are configured independently because their order is board-specific. The
 * Murmulator layout is clk at the base and data at base+2; the RP2350-PiZero
 * (like pico-spec's Zero boards) is the reverse, data at 32 and clk at 38.
 * HDMI_BASE_PIN alone keeps the Murmulator layout working. */
#ifndef HDMI_BASE_PIN
#define HDMI_BASE_PIN (6)
#endif

#ifndef HDMI_PIN_DATA_BASE
#define HDMI_PIN_DATA_BASE (HDMI_BASE_PIN + 2)
#endif
#ifndef HDMI_PIN_CLK_BASE
#define HDMI_PIN_CLK_BASE (HDMI_BASE_PIN)
#endif

/* A PIO instance can address either GPIO0-31 or GPIO16-47, never both. Pins
 * at or above 32 therefore force the upper window, which also means every
 * other PIO user on the same instance must live at GPIO16 or above. */
#if (HDMI_PIN_DATA_BASE + 6 > 32) || (HDMI_PIN_CLK_BASE + 2 > 32)
#define HDMI_PIO_GPIO_BASE (16)
#else
#define HDMI_PIO_GPIO_BASE (0)
#endif

/* Differential pair polarity and channel order are board wiring, not driver
 * policy. The Murmulator's resistor network wants both inverted; a board with
 * a real DVI connector such as the RP2350-PiZero wants neither. Getting
 * invert_diffpairs wrong produces a perfectly running PIO/DMA pipeline whose
 * TMDS no monitor will lock onto - no signal, no error, nothing to see.
 * (pico-spec sets both to 0 for its ZERO/ZERO2 boards and 1 elsewhere.) */
#ifndef HDMI_PIN_invert_diffpairs
#define HDMI_PIN_invert_diffpairs (1)
#endif
#ifndef HDMI_PIN_RGB_notBGR
#define HDMI_PIN_RGB_notBGR (1)
#endif
#define beginHDMI_PIN_data (HDMI_PIN_DATA_BASE)
#define beginHDMI_PIN_clk (HDMI_PIN_CLK_BASE)

#define TEXTMODE_COLS 53
#define TEXTMODE_ROWS 30

#define RGB888(r, g, b) ((r<<16) | (g << 8 ) | b )

// TODO: Сделать настраиваемо
static const uint8_t textmode_palette[16] = {
    200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215
};


static void graphics_set_flashmode(bool flash_line, bool flash_frame) {
    // dummy
}

/* Number of video DMA interrupts taken so far. Zero and staying zero means the
   PIO/DMA chain never started; climbing means it is running and any missing
   picture is further downstream. */
uint32_t hdmi_dbg_irq_count(void);

/* Prints DMA error/busy bits and PIO stall flags; call when the irq count has
   stopped moving. */
void hdmi_dbg_dump(void);


#ifdef __cplusplus
}
#endif
