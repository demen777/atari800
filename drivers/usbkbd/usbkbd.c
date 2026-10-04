#include "usbkbd.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tusb.h"

/* Defined in src/main.cpp. Takes an XT set-1 scancode: the make code, the make
   code | 0x80 for a break, or 0xE000 | code for the two extended keys it
   recognises (right Ctrl and right Alt). */
extern bool handleScancode(uint32_t ps2scancode);

#define XT_EXT 0xE000u

/* HID usage (keyboard page 0x07) -> XT set-1 make code. Unlisted usages are 0,
 * meaning "no mapping".
 *
 * Written with explicit indices rather than as a positional list: one stray
 * entry in a 100-element list silently shifts everything after it, and the
 * mistake is invisible on review. C does not allow _Static_assert on array
 * contents, so the indices themselves are the check.
 *
 * Arrow keys deliberately map to the *keypad* codes rather than the extended
 * 0xE048/0xE04B/0xE04D/0xE050 ones: handleScancode() decodes 0xE0 only for
 * right Ctrl and right Alt, and already treats 0x48/0x4B/0x4D/0x50 as both the
 * arrows and joystick 1. Sending the extended forms would make the arrows dead
 * keys. The keypad digits map to the same codes for the same reason.
 */
static const uint8_t hid_to_xt[0x68] = {
    /* letters */
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20,   /* a b c d */
    [0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23,   /* e f g h */
    [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,   /* i j k l */
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19,   /* m n o p */
    [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14,   /* q r s t */
    [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,   /* u v w x */
    [0x1C] = 0x15, [0x1D] = 0x2C,                                  /* y z     */

    /* digit row */
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05,   /* 1 2 3 4 */
    [0x22] = 0x06, [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09,   /* 5 6 7 8 */
    [0x26] = 0x0A, [0x27] = 0x0B,                                  /* 9 0     */

    /* control and punctuation */
    [0x28] = 0x1C,   /* Enter        */
    [0x29] = 0x01,   /* Escape       */
    [0x2A] = 0x0E,   /* Backspace    */
    [0x2B] = 0x0F,   /* Tab          */
    [0x2C] = 0x39,   /* Space        */
    [0x2D] = 0x0C,   /* -            */
    [0x2E] = 0x0D,   /* =            */
    [0x2F] = 0x1A,   /* [            */
    [0x30] = 0x1B,   /* ]            */
    [0x31] = 0x2B,   /* backslash    */
    [0x32] = 0x2B,   /* non-US #     */
    [0x33] = 0x27,   /* ;            */
    [0x34] = 0x28,   /* '            */
    [0x35] = 0x29,   /* `            */
    [0x36] = 0x33,   /* ,            */
    [0x37] = 0x34,   /* .            */
    [0x38] = 0x35,   /* /            */
    [0x39] = 0x3A,   /* CapsLock     */

    /* function keys */
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E,   /* F1..F4   */
    [0x3E] = 0x3F, [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42,   /* F5..F8   */
    [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,   /* F9..F12  */

    /* navigation, folded onto the keypad codes (see the note above) */
    [0x47] = 0x46,   /* ScrollLock   */
    [0x48] = 0x45,   /* Pause        */
    [0x49] = 0x52,   /* Insert  -> keypad 0 */
    [0x4A] = 0x47,   /* Home    -> keypad 7 */
    [0x4B] = 0x49,   /* PageUp  -> keypad 9 */
    [0x4C] = 0x53,   /* Delete  -> keypad . , which is what Ctrl+Alt+Del wants */
    [0x4D] = 0x4F,   /* End     -> keypad 1 */
    [0x4E] = 0x51,   /* PageDn  -> keypad 3 */
    [0x4F] = 0x4D,   /* Right   -> keypad 6 */
    [0x50] = 0x4B,   /* Left    -> keypad 4 */
    [0x51] = 0x50,   /* Down    -> keypad 2 */
    [0x52] = 0x48,   /* Up      -> keypad 8 */
    [0x53] = 0x45,   /* NumLock      */

    /* keypad proper */
    [0x54] = 0x35,   /* keypad /     */
    [0x55] = 0x37,   /* keypad *     */
    [0x56] = 0x4A,   /* keypad -     */
    [0x57] = 0x4E,   /* keypad +     */
    [0x58] = 0x1C,   /* keypad Enter */
    [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51,                   /* 1 2 3   */
    [0x5C] = 0x4B, [0x5D] = 0x4C, [0x5E] = 0x4D,                   /* 4 5 6   */
    [0x5F] = 0x47, [0x60] = 0x48, [0x61] = 0x49,                   /* 7 8 9   */
    [0x62] = 0x52,   /* keypad 0     */
    [0x63] = 0x53,   /* keypad .     */

    [0x64] = 0x2B,   /* non-US backslash */

    /* 0x46 PrintScreen, 0x65 Application, 0x66 Power and 0x67 keypad = have no
       non-extended equivalent the emulator reacts to, so they stay 0. */
};

/* HID modifier bit -> XT code. Right Ctrl and right Alt are the only extended
   codes handleScancode() decodes; the Gui keys mean nothing to the emulator. */
static const uint16_t mod_to_xt[8] = {
    0x1D,            /* bit0 left Ctrl   */
    0x2A,            /* bit1 left Shift  */
    0x38,            /* bit2 left Alt    */
    0,               /* bit3 left Gui    */
    XT_EXT | 0x1D,   /* bit4 right Ctrl  */
    0x36,            /* bit5 right Shift */
    XT_EXT | 0x38,   /* bit6 right Alt   */
    0,               /* bit7 right Gui   */
};

static uint8_t prev_mod;
static uint8_t prev_keys[6];
static bool    connected;

bool usbkbd_connected(void) { return connected; }

static void emit(uint16_t xt, bool release)
{
    if (!xt) return;
    uint8_t low = (uint8_t) ((xt & 0xFF) | (release ? 0x80u : 0u));
    handleScancode((xt & XT_EXT) ? (XT_EXT | low) : low);
}

static uint16_t xt_of(uint8_t hid)
{
    /* Usages 1..3 are the error/rollover codes, not keys. */
    return (hid > 3 && hid < sizeof hid_to_xt) ? hid_to_xt[hid] : 0;
}

static bool in_set(const uint8_t *set, uint8_t code)
{
    for (int i = 0; i < 6; i++)
        if (set[i] == code) return true;
    return false;
}

static void process_report(const uint8_t *rep, uint16_t len)
{
    if (len < 8) return;                 /* boot protocol: mod, reserved, 6 keys */

    const uint8_t mod = rep[0];
    const uint8_t *keys = rep + 2;

    /* Modifiers first, so a Shift pressed in the same report as a letter is
       already in effect when the letter's make code arrives - the emulator
       decides upper or lower case at make time. */
    const uint8_t changed = (uint8_t) (mod ^ prev_mod);
    for (int b = 0; b < 8; b++) {
        if (!(changed & (1u << b))) continue;
        emit(mod_to_xt[b], (mod & (1u << b)) == 0);
    }

    /* Releases before presses, so a key that rolls position within one report
       does not momentarily look released-and-pressed. */
    for (int i = 0; i < 6; i++)
        if (!in_set(keys, prev_keys[i])) emit(xt_of(prev_keys[i]), true);
    for (int i = 0; i < 6; i++)
        if (!in_set(prev_keys, keys[i])) emit(xt_of(keys[i]), false);

    prev_mod = mod;
    memcpy(prev_keys, keys, 6);
}

static void release_everything(void)
{
    for (int i = 0; i < 6; i++) emit(xt_of(prev_keys[i]), true);
    for (int b = 0; b < 8; b++)
        if (prev_mod & (1u << b)) emit(mod_to_xt[b], true);
    prev_mod = 0;
    memset(prev_keys, 0, sizeof prev_keys);
}

/* ---- TinyUSB host callbacks --------------------------------------------- */

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx,
                      uint8_t const *desc_report, uint16_t desc_len)
{
    (void) desc_report;
    (void) desc_len;

    if (tuh_hid_interface_protocol(dev_addr, idx) == HID_ITF_PROTOCOL_KEYBOARD) {
        connected = true;
        prev_mod = 0;
        memset(prev_keys, 0, sizeof prev_keys);
    }
    /* Request the first report on every interface. A keyboard commonly exposes
       a second one for media keys; draining it keeps the endpoint quiet. */
    tuh_hid_receive_report(dev_addr, idx);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx)
{
    if (tuh_hid_interface_protocol(dev_addr, idx) == HID_ITF_PROTOCOL_KEYBOARD) {
        connected = false;
        /* Otherwise the emulator keeps seeing whatever was held when it went. */
        release_everything();
    }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx,
                                uint8_t const *report, uint16_t len)
{
    if (tuh_hid_interface_protocol(dev_addr, idx) == HID_ITF_PROTOCOL_KEYBOARD)
        process_report(report, len);
    /* Other interfaces are drained and ignored. */

    tuh_hid_receive_report(dev_addr, idx);
}

/* ---- public API --------------------------------------------------------- */

void usbkbd_init(void)
{
    tuh_init(BOARD_TUH_RHPORT);
}

void usbkbd_task(void)
{
    tuh_task();
}
