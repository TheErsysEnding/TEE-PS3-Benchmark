/* The RSX's load. Everything here runs in the main thread, inside the flip (gfx_set_frame_hook): the
   RSX's command buffer has one writer. Other threads only read rsxload_get(). The app changes no clock:
   what it says about the RSX's clock comes from timing alone.

   The parts, every picture between two flips:
   - Shader (V9): a fragment program of nothing but multiply-adds on four floats - rotations like the
     SPUs' heat kernel - over 256x256 pixel tiles away from the screen. V0.7 has four ways to lay it out
     in registers (the RSX runs fewer pixels at once the more registers a program needs; V9's six
     registers reached a third of the peak); the probe at the start times each and the load takes the
     fastest. A 128x128 check tile of it goes into main memory every fourth picture and must equal the
     test's first one bit for bit: every pixel depends on its position only.
   - Fill (V0.7): full-screen layers (1920x1080, away from the screen) blended over each other, each
     pixel two filtered reads of a 2048x2048 texture in VRAM (the second half the texture away, nudged
     by the first) and a few multiply-adds - the blending units, the texture units and the VRAM's
     bandwidth, as the heavy transparent effects of games load them. Its own 128x128 check tile (eight
     layers) every fourth picture.
   - Vertices (V0.9): batches of points behind the eye, each vertex 256 multiply-adds on four floats in
     the RSX's eight vertex units, drawn behind each fill layer so they run while its pixels are drawn; a
     128x128 check tile of points (a pixel each, its colour the folded result) every fourth picture.
     V0.9 also draws the fill into a tile region when that is faster, and gives the fill pixels the number
     of multiply-adds that keeps the arithmetic units and the blending busiest together.
   - VRAM check (V0.7): every picture 64 KB of that texture are copied back into main memory and compared
     with what the PPU wrote there at the start; the whole 16 MB every 256 pictures. V0.8: the texture is
     swizzled (laid out in small squares, as games keep theirs), the same bytes in memory.
   - VRAM copy (V1.0): the same 16 MB read through a second, linear texture header and drawn texel for texel
     (point sampling, no blending) into a 2048x2048 target - 4 bytes read and 4 written a pixel, the VRAM at
     its limit. Every copy writes the same bytes, so slices of the target can be read back at any time and
     compared with the PPU's checksums of the texture: two slices a picture. With multiply-adds on the side
     (V1.0's RSX + VRAM load) the pixel units compute while they wait for the memory: the probe takes the
     most that keep the copy at 95 % of its rate.
   The three RSX loads (V1.0): RSX core (shader and computing fill, no copy), VRAM (the copy alone) and
   RSX + VRAM (shader, computing fill and the copy with multiply-adds) - rsxload_set_active's mode.
   How much a picture (V0.8): the RSX idles from reaching the flip until the TV's vertical blank (on the
   console the wait for the flip holds it, in RPCS3 the flip command). While the load runs a picture stays for two TV frames
   (30 a second), and the RSX's own time stamps (rsx_pace.h) set the load so that the RSX reaches the next
   flip a fraction of a millisecond before its blank: it waits once every two frames, briefly. The load
   comes in rows (a shader tile's 256-pixel rows, a fill layer's 1920-pixel rows), so it can be set to a
   few microseconds. The time stamps also say how busy the RSX was. Without them (an emulator, or if they
   do not agree with the PPU's clock) the load is set from the probe's timings as in V0.7. */
#ifndef RSX_LOAD_H
#define RSX_LOAD_H

#include <stdint.h>

#define RSXLOAD_TILE        256   /* shader tile: 256x256 pixels */
#define RSXLOAD_CHECK       128   /* check tiles: 128x128 pixels, A8R8G8B8 */
#define RSXLOAD_VARIANTS    4     /* register layouts of the shader */
#define RSXLOAD_FILL_W      1920  /* a fill layer */
#define RSXLOAD_FILL_H      1080
#define RSXLOAD_TEX         2048  /* the fill's texture, 2048x2048 A8R8G8B8 = 16 MB */
#define RSXLOAD_FRAMES      2     /* V0.8: TV frames a picture stays while the load runs */
#define RSXLOAD_COPY        2048  /* V1.0: the copy target, 2048x2048 A8R8G8B8 (a row: 2048 pixels) */

/* V1.0: the RSX load's modes */
enum { RSXMODE_CORE, RSXMODE_VRAM, RSXMODE_BOTH, RSXMODES };

typedef struct {
	int ready;                    /* programs and buffers in place */
	/* the probe before the first frame, with nothing else on the RSX */
	int probe_ok, check_ok;       /* check_ok: one check pixel agrees with the PPU's own sum */
	double probe_gmad;            /* G four-float multiply-adds a second (long program less short one) */
	double probe_load_gmad;       /* the long program as it runs, setup and pixel writes included */
	double probe_tile_us;         /* one load tile with the long program */
	double probe_long_us, probe_short_us;   /* the probe's two runs, on the PPU's clock */
	int probe_tiles, check_x, check_y, check_from_top, first_differed;
	uint32_t check_bits, check_expect;   /* that pixel's bytes on the RSX; red and green as the PPU gets them */
	float check_ppu;              /* the PPU's v for it */
	int selftest_failed;          /* a self-test step hung the RSX: the app must end */
	uint64_t reference;           /* the check tile's checksum */
	/* the shader's layouts: each one's rate alone (G MADs/s, 0: not measured), whether its check pixel
	   agreed with the PPU, and the one the load uses */
	double variant_gmad[RSXLOAD_VARIANTS];
	int variant_ok[RSXLOAD_VARIANTS], variant;
	/* the fill: set up, a layer alone, G pixels a second alone */
	int fill_ok;
	double probe_layer_us, probe_gpix;
	/* without the time stamps: the load a picture from the probe's timings (rows at 100 %), and the share
	   given now (cut when a picture comes late) */
	int target_rows_s, target_rows_f;
	double scale, ceiling;
	uint32_t missed;              /* pictures that came a frame late in this test */
	/* V0.8: the RSX's time stamps agreed with the PPU's clock in the probe (their ratio); the load is set
	   by them (timed) - else by the probe's timings, or fixed in an emulator */
	int reports_ok, timed;
	double clock_ratio;
	/* the pacing now: the load's planned time, the margin before the blank, a row's time (shader, fill) */
	double plan_us, margin_us, unit_s_us, unit_f_us;
	/* the screen alone before any test (no load, a picture a frame): the RSX's average wait at the flip
	   command and at the wait for the flip, and its drawing; 0 until measured */
	double idle_flip_us, idle_wait_us, idle_draw_us;
	int idle_pictures;
	/* the VRAM check: set up and its first slice came back right */
	int vram_ok;
	/* V0.9: the fill's target (1: the tile region), its program (variant, multiply-adds a pixel), blending
	   alone on that target (G pixels a second: what the blending units manage), a layer's time with the
	   vertex batches behind it */
	int fill_tiled, fill_variant, fill_mads;
	double blend_gpix, layer_us;
	/* V0.9: the vertex load - set up (and its check pixel agreed with the PPU), batches behind each fill
	   layer, its rate alone (G four-float multiply-adds a second) and the share of it that ran under the
	   pixels in the probe */
	int vert_ok, vert_check_ok, vert_batches;
	double probe_vert_gmad, vert_overlap;
	/* V1.0: an emulator (the self-test passed, but the shader's and the fill's timings are impossible on a
	   PS3: an emulator writes the labels before the work) - then clocks, latency and rates describe the host */
	int emulator;
	/* V1.0: the RSX's core clock, measured: each shader layout's rate alone against its multiply-adds a clock
	   on a stock RSX at 500 MHz (the median of the layouts; 0: not measured), and each layout's own figure */
	double rsx_mhz, variant_mhz[RSXLOAD_VARIANTS];
	/* V1.0: the VRAM copy - set up and checked (its slices came back as the texture), its target (1: the tile
	   region; V1.0 keeps it linear), drawn upside down (the RSX's rows the other way round); the plain copy's rate alone (GB/s, 8
	   bytes a pixel: read + write) and a row's time; the multiply-adds a pixel the RSX + VRAM load gives it,
	   that program's rate alone and a row's time */
	int copy_ok, copy_tiled, copy_flip, copy_mads;
	double copy_gbs, copy_row_us, copy_mad_gbs, copy_mad_row_us;
	/* V1.0: the VRAM's clock, measured: the plain copy's rate alone against a stock PS3's at 650 MHz (0: not
	   measured, or no stock reference yet - then copy_gbs is shown) */
	double vram_mhz;
	/* V1.0: the mode the load runs (or ran) in, the share of its second part's rows that are copy rows (the
	   rest fill rows), and that part's row time at the start (fill and copy rows mixed) */
	int mode;
	double copy_frac, mix_row_us;
	volatile uint64_t fmads, vbatches, vmads, vert_checks, vert_errors;
	/* V1.0: the copy - rows and passes drawn, pixels, its multiply-adds, slices read back and wrong ones */
	volatile uint64_t copy_rows, copy_passes, copy_pixels, cmads, copy_slices, copy_errors;
	/* since the start of the app, main thread writes: other threads take differences */
	volatile uint64_t tiles, mads, frames, checks, check_errors;
	volatile uint64_t layers, pixels, fill_checks, fill_errors;
	volatile uint64_t vram_slices, vram_errors;
	/* tiles: shader draws (a tile or part of one), layers: fill draws; rows of each, for the rates */
	volatile uint64_t rows_s, rows_f;
	/* the RSX's time on the load, by the probe's timings (a row alone): microseconds */
	volatile double busy_tile_us, busy_fill_us;
	/* V0.8, by the RSX's time stamps over the pictures of the load: their time, the RSX's waits in it
	   (at the flip command and anywhere else), its time on the shader and on the fill (us) */
	volatile double ts_span_us, ts_idle_us, ts_flip_wait_us, ts_tile_us, ts_fill_us;
	volatile uint64_t ts_pictures, ts_seen, ts_bad;
	volatile uint64_t progress_tb;        /* timebase when the RSX last finished a picture's load */
	volatile int active, pending;
	volatile double tiles_per_picture, layers_per_picture, copies_per_picture;
	volatile uint64_t last_bad;           /* checksum of the last check tile that differed */
} rsxload_status;

/* after gfx_init, in the main thread: 0 when the load can run */
int  rsxload_init(void);
/* in the main thread before the first frame: self-test, measures, makes the check tiles' references */
void rsxload_probe(void);
/* main thread, once a picture before the flip: load on or off (it finishes what it queued), and in which mode
   (RSXMODE_*: taken when the load goes on) */
void rsxload_set_active(int on, int mode);
/* the mode's name for the log: "RSX core", "VRAM", "RSX + VRAM" */
const char *rsxload_mode_name(int mode);
const rsxload_status *rsxload_get(void);
/* V1.0 (6th build): the error pattern of each kind of check since the test started (bits, 0 -> 1 or 1 -> 0, the place in
   a 16-byte memory word) into the log, then cleared; the stress test's summary calls it */
void rsxload_errpat_log(void);
/* at the end, after gfx_shutdown (the RSX idle): the fill's tile region given back */
void rsxload_shutdown(void);
/* the shader layout's name, for the log and the screen */
const char *rsxload_variant_name(int v);

#endif
