# Changelog

## 1.0.0

The first full release (the app shows V1.0). Made for overclockers: every load is checked bit for bit, and the log says at which clock and how hot each error came.

- **Eight stress loads.** RSX core, VRAM and RSX + VRAM are now loads of their own:
  - **VRAM:** the RSX copies its 16 MB texture texel for texel into a 2048x2048 target, as fast as the graphics memory goes. Every 64 KB of the copy is read back and checked.
  - **RSX + VRAM:** the copy computes on the side (the multiply-adds a pixel are picked at start), together with the shader tiles and the fill layers.
  - **CPU + RSX and Everything** now run RSX + VRAM: the whole console at once.
- **Clocks.**
  - The RSX core clock is measured at start from the shader's rate: 500 MHz on a stock PS3.
  - The RSX core and VRAM clocks are also read from lv1, read only, from the registers webMAN MOD shows. This needs a custom firmware with lv1 peek; without it the screen shows the measured figures.
  - The clocks are read again once a second. The screen always shows the current clocks, the log notes every change ("RSX core 500 -> 600 MHz"), and every stress test result names the clocks it ran at.
  - At start the lv1 core clock is checked against the measured one.
- **Errors for overclockers.**
  - Every error line names its moment: " - at RSX 73 C, core 700 MHz, VRAM 750 MHz" (CPU errors: the CPU temperature).
  - Every stress test ends with each check's error pattern:
    - which bits differed, and whether from 0 to 1 or from 1 to 0;
    - in which word of a 16-byte memory word they sat (the VRAM bus is 128 bits wide);
    - how many words came back 00000000.
  - The pattern also gets a note in words when the same place fails again and again.
  - A stability report says when the first error came, in which part, and how many came a minute.
- **The VRAM chip next to the RSX** in the menu, the setup line and the running test.
- **Emulator detection.** In RPCS3 the app says "Emulator - host figures" instead of showing clocks no PS3 has.
- **Safe start.**
  - During the start, the log is written to the disk line by line, so a start that freezes the console still leaves its log.
  - A start guard notes each part of the start. A part that stopped a start (the RSX load, the VRAM copy, the clock read, the network) is left out the next time, with a red note on the screen. A new version tries it again.
  - Any RSX timeout at start ends the app, so the system can reset the graphics chip.
- **Fixes.**
  - The tiled fill target's region now holds whole rows of the RSX's 64-line tiles. With 1080 lines, the RSX wrote the target's bottom-right corner up to 47 KB past the region. V0.9 had this too, but there it hit no checked data.
  - The PPU writes vertex data into VRAM four bytes at a time. An 8-byte access that is not 8-byte aligned raises an alignment exception in the RSX's memory.
  - The fill check tile always uses the program its reference came from.
- **New self-tests** for the VRAM copy and for the texture after the tiled fill.

## V0.9 BETA

- **All RSX units at once.**
  - The fill layers go into a tiled render target (`gcmSetTile`) when that is faster. On a PS3 that doubled the fill rate: 1.05 instead of 0.53 G pixels a second.
  - The fill pixels also compute: the app tries 8 to 96 multiply-adds a pixel at start and takes the count that keeps the arithmetic units and the blending busiest together. On a PS3 the pixel arithmetic reached 155 GFLOP/s during the test, 87 % of the shader's rate alone, at 99.2 % RSX busy.
  - A vertex load (points, 256 multiply-adds a vertex, with its own bit-exact check tile) runs only when it overlaps the pixels. On a PS3 it does not, so it turns itself off.
  - The screen shows each RSX unit's share of what it managed alone (ALU, blend, vertex).
- **Fix: vertex programs loaded safely across command-buffer segments** (a PSL1GHT bug, see `docs/PSL1GHT-NOTES.md`).
- **More self-test steps** for the tiled target, the longest fill program and the vertex load.

## V0.8 BETA

- **RSX pacing by its own time stamps.**
  - While the RSX load runs, a picture stays two TV frames. The load is set so that the RSX reaches the next flip a fraction of a millisecond before the blank.
  - 99.24 % RSX busy on a PS3, 0 late pictures. Before it was 74 %.
- **Three display buffers.**
- **Fix: the background flicker under the RSX load.** A cached clear colour did not survive the flip.
- The fill texture is swizzled.

## V0.7 BETA

- **Heavier RSX load:**
  - four register layouts of the shader, timed at start (the fastest reaches about 97 % of the arithmetic peak);
  - full-screen texture + blend layers;
  - a VRAM read-back check.
- **The interface draws at 30 frames a second under CPU loads**, which leaves more PPU time to the load.

## V0.6 BETA

- **New interface:** Roboto font, card layout, a live history, and six stress loads including RSX, CPU + RSX and Everything.

## V0.5 BETA

- Renamed from *TEE SPE Bench* to **TEE PS3 Benchmark**, with a new version numbering.

## Before (TEE SPE Bench V1–V9.0)

- The SPU and PPU benchmarks.
- The CPU stress loads with checks for every job.
- The Cell clock measured by the SPEs.
- The NTP clock check.
- webMAN temperatures and fan.
- The first RSX load with its self-test.
