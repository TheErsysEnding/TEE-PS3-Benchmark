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
	volatile uint64_t fmads, vbatches, vmads, vert_checks, vert_errors;
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
	volatile double tiles_per_picture, layers_per_picture;
	volatile uint64_t last_bad;           /* checksum of the last check tile that differed */
} rsxload_status;

/* after gfx_init, in the main thread: 0 when the load can run */
int  rsxload_init(void);
/* in the main thread before the first frame: self-test, measures, makes the check tiles' references */
void rsxload_probe(void);
/* main thread, once a picture before the flip: load on or off (it finishes what it queued) */
void rsxload_set_active(int on);
const rsxload_status *rsxload_get(void);
/* at the end, after gfx_shutdown (the RSX idle): the fill's tile region given back */
void rsxload_shutdown(void);
/* the shader layout's name, for the log and the screen */
const char *rsxload_variant_name(int v);

#endif
