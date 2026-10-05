#ifndef FRAME_DIAG_H_
#define FRAME_DIAG_H_

/* -DFRAME_DIAG=ON: where the time of an emulated frame goes.
   Atari800_Frame() marks the boundaries between its stages; every 10 seconds
   one block is appended to \frames.txt with the averages and the slowest
   frames of that window. Nothing else touches the card, and the frame that
   pays for the write is left out of the next window. */
#ifdef FRAME_DIAG

#include <stdint.h>

enum {
	FRAME_DIAG_PRE,    /* Devices_Frame, INPUT_Frame, GTIA_Frame */
	FRAME_DIAG_ANTIC,  /* ANTIC_Frame: the 6502 and the picture */
	FRAME_DIAG_POKEY,  /* POKEY_Frame */
	FRAME_DIAG_SOUND,  /* Sound_Update: synthesis and hand-off */
	FRAME_DIAG_STAGES
};

void frame_diag_mark(int stage);  /* the named stage starts now */
void frame_diag_work_done(void);  /* the frame's work is over; the throttle wait follows */
void frame_diag_wait_done(void);  /* the throttle wait is over */

/* Bumped from the SD driver and the PS/2 clock interrupt. */
extern volatile uint32_t frame_diag_sd_us, frame_diag_sd_calls, frame_diag_ps2_irqs;

#else

#define frame_diag_mark(stage) ((void)0)
#define frame_diag_work_done() ((void)0)
#define frame_diag_wait_done() ((void)0)

#endif

#endif /* FRAME_DIAG_H_ */
