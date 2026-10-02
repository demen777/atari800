#pragma once

#include "_ansi.h"
int	snprintf (char *__restrict, unsigned int size, const char *__restrict, ...) _ATTRIBUTE ((__format__ (__printf__, 3, 4)));

#ifdef MNGR_DEBUG
extern void logMsg(char* msg);
#define printf(...) { char tmp[256]; snprintf(tmp, 256, __VA_ARGS__); logMsg(tmp); }
#define Log_print(...) { char tmp[256]; snprintf(tmp, 256, __VA_ARGS__); logMsg(tmp); }
#define DBGM_PRINT( X) printf X
#elif defined(USB_LOG)
/* printf deliberately left undefined so the real one is used. Declared here
   rather than via <stdio.h> because ff.h shadows fopen/fread. */
int printf(const char *__restrict, ...) _ATTRIBUTE ((__format__ (__printf__, 1, 2)));
#define Log_print(...) (printf(__VA_ARGS__), printf("\n"))
#define DBGM_PRINT( X) printf X
#else
#define DBGM_PRINT( X)
#define printf(...)
#define Log_print(...)
#endif

