#include "graphics.h"
#include <stdio.h>
#include <string.h>
#include "malloc.h"
#include <stdalign.h>
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/time.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"

//PIO параметры
static uint offs_prg0 = 0;
static uint offs_prg1 = 0;

//SM
static int SM_video = -1;
static int SM_conv = -1;

//активный видеорежим
static enum graphics_mode_t graphics_mode = GRAPHICSMODE_DEFAULT;

//буфер  палитры 256 цветов в формате R8G8B8
static uint32_t palette[256];
static uint32_t bgcolor;


#define SCREEN_WIDTH (320)
#define SCREEN_HEIGHT (240)
//графический буфер
static uint8_t* __not_in_flash("hdmi_data") graphics_buffer = NULL;
static int graphics_buffer_width = 0;
static int graphics_buffer_height = 0;
static int graphics_buffer_shift_x = 0;
static int graphics_buffer_shift_y = 0;

//текстовый буфер
uint8_t* text_buffer = NULL;


//DMA каналы
//каналы работы с первичным графическим буфером
static int dma_chan_ctrl;
static int dma_chan;
//каналы работы с конвертацией палитры
static int dma_chan_pal_conv_ctrl;
static int dma_chan_pal_conv;

/* A scanline is 400 entries of two pixel clocks each: 48 hsync, 24 back
   porch, 320 picture, 8 front porch. Each entry is a 9-bit index into
   conv_color, which holds the two serialised TMDS symbols it stands for. */
#define LINE_UNITS (400)
#define H_SYNC_UNITS (48)
#define H_ACTIVE_START (72)
#define H_ACTIVE_END (H_ACTIVE_START + SCREEN_WIDTH)

/* Indices 0-255 are the picture's palette, all of them. Everything the link
   itself needs lives above, which is what the ninth index bit is for: with an
   8-bit index the sync codes had to be carved out of the palette. */
#define CONV_ENTRIES (512)
#define INX_SYNC (256)           /* +1 hsync asserted, +2 vsync asserted */
#define INX_BG (260)             /* border colour */
#define INX_DI_PREAMBLE (261)    /* +1 inside vsync; hsync asserted */
#define INX_VIDEO_PREAMBLE (263)
#define INX_VIDEO_GUARD (264)
/* Two sets of entries that are rewritten every scanline with that line's data
   island, 18 entries for 36 symbols. A set belongs to one line buffer, so the
   set being rewritten is never the one being transmitted. */
#define INX_DI_SLOTS (272)
#define DI_SLOT_ENTRIES (18)
#define DI_PREAMBLE_UNITS (4)

//DMA буферы
//основные строчные данные
static uint16_t __not_in_flash("hdmi_data") dma_lines[2][LINE_UNITS];
static uint16_t* __not_in_flash("hdmi_data") DMA_BUF_ADDR[2];

//ДМА палитра для конвертации
static alignas(CONV_ENTRIES * 16)
uint32_t conv_color[CONV_ENTRIES * 4];


//индекс, проверяющий зависание
static uint32_t irq_inx = 0;

//функции и константы HDMI

//программа конвертации адреса
/* Turns a 9-bit index into the address of its 16-byte conv_color entry: x holds
   the table's address >> 13, so isr ends up as x:index:0000. */

uint16_t pio_program_instructions_conv_HDMI[] = {
    //         //     .wrap_target
    0x80a0, //  0: pull   block
    0x40e9, //  1: in     osr, 9
    0x4033, //  2: in     x, 19
    0x8020, //  3: push   block
    //     .wrap
};


const struct pio_program pio_program_conv_addr_HDMI = {
    .instructions = pio_program_instructions_conv_HDMI,
    .length = 4,
    .origin = -1,
};

//программа видеовывода
static const uint16_t instructions_PIO_HDMI[] = {
    0x7006, //  0: out    pins, 6         side 2
    0x7006, //  1: out    pins, 6         side 2
    0x7006, //  2: out    pins, 6         side 2
    0x7006, //  3: out    pins, 6         side 2
    0x7006, //  4: out    pins, 6         side 2
    0x6806, //  5: out    pins, 6         side 1
    0x6806, //  6: out    pins, 6         side 1
    0x6806, //  7: out    pins, 6         side 1
    0x6806, //  8: out    pins, 6         side 1
    0x6806, //  9: out    pins, 6         side 1
};

static const struct pio_program program_PIO_HDMI = {
    .instructions = instructions_PIO_HDMI,
    .length = 10,
    .origin = -1,
};

static uint64_t get_ser_diff_data(const uint16_t dataR, const uint16_t dataG, const uint16_t dataB) {
    uint64_t out64 = 0;
    for (int i = 0; i < 10; i++) {
        out64 <<= 6;
        if (i == 5) out64 <<= 2;
        uint8_t bR = (dataR >> (9 - i)) & 1;
        uint8_t bG = (dataG >> (9 - i)) & 1;
        uint8_t bB = (dataB >> (9 - i)) & 1;

        bR |= (bR ^ 1) << 1;
        bG |= (bG ^ 1) << 1;
        bB |= (bB ^ 1) << 1;

        if (HDMI_PIN_invert_diffpairs) {
            bR ^= 0b11;
            bG ^= 0b11;
            bB ^= 0b11;
        }
        uint8_t d6;
        if (HDMI_PIN_RGB_notBGR) {
            d6 = (bR << 4) | (bG << 2) | (bB << 0);
        }
        else {
            d6 = (bB << 4) | (bG << 2) | (bR << 0);
        }


        out64 |= d6;
    }
    return out64;
}

//конвертор TMDS
static uint tmds_encoder(const uint8_t d8) {
    int s1 = 0;
    for (int i = 0; i < 8; i++) s1 += (d8 & (1 << i)) ? 1 : 0;
    bool is_xnor = false;
    if ((s1 > 4) || ((s1 == 4) && ((d8 & 1) == 0))) is_xnor = true;
    uint16_t d_out = d8 & 1;
    uint16_t qi = d_out;
    for (int i = 1; i < 8; i++) {
        d_out |= ((qi << 1) ^ (d8 & (1 << i))) ^ (is_xnor << i);
        qi = d_out & (1 << i);
    }

    if (is_xnor) d_out |= 1 << 9;
    else d_out |= 1 << 8;

    return d_out;
}

#if HDMI_AUDIO
uint64_t hdmi_tmds_serialise(uint16_t ch2, uint16_t ch1, uint16_t ch0) {
    return get_ser_diff_data(ch2, ch1, ch0);
}
#endif

static inline __attribute__((always_inline)) void fill16(uint16_t* p, const uint16_t v, int n) {
    while (n-- > 0) *p++ = v;
}

uint32_t hdmi_dbg_irq_count(void) { return irq_inx; }

/* A frozen irq count means the chain stopped rather than never started, and a
   DMA channel that hit a bus error halts exactly like that. Dump the error and
   busy bits for all four channels plus the PIO's stall flags. */
void hdmi_dbg_dump(void) {
    const int ch[4] = { dma_chan_ctrl, dma_chan, dma_chan_pal_conv_ctrl, dma_chan_pal_conv };
    const char *nm[4] = { "ctrl", "data", "palctl", "palcnv" };
    printf("hdmi dbg: irq=%u ints0=%08x pio.ctrl=%08x pio.fdebug=%08x pio.flevel=%08x\n",
           (unsigned)irq_inx, (unsigned)dma_hw->ints0,
           (unsigned)PIO_VIDEO->ctrl, (unsigned)PIO_VIDEO->fdebug,
           (unsigned)PIO_VIDEO->flevel);
#if HDMI_AUDIO
    printf("hdmi audio: underruns=%u overruns=%u\n",
           (unsigned)hdmi_audio_underruns(), (unsigned)hdmi_audio_overruns());
#endif
    for (int i = 0; i < 4; i++) {
        const int c = ch[i];
        if (c < 0) continue;
        const uint32_t ctrl = dma_hw->ch[c].ctrl_trig;
        printf("  ch%-2d %-6s ctrl=%08x busy=%u ahb_err=%u rd_err=%u wr_err=%u"
               " cnt=%u rd=%08x wr=%08x\n",
               c, nm[i], (unsigned)ctrl,
               (unsigned)((ctrl >> 24) & 1u),   /* BUSY */
               (unsigned)((ctrl >> 31) & 1u),   /* AHB_ERROR */
               (unsigned)((ctrl >> 30) & 1u),   /* READ_ERROR */
               (unsigned)((ctrl >> 29) & 1u),   /* WRITE_ERROR */
               (unsigned)dma_hw->ch[c].transfer_count,
               (unsigned)dma_hw->ch[c].read_addr,
               (unsigned)dma_hw->ch[c].write_addr);
    }
}

static void pio_set_x(PIO pio, const int sm, uint32_t v) {
    uint instr_shift = pio_encode_in(pio_x, 4);
    uint instr_mov = pio_encode_mov(pio_x, pio_isr);
    for (int i = 0; i < 8; i++) {
        const uint32_t nibble = (v >> (i * 4)) & 0xf;
        pio_sm_exec(pio, sm, pio_encode_set(pio_x, nibble));
        pio_sm_exec(pio, sm, instr_shift);
    }
    pio_sm_exec(pio, sm, instr_mov);
}


/* Nothing in here may run from flash. The optimize attribute stops GCC turning
   the copy loops back into calls to memcpy, which lives there. */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void __not_in_flash_func(dma_handler_HDMI)() {
    static uint32_t inx_buf_dma;
    /* Starts on the last line so the first one built is row 0's first
       scanline: an odd line copies its picture from the previous buffer. */
    static uint line = 524;
    irq_inx++;

    dma_hw->ints0 = 1u << dma_chan_ctrl;

    /* The line that has just started is going out of one buffer; build the
       next line in the other and queue it. Every line gets its own buffer
       contents, because every line carries a different data island. */
    inx_buf_dma ^= 1;
    dma_channel_set_read_addr(dma_chan_ctrl, &DMA_BUF_ADDR[inx_buf_dma], false);

    line = line >= 524 ? 0 : line + 1;

    uint16_t* activ_buf = dma_lines[inx_buf_dma];
    const bool vsync = (line >= 490) && (line < 492);
    //ССИ
    // --|_|---|_|---|_|----
    //---|___________|-----
    const uint16_t sync_idle = INX_SYNC + (vsync ? 2 : 0);
    const uint16_t sync_h = sync_idle + 1;

#if HDMI_AUDIO
    /* Preamble, then the island, then the rest of the hsync pulse. The island
       entries themselves never change - what they point at does. */
    const uint16_t slots = INX_DI_SLOTS + inx_buf_dma * DI_SLOT_ENTRIES;
    const uint32_t* island = hdmi_audio_line_island(line);
    uint32_t* slot_words = &conv_color[slots * 4];
    for (int i = 0; i < HDMI_ISLAND_WORDS; i++) slot_words[i] = island[i];
    fill16(activ_buf, INX_DI_PREAMBLE + (vsync ? 1 : 0), DI_PREAMBLE_UNITS);
    for (int i = 0; i < DI_SLOT_ENTRIES; i++) activ_buf[DI_PREAMBLE_UNITS + i] = slots + i;
    fill16(activ_buf + DI_PREAMBLE_UNITS + DI_SLOT_ENTRIES, sync_h,
           H_SYNC_UNITS - DI_PREAMBLE_UNITS - DI_SLOT_ENTRIES);
#else
    fill16(activ_buf, sync_h, H_SYNC_UNITS);
#endif

    if (!graphics_buffer || line >= 480) {
        //ССИ без изображения, кадровый синхроимпульс
        fill16(activ_buf + H_SYNC_UNITS, sync_idle, LINE_UNITS - H_SYNC_UNITS);
        return;
    }

    //область изображения
#if HDMI_AUDIO
    /* HDMI wants active video announced: 8 pixels of preamble and a 2-pixel
       guard band at the end of the back porch. */
    fill16(activ_buf + H_SYNC_UNITS, INX_SYNC, H_ACTIVE_START - H_SYNC_UNITS - 5);
    fill16(activ_buf + H_ACTIVE_START - 5, INX_VIDEO_PREAMBLE, 4);
    activ_buf[H_ACTIVE_START - 1] = INX_VIDEO_GUARD;
#else
    fill16(activ_buf + H_SYNC_UNITS, INX_SYNC, H_ACTIVE_START - H_SYNC_UNITS);
#endif
    fill16(activ_buf + H_ACTIVE_END, INX_SYNC, LINE_UNITS - H_ACTIVE_END);

    uint16_t* output_buffer = activ_buf + H_ACTIVE_START;
    if (line & 1) {
        /* Second scanline of a doubled row: the other buffer holds the first. */
        const uint16_t* first = dma_lines[inx_buf_dma ^ 1] + H_ACTIVE_START;
        for (int i = 0; i < SCREEN_WIDTH; i++) output_buffer[i] = first[i];
        return;
    }

    const int y = line / 2;
    switch (graphics_mode) {
        case TEXTMODE_DEFAULT:
        case TEXTMODE_53x30: {
            *output_buffer++ = INX_BG;

            for (int x = 0; x < TEXTMODE_COLS; x++) {
                const uint16_t offset = (y / 8) * (TEXTMODE_COLS * 2) + x * 2;
                const uint8_t c = text_buffer[offset];
                const uint8_t colorIndex = text_buffer[offset + 1];
                uint8_t glyph_row = font_6x8[c * 8 + y % 8];

                for (int bit = 6; bit--;) {
                    *output_buffer++ = glyph_row & 1
                                           ? textmode_palette[colorIndex & 0xf] //цвет шрифта
                                           : textmode_palette[colorIndex >> 4]; //цвет фона

                    glyph_row >>= 1;
                }
            }
            *output_buffer = INX_BG;
            break;
        }
        default: {
            //пространство слева от буфера, сам видеобуфер, пространство справа
            const int row = y - graphics_buffer_shift_y;
            int left = graphics_buffer_shift_x;
            int skip = 0;
            if (left < 0) {
                skip = -left;
                left = 0;
            }
            int n = graphics_buffer_width - skip;
            if (n > SCREEN_WIDTH - left) n = SCREEN_WIDTH - left;

            if ((row < 0) || (row >= graphics_buffer_height) || (n <= 0)) {
                fill16(output_buffer, INX_BG, SCREEN_WIDTH);
                break;
            }

            fill16(output_buffer, INX_BG, left);
            output_buffer += left;
            const uint8_t* input_buffer = &graphics_buffer[row * graphics_buffer_width + skip];
            for (int i = 0; i < n; i++) output_buffer[i] = input_buffer[i];
            fill16(output_buffer + n, INX_BG, SCREEN_WIDTH - left - n);
            break;
        }
    }
}


static inline void irq_remove_handler_DMA_core1() {
    irq_set_enabled(VIDEO_DMA_IRQ, false);
    irq_remove_handler(VIDEO_DMA_IRQ, irq_get_exclusive_handler(VIDEO_DMA_IRQ));
}

static inline void irq_set_exclusive_handler_DMA_core1() {
    irq_set_exclusive_handler(VIDEO_DMA_IRQ, dma_handler_HDMI);
    irq_set_priority(VIDEO_DMA_IRQ, 0);
    irq_set_enabled(VIDEO_DMA_IRQ, true);
}

//деинициализация - инициализация ресурсов
static inline bool hdmi_init() {
    //выключение прерывания DMA
    if (VIDEO_DMA_IRQ == DMA_IRQ_0) {
        dma_channel_set_irq0_enabled(dma_chan_ctrl, false);
    }
    else {
        dma_channel_set_irq1_enabled(dma_chan_ctrl, false);
    }

    irq_remove_handler_DMA_core1();


    //остановка всех каналов DMA
    dma_hw->abort = (1 << dma_chan_ctrl) | (1 << dma_chan) | (1 << dma_chan_pal_conv) | (
                        1 << dma_chan_pal_conv_ctrl);
    while (dma_hw->abort) tight_loop_contents();

    //выключение SM основной и конвертора

    //pio_sm_restart(PIO_VIDEO, SM_video);
    pio_sm_set_enabled(PIO_VIDEO, SM_video, false);

    //pio_sm_restart(PIO_VIDEO_ADDR, SM_conv);
    pio_sm_set_enabled(PIO_VIDEO_ADDR, SM_conv, false);


    //удаление программ из соответствующих PIO
    pio_remove_program(PIO_VIDEO_ADDR, &pio_program_conv_addr_HDMI, offs_prg1);
    pio_remove_program(PIO_VIDEO, &program_PIO_HDMI, offs_prg0);


    /* Must happen before the state machines are configured: it selects which
     * 32-GPIO window this PIO can see. */
    pio_set_gpio_base(PIO_VIDEO, HDMI_PIO_GPIO_BASE);
    if (PIO_VIDEO_ADDR != PIO_VIDEO) pio_set_gpio_base(PIO_VIDEO_ADDR, HDMI_PIO_GPIO_BASE);

    offs_prg1 = pio_add_program(PIO_VIDEO_ADDR, &pio_program_conv_addr_HDMI);
    offs_prg0 = pio_add_program(PIO_VIDEO, &program_PIO_HDMI);
    pio_set_x(PIO_VIDEO_ADDR, SM_conv, ((uint32_t)conv_color >> 13));

    //заполнение палитры
    for (int ci = 0; ci < 256; ci++) graphics_set_palette(ci, palette[ci]); //
    graphics_set_bgcolor(bgcolor);

    //служебные данные(синхра) напрямую вносим в массив -конвертер
    uint64_t* conv_color64 = (uint64_t *)conv_color;
    const uint16_t b0 = 0b1101010100;
    const uint16_t b1 = 0b0010101011;
    const uint16_t b2 = 0b0101010100;
    const uint16_t b3 = 0b1010101011;
    const int base_inx = INX_SYNC;

    conv_color64[2 * base_inx + 0] = get_ser_diff_data(b0, b0, b3);
    conv_color64[2 * base_inx + 1] = get_ser_diff_data(b0, b0, b3);

    conv_color64[2 * (base_inx + 1) + 0] = get_ser_diff_data(b0, b0, b2);
    conv_color64[2 * (base_inx + 1) + 1] = get_ser_diff_data(b0, b0, b2);

    conv_color64[2 * (base_inx + 2) + 0] = get_ser_diff_data(b0, b0, b1);
    conv_color64[2 * (base_inx + 2) + 1] = get_ser_diff_data(b0, b0, b1);

    conv_color64[2 * (base_inx + 3) + 0] = get_ser_diff_data(b0, b0, b0);
    conv_color64[2 * (base_inx + 3) + 1] = get_ser_diff_data(b0, b0, b0);

#if HDMI_AUDIO
    /* HDMI 1.3a table 5-2: CTL0-3 on channels 1 and 2 say what follows the
       control period - 1000 a video period, 1010 a data island. Channel 0
       keeps carrying sync. The guard band symbols are table 5-5. */
    conv_color64[2 * INX_DI_PREAMBLE + 0] = get_ser_diff_data(b1, b1, b2);
    conv_color64[2 * INX_DI_PREAMBLE + 1] = get_ser_diff_data(b1, b1, b2);

    conv_color64[2 * (INX_DI_PREAMBLE + 1) + 0] = get_ser_diff_data(b1, b1, b0);
    conv_color64[2 * (INX_DI_PREAMBLE + 1) + 1] = get_ser_diff_data(b1, b1, b0);

    conv_color64[2 * INX_VIDEO_PREAMBLE + 0] = get_ser_diff_data(b0, b1, b3);
    conv_color64[2 * INX_VIDEO_PREAMBLE + 1] = get_ser_diff_data(b0, b1, b3);

    conv_color64[2 * INX_VIDEO_GUARD + 0] = get_ser_diff_data(0b1011001100, 0b0100110011, 0b1011001100);
    conv_color64[2 * INX_VIDEO_GUARD + 1] = get_ser_diff_data(0b1011001100, 0b0100110011, 0b1011001100);

    hdmi_audio_init();
#endif

    //настройка PIO SM для конвертации

    pio_sm_config c_c = pio_get_default_sm_config();
    sm_config_set_wrap(&c_c, offs_prg1, offs_prg1 + (pio_program_conv_addr_HDMI.length - 1));
    sm_config_set_in_shift(&c_c, true, false, 32);

    int rc_conv = pio_sm_init(PIO_VIDEO_ADDR, SM_conv, offs_prg1, &c_c);
    pio_sm_set_enabled(PIO_VIDEO_ADDR, SM_conv, true);

    //настройка PIO SM для вывода данных
    c_c = pio_get_default_sm_config();
    sm_config_set_wrap(&c_c, offs_prg0, offs_prg0 + (program_PIO_HDMI.length - 1));

    //настройка side set
    sm_config_set_sideset_pins(&c_c,beginHDMI_PIN_clk);
    sm_config_set_sideset(&c_c, 2,false,false);
    for (int i = 0; i < 2; i++) {
        pio_gpio_init(PIO_VIDEO, beginHDMI_PIN_clk + i);
        gpio_set_drive_strength(beginHDMI_PIN_clk + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(beginHDMI_PIN_clk + i, GPIO_SLEW_RATE_FAST);
    }

    /* 64-bit variants take absolute GPIO numbers; the 32-bit ones are relative
     * to the PIO's GPIO base and would overflow for a clock pair at GPIO38. */
    pio_sm_set_pins_with_mask64(PIO_VIDEO, SM_video, 3ull << beginHDMI_PIN_clk, 3ull << beginHDMI_PIN_clk);
    pio_sm_set_pindirs_with_mask64(PIO_VIDEO, SM_video, 3ull << beginHDMI_PIN_clk, 3ull << beginHDMI_PIN_clk);
    //пины

    for (int i = 0; i < 6; i++) {
        gpio_set_slew_rate(beginHDMI_PIN_data + i, GPIO_SLEW_RATE_FAST);
        pio_gpio_init(PIO_VIDEO, beginHDMI_PIN_data + i);
        gpio_set_drive_strength(beginHDMI_PIN_data + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(beginHDMI_PIN_data + i, GPIO_SLEW_RATE_FAST);
    }
    pio_sm_set_consecutive_pindirs(PIO_VIDEO, SM_video, beginHDMI_PIN_data, 6, true);
    //конфигурация пинов на выход
    sm_config_set_out_pins(&c_c, beginHDMI_PIN_data, 6);

    //
    sm_config_set_out_shift(&c_c, true, true, 30);
    sm_config_set_fifo_join(&c_c, PIO_FIFO_JOIN_TX);

    sm_config_set_clkdiv(&c_c, clock_get_hz(clk_sys) / 252000000.0f);
    int rc_video = pio_sm_init(PIO_VIDEO, SM_video, offs_prg0, &c_c);
    pio_sm_set_enabled(PIO_VIDEO, SM_video, true);
    /* Decisive bring-up facts: a negative rc means the pin config was rejected,
       and pinctrl shows which pins the SM was actually given. */
    printf("hdmi: rc_conv=%d rc_video=%d gpio_base=%u sm_video.pinctrl=%08x clkdiv=%08x\n",
           rc_conv, rc_video,
           (unsigned)pio_get_gpio_base(PIO_VIDEO),
           (unsigned)PIO_VIDEO->sm[SM_video].pinctrl,
           (unsigned)PIO_VIDEO->sm[SM_video].clkdiv);
    printf("hdmi: clk=GP%d data=GP%d conv_color=%p (x=%08x)\n",
           beginHDMI_PIN_clk, beginHDMI_PIN_data,
           (void *)conv_color, (unsigned)((uint32_t)conv_color >> 13));

    //настройки DMA
    /* Both buffers start as a plain blank line, so nothing references an
       island slot before the interrupt has filled one. */
    for (int i = 0; i < 2; i++) {
        fill16(dma_lines[i], INX_SYNC + 1, H_SYNC_UNITS);
        fill16(dma_lines[i] + H_SYNC_UNITS, INX_SYNC, LINE_UNITS - H_SYNC_UNITS);
    }

    //основной рабочий канал
    /* 16-bit transfers: the bus replicates the half-word across the FIFO word
       and the converter takes its low 9 bits. */
    dma_channel_config cfg_dma = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_16);
    channel_config_set_chain_to(&cfg_dma, dma_chan_ctrl); // chain to other channel

    channel_config_set_read_increment(&cfg_dma, true);
    channel_config_set_write_increment(&cfg_dma, false);


    uint dreq = DREQ_PIO1_TX0 + SM_conv;
    if (PIO_VIDEO_ADDR == pio0) dreq = DREQ_PIO0_TX0 + SM_conv;

    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan,
        &cfg_dma,
        &PIO_VIDEO_ADDR->txf[SM_conv], // Write address
        &dma_lines[0][0], // read address
        LINE_UNITS, //
        false // Don't start yet
    );

    //контрольный канал для основного
    cfg_dma = dma_channel_get_default_config(dma_chan_ctrl);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_32);
    channel_config_set_chain_to(&cfg_dma, dma_chan); // chain to other channel

    channel_config_set_read_increment(&cfg_dma, false);
    channel_config_set_write_increment(&cfg_dma, false);

    DMA_BUF_ADDR[0] = &dma_lines[0][0];
    DMA_BUF_ADDR[1] = &dma_lines[1][0];

    dma_channel_configure(
        dma_chan_ctrl,
        &cfg_dma,
        &dma_hw->ch[dma_chan].read_addr, // Write address
        &DMA_BUF_ADDR[0], // read address
        1, //
        false // Don't start yet
    );

    //канал - конвертер палитры

    cfg_dma = dma_channel_get_default_config(dma_chan_pal_conv);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_32);
    channel_config_set_chain_to(&cfg_dma, dma_chan_pal_conv_ctrl); // chain to other channel

    channel_config_set_read_increment(&cfg_dma, true);
    channel_config_set_write_increment(&cfg_dma, false);

    dreq = DREQ_PIO1_TX0 + SM_video;
    if (PIO_VIDEO == pio0) dreq = DREQ_PIO0_TX0 + SM_video;

    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan_pal_conv,
        &cfg_dma,
        &PIO_VIDEO->txf[SM_video], // Write address
        &conv_color[0], // read address
        4, //
        false // Don't start yet
    );

    //канал управления конвертером палитры

    cfg_dma = dma_channel_get_default_config(dma_chan_pal_conv_ctrl);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_32);
    channel_config_set_chain_to(&cfg_dma, dma_chan_pal_conv); // chain to other channel

    channel_config_set_read_increment(&cfg_dma, false);
    channel_config_set_write_increment(&cfg_dma, false);

    dreq = DREQ_PIO1_RX0 + SM_conv;
    if (PIO_VIDEO_ADDR == pio0) dreq = DREQ_PIO0_RX0 + SM_conv;

    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan_pal_conv_ctrl,
        &cfg_dma,
        &dma_hw->ch[dma_chan_pal_conv].read_addr, // Write address
        &PIO_VIDEO_ADDR->rxf[SM_conv], // read address
        1, //
        true // start yet
    );

    //стартуем прерывание и канал
    if (VIDEO_DMA_IRQ == DMA_IRQ_0) {
        dma_channel_acknowledge_irq0(dma_chan_ctrl);
        dma_channel_set_irq0_enabled(dma_chan_ctrl, true);
    }
    else {
        dma_channel_acknowledge_irq1(dma_chan_ctrl);
        dma_channel_set_irq1_enabled(dma_chan_ctrl, true);
    }

    irq_set_exclusive_handler_DMA_core1();

    dma_start_channel_mask((1u << dma_chan_ctrl));

    return true;
};
//выбор видеорежима
void graphics_set_mode(enum graphics_mode_t mode) {
    graphics_mode = mode;
    clrScr(0);
};

static void set_conv_color(const int inx, const uint32_t color888) {
    uint64_t* conv_color64 = (uint64_t *)conv_color;
    const uint8_t R = (color888 >> 16) & 0xff;
    const uint8_t G = (color888 >> 8) & 0xff;
    const uint8_t B = (color888 >> 0) & 0xff;
    conv_color64[inx * 2] = get_ser_diff_data(tmds_encoder(R), tmds_encoder(G), tmds_encoder(B));
    conv_color64[inx * 2 + 1] = conv_color64[inx * 2] ^ 0x0003ffffffffffffl;
}

void graphics_set_palette(uint8_t i, uint32_t color888) {
    palette[i] = color888 & 0x00ffffff;
    set_conv_color(i, color888);
};

void graphics_set_buffer(uint8_t* buffer, uint16_t width, uint16_t height) {
    graphics_buffer = buffer;
    graphics_buffer_width = width;
    graphics_buffer_height = height;
};


//выделение и настройка общих ресурсов - 4 DMA канала, PIO программ и 2 SM
void graphics_init() {
    //настройка PIO
    SM_video = pio_claim_unused_sm(PIO_VIDEO, true);
    SM_conv = pio_claim_unused_sm(PIO_VIDEO_ADDR, true);
    //выделение и преднастройка DMA каналов
    dma_chan_ctrl = dma_claim_unused_channel(true);
    dma_chan = dma_claim_unused_channel(true);
    dma_chan_pal_conv_ctrl = dma_claim_unused_channel(true);
    dma_chan_pal_conv = dma_claim_unused_channel(true);


    // FIXME сделать конфигурацию пользователем
    graphics_set_palette(200, RGB888(0x00, 0x00, 0x00)); //black
    graphics_set_palette(201, RGB888(0x00, 0x00, 0xC4)); //blue
    graphics_set_palette(202, RGB888(0x00, 0xC4, 0x00)); //green
    graphics_set_palette(203, RGB888(0x00, 0xC4, 0xC4)); //cyan
    graphics_set_palette(204, RGB888(0xC4, 0x00, 0x00)); //red
    graphics_set_palette(205, RGB888(0xC4, 0x00, 0xC4)); //magenta
    graphics_set_palette(206, RGB888(0xC4, 0x7E, 0x00)); //brown
    graphics_set_palette(207, RGB888(0xC4, 0xC4, 0xC4)); //light gray
    graphics_set_palette(208, RGB888(0x4E, 0x4E, 0x4E)); //dark gray
    graphics_set_palette(209, RGB888(0x4E, 0x4E, 0xDC)); //light blue
    graphics_set_palette(210, RGB888(0x4E, 0xDC, 0x4E)); //light green
    graphics_set_palette(211, RGB888(0x4E, 0xF3, 0xF3)); //light cyan
    graphics_set_palette(212, RGB888(0xDC, 0x4E, 0x4E)); //light red
    graphics_set_palette(213, RGB888(0xF3, 0x4E, 0xF3)); //light magenta
    graphics_set_palette(214, RGB888(0xF3, 0xF3, 0x4E)); //yellow
    graphics_set_palette(215, RGB888(0xFF, 0xFF, 0xFF)); //white

    hdmi_init();
}

void graphics_set_bgcolor(uint32_t color888) //цвет фона - отдельная запись вне палитры
{
    bgcolor = color888 & 0x00ffffff;
    set_conv_color(INX_BG, color888);
};

void graphics_set_offset(int x, int y) {
    graphics_buffer_shift_x = x;
    graphics_buffer_shift_y = y;
};

void graphics_set_textbuffer(uint8_t* buffer) {
    text_buffer = buffer;
};


void clrScr(const uint8_t color) {
    if (text_buffer)
        memset(text_buffer, color, TEXTMODE_COLS * TEXTMODE_ROWS * 2);
}
