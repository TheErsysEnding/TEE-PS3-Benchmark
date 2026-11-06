# Changelog

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
