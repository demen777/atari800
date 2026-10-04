/* TinyUSB configuration for the USB keyboard host.
 *
 * This directory is added to the include path ONLY when KBD_USB is on. The
 * project once carried a host tusb_config.h on the general include path, where
 * it shadowed pico_stdio_usb's own and silently produced a TinyUSB build with
 * no device CDC - no serial port, picotool blind without BOOT, and
 * stdio_init_all() able to block forever. Keep this file out of any
 * unconditional -I.
 *
 * CFG_TUSB_MCU is supplied by the SDK's tinyusb integration - do not define it.
 */
#pragma once

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS                 OPT_OS_PICO
#endif

#define CFG_TUSB_DEBUG              0

/* Host on the native controller. The board advertises host capability on its
   PIO-USB port, but pico-spec drives this board through the native one and that
   is what was verified here: tuh_init returns 1 and a HID keyboard enumerates
   at 252 MHz. Using PIO-USB instead would need the Pico-PIO-USB library. */
#define CFG_TUH_ENABLED             1
#define CFG_TUD_ENABLED             0

#define CFG_TUH_RPI_PIO_USB         0
#define BOARD_TUH_RHPORT            0

#define CFG_TUH_ENUMERATION_BUFSIZE 256

/* Hubs allowed so a keyboard with an internal hub still works. */
#define CFG_TUH_HUB                 1
#define CFG_TUH_HID                 4
#define CFG_TUH_CDC                 0
#define CFG_TUH_MSC                 0
#define CFG_TUH_VENDOR              0

#define CFG_TUH_DEVICE_MAX          (CFG_TUH_HUB ? 4 : 1)

#define CFG_TUH_HID_EPIN_BUFSIZE    64
#define CFG_TUH_HID_EPOUT_BUFSIZE   64

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))
