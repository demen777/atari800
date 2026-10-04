/* USB HID keyboard, translated into the XT set-1 scancodes the emulator's
 * handleScancode() already understands.
 *
 * The point of translating rather than feeding HID straight in is that
 * handleScancode() in src/main.cpp is not a keymap - it also implements the
 * joystick emulation on QWE/ASD/ZXC and the numeric keypad, the Alt+letter UI
 * shortcuts, Ctrl/Shift+Fn save states and Ctrl+Alt+Del. Reproducing that for
 * a second input source would duplicate a lot of behaviour and drift from it.
 *
 * Runs on the native USB controller as host, which means there is no USB CDC
 * device on that port any more: KBD_USB and USB_LOG are mutually exclusive and
 * CMake refuses to build both.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up the USB host stack. Call once, after the system clock is set. */
void usbkbd_init(void);

/* Services the stack and emits scancodes for whatever changed. Call regularly;
   once per emulated frame is enough for a keyboard. */
void usbkbd_task(void);

/* True once a HID keyboard has been enumerated. Diagnostics only. */
bool usbkbd_connected(void);

#ifdef __cplusplus
}
#endif
