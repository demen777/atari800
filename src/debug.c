#include "debug.h"
#include "config.h"
#include "ff.h"
#include <pico.h>
#include <hardware/pio.h>
#include <stdbool.h>
#include <string.h>

static FIL f;
void logMsg(char* msg) {
    LED_ACTIVITY(true);
    FRESULT fr = f_open(&f, "\\atari.log", FA_WRITE | FA_OPEN_APPEND | FA_OPEN_ALWAYS);
    if (fr != FR_OK) return;
    UINT bw;
    f_write(&f, msg, strlen(msg), &bw);
    f_write(&f, "\n", 1, &bw);
    f_close(&f);
    LED_ACTIVITY(false);
}
