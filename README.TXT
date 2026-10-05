AI adaptation https://github.com/DnCraptor/atari800 for Waveshare RP2350-PiZero with USB keyboard and video+sound over DVI

Atari emulator for the Waveshare RP2350-PiZero board
(RP2350B, Arm Cortex-M33, 520K SRAM, 16M flash, on-board DVI and microSD).

Board wiki: https://www.waveshare.com/wiki/RP2350-PiZero

Video goes out of the board's own DVI connector. Note that HSTX is hard-wired to
GPIO12-19 on RP2350 while the connector sits on GPIO32-39, so the output is
generated as TMDS from PIO, not from HSTX. The system clock is 252 MHz because
that yields an exact 25.2 MHz pixel clock for 640x480p60; changing it changes
the video timing.

The PSRAM pad is unpopulated on a stock board, so the banked memory the emulator
used to keep in external PSRAM now lives in a 40K SRAM array
(drivers/psram-sram). No PSRAM chip is required.

Build (the toolchain comes from the VS Code Raspberry Pi Pico extension;
SDK 2.3.1 or newer is required, as that is the first with a board header for
this board):

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
    cmake --build build

The result is bin/MinSizeRel/pico-atari-16384K-DVI.uf2.

INPUT IS NOT FINISHED. The PS/2 keyboard and NES gamepad pins in CMakeLists.txt
are placeholders on free 40-pin header GPIOs, picked only to avoid the pins the
PCB already uses (DVI 32-39, microSD 30/31/40/43, card-detect 22, PIO-USB
28/29). They have not been verified against real wiring, so the key map below is
inherited from the earlier ZX Murmulator port and should be treated as such.

This port previously targeted the ZX Murmulator board (RP2040); that history is
in git. Murmulator links, kept for reference:
  https://murmulator.ru/howto
  https://github.com/AlexEkb4ever/MURMULATOR_classical_scheme
  https://t.me/ZX_MURMULATOR
  https://t.me/ZX_MURMULATOR_DATA
  https://file.murmulator.ru/data/

This port is based on Atari800 emulator version 5.2.0: https://github.com/atari800/atari800
-------------------------------

This is a free and portable Atari 8-bit computer and 5200 console emulator,
originally written by David Firth and now developed by the Atari800
Development Team (please see DOC/CREDITS). This program is copyrighted and
released under the GPL (see COPYING).

Optimised for ZX Murmulator by me ;) then retargeted to RP2350-PiZero.
PS2 Keyboard mapping:
F1 - UI
F2 - Option
F3 - Select
F4 - Start
F5 - Help
Ctrl + Alt + Del - Cold Restart

Joystick 0 emulation:
Q / W / E - up left   / up   / up right
A /   / D - left      /      / right
Z / X / C - down left / down / down right
Left Ctrl - fire

Joystick 1 emulation (numpad):
7 / 8 / 9 - up left   / up   / up right
4 /   / 6 - left      /      / right
1 / 2 / 3 - down left / down / down right
RightCtrl - fire

Dendy-8 joysticks:
Start + Select - UI
Start  - Start
Select - Select
A - Fire
TBA

TBA:
Please read DOC/README, DOC/INSTALL and DOC/USAGE and eventual platform
specific versions of the above mentioned files.

For news in this release please read DOC/NEWS.

If you have a problem running this emulator please read DOC/FAQ first.
If the problem persists then please ask in newsgroup comp.sys.atari.8bit
or in the atari800-users mailing list (see below).

Look at DOC/BUGS before reporting something you consider a bug.

If you want to help developing Atari800 emulator you can subscribe to our
mailing list (http://lists.sourceforge.net/lists/listinfo/atari800-users).
DOC/TODO might point you in the right direction.

This version, and all previous versions back to v0.8.2 have been released Petr
Stehlik and are available at internet (current project URL is below). Thanks
go to original author David Firth and all the people that contributed since then.

EnJoy!

Petr Stehlik
Dec 29, 2023

E-mail: pstehlik@sophics.cz

Project homepage: http://atari800.github.io/
