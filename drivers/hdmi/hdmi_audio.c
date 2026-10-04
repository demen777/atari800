/* HDMI audio for the PIO TMDS driver.
 *
 * Packets (audio samples, ACR, InfoFrames) and their TERC4 encoding come from
 * the vendored pico_hdmi packet layer. That library emits one 30-bit word per
 * pixel clock for the HSTX peripheral; here each word is re-serialised into
 * the two 30-bit PIO words hdmi.c shifts out, so an island is 72 words that
 * the scanline interrupt only has to copy.
 *
 * Three stages, each single-producer/single-consumer:
 *   emulator core:  hdmi_audio_write_u8()    -> sample ring
 *   video core:     hdmi_audio_task()        -> island queue  (thread context)
 *   video core:     hdmi_audio_line_island() <- island queue  (video interrupt)
 */
#include "graphics.h"

#if HDMI_AUDIO

#include <stdio.h>
#include <string.h>
#include "pico.h"
#include "pico_hdmi/hstx_packet.h"

#define PIXEL_CLOCK_HZ (25200000u)
#define LINE_PIXELS (800u)
#define SAMPLES_PER_PACKET (4u)

/* First line of each vertical region, counted from the top of the picture as
   the scanline interrupt does. */
#define LINE_FRONT_PORCH (480u)
#define LINE_VSYNC (490u)
#define LINE_BACK_PORCH (492u)

typedef struct {
    uint32_t w[HDMI_ISLAND_WORDS];
} island_t;

/* Everything that depends on the sample rate, swapped as one pointer so the
   interrupt never sees half of a rate change. */
typedef struct {
    uint32_t rate;
    uint32_t acc_inc;   /* rate * LINE_PIXELS: samples per line, scaled by the pixel clock */
    island_t acr[2];    /* [0] outside vsync, [1] inside */
    island_t audio_infoframe; /* sent inside vsync */
    island_t silence;
} audio_cfg_t;

static audio_cfg_t cfgs[2];
static audio_cfg_t *volatile cur_cfg = &cfgs[0];
static island_t island_null;
static island_t island_avi;

static volatile uint32_t pending_rate = 48000;
static volatile bool muted;

/* Sample ring: emulator core -> hdmi_audio_task(). */
#define RING_FRAMES (8192u)
static audio_sample_t ring[RING_FRAMES];
static volatile uint32_t ring_w, ring_r;
static volatile uint32_t overruns;

/* Island queue: hdmi_audio_task() -> video interrupt. 128 packets is 10 ms at
   48 kHz, which is how long the video core may stay away from the task. */
#define QUEUE_ISLANDS (128u)
static island_t queue[QUEUE_ISLANDS];
static volatile uint32_t queue_head, queue_tail;
static volatile uint32_t underruns;

/* Diagnostics, see hdmi_audio_stats(). */
static volatile uint32_t stat_written, stat_encoded, stat_sent, stat_primes, stat_peak, stat_raw_min = 255, stat_raw_max;

static uint32_t sample_acc;
static int iec_frame;
static bool primed;

/* Serialised form of 5 bits of one TMDS lane: bit j lands at 6*j plus the
   lane's position inside the 6-pin group, as a differential pair. */
static uint32_t ser5[3][32];

static void build_ser_table(void) {
    const int shift[3] = { HDMI_PIN_RGB_notBGR ? 0 : 4, 2, HDMI_PIN_RGB_notBGR ? 4 : 0 };
    for (int lane = 0; lane < 3; lane++) {
        for (int v = 0; v < 32; v++) {
            uint32_t w = 0;
            for (int j = 0; j < 5; j++) {
                uint32_t pair = ((v >> j) & 1) ? 0b01 : 0b10;
                if (HDMI_PIN_invert_diffpairs) pair ^= 0b11;
                w |= pair << (6 * j + shift[lane]);
            }
            ser5[lane][v] = w;
        }
    }
}

static void serialise(island_t *out, const hstx_data_island_t *in) {
    for (int i = 0; i < HDMI_ISLAND_SYMBOLS; i++) {
        const uint32_t w = in->words[i];
        const uint32_t l0 = w & 0x3ff, l1 = (w >> 10) & 0x3ff, l2 = (w >> 20) & 0x3ff;
        out->w[2 * i] = ser5[0][l0 & 31] | ser5[1][l1 & 31] | ser5[2][l2 & 31];
        out->w[2 * i + 1] = ser5[0][l0 >> 5] | ser5[1][l1 >> 5] | ser5[2][l2 >> 5];
    }
}

/* Islands always sit inside the hsync pulse, so hsync is asserted throughout. */
static void encode(island_t *out, const hstx_packet_t *packet, bool vsync) {
    hstx_data_island_t hstx;
    hstx_encode_data_island(&hstx, packet, vsync, true);
    serialise(out, &hstx);
}

static bool rate_supported(uint32_t hz) {
    return hz == 32000 || hz == 44100 || hz == 48000;
}

static void build_cfg(audio_cfg_t *cfg, uint32_t rate) {
    hstx_packet_t packet;

    cfg->rate = rate;
    cfg->acc_inc = rate * LINE_PIXELS;

    /* N from HDMI 1.3a table 7-1; CTS follows from the pixel clock and comes
       out exact for all three rates (25200, 28000, 25200). */
    const uint32_t n = rate == 32000 ? 4096 : rate == 44100 ? 6272 : 6144;
    const uint32_t cts = (uint32_t)(((uint64_t)PIXEL_CLOCK_HZ * n) / (128ull * rate));
    hstx_packet_set_acr(&packet, n, cts);
    encode(&cfg->acr[0], &packet, false);
    encode(&cfg->acr[1], &packet, true);

    hstx_packet_set_audio_infoframe(&packet, rate, 2, 16);
    encode(&cfg->audio_infoframe, &packet, true);

    /* Frame 4 rather than 0: frame 0 carries the IEC 60958 block-start flag,
       and repeating that on every underrun would keep resetting the sink's
       channel-status sync. */
    const audio_sample_t zero[SAMPLES_PER_PACKET] = { 0 };
    hstx_packet_set_audio_samples_cs_rate(&packet, zero, SAMPLES_PER_PACKET, 4, rate);
    encode(&cfg->silence, &packet, false);
}

void hdmi_audio_init(void) {
    hstx_packet_t packet;

    build_ser_table();

    hstx_packet_set_null(&packet);
    encode(&island_null, &packet, false);

    /* VIC 1 = 640x480p60, no pixel repetition. */
    hstx_packet_set_avi_infoframe(&packet, 1, 0);
    encode(&island_avi, &packet, false);

    uint32_t rate = pending_rate;
    if (!rate_supported(rate)) rate = 48000;
    build_cfg(&cfgs[0], rate);
    cur_cfg = &cfgs[0];

    /* The table above must agree with the serialiser the picture goes through,
       or islands come out on the wrong pins while video looks perfect. */
    hstx_data_island_t hstx;
    hstx_encode_data_island(&hstx, &packet, false, true);
    bool ok = true;
    for (int i = 0; i < HDMI_ISLAND_SYMBOLS; i++) {
        const uint32_t w = hstx.words[i];
        const uint64_t ref = hdmi_tmds_serialise((w >> 20) & 0x3ff, (w >> 10) & 0x3ff, w & 0x3ff);
        if (island_avi.w[2 * i] != (uint32_t)ref || island_avi.w[2 * i + 1] != (uint32_t)(ref >> 32)) ok = false;
    }
    printf("hdmi audio: %u Hz, island serialiser self-check %s\n", (unsigned)rate, ok ? "ok" : "FAILED");
}

bool hdmi_audio_set_sample_rate(uint32_t hz) {
    if (!rate_supported(hz)) {
        muted = true;
        return false;
    }
    pending_rate = hz;
    muted = false;
    return true;
}

uint32_t hdmi_audio_rate(void) { return muted ? 0 : cur_cfg->rate; }
uint32_t hdmi_audio_buffered(void) { return (ring_w + RING_FRAMES - ring_r) % RING_FRAMES; }
uint32_t hdmi_audio_underruns(void) { return underruns; }

/* One line describing every stage of the pipeline since the last call:
   what came in (count, raw range, peak after the DC blocker), what was
   packetised, and what the video interrupt actually sent. */
int hdmi_audio_stats(char *buf, unsigned size) {
    const int n = snprintf(buf, size,
        "rate=%u muted=%d in=%u raw=%u..%u peak=%u primes=%u encoded=%u sent=%u silence=%u dropped=%u buffered=%u queued=%u",
        (unsigned)cur_cfg->rate, (int)muted, (unsigned)stat_written, (unsigned)stat_raw_min, (unsigned)stat_raw_max,
        (unsigned)stat_peak, (unsigned)stat_primes, (unsigned)stat_encoded, (unsigned)stat_sent,
        (unsigned)underruns, (unsigned)overruns, (unsigned)hdmi_audio_buffered(),
        (unsigned)((queue_head + QUEUE_ISLANDS - queue_tail) % QUEUE_ISLANDS));
    stat_peak = 0; stat_raw_min = 255; stat_raw_max = 0;
    return n;
}
uint32_t hdmi_audio_overruns(void) { return overruns; }

void hdmi_audio_write_u8(const uint8_t *data, unsigned frames, unsigned channels) {
    /* POKEY output rests at 0, not at mid-scale, so a plain conversion would
       sit at negative full scale and every underrun (which inserts true zero)
       would be a full-scale click. Track the DC level and remove it; the
       corner is around 15 Hz. */
    static int32_t dc[2];

    if (muted || channels < 1 || channels > 2) return;

    uint32_t w = ring_w;
    while (frames--) {
        int32_t s[2];
        if (data[0] < stat_raw_min) stat_raw_min = data[0];
        if (data[0] > stat_raw_max) stat_raw_max = data[0];
        s[0] = ((int32_t)data[0] - 128) << 8;
        s[1] = channels == 2 ? ((int32_t)data[1] - 128) << 8 : s[0];
        data += channels;
        for (int c = 0; c < 2; c++) {
            dc[c] += (s[c] - dc[c]) >> 9;
            s[c] -= dc[c];
            if (s[c] > 32767) s[c] = 32767;
            if (s[c] < -32768) s[c] = -32768;
        }
        const uint32_t next = (w + 1) % RING_FRAMES;
        if (next == ring_r) {
            overruns += frames + 1;
            break;
        }
        if ((uint32_t)(s[0] < 0 ? -s[0] : s[0]) > stat_peak) stat_peak = s[0] < 0 ? -s[0] : s[0];
        stat_written++;
        ring[w].left = (int16_t)s[0];
        ring[w].right = (int16_t)s[1];
        w = next;
    }
    ring_w = w;
}

void hdmi_audio_task(void) {
    audio_cfg_t *cfg = cur_cfg;
    const uint32_t rate = pending_rate;
    if (rate != cfg->rate) {
        /* Build into the copy the interrupt is not reading, then switch. */
        audio_cfg_t *other = cfg == &cfgs[0] ? &cfgs[1] : &cfgs[0];
        build_cfg(other, rate);
        cur_cfg = cfg = other;
    }

    /* A few packets per call keeps the caller's other polling responsive. */
    for (int budget = 8; budget--;) {
        const uint32_t head = queue_head;
        const uint32_t next = (head + 1) % QUEUE_ISLANDS;
        if (next == queue_tail) break;

        uint32_t r = ring_r;
        const uint32_t avail = (ring_w + RING_FRAMES - r) % RING_FRAMES;
        if (!primed) {
            /* The emulator delivers one video frame's worth of samples at a
               time. Starting on the first burst would run the ring down to
               empty just as the next one is due, so any late frame underruns;
               25 ms waits for the second burst on both NTSC and PAL. */
            if (avail < cfg->rate / 40) break;
            primed = true;
            stat_primes++;
        }
        if (avail < SAMPLES_PER_PACKET) {
            primed = false;
            break;
        }
        if (avail > cfg->rate / 8) {
            /* Every slot that went out as silence is a slot the input never
               gets back, so a backlog only ever grows - into a delay of the
               whole ring. Past 125 ms, throw the oldest away instead. */
            const uint32_t skip = avail - cfg->rate / 40;
            r = (r + skip) % RING_FRAMES;
            overruns += skip;
        }

        audio_sample_t samples[SAMPLES_PER_PACKET];
        for (unsigned i = 0; i < SAMPLES_PER_PACKET; i++) {
            samples[i] = ring[r];
            r = (r + 1) % RING_FRAMES;
        }
        ring_r = r;

        hstx_packet_t packet;
        iec_frame = hstx_packet_set_audio_samples_cs_rate(&packet, samples, SAMPLES_PER_PACKET, iec_frame, cfg->rate);
        encode(&queue[head], &packet, false);
        queue_head = next;
        stat_encoded++;
    }
}

/* One call per scanline, from the video interrupt. `line` is the line the
   island will be sent on. The schedule is pico_hdmi's: clock regeneration and
   the audio InfoFrame inside vsync, the AVI InfoFrame on the first blanking
   line, ACR again every fourth back-porch line, audio everywhere else. */
const uint32_t *__not_in_flash_func(hdmi_audio_line_island)(unsigned line) {
    const audio_cfg_t *cfg = cur_cfg;

    /* Every line advances the audio clock, including the ones that carry
       something else: delivery then catches up at one packet per line. */
    sample_acc += cfg->acc_inc;

    if (line == LINE_VSYNC) return cfg->acr[1].w;
    if (line == LINE_VSYNC + 1) return cfg->audio_infoframe.w;
    if (line == LINE_FRONT_PORCH) return island_avi.w;
    if (line >= LINE_BACK_PORCH && ((line - LINE_BACK_PORCH) & 3) == 0) return cfg->acr[0].w;

    if (sample_acc >= SAMPLES_PER_PACKET * PIXEL_CLOCK_HZ) {
        sample_acc -= SAMPLES_PER_PACKET * PIXEL_CLOCK_HZ;
        const uint32_t tail = queue_tail;
        if (tail != queue_head) {
            queue_tail = (tail + 1) % QUEUE_ISLANDS;
            stat_sent++;
            return queue[tail].w;
        }
        underruns++;
        return cfg->silence.w;
    }
    return island_null.w;
}

#endif /* HDMI_AUDIO */
