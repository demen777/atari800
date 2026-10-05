#include "frame_diag.h"

#ifdef FRAME_DIAG

#include <string.h>
#include <pico/time.h>
#include "ff.h"
#include "atari.h"
#include "pokeysnd.h"
#include "cpu.h"
#include "pokey.h"
#include "antic.h"
#include "memory.h"

int snprintf(char *, unsigned int, const char *, ...);

#define WINDOW_FRAMES 600u   /* 10 s */
#define PC_HISTORY 12
#define WORST 5
#define PAUSE_US 500000u     /* longer than this is the UI or a load, not a slow frame */

volatile uint32_t frame_diag_sd_us, frame_diag_sd_calls, frame_diag_ps2_irqs;

typedef struct {
	uint32_t frame;
	uint32_t total;
	uint32_t outside;                  /* between the end of one frame's wait and the next frame's first stage */
	uint32_t stage[FRAME_DIAG_STAGES];
	uint32_t sd_us, sd_calls, ps2_irqs;
} sample_t;

static uint32_t mark[FRAME_DIAG_STAGES];
static uint32_t wait_end;       /* when the previous frame's throttle wait finished */
static uint32_t frame_no;
static int skip_next = 1;       /* the first frame has no meaningful "outside" */

static uint32_t win_start, win_frames, win_slow, win_pauses;
static uint64_t win_total, win_outside, win_stage[FRAME_DIAG_STAGES];
static uint32_t win_sd_us, win_sd_calls, win_ps2;
static sample_t worst[WORST];
static UWORD pc_history[PC_HISTORY];  /* where the 6502 was at the end of the last few frames */

void frame_diag_mark(int stage)
{
	mark[stage] = time_us_32();
}

static void write_window(void)
{
	static char buf[1536];
	static const char *const names[FRAME_DIAG_STAGES] = { "pre", "antic", "pokey", "sound" };
	const uint32_t n = win_frames ? win_frames : 1;
	const uint32_t period = (uint32_t)(1e6 / (Atari800_tv_mode == Atari800_TV_PAL ? Atari800_FPS_PAL : Atari800_FPS_NTSC));
	int len = snprintf(buf, sizeof buf,
		"frames %u..%u: %u measured, %u slower than %u us, %u pauses; avg us: total %u outside %u",
		(unsigned)win_start, (unsigned)frame_no, (unsigned)win_frames, (unsigned)win_slow, (unsigned)period,
		(unsigned)win_pauses, (unsigned)(win_total / n), (unsigned)(win_outside / n));
	for (int i = 0; i < FRAME_DIAG_STAGES; i++)
		len += snprintf(buf + len, sizeof buf - len, " %s %u", names[i], (unsigned)(win_stage[i] / n));
	len += snprintf(buf + len, sizeof buf - len, "; sd %u us in %u calls, ps2 irqs %u; pokey hifi=%d bienias=%d %d Hz\n",
		(unsigned)win_sd_us, (unsigned)win_sd_calls, (unsigned)win_ps2,
		(int)POKEYSND_enable_new_pokey, (int)POKEYSND_bienias_fix, (int)POKEYSND_playback_freq);
	for (int w = 0; w < WORST && worst[w].total; w++) {
		const sample_t *s = &worst[w];
		len += snprintf(buf + len, sizeof buf - len, "  worst #%u: total %u outside %u",
			(unsigned)s->frame, (unsigned)s->total, (unsigned)s->outside);
		for (int i = 0; i < FRAME_DIAG_STAGES; i++)
			len += snprintf(buf + len, sizeof buf - len, " %s %u", names[i], (unsigned)s->stage[i]);
		len += snprintf(buf + len, sizeof buf - len, "; sd %u us in %u calls, ps2 irqs %u\n",
			(unsigned)s->sd_us, (unsigned)s->sd_calls, (unsigned)s->ps2_irqs);
	}

	/* What the emulated machine is doing, for the case where the game itself
	   has stopped: the 6502's registers, the code under the program counter,
	   where it was at the end of the last frames (a tight loop repeats), and
	   the interrupt and sound state it could be waiting on or stuck with. */
	len += snprintf(buf + len, sizeof buf - len,
		"  6502: PC %04X A %02X X %02X Y %02X S %02X P %02X; code",
		CPU_regPC, CPU_regA, CPU_regX, CPU_regY, CPU_regS, CPU_regP);
	for (int i = 0; i < 8; i++)
		len += snprintf(buf + len, sizeof buf - len, " %02X", MEMORY_mem[(UWORD)(CPU_regPC + i)]);
	len += snprintf(buf + len, sizeof buf - len, "; frame-end PCs");
	for (int i = 0; i < PC_HISTORY; i++)
		len += snprintf(buf + len, sizeof buf - len, " %04X", pc_history[(frame_no + i) % PC_HISTORY]);
	len += snprintf(buf + len, sizeof buf - len,
		"\n  chips: IRQEN %02X IRQST %02X SKCTL %02X SKSTAT %02X NMIEN %02X DMACTL %02X AUDCTL %02X AUDF/C %02X/%02X %02X/%02X %02X/%02X %02X/%02X\n",
		POKEY_IRQEN, POKEY_IRQST, POKEY_SKCTL, POKEY_SKSTAT, ANTIC_NMIEN, ANTIC_DMACTL, POKEY_AUDCTL[0],
		POKEY_AUDF[0], POKEY_AUDC[0], POKEY_AUDF[1], POKEY_AUDC[1],
		POKEY_AUDF[2], POKEY_AUDC[2], POKEY_AUDF[3], POKEY_AUDC[3]);

	FIL f;
	if (f_open(&f, "\\frames.txt", FA_WRITE | FA_OPEN_APPEND | FA_OPEN_ALWAYS) == FR_OK) {
		UINT bw;
		f_write(&f, buf, len, &bw);
		f_close(&f);
	}

	win_start = frame_no;
	win_frames = win_slow = win_pauses = 0;
	win_total = win_outside = 0;
	memset(win_stage, 0, sizeof win_stage);
	win_sd_us = win_sd_calls = win_ps2 = 0;
	memset(worst, 0, sizeof worst);
	skip_next = 1;
}

void frame_diag_work_done(void)
{
	const uint32_t now = time_us_32();
	sample_t s;

	pc_history[frame_no % PC_HISTORY] = CPU_regPC;
	frame_no++;
	s.frame = frame_no;
	s.outside = mark[FRAME_DIAG_PRE] - wait_end;
	for (int i = 0; i < FRAME_DIAG_STAGES; i++)
		s.stage[i] = (i + 1 < FRAME_DIAG_STAGES ? mark[i + 1] : now) - mark[i];
	s.total = now - wait_end;
	s.sd_us = frame_diag_sd_us;
	s.sd_calls = frame_diag_sd_calls;
	s.ps2_irqs = frame_diag_ps2_irqs;
	frame_diag_sd_us = frame_diag_sd_calls = frame_diag_ps2_irqs = 0;

	if (skip_next) {
		skip_next = 0;
	} else if (s.total > PAUSE_US) {
		win_pauses++;
	} else {
		const uint32_t period = (uint32_t)(1e6 / (Atari800_tv_mode == Atari800_TV_PAL ? Atari800_FPS_PAL : Atari800_FPS_NTSC));
		win_frames++;
		if (s.total > period) win_slow++;
		win_total += s.total;
		win_outside += s.outside;
		for (int i = 0; i < FRAME_DIAG_STAGES; i++) win_stage[i] += s.stage[i];
		win_sd_us += s.sd_us;
		win_sd_calls += s.sd_calls;
		win_ps2 += s.ps2_irqs;
		/* keep the slowest few, longest first */
		for (int w = 0; w < WORST; w++) {
			if (s.total > worst[w].total) {
				memmove(&worst[w + 1], &worst[w], (WORST - 1 - w) * sizeof(sample_t));
				worst[w] = s;
				break;
			}
		}
	}

	if (frame_no - win_start >= WINDOW_FRAMES)
		write_window();
}

void frame_diag_wait_done(void)
{
	wait_end = time_us_32();
}

#endif /* FRAME_DIAG */
