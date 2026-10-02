#ifndef CONFIG_H
#define CONFIG_H

//#define BIT3
#define HAVE_INTTYPES_H
#define SOUND
#define SOUND_THIN_API
#define SUPPORTS_SOUND_REINIT
#define SUPPORTS_PLATFORM_CONFIGINIT
#define SUPPORTS_PLATFORM_CONFIGURE
#define SUPPORTS_PLATFORM_TIME
//#define SUPPORTS_CHANGE_VIDEOMODE 1
#define LIBATARI800
#define VOL_ONLY_SOUND 1
#define PAGED_ATTRIB
#define EMUOS_ALTIRRA 1
#define SUPPORTS_PLATFORM_SLEEP 1
#define DIR_SEP_BACKSLASH 1

#include "debug.h"
#include <pico.h>
#include <hardware/gpio.h>

/* Disk/state activity blink. The RP2040 Murmulator build got a plain LED on
 * GPIO25 from boards/pico.h; the RP2350-PiZero board header defines no
 * PICO_DEFAULT_LED_PIN (its only LED is addressable), so this is a no-op
 * there rather than driving an arbitrary pin. */
#ifdef PICO_DEFAULT_LED_PIN
#define LED_ACTIVITY(on) gpio_put(PICO_DEFAULT_LED_PIN, (on))
#else
#define LED_ACTIVITY(on) ((void)0)
#endif

#endif
