# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A port of the **Atari800** 8-bit emulator (upstream v5.2.0) to the **Waveshare RP2350-PiZero**.
This is bare-metal firmware built with the Raspberry Pi Pico SDK — not a desktop application. The
upstream emulator core in [src/](src/) has been modified in place to run on a Cortex-M33 with 520 KB of
SRAM, the board's microSD slot, and its on-board DVI connector.

It began as a port to the RP2040 / ZX Murmulator board and was retargeted. History in `git log` from
before the retarget is Murmulator history, and the VGA/TFT/TV drivers under [drivers/](drivers/) are
leftovers from it that are **not wired for this board** — see *Board facts* below.

Board wiki: https://www.waveshare.com/wiki/RP2350-PiZero

## Build

Toolchain lives under `~/.pico-sdk` (installed by the VS Code *Raspberry Pi Pico* extension): SDK 2.3.1,
arm-none-eabi 15_2_Rel1, CMake 3.28.6, Ninja 1.12.1, picotool 2.3.1. SDK 2.3.1 is the floor — it is the
first release carrying `waveshare_rp2350_pizero.h`.

The `pico-vscode.cmake` block at the top of [CMakeLists.txt](CMakeLists.txt) supplies `PICO_SDK_PATH`,
`PICO_TOOLCHAIN_PATH` and `picotool_DIR`, and it **overrides the environment** — `PICO_SDK_PATH` exported
in a shell is ignored, and no toolchain file needs passing on the command line. `PICO_PLATFORM` and
`PICO_BOARD` are set before `pico_sdk_import.cmake` because the SDK pre-load reads them.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build          # or: ninja -C build   ("Compile Project" task)
```

The configure line needs no toolchain file, no board flag and no env vars — just the build type.

Display backend is mutually exclusive and **DVI is the default when none is set**; output lands in
`bin/<CMAKE_BUILD_TYPE>/pico-atari-16384K-DVI.uf2`. `-DTFT=ON` (plus `-DILI9341=ON`, else ST7789) still
builds, but `-DVGA=ON` and `-DTV=ON` now emit a CMake warning: both need 8 contiguous GPIOs that this
board's header does not offer. `FLASH_SIZE` only feeds the output name and a `FLASH_SIZE` define now; the
linker region comes from the board header (16 MB).

Flashing: VS Code tasks *Run Project* (`picotool load -fx`) or *Flash* (OpenOCD + CMSIS-DAP). On the device,
holding the NES-pad Start button or pressing F12 at boot enters `reset_usb_boot` for drag-and-drop UF2.
`picotool info -a <uf2>` is the fast check that an image can boot at all: it must report family
`rp2350-arm-s` and a metadata block of type `image def`, which the RP2350 bootrom requires.

**The RP2040 build's `slower_boot2` is gone on purpose.** That build forced `PICO_FLASH_SPI_CLKDIV=4`;
this one leaves the board header's default of 2 (126 MHz QSPI at a 252 MHz system clock), because
`pico-spec` drives this board the same way. Do not reintroduce the override without a measured reason.

**Never put `src/` or `drivers/usb` on the include path ahead of the SDK.** Both carried a USB *host*
`tusb_config.h` (`CFG_TUH_ENABLED 1`, no `CFG_TUD_*` at all) inherited from the unused `drivers/usb`
pico-pio-usb code. Because `-I` beats `-isystem`, it shadowed `pico_stdio_usb`'s own `tusb_config.h` and
TinyUSB got built with no device CDC. Symptoms: no serial port ever enumerates, `picotool` cannot see the
board unless BOOT is held, and — with a non-zero connect-wait — `stdio_init_all()` blocks forever waiting
for a CDC connection that can never happen, so the board looks completely dead with no video and no log.
`src/tusb_config.h` has been deleted and the `drivers/usb` include dropped; nothing compiled calls
`tusb_*`. If USB diagnostics ever go silent again, check this first: `_Static_assert(CFG_TUD_CDC == 1)`
in any TU settles it in one build.

Six build knobs: `-DUSB_LOG=ON` turns `printf`/`Log_print` into real output on the USB CDC port,
`-DSD_LOG=ON` appends to `tari.log` on the card (`MNGR_DEBUG`), and `-DSYS_CLOCK_KHZ=` overrides the
clock for testing — note that anything other than 252000 skews DVI timing. Both logs only start once
execution reaches them, so neither helps with a hang before `stdio_init_all()`. The fourth is `-DKBD_USB=ON`
for the USB keyboard, which is incompatible with `USB_LOG` (see below). The fifth is `-DHDMI_AUDIO=OFF`,
which drops HDMI audio and puts a plain DVI signal on the connector (see *HDMI audio*); it is ON by
default and the OFF image gets a `-noaudio` suffix. The sixth is `-DPWM_AUDIO=ON`, which adds the PWM
sound output back to a build that has HDMI audio.

**There are no tests.** `configure.ac`, `Makefile.am`, `autogen.sh`, `.travis*`, `atari800.spec`, `debian/`,
`src/libatari800/libatari800_test.c` and `tools/` are upstream autotools leftovers — not wired into the
CMake build and not maintained here. CMake globs `src/*.c` and `src/*.cpp` recursively, so adding a source
file needs no CMakeLists edit (but does need a re-configure).

**RAM used to be the binding constraint and no longer is.** On RP2040 it was a 256 KB budget run at ~87%
full, and commit messages in `git log` are literally recorded RAM percentages
(e.g. `RAM: 228640 B / 256 KB 87.22%`). RP2350B has 512 KB and the current build sits at **379 892 B
(72.46%)**, flash at 3.58% of 16 MB (290 812 B with `-DHDMI_AUDIO=OFF`; the difference is the audio
queues). The link still prints `--print-memory-usage`, but there is now
headroom to move banked memory *into* SRAM rather than out of it.

The retarget dropped the hand-written linker script in favour of the SDK default. The custom one pulled
libc/libgcc/libm `.text` into RAM on a cache-less RP2040 *and* moved core 0's stack out of SCRATCH_Y —
and that second job was load-bearing. In the SDK's default script
(`__StackTop = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y)`) core 0's
stack is the top of the 4 KB SCRATCH_Y bank, growing down. The HDMI driver used to keep `DMA_BUF_ADDR`,
`dma_lines` and its IRQ handler in the same bank via `__scratch_y`, so once the emulation loop pushed the
stack past the 2 KB default it overwrote `DMA_BUF_ADDR` with stack garbage; the video DMA then took a bus
error (`AHB_ERROR`/`READ_ERROR`) on a nonsense read address and halted permanently. Symptom: picture for
about two seconds after boot, then nothing, with the emulator still running happily at 60 fps.

Moving the driver's data out of the bank was not enough: a measured high-water mark showed core 0 using
the full 4 KB, i.e. still overflowing, just into core 1's stack in SCRATCH_X instead. **Core 0's stack is
therefore relocated to the top 32 KB of main RAM** (`0x20078000`–`0x20080000`) by three small overrides in
[linker_overrides/](linker_overrides/), wired up with `pico_add_linker_script_override_path`: 
`sections_stack.incl` places it, `section_heap.incl` stops the heap below it, and `section_end.incl`
recomputes the stack symbols. `CORE0_STACK_SIZE` is defined once in `sections_stack.incl` and referenced by
the other two, so it cannot drift; the SDK's own `ASSERT(__StackLimit >=
__HeapLimit)` catches a bad value. It is **64 KB**, not the 32 KB first tried:
the UI call chain is full of `FILENAME_MAX` (1024-byte) stack buffers —
`current_dir`, `highlighted_file`, `temp`, `filename`, `fullfilename`, `szbuf`,
plus whatever the caller in [ui.c](src/ui.c) holds — so opening the file
selector adds roughly 8–10 KB on top of the ~11.5 KB the emulator loop uses.
Overflowing runs straight into the heap, which is where `Util_malloc` is
building the very filename list being scanned — so it corrupts the data it is
walking rather than faulting, and presents as a hang inside the file selector
with no other symptom. Confirmed on hardware: at 32 KB the selector hung, at
64 KB it works. Core 1 keeps SCRATCH_X (4 KB, `PICO_CORE1_STACK_SIZE`),
SCRATCH_Y is entirely unused, and the heap still has ~180 KB.

**Measure before shrinking this.** A release build prints nothing, but
`-DSD_LOG=ON` (or `-DUSB_LOG=ON` on a non-`KBD_USB` build) reports
`stack: NNNNN of 65536 bytes used` every five seconds, from a pattern painted
over the stack at boot. The emulator loop alone sits around 11.5 KB; the UI is
what needs the rest.

The HDMI driver's data and handler also moved from `__scratch_y` to `__not_in_flash`/`__not_in_flash_func`,
which keeps them in RAM — the real point of `__scratch_y` — without sharing a bank with any stack. Do not
put data back in either scratch bank. Note
[st7789.c](drivers/st7789/st7789.c) and [tv.c](drivers/tv/tv.c) still use `__scratch_y`/`__scratch_x`; they
are not linked in a DVI build, and because the banks are now exactly full a `-DTFT=ON` build will fail at
link time rather than corrupt itself, which is the preferable failure. The diagnostic build also paints the
stack at boot and prints a high-water figure, so the real requirement is measurable rather than guessed.

## Architecture

### Two cores, two loops

- **Core 0** — [src/main.cpp](src/main.cpp) `main()`: raises VREG to 1.20 V via `vreg_set_voltage()`,
  overclocks to **252 MHz**, mounts FatFS, `init_psram()`, calls `libatari800_init()`, then spins forever
  on `libatari800_next_frame(&input_map)`. 252 MHz is not arbitrary: [hdmi.c](drivers/hdmi/hdmi.c) derives
  its TMDS divider as `clock_get_hz(clk_sys) / 252000000.0f`, so that clock gives divider 1.0 and an exact
  25.2 MHz pixel clock for 640x480p60. Changing the system clock changes the video timing.
- **Core 1** — `render_core()`: owns the display driver (`graphics_init()`), refreshes the LCD on a 60 Hz
  tick (TFT only; VGA/HDMI/TV scan out via PIO+DMA autonomously), and polls the NES pads every 5th frame.
  Started via `multicore_launch_core1` and gated by `vga_start_semaphore`; it calls
  `multicore_lockout_victim_init()` so flash writes can stall it.
- **Sound** — HDMI audio in the default build (see *HDMI audio*). The older PWM output is a
  `repeating_timer` at the POKEY sample rate (`snd_timer_callback`) that walks the buffer
  `PLATFORM_SoundWrite` captured and pushes samples to the PWM pins — tens of thousands of interrupts a
  second on the emulator's core. It is therefore compiled in only with `-DPWM_AUDIO=ON` (image suffix
  `-pwm`) or automatically in any build that has no HDMI audio (`-DHDMI_AUDIO=OFF`, TFT, VGA, TV).

**High Fidelity POKEY is off by default, for speed.** `ENABLE_NEW_POKEY=1` selects MZ POKEY
([mzpokeysnd.c](src/mzpokeysnd.c)), which resamples in doubles and scales with how busy the sound
channels are: measured on hardware, International Karate's music took emulation down to 55%, and with it
off the same scene holds 90-100%. Slow emulation also means silence, because the emulator then produces
fewer samples than HDMI consumes. Both the compiled-in default ([pokeysnd.c](src/pokeysnd.c)) and
[data/atari800.cfg](data/atari800.cfg) are 0 now; it remains selectable under F1 → Sound Settings. A card
seeded earlier still carries its own `ENABLE_NEW_POKEY` line, and that wins.

**Digitised sound needs `POKEYSND_SAMPBUF_MAX` to stay large.** The standard engine queues every
volume-only write of a frame in a ring of that many entries ([pokeysnd.h](src/pokeysnd.h)); upstream has
2000, the RP2040 port had cut it to 16, which keeps the last 16 writes of each frame and drops the rest —
speech and samples as noise. It is 1024 now (16 KB across four arrays, two of them only used with stereo
POKEY).

**Moving the emulator's inner loop into RAM was tried and did not help.** `cpu.c`, `antic.c`, `gtia.c`,
`pokey.c`, `pokeysnd.c` and `pia.c` (about 50 KB) were added to the exclude lists in
[linker_overrides/](linker_overrides/) on the theory that they thrash the 16 KB XIP cache. On hardware
the dips in International Karate were no rarer, and it cost 32 KB of heap, so it was reverted. Whatever
causes the remaining dips, it is not XIP misses in those modules.

### Zero-copy video

**The 384-wide buffer is wider than the 320-pixel output window, so the offset matters.**
[screen.h](src/screen.h) says only the middle 336 columns (`Screen_visible_x1` = 24 to
`Screen_visible_x2` = 360) may ever be displayed; the rest is scratch that antic.c overruns into.
With `graphics_set_offset(0, 0)` the window showed columns 0–319: 24 columns of that scratch on the left
and the rightmost 40 of the visible area — five characters — cut off, which on a TV with overscan looks
like two or three. [main.cpp](src/main.cpp) now derives the offset from those variables rather than
hardcoding it; it works out to **-32**, which lands exactly on the Atari's 40-column text field and drops
8 columns symmetrically from each side of the visible area. A negative offset makes the renderer start
further into each row; the copy length is clamped to what is left of the row, so there is no overrun.

There is no blit. [src/screen.c](src/screen.c) defines `__screen[384*240]` as the one 8-bit framebuffer,
`Screen_atari` points at it, and `main.cpp` hands that exact pointer to `graphics_set_buffer()`. Hence
`PLATFORM_DisplayScreen()` in [src/libatari800/video.c](src/libatari800/video.c) is an empty function, and
`Colours_SetRGB()` in [src/colours.c:86](src/colours.c#L86) writes straight into the display driver's
hardware palette via `graphics_set_palette()` instead of filling a `Colours_table`.

All four display drivers implement the **same** [drivers/graphics/graphics.h](drivers/graphics/graphics.h)
API (`graphics_init`, `graphics_set_mode`, `graphics_set_buffer`, `graphics_set_palette`,
`graphics_set_textbuffer`, …); `graphics.h` `#ifdef`s on `VGA`/`HDMI`/`TFT`/`TV` to pull in the right
backend header, and only the matching driver library is linked. Add a new display by implementing that
header, not by touching emulator code.

**All 256 palette entries are real on DVI now.** A framebuffer byte is a raw GTIA colour register value
(`hue << 4 | luma`), so all 256 indices are live and `colours.c` assigns every one of them. The driver used
to index its symbol table with 8 bits and carve the sync codes out of the palette at 240–254, which turned
hue 15 into flat background. The line buffer is now 16 bits per entry and the table has 512 entries
(`conv_color`, 8 KB, 8 KB-aligned): 0–255 are the palette, and sync codes, the border colour
(`INX_BG`), HDMI preambles and guard bands live at 256 and up. If you see `BASE_HDMI_CTRL_INX` or a
`(c & 0xf0) == 0xf0` test in another driver or an old diff, that is the limitation this removed. Three
things have to stay in step if the table is ever resized: the `in osr, 9` / `in x, 19` pair in the
converter program, the `>> 13` used to load its X register, and the table's alignment.

Related HDMI/TV-only side effect: their `graphics_set_mode()` calls `clrScr()` (VGA's does not). Because
`main.cpp` passes the one buffer as both framebuffer and text buffer, that memsets 53×30×2 = 3180 bytes —
roughly the top 8 scanlines — and `colours.c` / `colours_ntsc.c` / `colours_pal.c` call `graphics_set_mode`
on every palette update, so palette changes flicker the top of the screen for a frame.

### HDMI audio: pico_hdmi's packets over the PIO driver

**Confirmed on hardware at 48 kHz mono**: picture and sound on one display, with the USB keyboard, in a
build without logging. 44.1 and 32 kHz, stereo, and other displays have only been through a host-side
model that expands each line buffer through `conv_color` and decodes the TMDS/TERC4 stream like a sink
(sync, preambles, guard bands, packet types on the right lines, audio samples bit-exact, pixels against
the palette). If some other display shows nothing, build with `-DHDMI_AUDIO=OFF` first: that is the old
DVI signal apart from the 9-bit table.

[pico_hdmi](https://github.com/fliperama86/pico_hdmi) is an HSTX library, and HSTX cannot reach this
board's connector (see *Board facts*), so its output stage is unusable here. What is vendored under
[drivers/pico_hdmi/](drivers/pico_hdmi/) is its packet layer only — `hstx_packet.c`, unmodified, plus the
two headers it includes (`video_output.h` just for the `MODE_*` sync-polarity macros). It builds audio
sample, ACR and InfoFrame packets and TERC4-encodes them into one 30-bit word per pixel clock.
[hdmi_audio.c](drivers/hdmi/hdmi_audio.c) re-serialises those words into the PIO's pin order (72 words
per island) and [hdmi.c](drivers/hdmi/hdmi.c) puts one island in every hsync pulse. Do not pull in the
library's `hstx_data_island_queue.c`: it places code in `__scratch_x`, which is core 1's stack here.

How an island gets onto the wire, since it is not obvious from the code: a line-buffer entry stands for
two pixel clocks and is only an index, so arbitrary symbols cannot be written into the line. Instead 36
table entries (`INX_DI_SLOTS`, two sets of 18) are *rewritten* every scanline with that line's 36 island
symbols, and the line buffer points at them in order. Each set belongs to one of the two line buffers,
so the set being rewritten is never the one being transmitted. This is also why the handler now builds
every scanline rather than every second one — the second line of a doubled row copies its pixels from
the other buffer — and why the two buffers alternate every line.

Three stages, each lock-free single-producer/single-consumer:

- core 0, `PLATFORM_SoundWrite` → `hdmi_audio_write_u8()` → an 8192-frame sample ring. POKEY output is
  unsigned 8-bit and rests at 0, not mid-scale, so a DC blocker runs here; without it every underrun
  (which inserts true zero) is a full-scale click.
- core 1 main loop, `hdmi_audio_task()` → a 128-island queue. **Must stay on core 1**: the video interrupt
  consumes the queue with no locking, which is only safe because it preempts this code rather than
  racing it.
- core 1 video interrupt, `hdmi_audio_line_island()`, once per scanline. The schedule is pico_hdmi's: ACR
  and the audio InfoFrame inside vsync, the AVI InfoFrame on the first blanking line, ACR on every fourth
  back-porch line, an audio packet whenever four samples' worth of pixel clock has elapsed, a null
  packet otherwise.

Only 32000, 44100 and 48000 Hz can be carried; any other `SOUND_RATE` mutes HDMI audio (and logs it)
rather than playing at the wrong pitch. [data/atari800.cfg](data/atari800.cfg) uses 48000. `-DSD_LOG=ON` /
`-DUSB_LOG=ON` print an `audio: ... buffered N, underruns N, overruns N` line every five seconds: underruns are audio slots
that found the queue empty (expect a burst at start-up and whenever the UI is open, since the emulator
stops producing sound), overruns are samples dropped because the ring was full. Either one climbing
during normal play is audible. The init line also reports a serialiser self-check, which compares
`hdmi_audio.c`'s lookup table against the `get_ser_diff_data()` the picture goes through — a mismatch
would put islands on the wrong pins while video stays perfect.

**The whole audio path runs from RAM, and has to.** `hdmi_audio.c`, `hstx_packet.c`, their lookup tables
and libc's `memset`/`memcpy` are pulled out of flash by
[default_text_excludes.incl](linker_overrides/default_text_excludes.incl) and its `rodata` twin, and the
scanline handler uses plain loops (with `no-tree-loop-distribute-patterns`, or GCC turns them back into
`memcpy` calls). Core 1 encodes 12 000 packets a second; from flash that code shares the 16 KB XIP cache
with the emulator on core 0, and how much it loses depends on link layout. Symptom on hardware: one image
had sound, the next — same sources, logging off — was silent, and a third with a few counters added
worked again while still losing 2.6% of its audio slots (`encoded` short of `sent + silence` in
`audio.txt`). `-DAUDIO_DIAG=ON` writes that counter line to `\audio.txt` at 20 s and 60 s and touches the
card at no other time; use it rather than SD_LOG when the question is audio timing. A slot sent as silence
is never made up, so the task also discards backlog beyond 125 ms instead of letting delay grow.
Check any new code on this path with `nm`: it must land at `0x2000....`, and `objdump` of the handler
must show no `veneer`.

**Sound delivery depends on the frame throttle in `Atari800_Frame()`** ([atari.c](src/atari.c)). It paces
every frame at the Atari's own rate (59.92 / 49.86 Hz), so samples arrive one frame at a time and at
exactly the configured rate. It used to run six frames flat out and then wait for 100 ms to elapse,
which delivered sound in 100 ms lumps; it also kept its start time in an `int`, so the comparison
broke about 36 minutes after boot. Do not go back to batching frames.

**An SD_LOG build no longer logs `alive:` every second.** Each log line is an open/append/close on
core 0; once a second that stalled emulation for long enough to halve its speed and swallow key
presses. What remains is the five-second block, which still stalls briefly — an SD_LOG image is for
diagnosis, not for playing.

### libatari800 as the platform layer

The port is built on the upstream **libatari800** frontend ([src/libatari800/](src/libatari800/)) rather
than a bespoke `PLATFORM_*` backend: `LIBATARI800` is defined in [src/config.h](src/config.h), and the
emulator is driven frame-by-frame from `main()`. Input is an `input_template_t` struct (see
[src/libatari800/libatari800.h](src/libatari800/libatari800.h)) that three producers mutate concurrently:
`handleScancode()` (PS/2 keyboard, runs in the keyboard ISR, hence `__time_critical_func`),
`nespad_update()` (two NES pads, on core 1), and the Wii joystick via
[src/util_Wii_Joy.c](src/util_Wii_Joy.c). Keyboard handling is raw scancode → `input_map` field; there is
no intermediate keymap table.

### USB keyboard: HID translated to XT scancodes

`-DKBD_USB=ON` adds [drivers/usbkbd/](drivers/usbkbd/), a USB HID keyboard host
on the **native** USB controller. Verified on hardware at 252 MHz: `tuh_init`
returns 1, the keyboard enumerates (`proto=1`), and multi-key rollover works.
The board's wiki advertises host capability on its PIO-USB port instead, but
pico-spec drives this board through the native controller and that is the path
that was tested; PIO-USB would additionally need the Pico-PIO-USB library.

**It costs the USB CDC log.** One controller cannot be host and device at once,
so `KBD_USB` turns `pico_enable_stdio_usb` off and CMake **refuses**
`-DKBD_USB=ON -DUSB_LOG=ON` outright rather than silently dropping the log.
Use `-DSD_LOG=ON` for diagnostics in that configuration.

**It is serviced from core 1, not core 0, and that is load-bearing.** The
emulator's UI (`UI_Run` via `Atari800_Frame`) spins inside `GetKeyPress()`
polling `PLATFORM_Keyboard()`, so `libatari800_next_frame()` does not return
while a menu is open. Anything driven from core 0's main loop is therefore
starved for as long as the UI is up — which presented as a keyboard that typed
fine in Memo Pad and went completely dead the moment F1 opened the menu. PS/2
never had this problem because it is interrupt-driven. `render_core()` on core 1
spins regardless, so both `usbkbd_init()` and `usbkbd_task()` live there;
`tuh_init()` has to run on the same core as `tuh_task()` because it is what
enables the USB interrupt.

One consequence to be aware of: `handleScancode()` now runs on core 1, and its
Ctrl/Shift+Fn paths call `StateSav_*`, which touches FatFS. With `FF_FS_LOCK`
at 0 that is unsynchronised against core 0 doing its own card I/O. The PS/2 path
has the same hazard from an ISR, so this is not new in kind, but a save state
triggered while the emulator is loading a disk is a plausible way to corrupt
both.

The driver translates HID usages into **XT set-1 scancodes** and calls
`handleScancode()`, rather than feeding HID in directly. That function is not a
keymap: it also implements the joystick emulation on QWE/ASD/ZXC and the keypad,
the Alt+letter UI shortcuts, Ctrl/Shift+Fn save states and Ctrl+Alt+Del.
Translating reuses all of it; a second input path would duplicate that behaviour
and drift from it. PS/2 stays linked, so both keyboards work at once — they just
feed the same function.

Two things to know before editing the table in
[usbkbd.c](drivers/usbkbd/usbkbd.c):

- **Arrows map to the keypad codes, not the extended ones.** `handleScancode()`
  decodes `0xE0` only for right Ctrl and right Alt, and already treats
  `0x48`/`0x4B`/`0x4D`/`0x50` as both the arrows and joystick 1. Emitting
  `0xE048` and friends would make the arrows dead keys. HID Delete maps to
  `0x53` (keypad `.`) for the same reason — that is what Ctrl+Alt+Del expects.
- **The table uses designated initializers deliberately.** C cannot
  `_Static_assert` on array contents, and in a 104-entry positional list one
  stray element shifts everything after it invisibly. Explicit indices are the
  check. If you do change it, the compiled table can be read back with
  `arm-none-eabi-objdump -s --start-address=<hid_to_xt>` and compared by hand.

Note that [drivers/ps2kbd/](drivers/ps2kbd/) is **not** this: it is the reverse
direction, a PS/2 keyboard read over PIO that synthesises HID reports, which is
how pico-spec unifies its two input sources. It remains unused here.

### "PSRAM" is an SRAM array on this board

Banked/shadowed Atari memory is addressed byte-wise through `read8psram()` / `write8psram()` rather than
through pointers. [src/memory.c](src/memory.c) uses this for the RAM under the XL/XE OS, under cartridges
and the ANTIC self-test bank; [src/statesav.c](src/statesav.c) adds `StateSav_Save2PSRAM` /
`StateSav_Read2PSRAM` to spill those regions into save states. `MEMORY_mem[]` itself (64 KB) stays in SRAM
and is the fast path, and the 130XE extended banks are plain `Util_malloc` heap — they never went to PSRAM.

The RP2350-PiZero ships with its PSRAM pad **unpopulated**, so there is no PSRAM chip. Those call sites are
served by [drivers/psram-sram/](drivers/psram-sram/): a 40 KB SRAM array behind the same API and the same
header name (`psram_spi.h`), which is why `memory.c` and `statesav.c` needed no edits. The real requirement
is 34 KB (16K under-OS + 8K + 8K under-cart + 2K self-test); `PSRAM_SRAM_SIZE` carries the headroom, and
out-of-range accesses are clamped rather than left to scribble on neighbouring statics.

The original PIO driver still sits in [drivers/psram/](drivers/psram/), unreferenced. If you do solder an
APS6404 onto the pad, that is *not* the thing to reach for — RP2350 addresses PSRAM natively over QMI CS1
(GPIO47), so use the SDK's `hardware_psram`.

Note that `statesav.c` calls `write8psram`/`read8psram` without including any header, relying on implicit
declaration. That predates the retarget and keeps working only because `-w` and
`-Wno-error=implicit-function-declaration` are set.

### The OS ROM is selected, not baked in

`MEMORY_os` is a pointer that `SYSROM_SelectOS()` ([sysrom.c](src/sysrom.c)) aims at the chosen image: a
built-in array in flash, or a 16 KB heap copy when the ROM comes from a file on the card. It used to *be*
the OS-B array (`const ... MEMORY_os[16384]` in flash), with `SYSROM_LoadImage()` returning success
without copying anything for a flash destination — so every machine type ran the 400/800 OS and XL/XE
never booted. Two more things hid behind that: the built-in XL OS in
[ATARIXL_ROM.h](src/roms/ATARIXL_ROM.h) was declared `[8192]` around 16384 initialisers, silently
truncated because `-w` hides the warning, and `main()` passed `-atari`, which forced 400/800 at boot
whatever the config said. That argument is gone: the machine comes from `MACHINE_TYPE` / `RAM_SIZE` in
`atari800.cfg`, and with no card it is the emulator's default, XL/XE 64 KB. BASIC is still fixed to the
built-in revision C (`MEMORY_basic`, same flash-destination shortcut). XL/XE 64 KB is confirmed on
hardware: International Karate, which jams at the end of a match on a 48 KB 400/800, plays through.

`RAM_SIZE` is read from the config again, up to 128 KB (larger sizes would not fit the heap). The port had
that branch commented out, which only stayed hidden while `-atari` set 48 KB by hand: without it a
400/800 from the config kept the default 64 KB, and the System Settings menu hung, because upstream's
`FindMenuItem()` walks its array until it finds the value and 64 is not on the 400/800 list.
`Atari800_InitialiseMachine()` now corrects impossible machine/RAM pairs and `FindMenuItem()` stops at
the end of the menu.

### SD card: this card checks CRCs, both of them

[sdcard.c](drivers/sdcard/sdcard.c) is the FatFS sample SPI driver, which ships
the dummy CRC `0x01` on every command and hardcodes the two CRCs the spec fixes
(`0x95` for CMD0, `0x87` for CMD8). SPI mode usually ignores the command CRC — but
the card tested here does not, and answered `R1=0x09` (idle + CRC error) to every
command except those two. `send_cmd()` therefore computes a real CRC7 for every
command now. Validate any change to `crc7_sd()` against those two known values
before trusting it; `-DUSB_LOG=ON` prints a self-check of both at init.

A healthy init reads:

```
sd: CMD0 -> 01 (idle, ok)
sd: CMD8 -> 00            <- 0x00, not 0x01; the R7 echo is what identifies SDv2
sd: CMD8 R7 = f0 00 01 aa <- leading byte is a card quirk; 01 aa is the part that matters
sd: ACMD41[0] -> 00
sd: CMD58 -> 00
sd: OCR = c0 ff 80 00 -> SDHC/SDXC, block addressed
sd: CardType=0c (initialised)
```

Two related traps already fixed, both of which presented as a total failure to
init: CMD8's R1 is checked as `<= 1` because insisting on `0x01` left the four R7
bytes unread and desynchronised everything after it, and the start-up clocks go
out with CS deasserted as the spec requires.

**Data blocks need a real CRC16 too, and that one only breaks writes.** The same
driver shipped a dummy `0xFFFF` after the 512 payload bytes, so the card refused
every block at the data-response token. Reads were unaffected, because the
driver discards the CRC the card sends back — which makes the failure oddly
asymmetric: mounting works, directories list, `f_open` succeeds, and `f_write`
returns `FR_DISK_ERR` with zero bytes written. `crc16_sd()` is CRC-16/XMODEM
(poly `0x1021`, init 0), validated against the standard `"123456789"` → `0x31C3`
vector, which `-DUSB_LOG=ON` also prints as a self-check. A refused block logs
its response token, decoding `0x0b` as a CRC error and `0x0d` as a write error.
Note that GCC rewrites the bit loop into a 256-entry table, so `0x1021` does not
appear in the disassembly — look for the table load plus the two trailing
`xchg_spi` calls in `xmit_datablock` instead.

Both CRCs are now confirmed working on hardware. Anything the emulator writes to
the card — save states, `atari800.cfg` — depended on the second one.

Card contents: FAT32 (exFAT also works — `FF_FS_EXFAT` is 1 — but FAT32 is the
safer default for SDHC). The emulator reads `\atari800\atari800.cfg` and searches
`\atari800` for ROMs, so seed that directory from [data/](data/). Built-in Altirra
ROMs cover the no-card case.

### Storage: FatFS replaces stdio

There is no C stdio file layer. Upstream `FILE *` usage was rewritten to FatFS `FIL`/`f_open`/`f_read`
throughout ([src/util.c](src/util.c), [src/cfg.c](src/cfg.c), [src/ui_basic.c](src/ui_basic.c),
[src/statesav.c](src/statesav.c), [src/sio.c](src/sio.c)). Consequences to respect when editing:

- `DIR_SEP_BACKSLASH` is defined — paths are Windows-style, rooted at the SD card. Config lives at
  `\atari800\atari800.cfg`; `Util_getcwd()` always returns `\atari800`; F1–F12 save states go to
  `\atari800\~fN.sav`.
- `Util_*` helpers in [src/util.h](src/util.h) are the portable seam (`Util_flen`, `Util_uniqopen`,
  `Util_malloc`) — prefer them over raw `f_*` in emulator code.
- Seed an SD card from [data/](data/) (ROMs, `atari800.cfg`, demo disks/executables) into `\atari800\`.
  Built-in Altirra ROMs ([src/roms/](src/roms/), `EMUOS_ALTIRRA`) are the fallback when ROM files are absent.

### Logging is compiled out by default

[src/debug.h](src/debug.h) `#define`s `printf` and `Log_print` to **nothing** unless `MNGR_DEBUG` is set in
the CMake `target_compile_definitions`. With it set, both macros format into a 256-byte stack buffer and
append to `\atari.log` on the SD card via `logMsg()` — which opens/writes/closes the file per call and
blinks the LED, so it is far too slow for per-frame paths. `pico_enable_stdio_usb` is on, but the `printf`
macro means USB-CDC output only appears from code that bypasses `debug.h`.

## Conventions for editing emulator code

[src/](src/) is a vendored upstream tree. Keep its file layout, naming (`MODULE_Function`,
`MODULE_variable`), tabs-for-indent style, and GPL headers intact. Port-specific divergence is pervasive and
mostly *unguarded* (direct FatFS and PSRAM calls), so when changing these files assume the upstream version
no longer applies and read the local code. Feature switches that do exist are in
[src/config.h](src/config.h) (`SOUND_THIN_API`, `PAGED_ATTRIB`, `VOL_ONLY_SOUND`, `SUPPORTS_PLATFORM_SLEEP`,
…) and in the CMake definitions block (`STEREO_SOUND`, `VOICEBOX`, `USE_WII`, `PSRAM`, …).

Pin assignments are **not** in the source — they are all `target_compile_definitions` in
[CMakeLists.txt](CMakeLists.txt) (`HDMI_BASE_PIN`, `SDCARD_PIN_SPI0_*` plus
`SDCARD_SPI_BUS`, `TFT_*_PIN`, `KBD_*_PIN`, `NES_GPIO_*`, `WII_S*_PIN`, `PWM_PIN0/1`, `BEEPER_PIN`,
`LOAD_WAV_PIO`). The block is split into a fixed-by-the-PCB half and a placeholder half; see *Board facts*.
Note the legacy naming: the microSD defines still say `SPI0` while `SDCARD_SPI_BUS` is `spi1`.

Note that `drivers/` contains more backends than are built: only `ps2`, `fatfs`, `sdcard`, `nespad`,
`psram-sram`, `graphics`, the selected display driver and — with `-DKBD_USB=ON` — `usbkbd` are added as
subdirectories. `pico_hdmi/` has no CMakeLists of its own; `drivers/hdmi` compiles its one source file. `psram/` (the PIO PSRAM driver), `ps2kbd/`, `audio/`, `usb/`, `usbfs/`, `ws2812/` are
present but unused.

## Board facts

Everything below is fixed by the RP2350-PiZero PCB and verified against the Waveshare wiki plus two
independent emulator ports to the same board. Pin numbers live in `target_compile_definitions` in
[CMakeLists.txt](CMakeLists.txt), never in the sources.

| Function | GPIO | Notes |
|---|---|---|
| DVI TMDS **data** | 32–37 | three differential pairs, `HDMI_PIN_DATA_BASE` |
| DVI TMDS **clock** | 38–39 | `HDMI_PIN_CLK_BASE` |
| microSD | SCK 30, MOSI 31, MISO 40, CS 43 | hardware `spi1`; CS is a plain GPIO, not SPI1 CSn |
| card detect | 22 | unused by the firmware |
| PIO-USB | 28–29 | unused by the firmware |
| PSRAM pad | QMI CS1 / 47 | **unpopulated** on a stock board |

**Data comes first on this board, which is the reverse of the driver's usual layout.** Everywhere else
(Murmulator, PICO_DV) `HDMI_BASE_PIN` means clock at the base and data at base+2. pico-spec's `hdmi.h`
special-cases its Zero boards as `data = base`, `clk = base + 6`, and the PiZero follows that. Hence
[CMakeLists.txt](CMakeLists.txt) sets `HDMI_PIN_DATA_BASE`/`HDMI_PIN_CLK_BASE` explicitly instead of
deriving both from `HDMI_BASE_PIN` — do not "simplify" that back to a single base, and do not infer the
order from `HDMI_BASE_PIN=32` in pico-spec's CMakeLists without reading its `hdmi.h` first.

**Pair polarity and channel order are board wiring.** `HDMI_PIN_invert_diffpairs` and
`HDMI_PIN_RGB_notBGR` are **0** here and 1 on the Murmulator: that board drove a resistor network, this one
has a real DVI connector wired straight through. Both now default to 1 in
[hdmi.h](drivers/hdmi/hdmi.h) behind `#ifndef` and are overridden to 0 in
[CMakeLists.txt](CMakeLists.txt), matching pico-spec (0 for its Zero boards, 1 elsewhere). Getting
`invert_diffpairs` wrong is nasty to diagnose: the PIO and DMA run flawlessly, `pio_sm_init` returns 0,
the IRQ counter climbs, no DMA error bits set — and no monitor will lock onto the signal. If video is
silent with a provably healthy pipeline, check this before anything else.

Three consequences worth knowing before touching video:

- **HSTX cannot drive this board's DVI.** HSTX is hard-wired to GPIO12-19 on RP2350 and the connector is on
  32-39, so video has to be PIO TMDS - which is what [drivers/hdmi/](drivers/hdmi/) already did on RP2040.
  That is why the retarget reused it instead of writing an HSTX driver.
- **A PIO instance sees either GPIO0-31 or GPIO16-47, never both.** Data at 32 forces the upper window, so
  [hdmi.c](drivers/hdmi/hdmi.c) calls `pio_set_gpio_base(PIO_VIDEO, 16)` before configuring the state
  machines, and anything else sharing pio0 must live at GPIO16 or above. `HDMI_PIO_GPIO_BASE` in
  [hdmi.h](drivers/hdmi/hdmi.h) derives this from the pin numbers.
- **Pin masks above 31 need the 64-bit PIO calls.** `pio_sm_set_pins_with_mask()` takes a 32-bit mask
  relative to the PIO's GPIO base; a clock pair at GPIO38 overflows it (`3u << 38` is UB). The driver uses
  the `*_mask64` variants, which take absolute GPIO numbers. `pio_sm_init()` also returns
  `PICO_ERROR_BAD_ALIGNMENT` when a pin config does not fit the window - it is now checked and printed
  along with the SM's `pinctrl`, since silently ignoring it hides exactly this class of fault.

**Input: USB works, PS/2 and NES are unverified.** The USB keyboard is confirmed end to end on hardware —
typing, F1 into the menu, menu navigation and the disk file selector. The `KBD_*`, `NES_*` and audio pins in
[CMakeLists.txt](CMakeLists.txt) are copied from pico-spec's ZERO2 target rather than guessed, so they
match what that firmware expects on this board — but this port's PS/2 and NES code has not been run against
real wiring here, and a build without a PS/2 keyboard attached logs a harmless `KBD error 01` at boot. `WII_SDA_PIN` /
`WII_SCL_PIN` are parked on free I2C1 pins purely because `util_Wii_Joy.c` dereferences them
unconditionally; `USE_WII` is not defined and `init_wii()` is never called. Treat the key map below as
inherited from the Murmulator build.

## Device key map

Inherited from the Murmulator build. The USB keyboard path is verified on this board; the PS/2 and NES
pins are not (see *Board facts*).

F1 UI · F2 Option · F3 Select · F4 Start · F5 Help · Ctrl+Alt+Del cold restart · Ctrl+Fn save state ·
Shift+Fn load state · F12 at boot → USB firmware update mode.
Joystick 0: `Q W E / A D / Z X C` + LeftCtrl fire. Joystick 1: numpad `7 8 9 / 4 6 / 1 2 3` + RightCtrl fire.
NES pad: A = fire, Start, Select, Start+Select = UI.
