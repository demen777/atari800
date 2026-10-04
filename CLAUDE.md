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

Three bring-up knobs: `-DUSB_LOG=ON` turns `printf`/`Log_print` into real output on the USB CDC port,
`-DSD_LOG=ON` appends to `tari.log` on the card (`MNGR_DEBUG`), and `-DSYS_CLOCK_KHZ=` overrides the
clock for testing — note that anything other than 252000 skews DVI timing. Both logs only start once
execution reaches them, so neither helps with a hang before `stdio_init_all()`.

**There are no tests.** `configure.ac`, `Makefile.am`, `autogen.sh`, `.travis*`, `atari800.spec`, `debian/`,
`src/libatari800/libatari800_test.c` and `tools/` are upstream autotools leftovers — not wired into the
CMake build and not maintained here. CMake globs `src/*.c` and `src/*.cpp` recursively, so adding a source
file needs no CMakeLists edit (but does need a re-configure).

**RAM used to be the binding constraint and no longer is.** On RP2040 it was a 256 KB budget run at ~87%
full, and commit messages in `git log` are literally recorded RAM percentages
(e.g. `RAM: 228640 B / 256 KB 87.22%`). RP2350B has 512 KB and the current build sits at **277 596 B
(52.95%)**, flash at 3.48% of 16 MB. The link still prints `--print-memory-usage`, but there is now
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
recomputes the stack symbols. Keep `CORE0_STACK_SIZE` identical in all three — the SDK's own
`ASSERT(__StackLimit >= __HeapLimit)` is what catches a mismatch. Core 1 keeps SCRATCH_X (4 KB,
`PICO_CORE1_STACK_SIZE`), SCRATCH_Y is now entirely unused, and the heap still has ~210 KB.

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
- **Sound** — a `repeating_timer` at the POKEY sample rate (`snd_timer_callback`) walks the buffer that
  `PLATFORM_SoundWrite` captured from the core and pushes samples to PWM pins. The timer is re-armed from
  the main loop whenever `libatari800_get_sound_frequency()` changes.

### Zero-copy video

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

**The drivers are not palette-equivalent.** A framebuffer byte is a raw GTIA colour register value
(`hue << 4 | luma`), so all 256 indices are live and `colours.c` assigns every one of them. VGA honours all
256. **HDMI loses the top 16** (`0xF0`–`0xFF`, i.e. hue 15 at every luma): `BASE_HDMI_CTRL_INX` is 240, so
indices 240–254 are reserved for sync signalling — [hdmi.c:545](drivers/hdmi/hdmi.c#L545) returns without
storing those colours, and the scanline renderer at [hdmi.c:213](drivers/hdmi/hdmi.c#L213) additionally
forces any pixel matching `(c & 0xf0) == 0xf0` to index 255, the background colour. Result: hue-15 artwork
renders as flat background on DVI and correctly on VGA. Do not "fix" this by widening the palette write —
the reserved entries are what generates HSYNC/VSYNC. Since DVI is this board's only wired output, this is
now a limitation of the default build rather than of an optional backend, and it is the most likely
explanation for a game whose colours look wrong in exactly one hue.

Related HDMI/TV-only side effect: their `graphics_set_mode()` calls `clrScr()` (VGA's does not). Because
`main.cpp` passes the one buffer as both framebuffer and text buffer, that memsets 53×30×2 = 3180 bytes —
roughly the top 8 scanlines — and `colours.c` / `colours_ntsc.c` / `colours_pal.c` call `graphics_set_mode`
on every palette update, so palette changes flicker the top of the screen for a frame.

### libatari800 as the platform layer

The port is built on the upstream **libatari800** frontend ([src/libatari800/](src/libatari800/)) rather
than a bespoke `PLATFORM_*` backend: `LIBATARI800` is defined in [src/config.h](src/config.h), and the
emulator is driven frame-by-frame from `main()`. Input is an `input_template_t` struct (see
[src/libatari800/libatari800.h](src/libatari800/libatari800.h)) that three producers mutate concurrently:
`handleScancode()` (PS/2 keyboard, runs in the keyboard ISR, hence `__time_critical_func`),
`nespad_update()` (two NES pads, on core 1), and the Wii joystick via
[src/util_Wii_Joy.c](src/util_Wii_Joy.c). Keyboard handling is raw scancode → `input_map` field; there is
no intermediate keymap table.

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
`psram-sram`, `graphics` and the selected display driver are added as subdirectories. `psram/` (the PIO
PSRAM driver), `ps2kbd/`, `audio/`, `usb/`, `usbfs/`, `ws2812/` are present but unused.

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

**Input is unfinished.** The `KBD_*`, `NES_*` and audio pins in [CMakeLists.txt](CMakeLists.txt) are
copied from pico-spec's ZERO2 target rather than guessed, so they match what that firmware expects on this
board — but this port's PS/2 and NES code has not been run against real wiring here. `WII_SDA_PIN` /
`WII_SCL_PIN` are parked on free I2C1 pins purely because `util_Wii_Joy.c` dereferences them
unconditionally; `USE_WII` is not defined and `init_wii()` is never called. Treat the key map below as
inherited from the Murmulator build.

## Device key map

Inherited from the Murmulator build; unverified on this board (see *Board facts*).

F1 UI · F2 Option · F3 Select · F4 Start · F5 Help · Ctrl+Alt+Del cold restart · Ctrl+Fn save state ·
Shift+Fn load state · F12 at boot → USB firmware update mode.
Joystick 0: `Q W E / A D / Z X C` + LeftCtrl fire. Joystick 1: numpad `7 8 9 / 4 6 / 1 2 3` + RightCtrl fire.
NES pad: A = fire, Start, Select, Start+Select = UI.
