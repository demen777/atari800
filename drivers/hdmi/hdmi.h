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

/* HDMI audio. 1 turns the link from DVI into HDMI: every scanline carries a
 * data island in its hsync pulse (audio samples, clock regeneration,
 * InfoFrames) and active video gets its preamble and guard band. 0 is the
 * plain DVI signal this driver always produced. Packets are built by the
 * vendored pico_hdmi packet layer (drivers/pico_hdmi); only its HSTX output
 * stage is unusable here, since HSTX cannot reach GPIO32-39. */
#ifndef HDMI_AUDIO
#define HDMI_AUDIO (1)
#endif

#if HDMI_AUDIO
/* 32000, 44100 and 48000 are what HDMI can carry. Any other rate returns false
   and mutes the stream rather than playing it at the wrong pitch. Callable from
   either core; takes effect on the next hdmi_audio_task(). */
bool hdmi_audio_set_sample_rate(uint32_t hz);

/* Queue unsigned 8-bit samples, interleaved when channels == 2. Meant for the
   core that runs the emulator; excess is dropped when the buffer is full. */
void hdmi_audio_write_u8(const uint8_t *data, unsigned frames, unsigned channels);

/* Turns queued samples into data islands. Must be polled from the core that
   owns the video interrupt (the one that called graphics_init), in thread
   context: the interrupt consumes what this produces without any locking. */
void hdmi_audio_task(void);

/* Audio slots that found the queue empty and sent silence instead, and samples
   dropped because the buffer was full. Either one climbing is audible. */
uint32_t hdmi_audio_underruns(void);
/* The rate being sent, 0 while muted, and the samples waiting to be sent. */
uint32_t hdmi_audio_rate(void);
uint32_t hdmi_audio_buffered(void);
int hdmi_audio_stats(char *buf, unsigned size);
uint32_t hdmi_audio_overruns(void);

/* Driver internals shared between hdmi.c and hdmi_audio.c. */
#define HDMI_ISLAND_SYMBOLS (36) /* 2 guard + 32 packet + 2 guard */
#define HDMI_ISLAND_WORDS (HDMI_ISLAND_SYMBOLS * 2)
void hdmi_audio_init(void);
const uint32_t *hdmi_audio_line_island(unsigned line);
uint64_t hdmi_tmds_serialise(uint16_t ch2, uint16_t ch1, uint16_t ch0);
#endif

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
