/* See rsx_load.h. The fragment programs are put together here, word by word, in the RSX's own format
   (NV40, as RPCS3 reads it; checked against tiny3d's hand-written programs, which run on the console):
   cgcomp from ~/ps3dev cannot compile Cg without NVIDIA's toolkit, and its assembler drops constants.
   The shader tiles' vertex program, a plain copy of the position, is cgcomp's (shaders/pass.vasm); the
   fill layers use the interface's (shaders/ui.vasm: position, colour, texture coordinate).
   Two PSL1GHT traps, found in RPCS3: rsxDrawVertex4f sets the buffer's write position from the new
   segment after a segment change but writes at the old one, which leaves a gap of stale words (a
   corner's 1.0 then read as a jump) - so the corners sit in vertex arrays; rsxSetTimeStamp writes its
   argument one word too far (the RSX takes whatever stood there: RPCS3 rejected it) - so the probe times
   with the PPU's clock, waiting on labels, and V0.8 reads the time of plain reports (gfx_report); and
   rsxSetTransferData never ends when both pitches equal the line length (it queues copies until the
   command buffer is full and then waits for ever) - so copies are put together from their three parts. */
#include "rsx_load.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ppu-asm.h>
#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/rsx_program.h>
#include <sys/process.h>
#include <sys/systime.h>

#include "bench_log.h"
#include "err_info.h"
#include "gfx.h"
#include "pass_vpo.h"
#include "rsx_pace.h"
#include "ui_vpo.h"
#include "vload_vpo.h"

/* labels 0-63 belong to the system, gfx.c has 253-255, ui.c 245-246 */
#define LABEL_COPY      242       /* V1.0: the last slice of the copy target copied to main memory */
#define LABEL_VCHECK    243      /* V0.9: the last vertex check tile copied to main memory */
#define LABEL_VRAM      244       /* the last VRAM slice copied to main memory */
#define LABEL_FCHECK    247       /* the last fill check tile copied to main memory */
#define LABEL_CMD       248       /* the self-test's: the command reader got there */
#define LABEL_RSXSYNC   249
#define LABEL_LOAD      250       /* the last frame whose load the RSX finished */
#define LABEL_CHECK     251       /* the last shader check tile copied to main memory */
#define LABEL_PROBE     252

#define MADS            256       /* four-float multiply-adds a pixel, every layout's long program */
#define MADS_SHORT      64        /* the probe's second program */
#define CHECK_EVERY     4         /* pictures */
#define CHECK_SLOTS     8         /* check tiles of each kind in main memory, used in turn */
#define TILES_MAX       256
#define RING            64        /* frames in flight, far more than ever are */
#define LOG_ERRORS      20
#define WORDS_MAX       4096      /* a program's words */
#define FILL_MAX        64        /* layers a frame (the vertex buffer has as many quads) */
#define FILL_CHECK_LAYERS 8
#define FILL_VARIANTS   5         /* V0.9: fill programs with more and more multiply-adds a pixel */
#define FILL_SHARE      0.75      /* the share of the load's time on the fill (V0.9: it computes too) */
#define TILE_INDEX      1         /* V0.9: the tile region of the fill's target (gcmSetTile) */
#define VRAM_STOCK_MHZ  650.0     /* the GDDR3's clock on a stock PS3 */
#define VRAM_STOCK_GBS  0.0       /* the plain copy alone (linear target) on a stock PS3; 0: not measured yet */
#define COPY_VARIANTS   8         /* V1.0: the copy plain, then with more and more multiply-adds a pixel */
#define COPY_KEEP       0.95      /* V1.0: the RSX + VRAM load's copy keeps at least this share of the plain one's rate */
#define COPY_SLICES     2         /* V1.0: slices of the copy target read back a picture */
#define CSLOTS          64        /* their slots in main memory, used in turn */
#define CDELAY          (VDELAY * COPY_SLICES)   /* slices before one is read (RPCS3 lands copies late) */
#define COPY_HOST       (12u << 20)  /* main memory 12-16 MB on (gfx_host_mem: capture buffers 1 and 2, unused) */
#define COPY_W          RSXLOAD_COPY
#define ROWS_C_MAX      (64 * COPY_W)   /* copy rows a picture: 64 passes, far more than ever fit */
#define VLOAD_N         4096      /* V0.9: vertices in a batch of the vertex load (points, culled) */
#define VCHECK_N        (RSXLOAD_CHECK * RSXLOAD_CHECK)   /* the vertex check: a point a pixel */
#define VMADS           256       /* four-float multiply-adds a vertex (shaders/vload.vasm) */
#define VHOST           (7u << 20)   /* main memory 7-7.5 MB on: the vertex check tiles (gfx_host_mem) */
#define FRAME_LOAD      0.88      /* without time stamps: the share of a picture the load gets by the probe */
#define UI_RESERVE_US   1500.0    /* without time stamps: the RSX's time for the interface, kept free as well */
#define CALM_PICTURES   300       /* without time stamps: 10 s without a late picture - a step back up */
#define ROWS_S_MAX      (TILES_MAX * RSXLOAD_TILE)       /* shader rows a picture */
#define EVAL_LAG        3         /* a flip's time stamps are read three flips later: all written by then */
#define CRING           32        /* flips whose plan is kept for that */
#define TILE_WORDS      (RSXLOAD_CHECK * RSXLOAD_CHECK)            /* a check tile = a VRAM slice: 64 KB */
#define SLICES          (RSXLOAD_TEX * RSXLOAD_TEX / TILE_WORDS)   /* 256 slices of 8 texture rows */
#define VSLOTS          32        /* VRAM slices in main memory, used in turn */
#define VDELAY          24        /* pictures before a slice is read: RPCS3 lands copies up to 0.3 s late */
#define VRAM_HOST       (5u << 20)   /* main memory 5-7 MB on (gfx_host_mem) */

#define OP_MOV  1
#define OP_MUL  2
#define OP_ADD  3
#define OP_MAD  4
#define OP_DP4  6
#define OP_FRC  16
#define OP_TEX  23
#define T_TEMP  0
#define T_IN    1
#define T_CONST 2
#define ATTR_WPOS 0
#define ATTR_COL0 1
#define ATTR_TEX0 4
#define SW(x, y, z, w)  ((x) | (y) << 2 | (z) << 4 | (w) << 6)
#define XYZW    SW(0, 1, 2, 3)
#define YXWZ    SW(1, 0, 3, 2)

/* the program's numbers: positions (up to 256) scaled into [0.5, 2], the rotations' factors (the VMX
   kernel's), and the weights that fold the four lanes into one float for the check tile */
static const float K[4][4] = { { 0.0039f, 0.0041f, 0.0037f, 0.0043f }, { 0.0042f, 0.0038f, 0.0044f, 0.0036f },
                               { 0.0040f, 0.0035f, 0.0045f, 0.0039f }, { 0.0037f, 0.0044f, 0.0038f, 0.0041f } };
static const float B[4][4] = { { 0.50f, 0.60f, 0.70f, 0.80f }, { 0.55f, 0.65f, 0.75f, 0.85f },
                               { 0.52f, 0.62f, 0.72f, 0.82f }, { 0.57f, 0.67f, 0.77f, 0.87f } };
static const float A[2][4] = { { 0.0101f, 0.0113f, 0.0127f, 0.0131f }, { 0.0137f, 0.0149f, 0.0151f, 0.0163f } };
/* the one-register layout turns two lanes against each other at once: x += a*y, y -= a*x from the old x
   and y (it grows by sqrt(1 + a*a) a step: 1.3 % over the 256) */
static const float C3[4] = { 0.0101f, -0.0101f, 0.0127f, -0.0127f };
static const float W[4] = { 1.0f, 0.75f, 0.5f, 0.25f };
/* The check tiles are ordinary A8R8G8B8 surfaces (the console's RSX hung on its first draw into a float
   surface, F_X32, which RPCS3 took): the folded value v goes out as the fractions of 32v, 512v, 8192v
   and 131072v - red and green for the PPU to compare, blue and alpha hold v's last bits (a change of
   one unit there moves alpha by 16 steps), so a wrong result still changes the bytes */
static const float SCALE[4] = { 32.0f, 512.0f, 8192.0f, 131072.0f };
/* where each chain starts: R0 from x,y,x,y, R1 from y,x,y,x, R2 from x,x,y,y, R3 from y,y,x,x */
static const int INIT_SWZ[4] = { SW(0, 1, 0, 1), SW(1, 0, 1, 0), SW(0, 0, 1, 1), SW(1, 1, 0, 0) };

/* V0.9: the fill programs' rotation pairs - 2 multiply-adds each a pixel; the probe takes the one that keeps
   both the arithmetic units and the blending busiest */
static const int FILL_PAIRS[FILL_VARIANTS] = { 4, 8, 16, 32, 48 };
/* V1.0: the copy programs' rotation pairs (0: the plain copy) */
static const int COPY_PAIRS[COPY_VARIANTS] = { 0, 2, 4, 6, 8, 12, 16, 24 };
/* V1.0: each mode's share of the load's time on its second part (fill and copy rows; the rest: shader tiles), and
   the copy's share of that part's time */
static const double MODE_FILL_SHARE[RSXMODES] = { FILL_SHARE, 1.0, 0.80 };
static const double MODE_COPY_SHARE[RSXMODES] = { 0.0, 1.0, 0.75 };
static const char *const MODE_NAME[RSXMODES] = { "RSX core", "VRAM", "RSX + VRAM" };

static const char *const VARIANT_NAME[RSXLOAD_VARIANTS] = {
	"4 chains in 6 registers", "2 chains in 3 registers", "2 chains in 2 registers, factors inline",
	"1 chain in 1 register, factors inline"
};
/* the registers each layout says it needs (at least 2, as cgcomp writes) */
static const int VARIANT_REGS[RSXLOAD_VARIANTS] = { 6, 3, 2, 2 };

/* the programs: one colour (self-test, blending alone), the vertex colour (the vertex check), the fill's
   variants, the copy's (V1.0), and each layout long and short */
enum { P_FLAT, P_COLOR, P_FILL0, P_COPY0 = P_FILL0 + FILL_VARIANTS, P_ALU = P_COPY0 + COPY_VARIANTS };
#define P_FILL(i)   (P_FILL0 + (i))
#define P_COPY(i)   (P_COPY0 + (i))
#define PROGS       (P_ALU + 2 * RSXLOAD_VARIANTS)
#define P_LONG(v)   (P_ALU + 2 * (v))
#define P_SHORT(v)  (P_ALU + 2 * (v) + 1)

typedef struct {
	u32 *words;
	int n, insns;
} fp_buf;

/* a fill layer's corner: as the interface's (ui.c), the colour's bytes R, G, B, A */
typedef struct {
	float x, y, u, v;
	uint32_t rgba;
} fvtx;

/* V0.9: a vertex of the vertex load: the seed, the factors, the fold's weights and scales, where it goes
   (every number from the attributes: cgcomp's assembler drops a program's constants) */
typedef struct {
	float pos[4], fac[4], wgt[4], scl[4], to[4];
} vvtx;

/* a kind of check tile: its slots in main memory, its label, its reference */
typedef struct {
	const char *name;
	int label;
	u32 *host, host_offset, target;
	u32 no, verified;
	uint64_t reference;
	u32 ref[TILE_WORDS], start[TILE_WORDS];
	int rebase, logged, have_start;
	volatile uint64_t *checks, *errors;
	int pat;   /* V1.0 (6th build): its error pattern (pat[]) */
} checker;

/* V1.0 (6th build): what the wrong words of one kind of check have in common over a stress test - which bits differ,
   0 -> 1 or 1 -> 0, which word of a 16-byte memory word they sit in (the GDDR3 bus is 128 bits wide, the checked
   blocks start on 16 bytes: a place that keeps coming back points at the same data lines), how many came back
   00000000 (not drawn). The stress test logs it at its end */
typedef struct {
	const char *name;
	uint32_t blocks, skipped, words, single, zero, up, down;
	uint32_t bit[32], lane[4];
} errpat;

#define PAT_BLOCKS 256   /* tiles or slices a test looks into; an error burst past them would only cost the PPU */

enum { PAT_SHADER, PAT_FILL, PAT_VERTEX, PAT_VRAM, PAT_COPY, PATS };
static errpat pat[PATS] = { { .name = "RSX shader check tiles" }, { .name = "RSX fill check tiles" },
                            { .name = "RSX vertex check tiles" }, { .name = "VRAM check (the texture)" },
                            { .name = "VRAM copy check" } };

static rsxload_status st;
static rsxFragmentProgram fp_head[PROGS];
static u32 fp_offset[PROGS];
static const rsxVertexProgram *vp, *fvp, *vlp;
static void *vp_ucode, *fvp_ucode, *vlp_ucode;
static u32 tile_offset, depth_offset, quad_offset;
static u32 fill_offset, fill_depth_offset, tex_offset, fvb_offset;
/* V0.9: the fill's target in a tile region (as games keep theirs), its pitch; the one in use and its
   program; the vertex load's buffers */
static u32 tfill_offset, tfill_depth_offset, tfill_pitch;
static int fill_tiled, fill_prog = P_FILL(0), fill_best = P_FILL(0);
static int fill_implausible;    /* V1.0: the fill was timed, impossibly fast for a PS3 (an emulator) */
/* V1.0: the copy - its two targets (linear, tiled) and their pitches, the depth buffer under both (never read),
   the corners of both ways round, the texture's linear header; the target and program in use, the share of the
   second part's rows that are copy rows; its slots in main memory, slices sent and read */
static u32 copy_offset[2], copy_pitch[2], copy_depth_offset, cvb_offset, *copy_host, copy_host_offset;
static int copy_have[2], copy_tiled, copy_flip, copy_prog = P_COPY(0), copy_best = P_COPY(0);
static double copy_frac;
static gcmTexture ltex;
static u32 copy_no, copy_seen;
static int copy_logged, run_mode = RSXMODE_CORE, want_mode = RSXMODE_CORE;
static double fill0_row_us;     /* V1.0: a row of the plain fill program on the fill's target (VRAM without the copy) */
static void set_mode(int m, int log);
static u32 vload_offset, vcheck_offset;
static int vert_batches;   /* vertex batches after each fill layer (0: no vertex load) */
static u32 *tex_mem, *vram_host, vram_host_offset;
static uint64_t slice_sum[SLICES];
static gcmTexture ftex;
static u32 frame_no, counted, sync_value = 1, vram_no, vram_seen;
/* each picture's load (by frame_no): rows and draws of the shader and of the fill */
static int ring_rs[RING], ring_rf[RING], ring_rc[RING], ring_fm[RING], ring_cm[RING], ring_ds[RING], ring_df[RING],
           ring_dc[RING], ring_vb[RING], want, calm;
static double ring_b2[RING];   /* V1.0: the second part's time by the probe's timings (0: not timed) */
static double run_share = FILL_SHARE;   /* V1.0: the running mode's share of the load's time on the second part */
static uint32_t seen_late;   /* gfx_late_flips() at the last hook */
static uint64_t last_hook;
/* V0.8: each flip's plan (by gfx_cycle), for reading its time stamps EVAL_LAG flips later */
typedef struct {
	int active, paced, phase, checks;
	double rows_s, rows_f, target;
} flip_plan;
static flip_plan plans[CRING];
static pace pc;
static double idle_sum[3];   /* the screen alone: waits at the flip, at the wait, the drawing (us) */
static int idle_n;
static checker chk_alu = { .name = "shader", .label = LABEL_CHECK, .pat = PAT_SHADER },
               chk_fill = { .name = "fill", .label = LABEL_FCHECK, .pat = PAT_FILL },
               chk_vert = { .name = "vertex", .label = LABEL_VCHECK, .pat = PAT_VERTEX };
static u32 at_draw, at_wait, at_copy, at_label;   /* where the last check tile's commands went, for the log */
static int vram_logged;
static uint64_t zero_sum;   /* the checksum of a tile of zeros: a slot not written yet */

/* timings of each layout in the probe */
static double var_load_gmad[RSXLOAD_VARIANTS], var_tile_us[RSXLOAD_VARIANTS], var_long_us[RSXLOAD_VARIANTS],
              var_short_us[RSXLOAD_VARIANTS];
static int var_tiles[RSXLOAD_VARIANTS];

/* ---- the fragment programs ------------------------------------------------------------------------------ */

/* fp32 result, all four components; attr: the input attribute its T_IN sources read */
static u32 op_dst_a(int op, int reg, int end, int attr)
{
	return (u32)end | (u32)reg << 1 | 0xFu << 9 | (u32)attr << 13 | (u32)op << 24;
}

static u32 op_dst(int op, int reg, int end)
{
	return op_dst_a(op, reg, end, ATTR_WPOS);
}

static u32 op_src(int type, int reg, int swz, int neg)
{
	return (u32)type | (u32)reg << 2 | (u32)swz << 9 | (u32)neg << 17;
}

/* the first source also says when the instruction runs: always (and the condition's swizzle) */
static u32 op_src0(int type, int reg, int swz, int neg)
{
	return op_src(type, reg, swz, neg) | 7u << 18 | (u32)XYZW << 21;
}

static void put(fp_buf *b, u32 d, u32 s0, u32 s1, u32 s2, const float *c)
{
	if (b->n + 8 > WORDS_MAX)
		return;
	b->words[b->n++] = d;
	b->words[b->n++] = s0;
	b->words[b->n++] = s1;
	b->words[b->n++] = s2;
	if (c)
		for (int i = 0; i < 4; i++)
			memcpy(&b->words[b->n++], &c[i], 4);
	b->insns++;
}

static u32 rt(int reg, int neg)
{
	return op_src(T_TEMP, reg, XYZW, neg);
}

static u32 rc(void)
{
	return op_src(T_CONST, 0, XYZW, 0);
}

/* chain r from the pixel's position, as chain c of the four */
static void chain_init(fp_buf *b, int r, int c)
{
	put(b, op_dst(OP_MUL, r, 0), op_src0(T_IN, 0, INIT_SWZ[c], 0), rc(), 0, K[c]);
	put(b, op_dst(OP_ADD, r, 0), op_src0(T_TEMP, r, XYZW, 0), rc(), 0, B[c]);
}

/* R0 folded into one float, its fractions out; the end on a plain move, as cgcomp and tiny3d end: no
   constant behind the end mark */
static void fold_end(fp_buf *b)
{
	put(b, op_dst(OP_DP4, 0, 0), op_src0(T_TEMP, 0, XYZW, 0), rc(), 0, W);
	put(b, op_dst(OP_MUL, 0, 0), op_src0(T_TEMP, 0, XYZW, 0), rc(), 0, SCALE);
	put(b, op_dst(OP_FRC, 0, 0), op_src0(T_TEMP, 0, XYZW, 0), 0, 0, NULL);
	put(b, op_dst(OP_MOV, 0, 1), op_src0(T_TEMP, 0, XYZW, 0), 0, 0, NULL);
}

/* the shader in layout v with `mads` four-float multiply-adds a pixel */
static void build_alu(fp_buf *b, int v, int mads)
{
	b->n = b->insns = 0;
	switch (v) {
	case 0:   /* V9's: four chains R0-R3, the factors in R4 and R5 */
		for (int r = 0; r < 4; r++)
			chain_init(b, r, r);
		put(b, op_dst(OP_MOV, 4, 0), op_src0(T_CONST, 0, XYZW, 0), 0, 0, A[0]);
		put(b, op_dst(OP_MOV, 5, 0), op_src0(T_CONST, 0, XYZW, 0), 0, 0, A[1]);
		for (int g = 0; g < mads / 4; g++) {
			put(b, op_dst(OP_MAD, 0, 0), op_src0(T_TEMP, 1, XYZW, 0), rt(4, 0), rt(0, 0), NULL);   /* x += a*y */
			put(b, op_dst(OP_MAD, 1, 0), op_src0(T_TEMP, 0, XYZW, 1), rt(4, 0), rt(1, 0), NULL);   /* y -= a*x */
			put(b, op_dst(OP_MAD, 2, 0), op_src0(T_TEMP, 3, XYZW, 0), rt(5, 0), rt(2, 0), NULL);
			put(b, op_dst(OP_MAD, 3, 0), op_src0(T_TEMP, 2, XYZW, 1), rt(5, 0), rt(3, 0), NULL);
		}
		put(b, op_dst(OP_ADD, 0, 0), op_src0(T_TEMP, 0, XYZW, 0), rt(1, 0), 0, NULL);
		put(b, op_dst(OP_ADD, 2, 0), op_src0(T_TEMP, 2, XYZW, 0), rt(3, 0), 0, NULL);
		put(b, op_dst(OP_ADD, 0, 0), op_src0(T_TEMP, 0, XYZW, 0), rt(2, 0), 0, NULL);
		break;
	case 1:   /* two chains R0, R1, the factor in R2 */
		chain_init(b, 0, 0);
		chain_init(b, 1, 1);
		put(b, op_dst(OP_MOV, 2, 0), op_src0(T_CONST, 0, XYZW, 0), 0, 0, A[0]);
		for (int g = 0; g < mads / 2; g++) {
			put(b, op_dst(OP_MAD, 0, 0), op_src0(T_TEMP, 1, XYZW, 0), rt(2, 0), rt(0, 0), NULL);
			put(b, op_dst(OP_MAD, 1, 0), op_src0(T_TEMP, 0, XYZW, 1), rt(2, 0), rt(1, 0), NULL);
		}
		put(b, op_dst(OP_ADD, 0, 0), op_src0(T_TEMP, 0, XYZW, 0), rt(1, 0), 0, NULL);
		break;
	case 2:   /* two chains R0, R1, the factor inline in every multiply-add */
		chain_init(b, 0, 0);
		chain_init(b, 1, 1);
		for (int g = 0; g < mads / 2; g++) {
			put(b, op_dst(OP_MAD, 0, 0), op_src0(T_TEMP, 1, XYZW, 0), rc(), rt(0, 0), A[0]);
			put(b, op_dst(OP_MAD, 1, 0), op_src0(T_TEMP, 0, XYZW, 1), rc(), rt(1, 0), A[0]);
		}
		put(b, op_dst(OP_ADD, 0, 0), op_src0(T_TEMP, 0, XYZW, 0), rt(1, 0), 0, NULL);
		break;
	default:  /* one chain R0, its lanes turned against each other, the factors inline */
		chain_init(b, 0, 0);
		for (int i = 0; i < mads; i++)
			put(b, op_dst(OP_MAD, 0, 0), op_src0(T_TEMP, 0, YXWZ, 0), rc(), rt(0, 0), C3);
		break;
	}
	fold_end(b);
}

/* the self-test's colour: one constant into R0 */
static void build_flat(fp_buf *b)
{
	static const float C[4] = { 0.25f, 0.5f, 0.75f, 1.0f };

	b->n = b->insns = 0;
	put(b, op_dst(OP_MOV, 0, 0), op_src0(T_CONST, 0, XYZW, 0), 0, 0, C);
	put(b, op_dst(OP_MOV, 0, 1), op_src0(T_TEMP, 0, XYZW, 0), 0, 0, NULL);
}

/* A fill layer's pixel: the texture at the layer's coordinate, then once more half the texture away and
   nudged by the first colour (up to 8 texels), a few rotations of the two, and out times the layer's colour
   (its alpha blends it). Both reads walk along the texture's rows, as a game's effects read: V0.7's first
   build read the second at swapped coordinates, across the rows (a new 8 KB row every pixel) - slow
   (0.42 G pixels a second) and suspected of a flicker seen on the console */
static void build_fill(fp_buf *b, int pairs)
{
	static const float JIT[4] = { 1.0f / 256.0f, 1.0f / 256.0f, 0.0f, 0.0f };
	static const float AWAY[4] = { 0.5f, 0.375f, 0.0f, 0.0f };
	static const float HALF[4] = { 0.5f, 0.5f, 0.5f, 0.5f };

	b->n = b->insns = 0;
	put(b, op_dst_a(OP_TEX, 0, 0, ATTR_TEX0), op_src0(T_IN, 0, XYZW, 0), 0, 0, NULL);
	put(b, op_dst_a(OP_ADD, 1, 0, ATTR_TEX0), op_src0(T_IN, 0, XYZW, 0), rc(), 0, AWAY);
	put(b, op_dst(OP_MAD, 1, 0), op_src0(T_TEMP, 0, XYZW, 0), rc(), rt(1, 0), JIT);
	put(b, op_dst(OP_TEX, 1, 0), op_src0(T_TEMP, 1, XYZW, 0), 0, 0, NULL);
	for (int g = 0; g < pairs; g++) {
		put(b, op_dst(OP_MAD, 0, 0), op_src0(T_TEMP, 1, XYZW, 0), rc(), rt(0, 0), A[0]);
		put(b, op_dst(OP_MAD, 1, 0), op_src0(T_TEMP, 0, XYZW, 1), rc(), rt(1, 0), A[0]);
	}
	put(b, op_dst(OP_ADD, 0, 0), op_src0(T_TEMP, 0, XYZW, 0), rt(1, 0), 0, NULL);
	put(b, op_dst(OP_MUL, 0, 0), op_src0(T_TEMP, 0, XYZW, 0), rc(), 0, HALF);
	put(b, op_dst_a(OP_MUL, 0, 0, ATTR_COL0), op_src0(T_TEMP, 0, XYZW, 0), op_src(T_IN, 0, XYZW, 0), 0, NULL);
	put(b, op_dst(OP_MOV, 0, 1), op_src0(T_TEMP, 0, XYZW, 0), 0, 0, NULL);
}

/* V1.0: the copy's pixel: the texel under it (point sampling, the linear header: texel for texel), out as it
   came. With `pairs`, 2 x pairs multiply-adds on the side, a chain from the texel in R1 whose result is never
   used - the RSX runs every instruction, so the pixel units compute while the texture read waits for the VRAM,
   and the copy stays exact */
static void build_copy(fp_buf *b, int pairs)
{
	b->n = b->insns = 0;
	put(b, op_dst_a(OP_TEX, 0, 0, ATTR_TEX0), op_src0(T_IN, 0, XYZW, 0), 0, 0, NULL);
	for (int i = 0; i < 2 * pairs; i++)
		put(b, op_dst(OP_MAD, 1, 0), op_src0(T_TEMP, i ? 1 : 0, YXWZ, 0), rc(), rt(i ? 1 : 0, 0), C3);
	put(b, op_dst(OP_MOV, 0, 1), op_src0(T_TEMP, 0, XYZW, 0), 0, 0, NULL);
}

/* V0.9: the vertex check's pixels: the colour the vertex program gave the point */
static void build_color(fp_buf *b)
{
	b->n = b->insns = 0;
	put(b, op_dst_a(OP_MOV, 0, 1, ATTR_COL0), op_src0(T_IN, 0, XYZW, 0), 0, 0, NULL);
}

/* the vertex program (shaders/vload.vasm) on the PPU for one vertex: four chains, each turning its lanes
   against each other 64 times, summed, folded */
static float ppu_vertex(const vvtx *v)
{
	static const int SWAP[4] = { 1, 0, 3, 2 }, INIT[4][4] = { { 0, 1, 2, 3 }, { 1, 0, 3, 2 }, { 2, 3, 0, 1 },
	                                                          { 3, 2, 1, 0 } };
	float r[4][4], s = 0.0f;

	for (int c = 0; c < 4; c++)
		for (int l = 0; l < 4; l++)
			r[c][l] = v->pos[INIT[c][l]];
	for (int i = 0; i < VMADS / 4; i++)
		for (int c = 0; c < 4; c++) {
			float n[4];

			for (int l = 0; l < 4; l++)
				n[l] = r[c][SWAP[l]] * v->fac[l] + r[c][l];
			memcpy(r[c], n, sizeof(n));
		}
	for (int l = 0; l < 4; l++)
		s += ((r[0][l] + r[1][l]) + (r[2][l] + r[3][l])) * v->wgt[l];
	return s;
}

/* layout v on the PPU for one pixel, to see that the RSX runs what is meant (not bit for bit: the RSX
   need not round like the PPU) */
static float ppu_pixel(int v, float x, float y, int mads)
{
	float p[4] = { x, y, 0.0f, 1.0f }, r[4][4], s = 0.0f;
	int chains = v == 0 ? 4 : v == 3 ? 1 : 2;

	for (int c = 0; c < chains; c++)
		for (int l = 0; l < 4; l++)
			r[c][l] = p[(INIT_SWZ[c] >> (2 * l)) & 3] * K[c][l] + B[c][l];
	if (v == 0) {
		for (int g = 0; g < mads / 4; g++)
			for (int l = 0; l < 4; l++) {
				r[0][l] = r[1][l] * A[0][l] + r[0][l];
				r[1][l] = -r[0][l] * A[0][l] + r[1][l];
				r[2][l] = r[3][l] * A[1][l] + r[2][l];
				r[3][l] = -r[2][l] * A[1][l] + r[3][l];
			}
		for (int l = 0; l < 4; l++)
			s += (r[0][l] + r[1][l] + (r[2][l] + r[3][l])) * W[l];
	} else if (v == 3) {
		static const int SWAP[4] = { 1, 0, 3, 2 };

		for (int i = 0; i < mads; i++) {
			float n[4];

			for (int l = 0; l < 4; l++)
				n[l] = r[0][SWAP[l]] * C3[l] + r[0][l];
			memcpy(r[0], n, sizeof(n));
		}
		for (int l = 0; l < 4; l++)
			s += r[0][l] * W[l];
	} else {
		for (int g = 0; g < mads / 2; g++)
			for (int l = 0; l < 4; l++) {
				r[0][l] = r[1][l] * A[0][l] + r[0][l];
				r[1][l] = -r[0][l] * A[0][l] + r[1][l];
			}
		for (int l = 0; l < 4; l++)
			s += (r[0][l] + r[1][l]) * W[l];
	}
	return s;
}

const char *rsxload_variant_name(int v)
{
	return v >= 0 && v < RSXLOAD_VARIANTS ? VARIANT_NAME[v] : "?";
}

const char *rsxload_mode_name(int mode)
{
	return mode >= 0 && mode < RSXMODES ? MODE_NAME[mode] : "?";
}

/* ---- drawing ------------------------------------------------------------------------------------------ */

/* the shader tiles' state */
static void state_common(gcmContextData *ctx)
{
	rsxSetColorMask(ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(ctx, 0);
	rsxSetDepthTestEnable(ctx, GCM_FALSE);
	rsxSetDepthWriteEnable(ctx, GCM_FALSE);
	rsxSetStencilTestEnable(ctx, GCM_FALSE);
	rsxSetBlendEnable(ctx, GCM_FALSE);
	rsxSetAlphaTestEnable(ctx, GCM_FALSE);
	rsxSetCullFaceEnable(ctx, GCM_FALSE);
	rsxSetDitherEnable(ctx, GCM_FALSE);
	rsxSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
	rsxSetUserClipPlaneControl(ctx, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
	                           GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
	                           GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
	rsxSetZMinMaxControl(ctx, GCM_FALSE, GCM_TRUE, GCM_FALSE);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0, quad_offset, 16, 4, GCM_VERTEX_DATA_TYPE_F32,
	                         GCM_LOCATION_RSX);
	/* colour and texture coordinate (the interface's, the fill's) off: this vertex program reads the
	   position only */
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX0, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX1, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX2, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	gfx_load_vp(ctx, vp, vp_ucode);
}

/* a w x h surface as the target, with its depth buffer (never read: depth test and writes are off) */
static void set_surface(gcmContextData *ctx, u32 offset, u32 pitch, int w, int h, u8 format, u32 zoffset, u32 zpitch)
{
	gcmSurface sf;
	f32 scale[4] = { w * 0.5f, h * 0.5f, 0.5f, 0.0f }, shift[4] = { w * 0.5f, h * 0.5f, 0.5f, 0.0f };

	memset(&sf, 0, sizeof(sf));
	sf.colorFormat = format;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0] = offset;
	sf.colorPitch[0] = pitch;
	for (int i = 1; i < 4; i++) {
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorOffset[i] = 0;
		sf.colorPitch[i] = 64;
	}
	sf.depthFormat = GCM_SURFACE_ZETA_Z16;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = zoffset;
	sf.depthPitch = zpitch;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = (u16)w;
	sf.height = (u16)h;
	sf.x = 0;
	sf.y = 0;
	rsxSetSurface(ctx, &sf);
	rsxSetViewport(ctx, 0, 0, (u16)w, (u16)h, 0.0f, 1.0f, scale, shift);
	for (int i = 0; i < 8; i++)
		rsxSetViewportClip(ctx, (u8)i, (u16)w, (u16)h);
	rsxSetScissor(ctx, 0, 0, (u16)w, (u16)h);
}

static void set_target(gcmContextData *ctx, u32 offset, int size, u8 format)
{
	set_surface(ctx, offset, (u32)size * 4, size, size, format, depth_offset, RSXLOAD_TILE * 2);
}

/* one quad over the whole target, its four corners from the vertex array */
static void quad(gcmContextData *ctx)
{
	rsxDrawVertexArray(ctx, GCM_TYPE_QUADS, 0, 4);
}

static void load_fp(gcmContextData *ctx, int p)
{
	rsxLoadFragmentProgramLocation(ctx, &fp_head[p], fp_offset[p], GCM_LOCATION_RSX);
}

/* the fill's state: blending, the interface's vertex program over the layers' quads, the texture */
static void fill_state(gcmContextData *ctx)
{
	rsxSetBlendEnable(ctx, GCM_TRUE);
	rsxSetBlendFunc(ctx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_ONE, GCM_ONE_MINUS_SRC_ALPHA);
	rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
	gfx_load_vp(ctx, fvp, fvp_ucode);
	load_fp(ctx, fill_prog);
	rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
	rsxLoadTexture(ctx, 0, &ftex);
	rsxTextureControl(ctx, 0, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(ctx, 0, 0, GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(ctx, 0, GCM_TEXTURE_REPEAT, GCM_TEXTURE_REPEAT, GCM_TEXTURE_REPEAT, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0, fvb_offset, sizeof(fvtx), 2, GCM_VERTEX_DATA_TYPE_F32,
	                         GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX0, 0, fvb_offset + 8, sizeof(fvtx), 2, GCM_VERTEX_DATA_TYPE_F32,
	                         GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0, fvb_offset + 16, sizeof(fvtx), 4,
	                         GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX1, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX2, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxInvalidateVertexCache(ctx);
}

/* V0.9: the fill's 1920x1080 target - linear (V0.7) or in the tile region */
static void fill_target(gcmContextData *ctx, int tiled)
{
	if (tiled)
		set_surface(ctx, tfill_offset, tfill_pitch, RSXLOAD_FILL_W, RSXLOAD_FILL_H, GCM_SURFACE_A8R8G8B8,
		            tfill_depth_offset, tfill_pitch);
	else
		set_surface(ctx, fill_offset, RSXLOAD_FILL_W * 4, RSXLOAD_FILL_W, RSXLOAD_FILL_H, GCM_SURFACE_A8R8G8B8,
		            fill_depth_offset, RSXLOAD_FILL_W * 2);
}

/* V0.9: the vertex load's state: its program, a vertex's five attributes from buffer `offset`, points of one
   pixel, the vertex colour out */
static void vert_state(gcmContextData *ctx, u32 offset)
{
	rsxSetBlendEnable(ctx, GCM_FALSE);
	gfx_load_vp(ctx, vlp, vlp_ucode);
	load_fp(ctx, P_COLOR);
	rsxSetPointSize(ctx, 1.0f);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0, offset, sizeof(vvtx), 4, GCM_VERTEX_DATA_TYPE_F32,
	                         GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX0, 0, offset + 16, sizeof(vvtx), 4, GCM_VERTEX_DATA_TYPE_F32,
	                         GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX1, 0, offset + 32, sizeof(vvtx), 4, GCM_VERTEX_DATA_TYPE_F32,
	                         GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX2, 0, offset + 48, sizeof(vvtx), 4, GCM_VERTEX_DATA_TYPE_F32,
	                         GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0, offset + 64, sizeof(vvtx), 4, GCM_VERTEX_DATA_TYPE_F32,
	                         GCM_LOCATION_RSX);
	rsxInvalidateVertexCache(ctx);
}

/* n batches of the vertex load: VLOAD_N points each, every one behind the eye (culled) - the vertex units'
   work, no pixels */
static void vert_batches_draw(gcmContextData *ctx, int n)
{
	if (n <= 0)
		return;
	vert_state(ctx, vload_offset);
	for (int i = 0; i < n; i++)
		rsxDrawVertexArray(ctx, GCM_TYPE_POINTS, 0, VLOAD_N);
}

/* layer i (of the FILL_MAX in the vertex buffer), a draw call of its own */
static void layer(gcmContextData *ctx, int i)
{
	rsxDrawVertexArray(ctx, GCM_TYPE_QUADS, (u32)(i % FILL_MAX) * 4, 4);
}

/* n whole layers (the probe) */
static void fill_layers(gcmContextData *ctx, int n, int batches)
{
	fill_state(ctx);
	fill_target(ctx, fill_tiled);
	for (int i = 0; i < n; i++) {
		layer(ctx, i);
		if (batches) {   /* V0.9: the vertex units' work behind each layer, while its pixels are drawn */
			vert_batches_draw(ctx, batches);
			fill_state(ctx);
		}
	}
}

/* V0.8: the load in rows, so it can be set to a few microseconds. `rows` fill rows of program prog: whole
   layers one after the other from layer *l on, the last one cut by the scissor to the rows left; returns
   the draws */
static int fill_run(gcmContextData *ctx, int prog, int rows, int *l, int *batches)
{
	int draws = 0, was = fill_prog;

	if (rows <= 0)
		return 0;
	fill_prog = prog;
	fill_state(ctx);
	fill_target(ctx, fill_tiled);
	for (; rows > 0; (*l)++, draws++) {
		int h = rows < RSXLOAD_FILL_H ? rows : RSXLOAD_FILL_H;
		/* V0.9: behind each layer its vertex batches (a part layer: its share of them), while its pixels are
		   drawn - the pacing learns the time of a row with them */
		int vb = run_mode == RSXMODE_VRAM ? 0 : (vert_batches * h + RSXLOAD_FILL_H / 2) / RSXLOAD_FILL_H;

		if (h < RSXLOAD_FILL_H)
			rsxSetScissor(ctx, 0, 0, RSXLOAD_FILL_W, (u16)h);
		layer(ctx, *l);
		rows -= h;
		if (vb > 0) {
			vert_batches_draw(ctx, vb);
			*batches += vb;
			if (rows > 0)
				fill_state(ctx);
		}
	}
	rsxSetScissor(ctx, 0, 0, RSXLOAD_FILL_W, RSXLOAD_FILL_H);
	fill_prog = was;
	return draws;
}

/* V1.0: the copy's state: no blending, the interface's vertex program over one quad (texel centres on pixel
   centres; `flip`: the texture's rows the other way round), program p, the texture through its linear header
   with point sampling, the target (tiled or linear) */
static void copy_state(gcmContextData *ctx, int p, int tiled, int flip)
{
	u32 vb = cvb_offset + (u32)(flip ? 4 : 0) * sizeof(fvtx);

	rsxSetBlendEnable(ctx, GCM_FALSE);
	gfx_load_vp(ctx, fvp, fvp_ucode);
	load_fp(ctx, p);
	rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
	rsxLoadTexture(ctx, 0, &ltex);
	rsxTextureControl(ctx, 0, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(ctx, 0, 0, GCM_TEXTURE_NEAREST, GCM_TEXTURE_NEAREST, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(ctx, 0, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, 0,
	                   GCM_TEXTURE_ZFUNC_LESS, 0);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0, vb, sizeof(fvtx), 2, GCM_VERTEX_DATA_TYPE_F32,
	                         GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX0, 0, vb + 8, sizeof(fvtx), 2, GCM_VERTEX_DATA_TYPE_F32,
	                         GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0, vb + 16, sizeof(fvtx), 4, GCM_VERTEX_DATA_TYPE_U8,
	                         GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX1, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX2, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxInvalidateVertexCache(ctx);
	set_surface(ctx, copy_offset[tiled], copy_pitch[tiled], COPY_W, COPY_W, GCM_SURFACE_A8R8G8B8, copy_depth_offset,
	            COPY_W * 2);
}

/* V1.0: `rows` copy rows with program p: whole passes (the 16 MB each), the last one cut by the scissor; returns
   the passes (draws) */
static int copy_run(gcmContextData *ctx, int p, int rows)
{
	int draws = 0;

	if (rows <= 0)
		return 0;
	copy_state(ctx, p, copy_tiled, copy_flip);
	for (; rows > 0; draws++) {
		int h = rows < COPY_W ? rows : COPY_W;

		if (h < COPY_W)
			rsxSetScissor(ctx, 0, 0, COPY_W, (u16)h);
		rsxDrawVertexArray(ctx, GCM_TYPE_QUADS, 0, 4);
		rows -= h;
	}
	rsxSetScissor(ctx, 0, 0, COPY_W, COPY_W);
	return draws;
}

/* V1.0: a picture's second part - the fill rows (fill_prog), then the copy rows (copy_prog, a copy_frac share of
   the rows); *crows, *cdraws: the copy's rows and passes; returns the fill's draws */
static int part2_rows(gcmContextData *ctx, int rows, int *batches, int *crows, int *cdraws)
{
	int l = 0, rc = copy_frac > 0.0 ? (int)(rows * copy_frac + 0.5) : 0, draws;

	*batches = 0;
	*crows = rc;
	draws = fill_run(ctx, fill_prog, rows - rc, &l, batches);
	*cdraws = copy_run(ctx, copy_prog, rc);
	return draws;
}

/* `rows` shader rows: whole tiles, then one cut to the rows left; returns the draws */
static int shader_rows(gcmContextData *ctx, int rows)
{
	int draws = 0;

	for (; rows >= RSXLOAD_TILE; rows -= RSXLOAD_TILE, draws++)
		quad(ctx);
	if (rows > 0) {
		rsxSetScissor(ctx, 0, 0, RSXLOAD_TILE, (u16)rows);
		quad(ctx);
		rsxSetScissor(ctx, 0, 0, RSXLOAD_TILE, RSXLOAD_TILE);
		draws++;
	}
	return draws;
}

static u32 *slot_of(const checker *c, u32 n)
{
	return c->host + (n % CHECK_SLOTS) * TILE_WORDS;
}

/* a check tile's picture into main memory as it is, byte for byte, once everything before is drawn
   (the screenshots' scaled copy treats pixels as colours: RPCS3 converted the floats with it) */
static void copy_check(gcmContextData *ctx, checker *c)
{
	u32 slot = (c->no + 1) % CHECK_SLOTS;

	rsxSetWriteBackendLabel(ctx, LABEL_RSXSYNC, sync_value);
	rsxAddressToOffset(ctx->current, &at_wait);
	rsxSetWaitLabel(ctx, LABEL_RSXSYNC, sync_value);
	sync_value++;
	rsxAddressToOffset(ctx->current, &at_copy);
	rsxSetTransferDataMode(ctx, GCM_TRANSFER_LOCAL_TO_MAIN);
	rsxSetTransferDataFormat(ctx, RSXLOAD_CHECK * 4, RSXLOAD_CHECK * 4, RSXLOAD_CHECK * 4, RSXLOAD_CHECK, 1, 1);
	rsxSetTransferDataOffset(ctx, c->host_offset + slot * TILE_WORDS * 4, c->target);
	c->no++;
	rsxAddressToOffset(ctx->current, &at_label);
	rsxSetWriteBackendLabel(ctx, c->label, c->no);
}

/* the shader's check tile with program p */
static void check_tile(gcmContextData *ctx, int p)
{
	set_target(ctx, chk_alu.target, RSXLOAD_CHECK, GCM_SURFACE_A8R8G8B8);
	load_fp(ctx, p);
	rsxAddressToOffset(ctx->current, &at_draw);
	quad(ctx);
	copy_check(ctx, &chk_alu);
}

/* the fill's check tile: cleared, then the first FILL_CHECK_LAYERS layers blended over each other */
static void fill_check(gcmContextData *ctx)
{
	int was = fill_prog;

	/* V1.0: always the program the check's reference came from - the VRAM load without the copy runs the plain fill,
	   and a test's first check tile could still be drawn with the last test's program */
	fill_prog = fill_best;
	fill_state(ctx);
	fill_prog = was;
	set_target(ctx, chk_fill.target, RSXLOAD_CHECK, GCM_SURFACE_A8R8G8B8);
	rsxSetClearColor(ctx, 0x00000000);
	rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
	gfx_forget_clear_color();
	rsxAddressToOffset(ctx->current, &at_draw);
	for (int i = 0; i < FILL_CHECK_LAYERS; i++)
		layer(ctx, i);
	copy_check(ctx, &chk_fill);
}

/* V0.9: the vertex check tile: cleared, then a point a pixel, each the colour its vertex program folded */
static void vert_check(gcmContextData *ctx)
{
	set_target(ctx, chk_vert.target, RSXLOAD_CHECK, GCM_SURFACE_A8R8G8B8);
	rsxSetClearColor(ctx, 0x00000000);
	rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
	gfx_forget_clear_color();
	vert_state(ctx, vcheck_offset);
	rsxAddressToOffset(ctx->current, &at_draw);
	rsxDrawVertexArray(ctx, GCM_TYPE_POINTS, 0, VCHECK_N);
	copy_check(ctx, &chk_vert);
}

/* 64 KB of the texture (slice n % SLICES: eight rows) into main memory slot n % VSLOTS; the texture
   never changes, so no wait for the drawing */
static void copy_slice(gcmContextData *ctx, u32 n)
{
	rsxSetTransferDataMode(ctx, GCM_TRANSFER_LOCAL_TO_MAIN);
	rsxSetTransferDataFormat(ctx, RSXLOAD_TEX * 4, RSXLOAD_TEX * 4, RSXLOAD_TEX * 4, TILE_WORDS / RSXLOAD_TEX, 1, 1);
	rsxSetTransferDataOffset(ctx, vram_host_offset + (n % VSLOTS) * TILE_WORDS * 4,
	                         tex_offset + (n % SLICES) * TILE_WORDS * 4);
}

static void vram_copy(gcmContextData *ctx)
{
	/* a slot is only filled again once its last copy was read */
	if (!st.vram_ok || vram_no - vram_seen >= VSLOTS - 4)
		return;
	copy_slice(ctx, vram_no);
	vram_no++;
	rsxSetWriteBackendLabel(ctx, LABEL_VRAM, vram_no);
}

/* V1.0: slice s of copy target t (eight of its rows: 64 KB) into main memory slot `slot` */
static void copy_target_slice(gcmContextData *ctx, int t, u32 s, u32 slot)
{
	rsxSetTransferDataMode(ctx, GCM_TRANSFER_LOCAL_TO_MAIN);
	rsxSetTransferDataFormat(ctx, copy_pitch[t], COPY_W * 4, COPY_W * 4, TILE_WORDS / COPY_W, 1, 1);
	rsxSetTransferDataOffset(ctx, copy_host_offset + (slot % CSLOTS) * TILE_WORDS * 4,
	                         copy_offset[t] + (s % SLICES) * (TILE_WORDS / COPY_W) * copy_pitch[t]);
}

/* V1.0: COPY_SLICES slices of the copy target into main memory, the next ones in turn. Every copy writes the same
   bytes, so no wait for the drawing: whatever the slice holds when it is read must be the texture's */
static void copy_readback(gcmContextData *ctx)
{
	for (int i = 0; i < COPY_SLICES && copy_no - copy_seen < CSLOTS - 4; i++) {
		copy_target_slice(ctx, copy_tiled, copy_no, copy_no);
		copy_no++;
	}
	rsxSetWriteBackendLabel(ctx, LABEL_COPY, copy_no);
}

/* ---- checks, labels ------------------------------------------------------------------------------------- */

static uint64_t checksum(const u32 *p)
{
	uint64_t h = 0x9E3779B97F4A7C15ull;

	for (int i = 0; i < TILE_WORDS; i++)
		h = (h ^ p[i]) * 0x100000001B3ull + (uint64_t)i;
	return h;
}

/* the texture's texel i: a hash of its number */
static u32 texel(u32 i)
{
	i ^= i >> 16;
	i *= 0x7FEB352Du;
	i ^= i >> 15;
	i *= 0x846CA68Bu;
	i ^= i >> 16;
	return i;
}

static u32 label(int index)
{
	return *(vu32 *)gcmGetLabelAddress((u8)index);
}

/* where the RSX stands when a wait ran out: its read and write position against the check tile's
   commands, and the labels */
/* V1.0: an RSX that did not finish what it was given at the start: nothing more may go to it (a hung RSX and an
   app that keeps queueing froze the whole console in V9.0) - the probes stop, the app ends */
static int rsx_stuck;

static void log_where(const char *what)
{
	gcmControlRegister volatile *ctrl = gcmGetControlRegister();

	rsx_stuck = 1;

	log_line("RSX probe: %s - the RSX reads at %08x, written up to %08x; the check tile's draw at %08x, wait at %08x, "
	         "copy at %08x, label at %08x; labels sync %u check %u fill %u probe %u load %u", what, (unsigned)ctrl->get,
	         (unsigned)ctrl->put, (unsigned)at_draw, (unsigned)at_wait, (unsigned)at_copy, (unsigned)at_label,
	         (unsigned)label(LABEL_RSXSYNC), (unsigned)label(LABEL_CHECK), (unsigned)label(LABEL_FCHECK),
	         (unsigned)label(LABEL_PROBE), (unsigned)label(LABEL_LOAD));
}

/* by the clock, not by counting sleeps: in RPCS3 a 100 us sleep can take milliseconds */
static int wait_label_value(int index, u32 value, int timeout_ms)
{
	uint64_t start = __gettime(), limit = sysGetTimebaseFrequency() / 1000 * (uint64_t)timeout_ms;

	while ((int32_t)(label(index) - value) < 0) {
		if (__gettime() - start > limit)
			return -1;
		usleep(100);
	}
	return 0;
}

/* the timebase when the label got there, looking without a break; 0 after timeout_ms */
static uint64_t label_time(int index, u32 value, int timeout_ms)
{
	uint64_t start = __gettime(), limit = sysGetTimebaseFrequency() / 1000 * (uint64_t)timeout_ms;

	for (;;) {
		uint64_t now = __gettime();

		if ((int32_t)(label(index) - value) >= 0)
			return now;
		if (now - start > limit)
			return 0;
	}
}

/* main memory written by a copy: on the console it is there with the label, RPCS3 puts it there up to
   1.7 s later. Waits (up to 5 s) until the checksum stops being that of the old content; returns it */
static uint64_t settled_sum(const u32 *p, uint64_t old)
{
	uint64_t start = __gettime(), limit = 5 * sysGetTimebaseFrequency(), sum;

	do {
		__asm__ volatile("sync" ::: "memory");
		sum = checksum(p);
		if (sum != old)
			return sum;
		usleep(1000);
	} while (__gettime() - start < limit);
	return sum;
}

/* V1.0 (6th build): one wrong word into its kind's pattern; `index` is the word's number in a block that starts on
   16 bytes in VRAM */
static void pat_word(errpat *p, u32 index, u32 got, u32 want)
{
	u32 x = got ^ want;

	p->words++;
	p->lane[index & 3]++;
	if (!got)
		p->zero++;
	if (!(x & (x - 1)))
		p->single++;
	for (u32 m = x; m; m &= m - 1) {
		int b = __builtin_ctz(m);

		p->bit[b]++;
		if (got >> b & 1)
			p->up++;
		else
			p->down++;
	}
}

/* a block (tile or slice) with wrong words: 1 when its words go into the pattern, 0 past PAT_BLOCKS */
static int pat_block(errpat *p)
{
	if (p->blocks >= PAT_BLOCKS) {
		p->skipped++;
		return 0;
	}
	p->blocks++;
	return 1;
}

#ifdef ERR_FAKE
/* the test build for RPCS3 (make ERRTEST=1): now and then a wrong bit or a dark patch where the console would show
   one at too high a clock, so the error lines and the patterns can be checked. Main memory, the copies' slots */
static void err_fake(u32 *d, u32 n, int kind)
{
	if (kind == PAT_COPY && n % 61 == 17)
		d[(n * 52) % (TILE_WORDS / 4) * 4] ^= n & 1 ? 0x80000000u : 0x40000000u;   /* bits 31/30, word 0 of 16 bytes */
	else if (kind == PAT_VRAM && n % 89 == 31)
		d[(n * 7) % TILE_WORDS] ^= 0x00000100u;
	else if (kind == PAT_SHADER && n % 97 == 41)
		for (u32 i = 0; i < 300; i++)
			d[i] = 0;   /* a patch not drawn */
}
#endif

/* the check tiles that came back: each against its kind's reference */
static void verify(checker *c)
{
	u32 done = label(c->label);

	/* a check tile is read once the next one is back too: by then its copy has surely finished */
	while ((int32_t)(done - c->verified) > 1) {
		const u32 *d;
		uint64_t sum;

		c->verified++;
		d = slot_of(c, c->verified);
#ifdef ERR_FAKE
		if (!c->rebase && c->reference)
			err_fake((u32 *)d, c->verified, c->pat);
#endif
		sum = checksum(d);
		(*c->checks)++;
		if (c->rebase || !c->reference) {
			/* each test's first check tile is its reference. RPCS3 runs a new shader at first in a way that
			   rounds differently by one unit in the last place; the console runs the same code each time - there
			   the start's tile and this one should be the same */
			int n = 0;

			for (int i = 0; c->have_start && i < TILE_WORDS; i++)
				n += d[i] != c->start[i];
			if (n)
				log_line("RSX %s check tile: the test's first differs from the start's in %d of %d pixels (RPCS3 runs a "
				         "shader differently at first and can copy a surface's old memory; not counted as an error)",
				         c->name, n, TILE_WORDS);
			c->reference = sum;
			memcpy(c->ref, d, sizeof(c->ref));
			c->rebase = 0;
		} else if (sum != c->reference) {
			errpat *p = &pat[c->pat];
			int look = pat_block(p), n = 0, first = -1, last = -1;

			(*c->errors)++;
			st.last_bad = sum;
			if (!look && c->logged >= LOG_ERRORS)
				continue;
			for (int i = 0; i < TILE_WORDS; i++)
				if (d[i] != c->ref[i]) {
					n++;
					last = i;
					if (first < 0)
						first = i;
					if (look)
						pat_word(p, (u32)i, d[i], c->ref[i]);
				}
			if (c->logged++ < LOG_ERRORS) {
				char at[64];

				err_context(at, sizeof(at), 1);
				if (n)
					log_line("ERROR on the RSX: %s check tile %u came back different - %d of %d pixels, the first at (%d, "
					         "%d) %08x instead of %08x, the last at (%d, %d) %08x instead of %08x%s", c->name,
					         (unsigned)c->verified, n, TILE_WORDS, first % RSXLOAD_CHECK, first / RSXLOAD_CHECK,
					         (unsigned)d[first], (unsigned)c->ref[first], last % RSXLOAD_CHECK, last / RSXLOAD_CHECK,
					         (unsigned)d[last], (unsigned)c->ref[last], at);
				else
					log_line("ERROR on the RSX: %s check tile %u's checksum %016llx differs from the first's %016llx%s",
					         c->name, (unsigned)c->verified, (unsigned long long)sum, (unsigned long long)c->reference, at);
			}
		}
	}
}

/* V1.0 (6th build): a slice that came back wrong - the texture's (VRAM check) or the copy's - against the texture's
   words: into the pattern, and logged while LOG_ERRORS allow */
static void slice_wrong(const u32 *d, u32 s, int kind, int *logged)
{
	errpat *p = &pat[kind];
	int look = pat_block(p), bad = 0, first = -1;
	char at[64];

	if (!look && *logged >= LOG_ERRORS)
		return;
	for (int i = 0; i < TILE_WORDS; i++) {
		u32 want = texel(s * TILE_WORDS + (u32)i);

		if (d[i] != want) {
			bad++;
			if (first < 0)
				first = i;
			if (look)
				pat_word(p, (u32)i, d[i], want);
		}
	}
	if ((*logged)++ >= LOG_ERRORS)
		return;
	err_context(at, sizeof(at), 1);
	if (kind == PAT_VRAM) {
		if (first >= 0)
			log_line("ERROR in the VRAM: slice %u of the texture (rows %u-%u) came back with %d of %d words wrong, the "
			         "first at %u: %08x instead of %08x%s", (unsigned)s, (unsigned)(s * 8), (unsigned)(s * 8 + 7), bad,
			         TILE_WORDS, (unsigned)(s * TILE_WORDS + (u32)first), (unsigned)d[first],
			         (unsigned)texel(s * TILE_WORDS + (u32)first), at);
		else
			log_line("ERROR in the VRAM: slice %u's checksum differed, its words read later were right%s", (unsigned)s, at);
	} else if (first >= 0) {
		log_line("ERROR in the VRAM copy: rows %u-%u of the copy came back with %d of %d pixels wrong, the first at "
		         "(%d, %u): %08x instead of %08x%s", (unsigned)(s * 8), (unsigned)(s * 8 + 7), bad, TILE_WORDS,
		         first % COPY_W, (unsigned)(s * 8 + (u32)first / COPY_W), (unsigned)d[first],
		         (unsigned)texel(s * TILE_WORDS + (u32)first), at);
	} else {
		log_line("ERROR in the VRAM copy: rows %u-%u's checksum differed, the pixels read later were right%s",
		         (unsigned)(s * 8), (unsigned)(s * 8 + 7), at);
	}
}

/* the VRAM slices that came back VDELAY pictures ago: each against the PPU's checksum of what it wrote */
static void verify_vram(void)
{
	u32 done = label(LABEL_VRAM);

	while ((int32_t)(done - vram_seen) > VDELAY) {
		u32 n = vram_seen, s = n % SLICES;
		const u32 *d = vram_host + (n % VSLOTS) * TILE_WORDS;

		vram_seen++;
		st.vram_slices++;
#ifdef ERR_FAKE
		err_fake((u32 *)d, n, PAT_VRAM);
#endif
		if (checksum(d) == slice_sum[s])
			continue;
		st.vram_errors++;
		slice_wrong(d, s, PAT_VRAM, &vram_logged);
	}
}

/* V1.0: the copy target's slices that came back CDELAY slices ago: each against the PPU's checksum of the
   texture's same rows */
static void verify_copy(void)
{
	u32 done = label(LABEL_COPY);

	while ((int32_t)(done - copy_seen) > CDELAY) {
		u32 n = copy_seen, s = n % SLICES;
		const u32 *d = copy_host + (n % CSLOTS) * TILE_WORDS;

		copy_seen++;
		st.copy_slices++;
#ifdef ERR_FAKE
		err_fake((u32 *)d, n, PAT_COPY);
#endif
		if (checksum(d) == slice_sum[s])
			continue;
		st.copy_errors++;
		slice_wrong(d, s, PAT_COPY, &copy_logged);
	}
}

/* V1.0 (6th build): each kind's pattern into the log, then cleared - the stress test's summary calls it at its end */
void rsxload_errpat_log(void)
{
	for (int k = 0; k < PATS; k++) {
		errpat *p = &pat[k];
		const char *name = p->name;
		char bits[96] = "", lanes[64], more[64] = "", same[96] = "";
		int top[4] = { -1, -1, -1, -1 }, nb = 0, lane = 0, pair, one;
		uint32_t flips = p->up + p->down, two;

		if (!p->blocks)
			continue;
		/* the bits that differed most often, up to four */
		for (int t = 0; t < 4; t++) {
			int best = -1;

			for (int b = 0; b < 32; b++)
				if (p->bit[b] && b != top[0] && b != top[1] && b != top[2] && (best < 0 || p->bit[b] > p->bit[best]))
					best = b;
			if (best < 0)
				break;
			top[t] = best;
			nb += snprintf(bits + nb, sizeof(bits) - (size_t)nb, "%s%d x%u", t ? ", " : "", best, (unsigned)p->bit[best]);
		}
		for (int l = 1; l < 4; l++)
			if (p->lane[l] > p->lane[lane])
				lane = l;
		snprintf(lanes, sizeof(lanes), "word 0 x%u, 1 x%u, 2 x%u, 3 x%u", (unsigned)p->lane[0], (unsigned)p->lane[1],
		         (unsigned)p->lane[2], (unsigned)p->lane[3]);
		if (p->skipped)
			snprintf(more, sizeof(more), " (the first %u; %u more not looked into)", (unsigned)p->blocks,
			         (unsigned)p->skipped);
		/* the same place again and again - 9 in 10 wrong words in one word of the 16 bytes, 9 in 10 wrong bits in
		   one or two bit positions - is worth saying in words */
		two = top[0] >= 0 ? p->bit[top[0]] + (top[1] >= 0 ? p->bit[top[1]] : 0) : 0;
		pair = flips && two * 10 >= flips * 9 && top[1] >= 0 && p->bit[top[1]] * 10 >= flips;   /* two bits */
		one = flips && top[0] >= 0 && p->bit[top[0]] * 10 >= flips * 9;                       /* one bit */
		if (p->words >= 3 && p->zero * 10 >= p->words * 9) {
			snprintf(same, sizeof(same), " - nearly all came back 00000000: not drawn, or not written to VRAM");
		} else if (p->words >= 3 && p->lane[lane] * 10 >= p->words * 9) {
			if (pair)
				snprintf(same, sizeof(same), " - nearly all in word %d and bits %d/%d: the same place each time", lane,
				         top[0], top[1]);
			else if (one)
				snprintf(same, sizeof(same), " - nearly all in word %d and bit %d: the same place each time", lane, top[0]);
			else
				snprintf(same, sizeof(same), " - nearly all in word %d of the 16 bytes", lane);
		} else if (p->words >= 3 && pair) {
			snprintf(same, sizeof(same), " - nearly all in bits %d/%d, in any word", top[0], top[1]);
		} else if (p->words >= 3 && one) {
			snprintf(same, sizeof(same), " - nearly all in bit %d, in any word", top[0]);
		}
		log_line("  Error pattern, %s%s: %u wrong words in %u blocks, %u of them single bits, %u came back 00000000; bits "
		         "most often: %s; 0 -> 1 %u times, 1 -> 0 %u times; in a 16-byte memory word: %s%s", name, more,
		         (unsigned)p->words, (unsigned)p->blocks, (unsigned)p->single, (unsigned)p->zero, nb ? bits : "none",
		         (unsigned)p->up, (unsigned)p->down, lanes, same);
		memset(p, 0, sizeof(*p));
		p->name = name;
	}
}

/* ---- set-up and probe ----------------------------------------------------------------------------------- */

/* V1.0 (5th build): the PPU puts anything bigger than a word into the RSX's memory 4 bytes at a time, from a
   copy in main memory. VRAM is cache-inhibited for the PPU, and there an 8-byte load or store that is not on an
   8-byte boundary raises an alignment exception: GCC copied the VRAM copy's 20-byte corners with ld/std at
   offsets 20, 28, 60... and read them back from VRAM the same way - builds 2 to 4 froze the console right there
   (RPCS3 does not mind). `bytes` is a multiple of 4, `dst` 4-byte aligned.
   The source is read with memcpy, as bytes: read through a u32 pointer, its floats broke C's aliasing rule and GCC
   dropped the stores of the fill quads' and the vertices' floats (the 5th build's first try: empty check tiles) */
static void vram_put(void *dst, const void *src, u32 bytes)
{
	volatile u32 *d = (volatile u32 *)dst;
	const unsigned char *s = (const unsigned char *)src;

	for (u32 i = 0; i < bytes / 4; i++) {
		u32 w;

		memcpy(&w, s + 4 * i, 4);
		d[i] = w;
	}
}

static int make_program(int i, const fp_buf *b, int regs, int texcoords)
{
	u32 *mem = (u32 *)rsxMemalign(64, (u32)b->n * 4);

	if (!mem)
		return -1;
	for (int k = 0; k < b->n; k++)   /* the RSX reads each word with its halves swapped */
		mem[k] = b->words[k] << 16 | b->words[k] >> 16;
	rsxAddressToOffset(mem, &fp_offset[i]);
	memset(&fp_head[i], 0, sizeof(fp_head[i]));
	fp_head[i].magic = 0x4650;    /* "FP", as cgcomp writes it */
	fp_head[i].num_regs = (u16)regs;
	fp_head[i].num_insn = (u16)b->insns;
	fp_head[i].fp_control = 0x40; /* what cgcomp sets for a program whose result is R0 */
	fp_head[i].texcoords = (u16)texcoords;
	fp_head[i].texcoord2D = (u16)texcoords;
	return 0;
}

static int make_programs(void)
{
	static u32 words[WORDS_MAX];
	fp_buf b = { words, 0, 0 };

	build_flat(&b);
	if (make_program(P_FLAT, &b, 2, 0) != 0)
		return -1;
	build_color(&b);
	if (make_program(P_COLOR, &b, 2, 0) != 0)
		return -1;
	for (int i = 0; i < FILL_VARIANTS; i++) {
		build_fill(&b, FILL_PAIRS[i]);
		if (make_program(P_FILL(i), &b, 2, 1) != 0)
			return -1;
	}
	for (int i = 0; i < COPY_VARIANTS; i++) {
		build_copy(&b, COPY_PAIRS[i]);
		if (make_program(P_COPY(i), &b, 2, 1) != 0)
			return -1;
	}
	for (int v = 0; v < RSXLOAD_VARIANTS; v++) {
		build_alu(&b, v, MADS);
		if (make_program(P_LONG(v), &b, VARIANT_REGS[v], 0) != 0)
			return -1;
		build_alu(&b, v, MADS_SHORT);
		if (make_program(P_SHORT(v), &b, VARIANT_REGS[v], 0) != 0)
			return -1;
	}
	return 0;
}

/* The fill's buffers: the 1920x1080 target and its depth, the texture (the PPU's hash of each texel's
   number, and every 64 KB slice's checksum for the VRAM check), the layers' quads. 0 when in place */
static int fill_init(void)
{
	void *target = rsxMemalign(4096, RSXLOAD_FILL_W * RSXLOAD_FILL_H * 4);
	void *depth = rsxMemalign(4096, RSXLOAD_FILL_W * RSXLOAD_FILL_H * 2);
	fvtx *vb = (fvtx *)rsxMemalign(128, FILL_MAX * 4 * sizeof(fvtx));
	uint64_t t0 = __gettime();
	const float du = (float)RSXLOAD_FILL_W / RSXLOAD_TEX, dv = (float)RSXLOAD_FILL_H / RSXLOAD_TEX;

	tex_mem = (u32 *)rsxMemalign(4096, RSXLOAD_TEX * RSXLOAD_TEX * 4);
	vram_host = (u32 *)gfx_host_mem(VRAM_HOST, VSLOTS * TILE_WORDS * 4, &vram_host_offset);
	if (!target || !depth || !vb || !tex_mem || !vram_host) {
		log_line("RSX fill: no VRAM or main memory for it - the RSX load runs its shader only");
		return -1;
	}
	rsxAddressToOffset(target, &fill_offset);
	rsxAddressToOffset(depth, &fill_depth_offset);
	rsxAddressToOffset(tex_mem, &tex_offset);
	rsxAddressToOffset(vb, &fvb_offset);
	for (u32 s = 0; s < SLICES; s++) {
		uint64_t h = 0x9E3779B97F4A7C15ull;

		for (u32 j = 0; j < TILE_WORDS; j++) {
			u32 v = texel(s * TILE_WORDS + j);

			tex_mem[s * TILE_WORDS + j] = v;
			h = (h ^ v) * 0x100000001B3ull + (uint64_t)j;
		}
		slice_sum[s] = h;
	}
	/* each layer's quad over the whole target, about a texel a pixel, at its own place in the texture,
	   in its own colour, two thirds opaque on average */
	for (int i = 0; i < FILL_MAX; i++) {
		float u0 = (float)i * 0.137f, v0 = (float)i * 0.291f;
		uint32_t a = 0x80 + (uint32_t)(i * 29 % 0x60);
		uint32_t rgba = (0xC0u + (uint32_t)(i * 37 % 0x40)) << 24 | (0xA0u + (uint32_t)(i * 53 % 0x60)) << 16 |
		                (0x90u + (uint32_t)(i * 71 % 0x70)) << 8 | a;

		u0 -= floorf(u0);
		v0 -= floorf(v0);
		{
			const fvtx q[4] = { { -1.0f, -1.0f, u0, v0 + dv, rgba }, { 1.0f, -1.0f, u0 + du, v0 + dv, rgba },
			                    { 1.0f, 1.0f, u0 + du, v0, rgba }, { -1.0f, 1.0f, u0, v0, rgba } };

			vram_put(&vb[i * 4], q, sizeof(q));
		}
	}
	/* V0.8: swizzled - the RSX reads the same bytes as small squares of texels, as games keep their textures
	   (V0.7's linear one: 0.45 G pixels a second, its reads a new row of the texture in memory each time).
	   The bytes stay the PPU's hash of their place, so the VRAM check is the same */
	memset(&ftex, 0, sizeof(ftex));
	ftex.format = GCM_TEXTURE_FORMAT_A8R8G8B8;
	ftex.mipmap = 1;
	ftex.dimension = GCM_TEXTURE_DIMS_2D;
	ftex.cubemap = GCM_FALSE;
	ftex.remap = (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
	             (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
	             (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
	             (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
	             (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
	             (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
	             (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
	             (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT);
	ftex.width = RSXLOAD_TEX;
	ftex.height = RSXLOAD_TEX;
	ftex.depth = 1;
	ftex.location = GCM_LOCATION_RSX;
	ftex.pitch = RSXLOAD_TEX * 4;
	ftex.offset = tex_offset;
	log_line("RSX fill: texture of %d MB written in %.0f ms", RSXLOAD_TEX * RSXLOAD_TEX * 4 >> 20,
	         (double)(__gettime() - t0) * 1000.0 / (double)sysGetTimebaseFrequency());
	return 0;
}

/* V0.9: the fill's target once more, in a tile region - the RSX's memory controller lays it out in tiles,
   as games keep their render targets (V0.8's linear one: 0.53 G pixels a second). 0 when in place */
static int tiled_init(void)
{
	u32 pitch = gcmGetTiledPitchSize(RSXLOAD_FILL_W * 4), size;
	void *t, *d;

	if (pitch < RSXLOAD_FILL_W * 4 || pitch > 0x10000) {
		log_line("RSX fill: no tiled pitch for %d bytes a row (%u) - the target stays linear", RSXLOAD_FILL_W * 4,
		         (unsigned)pitch);
		return -1;
	}
	/* V1.0 (5th build): whole rows of tiles. The RSX lays a tile region out in tiles of 256 bytes x 64 rows, four to
	   a 64 KB block; 1080 rows are 16.9 tile rows, and the last one's right tiles (x 1792-1919, rows 1024-1079) fell
	   into a 136th block where the region had 135 - up to 47 KB past its end, onto the fill's texture that lies
	   right above: on a PS3 (4th build) its first 2 KB came back wrong now and then, and the fill check tiles were
	   1 bit off where they read those texels (RPCS3's tiling, rsx/Common/tiled_dma_copy.hpp, drops such writes).
	   1088 rows: 136 blocks, every pixel inside the region and the allocation */
	size = (pitch * ((RSXLOAD_FILL_H + 63) / 64 * 64) + 0xFFFFu) & ~0xFFFFu;   /* tile regions come in 64 KB */
	t = rsxMemalign(0x10000, size);
	d = rsxMemalign(0x10000, size);
	if (!t || !d) {
		log_line("RSX fill: no VRAM for a tiled target - it stays linear");
		return -1;
	}
	rsxAddressToOffset(t, &tfill_offset);
	rsxAddressToOffset(d, &tfill_depth_offset);
	gcmSetTile(TILE_INDEX, GCM_LOCATION_RSX, tfill_offset, size, pitch, GCM_COMPMODE_DISABLED, 0, 0);
	tfill_pitch = pitch;
	log_line("RSX fill: a second target in tile region %d - %u bytes a row, %u KB (%u rows: whole rows of 64-row tiles)",
	         TILE_INDEX, (unsigned)pitch, (unsigned)(size >> 10), (unsigned)(size / pitch));
	return 0;
}

/* V1.0: the copy's buffers - a linear 2048x2048 target (no tiled one, see below), a depth buffer, the
   corners (texel centres on pixel centres, both ways round), the texture's linear header, the read-back slots in
   main memory. Needs the fill's texture. 0 when the linear target is in place */
static int copy_init(void)
{
	void *lin = rsxMemalign(0x10000, COPY_W * COPY_W * 4), *depth = rsxMemalign(0x10000, COPY_W * COPY_W * 2);
	fvtx *vb = (fvtx *)rsxMemalign(128, 8 * sizeof(fvtx));
	fvtx q[8] = { { -1.0f, -1.0f, 0.0f, 0.0f, 0xFFFFFFFFu }, { 1.0f, -1.0f, 1.0f, 0.0f, 0xFFFFFFFFu },
	              { 1.0f, 1.0f, 1.0f, 1.0f, 0xFFFFFFFFu }, { -1.0f, 1.0f, 0.0f, 1.0f, 0xFFFFFFFFu } };

	copy_host = (u32 *)gfx_host_mem(COPY_HOST, CSLOTS * TILE_WORDS * 4, &copy_host_offset);
	if (!lin || !depth || !vb || !copy_host) {
		log_line("RSX copy: no VRAM or main memory for it - the VRAM copy stays off");
		return -1;
	}
	rsxAddressToOffset(lin, &copy_offset[0]);
	rsxAddressToOffset(depth, &copy_depth_offset);
	rsxAddressToOffset(vb, &cvb_offset);
	copy_pitch[0] = COPY_W * 4;
	copy_have[0] = 1;
	memset(copy_host, 0, CSLOTS * TILE_WORDS * 4);
	/* the quad over the whole target; the second one reads the texture's rows the other way round - put together
	   in main memory, into VRAM a word at a time (never read back from there) */
	for (int i = 0; i < 4; i++) {
		q[4 + i] = q[i];
		q[4 + i].v = 1.0f - q[i].v;
	}
	vram_put(vb, q, sizeof(q));
	/* the same 16 MB as the fill's texture, read as it lies in memory: row after row */
	ltex = ftex;
	ltex.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
	/* no second target in a tile region (copy_have[1] stays 0: the self-test, the probe and the load use the
	   linear target alone). The 2nd build's freeze here was not its gcmSetTile, as the 3rd build assumed: it was
	   the corners' misaligned 8-byte stores into VRAM above, which every build up to the 4th had (see vram_put) */
	log_line("RSX copy: a %dx%d linear target - the %d MB texture copied texel for texel", COPY_W, COPY_W,
	         RSXLOAD_TEX * RSXLOAD_TEX * 4 >> 20);
	*(vu32 *)gcmGetLabelAddress(LABEL_COPY) = 0;
	return 0;
}

/* V0.9: the vertex load's buffers: VLOAD_N vertices behind the eye (culled: the vertex units' work alone)
   and the check's VCHECK_N, one on each pixel of a 128x128 tile; its check slots in main memory */
static void vertex_of(vvtx *v, int i, int check)
{
	int px = i % RSXLOAD_CHECK, py = i / RSXLOAD_CHECK % RSXLOAD_CHECK;
	vvtx t = { { 0.5f + px * 0.0117f, 0.5f + py * 0.0117f, 0.75f + px * 0.0041f, 0.6f + py * 0.0053f },
	           { C3[0], C3[1], C3[2], C3[3] }, { W[0], W[1], W[2], W[3] },
	           { SCALE[0], SCALE[1], SCALE[2], SCALE[3] }, { 0.0f, 0.0f, 0.0f, -1.0f } };

	if (check) {
		t.to[0] = (px + 0.5f) / (RSXLOAD_CHECK / 2) - 1.0f;
		t.to[1] = (py + 0.5f) / (RSXLOAD_CHECK / 2) - 1.0f;
		t.to[3] = 1.0f;
	}
	vram_put(v, &t, sizeof(t));   /* `v` is in VRAM */
}

static int vert_init(void)
{
	vvtx *load = (vvtx *)rsxMemalign(128, VLOAD_N * sizeof(vvtx)), *chk = (vvtx *)rsxMemalign(128, VCHECK_N * sizeof(vvtx));
	void *target = rsxMemalign(64, RSXLOAD_CHECK * RSXLOAD_CHECK * 4);
	u32 host_offset, *host = (u32 *)gfx_host_mem(VHOST, CHECK_SLOTS * TILE_WORDS * 4, &host_offset);
	u32 size;

	vlp = (const rsxVertexProgram *)vload_vpo;
	rsxVertexProgramGetUCode(vlp, &vlp_ucode, &size);
	if (!load || !chk || !target || !host) {
		log_line("RSX vertices: no memory for them - the vertex load stays off");
		return -1;
	}
	for (int i = 0; i < VLOAD_N; i++)
		vertex_of(&load[i], i, 0);
	for (int i = 0; i < VCHECK_N; i++)
		vertex_of(&chk[i], i, 1);
	rsxAddressToOffset(load, &vload_offset);
	rsxAddressToOffset(chk, &vcheck_offset);
	rsxAddressToOffset(target, &chk_vert.target);
	chk_vert.host = host;
	chk_vert.host_offset = host_offset;
	chk_vert.checks = &st.vert_checks;
	chk_vert.errors = &st.vert_errors;
	memset(host, 0, CHECK_SLOTS * TILE_WORDS * 4);
	*(vu32 *)gcmGetLabelAddress(LABEL_VCHECK) = 0;
	return 0;
}

int rsxload_init(void)
{
	static const f32 CORNERS[4][4] = { { -1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, -1.0f, 0.0f, 1.0f },
	                                   { 1.0f, 1.0f, 0.0f, 1.0f }, { -1.0f, 1.0f, 0.0f, 1.0f } };
	gcmContextData *ctx = gfx_context();
	u32 size;
	void *tile, *check, *fcheck, *depth;
	f32 *quad_mem;
	u32 *host, host_offset;

	vp = (const rsxVertexProgram *)pass_vpo;
	rsxVertexProgramGetUCode(vp, &vp_ucode, &size);
	fvp = (const rsxVertexProgram *)ui_vpo;
	rsxVertexProgramGetUCode(fvp, &fvp_ucode, &size);
	if (make_programs() != 0)
		return -1;
	quad_mem = (f32 *)rsxMemalign(128, 4 * 4 * sizeof(f32));
	tile = rsxMemalign(64, RSXLOAD_TILE * RSXLOAD_TILE * 4);
	check = rsxMemalign(64, RSXLOAD_CHECK * RSXLOAD_CHECK * 4);
	fcheck = rsxMemalign(64, RSXLOAD_CHECK * RSXLOAD_CHECK * 4);
	depth = rsxMemalign(64, RSXLOAD_TILE * RSXLOAD_TILE * 2);
	host = (u32 *)gfx_host_area(&host_offset);   /* 1 MB: the shader's check tiles, then the fill's */
	if (!quad_mem || !tile || !check || !fcheck || !depth || !host)
		return -1;
	vram_put(quad_mem, CORNERS, sizeof(CORNERS));
	rsxAddressToOffset(quad_mem, &quad_offset);
	rsxAddressToOffset(tile, &tile_offset);
	rsxAddressToOffset(check, &chk_alu.target);
	rsxAddressToOffset(fcheck, &chk_fill.target);
	rsxAddressToOffset(depth, &depth_offset);
	chk_alu.host = host;
	chk_alu.host_offset = host_offset;
	chk_fill.host = host + CHECK_SLOTS * TILE_WORDS;
	chk_fill.host_offset = host_offset + CHECK_SLOTS * TILE_WORDS * 4;
	chk_alu.checks = &st.checks;
	chk_alu.errors = &st.check_errors;
	chk_fill.checks = &st.fill_checks;
	chk_fill.errors = &st.fill_errors;
	memset(host, 0, 2 * CHECK_SLOTS * TILE_WORDS * 4);
	zero_sum = checksum(host);
	st.fill_ok = fill_init() == 0;
	if (st.fill_ok) {
		memset(vram_host, 0, VSLOTS * TILE_WORDS * 4);
		tiled_init();
		/* V1.0: the start guard notes the VRAM copy's part (a start that froze in it leaves it out the next time) */
		st.copy_ok = !guard_part(GUARD_COPY) && copy_init() == 0;   /* until the self-test and the probe say otherwise */
#ifdef GUARD_TEST_STOP
		if (st.copy_ok) {   /* the guard's test: this start stops here, as a frozen one would */
			log_line("GUARD TEST: the start stops inside the VRAM copy's part");
			sysProcessExit(1);
		}
#endif
		guard_part(GUARD_RSX);
	}
	st.vert_ok = vert_init() == 0;   /* until the self-test and the probe say otherwise */
	*(vu32 *)gcmGetLabelAddress(LABEL_LOAD) = 0;
	*(vu32 *)gcmGetLabelAddress(LABEL_CHECK) = 0;
	*(vu32 *)gcmGetLabelAddress(LABEL_FCHECK) = 0;
	*(vu32 *)gcmGetLabelAddress(LABEL_VRAM) = 0;
	*(vu32 *)gcmGetLabelAddress(LABEL_PROBE) = 0;
	*(vu32 *)gcmGetLabelAddress(LABEL_CMD) = 0;
	*(vu32 *)gcmGetLabelAddress(LABEL_RSXSYNC) = 0;
	/* no vertex array but the corners' */
	for (int i = 0; i < 16; i++)
		rsxBindVertexArrayAttrib(ctx, (u8)i, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxInvalidateVertexCache(ctx);
	/* without timings (an emulator): 8 tiles and a layer a picture */
	st.scale = st.ceiling = 1.0;
	st.target_rows_s = 8 * RSXLOAD_TILE;
	st.target_rows_f = RSXLOAD_FILL_H;
	st.ready = 1;
	gfx_set_frame_hook(NULL);
	return 0;
}

/* one check pixel against the PPU: red and green (the fractions of 32v and 512v) must agree with the
   PPU's own v within two steps - the PPU need not round like the RSX, which only shows in blue and
   alpha. Where the RSX puts a pixel (its centre or corner, rows from the top or the bottom) is found by
   trying. Fills st.check_* for layout v; 1 when it agrees */
static int to_byte(float f)
{
	return (int)(f * 255.0f + 0.5f);
}

static int byte_gap(int a, int b)
{
	int d = a > b ? a - b : b - a;

	return d > 128 ? 256 - d : d;   /* a fraction near 1 and one near 0 are neighbours */
}

static int compare_pixel(int v, const u32 *p)
{
	int x = 37, y = 91, best = 1 << 30;
	u32 px = p[y * RSXLOAD_CHECK + x];
	int red = (int)(px >> 16) & 0xFF, green = (int)(px >> 8) & 0xFF;

	st.check_x = x;
	st.check_y = y;
	st.check_bits = px;
	for (int top = 0; top < 2; top++)
		for (int dy = -1; dy <= 1; dy++)
			for (int dx = -1; dx <= 1; dx++)
				for (int half = 0; half < 2; half++) {
					float py = top ? (float)(y + dy) : (float)(RSXLOAD_CHECK - 1 - y + dy);
					float f = ppu_pixel(v, (float)(x + dx) + half * 0.5f, py + half * 0.5f, MADS);
					float r = f * SCALE[0] - floorf(f * SCALE[0]), g = f * SCALE[1] - floorf(f * SCALE[1]);
					int gap = byte_gap(to_byte(r), red) + byte_gap(to_byte(g), green);

					if (gap < best) {
						best = gap;
						st.check_ppu = f;
						st.check_expect = (u32)to_byte(r) << 16 | (u32)to_byte(g) << 8;
						st.check_from_top = top;
					}
				}
	st.check_ok = best <= 4;
	return st.check_ok;
}

/* One step of the self-test: the command reader's label (it got there) and the backend's (everything
   before it is drawn); 0 when both came within half a second. On the console V9.0's first draw never
   finished (black screen) while RPCS3 drew it: the steps go from the plainest draw to the load's, so
   the log says which part the real RSX does not take. */
/* V0.9: a step may wait longer (ms) - RPCS3 translates a long vertex program before its first draw */
static int step_ms(gcmContextData *ctx, int n, const char *what, int ms)
{
	uint64_t t0, tc, tb;
	double us = 1e6 / (double)sysGetTimebaseFrequency();

	rsxSetWriteBackendLabel(ctx, LABEL_PROBE, 100 + (u32)n);
	rsxSetWriteCommandLabel(ctx, LABEL_CMD, 100 + (u32)n);
	t0 = __gettime();
	rsxFlushBuffer(ctx);
	tc = label_time(LABEL_CMD, 100 + (u32)n, ms);
	tb = tc ? label_time(LABEL_PROBE, 100 + (u32)n, ms) : 0;
	if (tc && tb) {
		log_line("RSX self-test %d (%s): read after %.0f us, drawn after %.0f us", n, what, (double)(tc - t0) * us,
		         (double)(tb - t0) * us);
		return 0;
	}
	log_where(tc ? "the self-test's draw did not finish in time" : "the self-test's commands were not read in time");
	log_line("RSX self-test %d (%s): FAILED after %d ms - the commands %s, the drawing %s; the RSX load stays off", n,
	         what, ms, tc ? "were read" : "were NOT read", tc ? "did NOT finish" : "is unknown");
	return -1;
}

static int step(gcmContextData *ctx, int n, const char *what)
{
	return step_ms(ctx, n, what, 500);
}

/* V1.0 (5th build): after a layer into the tile region, the texture's first 64 KB once more - it lies right above
   the region in VRAM. Changed, the region spills past its end (the 4th build's did, see tiled_init): then the fill
   stays on the linear target and the PPU writes the slice anew. -1 when the RSX did not answer */
static int tiled_spill_check(gcmContextData *ctx)
{
	uint64_t sum;

	copy_slice(ctx, 0);
	if (step(ctx, 20, "the texture's first 64 KB after the tile region's layer") != 0)
		return -1;
	sum = settled_sum(vram_host, zero_sum);   /* the slot was zeros */
	memset(vram_host, 0, TILE_WORDS * 4);
	if (sum == slice_sum[0])
		return 0;
	log_line("RSX fill: the tile region wrote past its end, onto the texture right above it - the fill stays on the "
	         "linear target (the texture's first 64 KB written anew)");
	gcmSetInvalidateTile(TILE_INDEX);
	tfill_pitch = 0;
	for (u32 j = 0; j < TILE_WORDS; j++)
		tex_mem[j] = texel(j);
	return 0;
}

static int selftest(gcmContextData *ctx)
{
	char what[96];

	if (step(ctx, 0, "labels alone") != 0)
		return -1;
	state_common(ctx);
	set_target(ctx, tile_offset, RSXLOAD_TILE, GCM_SURFACE_X8R8G8B8);
	load_fp(ctx, P_FLAT);
	quad(ctx);
	if (step(ctx, 1, "one colour, 8-bit target") != 0)
		return -1;
	set_target(ctx, chk_alu.target, RSXLOAD_CHECK, GCM_SURFACE_A8R8G8B8);
	quad(ctx);
	if (step(ctx, 2, "one colour, check target") != 0)
		return -1;
	set_target(ctx, tile_offset, RSXLOAD_TILE, GCM_SURFACE_X8R8G8B8);
	load_fp(ctx, P_LONG(0));
	quad(ctx);
	if (step(ctx, 3, "load shader, 8-bit target") != 0)
		return -1;
	set_target(ctx, chk_alu.target, RSXLOAD_CHECK, GCM_SURFACE_A8R8G8B8);
	quad(ctx);
	if (step(ctx, 4, "load shader, check target") != 0)
		return -1;
	rsxSetTransferDataMode(ctx, GCM_TRANSFER_LOCAL_TO_MAIN);
	rsxSetTransferDataFormat(ctx, RSXLOAD_CHECK * 4, RSXLOAD_CHECK * 4, RSXLOAD_CHECK * 4, RSXLOAD_CHECK, 1, 1);
	rsxSetTransferDataOffset(ctx, chk_alu.host_offset, chk_alu.target);
	if (step(ctx, 5, "copy to main memory") != 0)
		return -1;
	/* V0.7: the shader's other layouts, the fill, the VRAM copy */
	for (int v = 1; v < RSXLOAD_VARIANTS; v++) {
		set_target(ctx, tile_offset, RSXLOAD_TILE, GCM_SURFACE_X8R8G8B8);
		load_fp(ctx, P_LONG(v));
		quad(ctx);
		snprintf(what, sizeof(what), "load shader, %s", VARIANT_NAME[v]);
		if (step(ctx, 5 + v, what) != 0)
			return -1;
	}
	if (!st.fill_ok)
		return 0;
	fill_state(ctx);
	set_target(ctx, chk_fill.target, RSXLOAD_CHECK, GCM_SURFACE_A8R8G8B8);
	layer(ctx, 0);
	if (step(ctx, 9, "fill layer: texture, blending, check target") != 0)
		return -1;
	fill_target(ctx, 0);
	layer(ctx, 1);
	if (step(ctx, 10, "fill layer, 1920x1080") != 0)
		return -1;
	copy_slice(ctx, 0);
	if (step(ctx, 11, "VRAM slice to main memory") != 0)
		return -1;
	/* the slice must come back as the PPU wrote it */
	{
		uint64_t sum = settled_sum(vram_host, zero_sum);   /* the slot was zeros */

		st.vram_ok = sum == slice_sum[0];
		log_line("RSX VRAM check: the texture's first 64 KB came back %s", st.vram_ok ? "as written - the check runs" :
		         "DIFFERENT from what the PPU wrote - the VRAM check stays off");
		memset(vram_host, 0, TILE_WORDS * 4);
	}
	/* V0.9: the tiled target, the longest fill program, the vertex load and its check */
	if (tfill_pitch) {
		fill_state(ctx);
		fill_target(ctx, 1);
		layer(ctx, 2);
		if (step(ctx, 12, "fill layer into the tile region") != 0)
			return -1;
		if (st.vram_ok && tiled_spill_check(ctx) != 0)
			return -1;
	}
	fill_prog = P_FILL(FILL_VARIANTS - 1);
	fill_state(ctx);
	set_target(ctx, chk_fill.target, RSXLOAD_CHECK, GCM_SURFACE_A8R8G8B8);
	layer(ctx, 3);
	fill_prog = P_FILL(0);
	snprintf(what, sizeof(what), "fill program with %d multiply-adds, check target", 2 * FILL_PAIRS[FILL_VARIANTS - 1]);
	if (step(ctx, 13, what) != 0)
		return -1;
	if (st.vert_ok) {
		state_common(ctx);
		set_target(ctx, tile_offset, RSXLOAD_TILE, GCM_SURFACE_X8R8G8B8);
		vert_batches_draw(ctx, 1);
		if (step_ms(ctx, 14, "vertex load: a batch of points behind the eye", 5000) != 0)
			return -1;
		state_common(ctx);
		vert_check(ctx);
		if (step_ms(ctx, 15, "vertex check: a point a pixel, copied to main memory", 5000) != 0)
			return -1;
	}
	/* V1.0: the VRAM copy - into the linear target, a slice of it back, into the tile region, the longest program */
	if (st.copy_ok) {
		guard_part(GUARD_COPY);
		state_common(ctx);
		copy_state(ctx, P_COPY(0), 0, 0);
		rsxDrawVertexArray(ctx, GCM_TYPE_QUADS, 0, 4);
		if (step_ms(ctx, 16, "VRAM copy: the 16 MB texture into the linear 2048x2048 target", 5000) != 0)
			return -1;
		copy_target_slice(ctx, 0, 0, 0);
		if (step(ctx, 17, "copy target slice to main memory") != 0)
			return -1;
		if (copy_have[1]) {
			copy_state(ctx, P_COPY(0), 1, 0);
			rsxDrawVertexArray(ctx, GCM_TYPE_QUADS, 0, 4);
			if (step_ms(ctx, 18, "VRAM copy into the tile region", 5000) != 0)
				return -1;
		}
		copy_state(ctx, P_COPY(COPY_VARIANTS - 1), 0, 0);
		rsxDrawVertexArray(ctx, GCM_TYPE_QUADS, 0, 4);
		snprintf(what, sizeof(what), "VRAM copy with %d multiply-adds a pixel", 2 * COPY_PAIRS[COPY_VARIANTS - 1]);
		if (step_ms(ctx, 19, what, 5000) != 0)
			return -1;
		guard_part(GUARD_RSX);
	}
	return 0;
}

/* layout v's rate: four tiles for a first idea, then k tiles of the long program and k of the short
   one; the PPU looks at the labels without a break, so its clock times the RSX to a few microseconds.
   G MADs a second, 0 when not measured or not plausible */
static double time_variant(gcmContextData *ctx, int v)
{
	double tb_ns = 1e9 / (double)sysGetTimebaseFrequency(), gmad;
	uint64_t t0, t1, t2;
	int k;

	if (rsx_stuck)
		return 0.0;

	state_common(ctx);
	set_target(ctx, tile_offset, RSXLOAD_TILE, GCM_SURFACE_X8R8G8B8);
	load_fp(ctx, P_LONG(v));
	for (int i = 0; i < 4; i++)
		quad(ctx);
	rsxSetWriteBackendLabel(ctx, LABEL_PROBE, 2);
	t0 = __gettime();
	rsxFlushBuffer(ctx);
	t1 = label_time(LABEL_PROBE, 2, 2000);
	if (!t1) {
		log_where("four tiles did not finish in 2 s");
		return 0.0;
	}
	k = (int)(100e6 / ((double)(t1 - t0) * tb_ns / 4.0 + 1.0));   /* about 0.1 s */
	k = k < 8 ? 8 : k > 1024 ? 1024 : k;
	for (int i = 0; i < k; i++)
		quad(ctx);
	rsxSetWriteBackendLabel(ctx, LABEL_PROBE, 3);
	load_fp(ctx, P_SHORT(v));
	for (int i = 0; i < k; i++)
		quad(ctx);
	rsxSetWriteBackendLabel(ctx, LABEL_PROBE, 4);
	t0 = __gettime();
	rsxFlushBuffer(ctx);
	t1 = label_time(LABEL_PROBE, 3, 5000);
	t2 = t1 ? label_time(LABEL_PROBE, 4, 5000) : 0;
	*(vu32 *)gcmGetLabelAddress(LABEL_PROBE) = 0;
	if (!t1 || !t2) {
		log_where("the timed tiles did not finish in 5 s");
		return 0.0;
	}
	var_tiles[v] = k;
	var_long_us[v] = (double)(t1 - t0) * tb_ns / 1000.0;
	var_short_us[v] = (double)(t2 - t1) * tb_ns / 1000.0;
	var_tile_us[v] = var_long_us[v] / k;
	if (t1 - t0 <= t2 - t1)
		return 0.0;
	{
		double px = (double)RSXLOAD_TILE * RSXLOAD_TILE * k;

		gmad = px * (MADS - MADS_SHORT) / ((double)((t1 - t0) - (t2 - t1)) * tb_ns);
		var_load_gmad[v] = px * MADS / ((double)(t1 - t0) * tb_ns);
	}
	/* not faster than an RSX could be (192 GFLOP/s at 500 MHz; an emulator writes the labels before the
	   work is done) */
	return gmad * 8.0 < 400.0 ? gmad : 0.0;
}

/* layout v's check tile twice (RPCS3 runs a new shader in its interpreter first, which rounds a little
   differently - the console gives the same twice), a label behind them; 0 when they came back */
static int check_twice(gcmContextData *ctx, int v)
{
	if (rsx_stuck)
		return -1;
	memset(slot_of(&chk_alu, chk_alu.no + 1), 0, TILE_WORDS * 4);
	memset(slot_of(&chk_alu, chk_alu.no + 2), 0, TILE_WORDS * 4);
	state_common(ctx);
	check_tile(ctx, P_LONG(v));
	check_tile(ctx, P_LONG(v));
	rsxSetWriteBackendLabel(ctx, LABEL_PROBE, 1);
	rsxFlushBuffer(ctx);
	if (wait_label_value(LABEL_CHECK, chk_alu.no, 2000) != 0 || wait_label_value(LABEL_PROBE, 1, 2000) != 0) {
		log_where("the check tile did not come back in 2 s");
		return -1;
	}
	*(vu32 *)gcmGetLabelAddress(LABEL_PROBE) = 0;
	chk_alu.verified = chk_alu.no;
	settled_sum(slot_of(&chk_alu, chk_alu.no - 1), zero_sum);   /* RPCS3: until both copies are there */
	settled_sum(slot_of(&chk_alu, chk_alu.no), zero_sum);
	return 0;
}

static double gpix_of(double layer_us)
{
	return layer_us > 0.0 ? (double)RSXLOAD_FILL_W * RSXLOAD_FILL_H / (layer_us * 1000.0) : 0.0;
}

/* us a full-screen layer of fill program p on the target (tiled or linear), with `batches` vertex batches
   behind each: a few layers for a first idea, then about 0.05 s of them, timed by the PPU watching a label;
   0 when they did not finish */
static double time_fill(gcmContextData *ctx, int p, int tiled, int batches)
{
	double tb_ns = 1e9 / (double)sysGetTimebaseFrequency(), us = 0.0;
	int k = 2, prog = fill_prog, was = fill_tiled;

	if (rsx_stuck)
		return 0.0;
	fill_prog = p;
	fill_tiled = tiled;
	for (int pass = 0; pass < 2; pass++) {
		uint64_t t0, t1;

		fill_layers(ctx, k, batches);
		rsxSetWriteBackendLabel(ctx, LABEL_PROBE, 6 + (u32)pass);
		t0 = __gettime();
		rsxFlushBuffer(ctx);
		t1 = label_time(LABEL_PROBE, 6 + (u32)pass, 3000);
		if (!t1) {
			log_where("timed fill layers did not finish in 3 s");
			us = 0.0;
			break;
		}
		us = (double)(t1 - t0) * tb_ns / 1000.0 / k;
		k = (int)(50000.0 / (us + 1.0));
		/* with vertex batches each layer loads both vertex programs again (about 5 KB of commands) */
		k = k < 4 ? 4 : k > (batches ? 32 : 4 * FILL_MAX) ? (batches ? 32 : 4 * FILL_MAX) : k;
		gfx_rewind();
	}
	*(vu32 *)gcmGetLabelAddress(LABEL_PROBE) = 0;
	fill_prog = prog;
	fill_tiled = was;
	return us;
}

/* The fill alone (V0.9): blending alone (a flat colour) and the fill program on the linear target and on the
   tiled one - the faster takes the load; then the fill programs with more and more multiply-adds on it: the
   load takes the one where the arithmetic units (against the shader's rate alone) and the blending (against
   blending alone) are busiest together. Then the check tile's start, with that program. */
static void probe_fill(gcmContextData *ctx)
{
	double blend[2] = { 0.0, 0.0 }, plain[2] = { 0.0, 0.0 }, rate[FILL_VARIANTS], best = -1.0;
	int tiled_ok = tfill_pitch != 0, plausible, v_best = 0, distinct = 0, samples = 0;
	const u32 *d;

	for (int t = 0; t <= tiled_ok; t++) {
		blend[t] = gpix_of(time_fill(ctx, P_FLAT, t, 0));
		plain[t] = gpix_of(time_fill(ctx, P_FILL(0), t, 0));
		log_line("RSX fill alone on the %s target: blending alone %.2f G pixels a second, the fill program (%d "
		         "multiply-adds, two texture reads) %.2f", t ? "tiled" : "linear", blend[t], 2 * FILL_PAIRS[0], plain[t]);
	}
	/* not more than the RSX's 8 blending units make at 500 MHz (an emulator writes labels before the work) */
	plausible = plain[0] > 0.0 && plain[0] < 4.5 && blend[0] < 4.5 && (!tiled_ok || (plain[1] > 0.0 && plain[1] < 4.5));
	fill_tiled = plausible ? tiled_ok && plain[1] > plain[0] : tiled_ok;
	for (int v = 0; v < FILL_VARIANTS; v++)
		rate[v] = plausible ? gpix_of(time_fill(ctx, P_FILL(v), fill_tiled, 0)) : 0.0;
	if (plausible && st.probe_ok && blend[fill_tiled] > 0.0) {
		for (int v = 0; v < FILL_VARIANTS; v++) {
			double alu = rate[v] * 2.0 * FILL_PAIRS[v] / st.probe_gmad, rop = rate[v] / blend[fill_tiled];

			log_line("  fill program with %2d multiply-adds a pixel: %.2f G pixels a second - arithmetic at %3.0f %% of "
			         "the shader's rate alone, blending at %3.0f %% of blending alone", 2 * FILL_PAIRS[v], rate[v],
			         100.0 * alu, 100.0 * rop);
			if (rate[v] > 0.0 && alu + rop > best) {
				best = alu + rop;
				v_best = v;
			}
		}
	} else {
		v_best = 2;   /* no timings (an emulator): a middle one, so it runs */
	}
	fill_implausible = !plausible && plain[0] > 0.0;
	fill_prog = fill_best = P_FILL(v_best);
	st.fill_variant = v_best;
	st.fill_mads = 2 * FILL_PAIRS[v_best];
	fill0_row_us = plausible && rate[0] > 0.0 ? RSXLOAD_FILL_W / (rate[0] * 1000.0) : 0.0;
	st.fill_tiled = fill_tiled;
	st.blend_gpix = plausible ? blend[fill_tiled] : 0.0;
	st.probe_gpix = plausible ? rate[v_best] : 0.0;
	st.probe_layer_us = st.probe_gpix > 0.0 ? (double)RSXLOAD_FILL_W * RSXLOAD_FILL_H / (st.probe_gpix * 1000.0) : 0.0;
	st.layer_us = st.probe_layer_us;   /* with the vertex batches behind it, if they run (probe_vert) */
	if (plausible)
		log_line("RSX fill: the load uses the %s target and %d multiply-adds a pixel: %.2f G pixels a second alone "
		         "(%.1f GFLOP/s on the side)", fill_tiled ? "tiled" : "linear", st.fill_mads, st.probe_gpix,
		         st.probe_gpix * st.fill_mads * 8.0);
	else
		log_line("RSX fill alone: not plausible (%.1f G pixels a second; 8 blending units at 500 MHz make 4) - an "
		         "emulator? The load uses the %s target and %d multiply-adds a pixel", plain[0],
		         fill_tiled ? "tiled" : "linear", st.fill_mads);

	/* the check tile's start, with the program the load uses */
	if (rsx_stuck)
		return;
	memset(slot_of(&chk_fill, chk_fill.no + 1), 0, TILE_WORDS * 4);
	memset(slot_of(&chk_fill, chk_fill.no + 2), 0, TILE_WORDS * 4);
	fill_check(ctx);
	fill_check(ctx);
	rsxSetWriteBackendLabel(ctx, LABEL_PROBE, 5);
	rsxFlushBuffer(ctx);
	if (wait_label_value(LABEL_FCHECK, chk_fill.no, 2000) != 0 || wait_label_value(LABEL_PROBE, 5, 2000) != 0) {
		log_where("the fill check tile did not come back in 2 s");
		st.fill_ok = 0;
		return;
	}
	*(vu32 *)gcmGetLabelAddress(LABEL_PROBE) = 0;
	chk_fill.verified = chk_fill.no;
	d = slot_of(&chk_fill, chk_fill.no);
	settled_sum(d, zero_sum);   /* RPCS3: until the copy is there */
	memcpy(chk_fill.start, d, sizeof(chk_fill.start));
	chk_fill.have_start = 1;
	chk_fill.reference = checksum(d);
	for (int i = 1; i < TILE_WORDS; i += 61, samples++)
		distinct += d[i] != d[i - 1];
	log_line("RSX fill check tile: %d layers blended, pixel (37, 91) %08x, %d of %d sampled pixels differ from their "
	         "neighbour%s, checksum %016llx", FILL_CHECK_LAYERS, (unsigned)d[91 * RSXLOAD_CHECK + 37], distinct,
	         samples, distinct ? "" : " - FLAT: the texture did not come through", (unsigned long long)chk_fill.reference);
}

/* V1.0: slices 0, 137 and the last of copy target t, once everything before is drawn, against the texture's;
   returns how many agree, -1 when they did not come back */
static int copy_check_now(gcmContextData *ctx, int t)
{
	static const u32 S[3] = { 0, 137, SLICES - 1 };
	int good = 0;

	if (rsx_stuck)
		return -1;
	memset(copy_host, 0, 3 * TILE_WORDS * 4);
	rsxSetWriteBackendLabel(ctx, LABEL_RSXSYNC, sync_value);
	rsxSetWaitLabel(ctx, LABEL_RSXSYNC, sync_value);
	sync_value++;
	for (u32 i = 0; i < 3; i++)
		copy_target_slice(ctx, t, S[i], i);
	rsxSetWriteBackendLabel(ctx, LABEL_PROBE, 13);
	rsxFlushBuffer(ctx);
	if (wait_label_value(LABEL_PROBE, 13, 3000) != 0) {
		log_where("the copy target's slices did not come back in 3 s");
		return -1;
	}
	*(vu32 *)gcmGetLabelAddress(LABEL_PROBE) = 0;
	for (int i = 0; i < 3; i++)   /* RPCS3: until each copy is there */
		good += settled_sum(copy_host + i * TILE_WORDS, zero_sum) == slice_sum[S[i]];
	return good;
}

/* V1.0: us a whole copy pass (the 16 MB) of program p into target t: two for a first idea, then about 0.05 s of
   them, timed by the PPU watching a label; 0 when they did not finish */
static double time_copy(gcmContextData *ctx, int p, int t)
{
	double tb_ns = 1e9 / (double)sysGetTimebaseFrequency(), us = 0.0;
	int k = 2;

	if (rsx_stuck)
		return 0.0;
	for (int pass = 0; pass < 2; pass++) {
		uint64_t t0, t1;

		copy_state(ctx, p, t, copy_flip);
		for (int i = 0; i < k; i++)
			rsxDrawVertexArray(ctx, GCM_TYPE_QUADS, 0, 4);
		rsxSetWriteBackendLabel(ctx, LABEL_PROBE, 20 + (u32)pass);
		t0 = __gettime();
		rsxFlushBuffer(ctx);
		t1 = label_time(LABEL_PROBE, 20 + (u32)pass, 3000);
		if (!t1) {
			log_where("timed copy passes did not finish in 3 s");
			us = 0.0;
			break;
		}
		us = (double)(t1 - t0) * tb_ns / 1000.0 / k;
		k = (int)(50000.0 / (us + 1.0));
		k = k < 4 ? 4 : k > 64 ? 64 : k;
		gfx_rewind();
	}
	*(vu32 *)gcmGetLabelAddress(LABEL_PROBE) = 0;
	return us;
}

static double gpix_copy(double pass_us)
{
	return pass_us > 0.0 ? (double)COPY_W * COPY_W / (pass_us * 1000.0) : 0.0;
}

/* V1.0: the copy alone. Which way round the RSX lays the rows (the slices must come back as the texture's), on
   each target; the plain copy on both, the faster takes the load; then the copy with more and more multiply-adds
   a pixel: RSX + VRAM takes the most that keep it at COPY_KEEP of the plain one's rate - and checks once more
   that this program copies exactly too */
static void probe_copy(gcmContextData *ctx)
{
	double rate[2] = { 0.0, 0.0 }, mrate[COPY_VARIANTS];
	int ok[2] = { 0, 0 }, plausible, best = 0, good = 0;

	if (rsx_stuck)
		return;

	for (int t = 0; t < 2; t++) {
		if (!copy_have[t])
			continue;
		for (int i = 0; i < (t == 1 && ok[0] ? 1 : 2) && !ok[t]; i++) {
			int flip = i ? !copy_flip : copy_flip;

			copy_state(ctx, P_COPY(0), t, flip);
			rsxDrawVertexArray(ctx, GCM_TYPE_QUADS, 0, 4);
			good = copy_check_now(ctx, t);
			if (good < 0) {
				st.copy_ok = 0;
				return;
			}
			if (good == 3) {
				ok[t] = 1;
				copy_flip = flip;
			}
		}
		log_line("RSX copy into the %s target: %s", t ? "tiled" : "linear", ok[t] ? (copy_flip ?
		         "the slices came back as the texture (its rows drawn the other way round)" :
		         "the slices came back as the texture") : "the slices came back DIFFERENT from the texture");
	}
	if (!ok[0] && !ok[1]) {
		st.copy_ok = 0;
		log_line("RSX copy: no target gave the texture back - the VRAM copy stays off");
		return;
	}
	for (int t = 0; t < 2; t++)
		rate[t] = ok[t] ? gpix_copy(time_copy(ctx, P_COPY(0), t)) : 0.0;
	/* 8 bytes a pixel: more than 5 G pixels a second (40 GB/s) no RSX's memory makes (an emulator: labels early) */
	plausible = (!ok[0] || (rate[0] > 0.0 && rate[0] < 5.0)) && (!ok[1] || (rate[1] > 0.0 && rate[1] < 5.0));
	copy_tiled = ok[1] && (!ok[0] || !plausible || rate[1] > rate[0]);
	for (int t = 0; t < 2; t++)
		if (ok[t] && plausible)
			log_line("RSX copy alone on the %s target: %.2f G pixels a second = %.2f GB/s (4 bytes read and 4 written a "
			         "pixel)", t ? "tiled" : "linear", rate[t], rate[t] * 8.0);
	mrate[0] = rate[copy_tiled];
	for (int v = 1; v < COPY_VARIANTS; v++) {
		mrate[v] = plausible ? gpix_copy(time_copy(ctx, P_COPY(v), copy_tiled)) : 0.0;
		if (!plausible)
			continue;
		log_line("  copy with %2d multiply-adds a pixel: %.2f G pixels a second = %.2f GB/s, %3.0f %% of the plain copy, "
		         "arithmetic at %3.0f %% of the shader's rate alone", 2 * COPY_PAIRS[v], mrate[v], mrate[v] * 8.0,
		         mrate[0] > 0.0 ? 100.0 * mrate[v] / mrate[0] : 0.0,
		         st.probe_gmad > 0.0 ? 100.0 * mrate[v] * 2.0 * COPY_PAIRS[v] / st.probe_gmad : 0.0);
		if (mrate[0] > 0.0 && mrate[v] >= COPY_KEEP * mrate[0])
			best = v;
	}
	if (!plausible)
		best = 3;   /* no timings (an emulator): a middle one, so it runs */
	/* the program RSX + VRAM uses must copy exactly too (it leaves the target holding the texture) */
	copy_state(ctx, P_COPY(best), copy_tiled, copy_flip);
	rsxDrawVertexArray(ctx, GCM_TYPE_QUADS, 0, 4);
	good = copy_check_now(ctx, copy_tiled);
	if (good < 0) {
		st.copy_ok = 0;
		return;
	}
	if (good != 3 && best) {
		log_line("RSX copy with %d multiply-adds: the slices came back DIFFERENT - RSX + VRAM copies without them",
		         2 * COPY_PAIRS[best]);
		best = 0;
		copy_state(ctx, P_COPY(0), copy_tiled, copy_flip);
		rsxDrawVertexArray(ctx, GCM_TYPE_QUADS, 0, 4);
		good = copy_check_now(ctx, copy_tiled);
	}
	if (good != 3) {
		st.copy_ok = 0;
		log_line("RSX copy: the target did not hold the texture at the end - the VRAM copy stays off");
		return;
	}
	copy_best = P_COPY(best);
	st.copy_tiled = copy_tiled;
	st.copy_flip = copy_flip;
	st.copy_mads = 2 * COPY_PAIRS[best];
	st.copy_gbs = plausible ? mrate[0] * 8.0 : 0.0;
	st.copy_row_us = plausible && mrate[0] > 0.0 ? COPY_W / (mrate[0] * 1000.0) : 0.0;
	st.copy_mad_gbs = plausible ? mrate[best] * 8.0 : 0.0;
	st.copy_mad_row_us = plausible && mrate[best] > 0.0 ? COPY_W / (mrate[best] * 1000.0) : 0.0;
	/* the VRAM clock from the copy's rate alone, as the core's from the shader's: the copy is bound by the
	   graphics memory (8 bytes a pixel; the ROPs could write more than the GDDR3 carries) */
	st.vram_mhz = 0.0;
	if (VRAM_STOCK_GBS > 0.0 && st.copy_gbs > 0.0) {
		st.vram_mhz = st.copy_gbs / VRAM_STOCK_GBS * VRAM_STOCK_MHZ;
		if (st.vram_mhz < 300.0 || st.vram_mhz > 1500.0)
			st.vram_mhz = 0.0;
	}
	if (st.vram_mhz > 0.0)
		log_line("VRAM clock: %.0f MHz, measured (the copy's %.2f GB/s alone against %.2f GB/s on a stock PS3 at %.0f MHz)",
		         st.vram_mhz, st.copy_gbs, VRAM_STOCK_GBS, VRAM_STOCK_MHZ);
	else if (st.copy_gbs > 0.0)
		log_line("VRAM clock: not known yet - the copy's %.2f GB/s alone needs a stock PS3's rate as its reference "
		         "(GDDR3 at %.0f MHz); the screen shows the GB/s", st.copy_gbs, VRAM_STOCK_MHZ);
	if (plausible)
		log_line("RSX copy: the load uses the %s target - %.2f GB/s alone; RSX + VRAM gives it %d multiply-adds a pixel "
		         "(%.2f GB/s, %.0f %% of the plain copy, %.1f GFLOP/s on the side)", copy_tiled ? "tiled" : "linear",
		         st.copy_gbs, st.copy_mads, st.copy_mad_gbs, 100.0 * mrate[best] / mrate[0],
		         mrate[best] * st.copy_mads * 8.0);
	else
		log_line("RSX copy alone: not plausible (%.1f G pixels a second; 8 bytes each, no RSX's memory makes 40 GB/s) - "
		         "an emulator? The load uses the %s target, RSX + VRAM %d multiply-adds a pixel", rate[copy_tiled],
		         copy_tiled ? "tiled" : "linear", st.copy_mads);
}

/* V0.9: the vertex load. Its check tile twice (the start) and one pixel against the PPU's sum - the load
   runs only when they agree; its rate alone; then fill layers with vertex batches behind each (about half
   a layer's time of them): the load keeps them when at least 30 % of their work ran under the pixels */
static void probe_vert(gcmContextData *ctx)
{
	double tb_ns = 1e9 / (double)sysGetTimebaseFrequency(), batch_us = 0.0;
	const int x = 37, y = 91;
	const u32 *d;
	u32 px;
	int distinct = 0, samples = 0, best = 1 << 30, k = 2;

	if (rsx_stuck)
		return;
	memset(slot_of(&chk_vert, chk_vert.no + 1), 0, TILE_WORDS * 4);
	memset(slot_of(&chk_vert, chk_vert.no + 2), 0, TILE_WORDS * 4);
	state_common(ctx);
	vert_check(ctx);
	vert_check(ctx);
	rsxSetWriteBackendLabel(ctx, LABEL_PROBE, 10);
	rsxFlushBuffer(ctx);
	if (wait_label_value(LABEL_VCHECK, chk_vert.no, 2000) != 0 || wait_label_value(LABEL_PROBE, 10, 2000) != 0) {
		log_where("the vertex check tile did not come back in 2 s");
		st.vert_ok = 0;
		return;
	}
	*(vu32 *)gcmGetLabelAddress(LABEL_PROBE) = 0;
	chk_vert.verified = chk_vert.no;
	d = slot_of(&chk_vert, chk_vert.no);
	settled_sum(slot_of(&chk_vert, chk_vert.no - 1), zero_sum);
	settled_sum(d, zero_sum);
	memcpy(chk_vert.start, d, sizeof(chk_vert.start));
	chk_vert.have_start = 1;
	chk_vert.reference = checksum(d);
	for (int i = 1; i < TILE_WORDS; i += 61, samples++)
		distinct += d[i] != d[i - 1];
	/* one pixel against the PPU: the vertex whose point landed there (rows from the top or the bottom) */
	px = d[y * RSXLOAD_CHECK + x];
	for (int top = 0; top < 2; top++) {
		vvtx v;
		float f, r, g;
		int gap;

		vertex_of(&v, (top ? y : RSXLOAD_CHECK - 1 - y) * RSXLOAD_CHECK + x, 1);
		f = ppu_vertex(&v);
		r = f * SCALE[0] - floorf(f * SCALE[0]);
		g = f * SCALE[1] - floorf(f * SCALE[1]);
		gap = byte_gap(to_byte(r), (int)(px >> 16) & 0xFF) + byte_gap(to_byte(g), (int)(px >> 8) & 0xFF);
		best = gap < best ? gap : best;
	}
	st.vert_check_ok = best <= 4 && distinct > samples / 2;
	log_line("RSX vertex check tile: a point a pixel, %d of %d sampled pixels differ from their neighbour, pixel (%d, "
	         "%d) %08x %s the PPU's sum (gap %d), checksum %016llx", distinct, samples, x, y, (unsigned)px,
	         best <= 4 ? "agrees with" : "DIFFERS from", best, (unsigned long long)chk_vert.reference);
	if (!st.vert_check_ok) {
		st.vert_ok = 0;
		log_line("RSX vertices: the vertex load stays off (its check did not come out as the PPU has it)");
		return;
	}

	/* the vertex units alone: batches of points behind the eye */
	for (int pass = 0; pass < 2; pass++) {
		uint64_t t0, t1;

		state_common(ctx);
		set_target(ctx, tile_offset, RSXLOAD_TILE, GCM_SURFACE_X8R8G8B8);
		vert_batches_draw(ctx, k);
		rsxSetWriteBackendLabel(ctx, LABEL_PROBE, 11 + (u32)pass);
		t0 = __gettime();
		rsxFlushBuffer(ctx);
		t1 = label_time(LABEL_PROBE, 11 + (u32)pass, 3000);
		if (!t1) {
			log_where("the timed vertex batches did not finish in 3 s");
			st.vert_ok = 0;
			return;
		}
		batch_us = (double)(t1 - t0) * tb_ns / 1000.0 / k;
		k = (int)(30000.0 / (batch_us + 1.0));
		k = k < 4 ? 4 : k > 512 ? 512 : k;
	}
	*(vu32 *)gcmGetLabelAddress(LABEL_PROBE) = 0;
	st.probe_vert_gmad = batch_us > 0.0 ? (double)VLOAD_N * VMADS / (batch_us * 1000.0) : 0.0;
	/* 8 vertex units at 500 MHz make at most 4 G four-float multiply-adds a second (an emulator: more) */
	if (st.probe_vert_gmad <= 0.0 || st.probe_vert_gmad > 6.0 || st.probe_layer_us <= 0.0) {
		log_line("RSX vertices alone: %.1f G multiply-adds a second is not plausible or the fill was not timed - an "
		         "emulator? The load draws one batch behind each fill layer", st.probe_vert_gmad);
		st.probe_vert_gmad = 0.0;
		vert_batches = 1;
	} else {
		int vb = (int)(0.5 * st.probe_layer_us / batch_us + 0.5);
		double with_us, vert_us, overlap;

		vb = vb < 1 ? 1 : vb > 64 ? 64 : vb;
		with_us = time_fill(ctx, fill_prog, fill_tiled, vb);
		vert_us = vb * batch_us;
		overlap = with_us > 0.0 ? (st.probe_layer_us + vert_us - with_us) / vert_us : 0.0;
		st.vert_overlap = overlap;
		vert_batches = overlap >= 0.3 ? vb : 0;
		if (vert_batches)
			st.layer_us = with_us;
		log_line("RSX vertices alone: %.2f M vertices a second = %.2f G multiply-adds = %.1f GFLOP/s (%.0f us a batch "
		         "of %d); behind each fill layer %d batches: the layer took %.0f us alone and %.0f us with them, %.0f us "
		         "of vertex work alone - %.0f %% of it ran under the pixels: %s", (double)VLOAD_N / batch_us,
		         st.probe_vert_gmad, st.probe_vert_gmad * 8.0, batch_us, VLOAD_N, vb, st.probe_layer_us, with_us,
		         vert_us, 100.0 * overlap, vert_batches ? "the load keeps them" : "too little - the vertex load stays off");
	}
	st.vert_batches = vert_batches;
	if (!vert_batches)
		st.vert_ok = 0;
}

/* V0.8: do the RSX's time stamps (gfx_report) keep time? Shader tiles for about 20 ms between two of them,
   timed by the PPU watching labels before and after: both clocks must agree within 20 %. Then the load
   can be paced by them (rsx_pace.h) and they say how busy the RSX was. */
static void probe_clock(gcmContextData *ctx)
{
	double tb_ns = 1e9 / (double)sysGetTimebaseFrequency(), ppu_us, rsx_us;
	const u32 r0 = GFX_REPORT_FREE, r1 = GFX_REPORT_FREE + 1;
	uint64_t t1, t2, n0 = 0, n1 = 0, waited;
	int k = st.probe_ok ? (int)(20000.0 / st.probe_tile_us) : 8;

	if (rsx_stuck)
		return;
	k = k < 4 ? 4 : k > 1024 ? 1024 : k;
	gfx_report_index_clear(r0);
	gfx_report_index_clear(r1);
	state_common(ctx);
	set_target(ctx, tile_offset, RSXLOAD_TILE, GCM_SURFACE_X8R8G8B8);
	load_fp(ctx, P_LONG(st.variant));
	rsxSetWriteBackendLabel(ctx, LABEL_PROBE, 8);
	gfx_report_at(ctx, r0);
	for (int i = 0; i < k; i++)
		quad(ctx);
	gfx_report_at(ctx, r1);
	rsxSetWriteBackendLabel(ctx, LABEL_PROBE, 9);
	rsxFlushBuffer(ctx);
	t1 = label_time(LABEL_PROBE, 8, 2000);
	t2 = t1 ? label_time(LABEL_PROBE, 9, 3000) : 0;
	*(vu32 *)gcmGetLabelAddress(LABEL_PROBE) = 0;
	/* RPCS3 may write a report later than the label behind it */
	for (waited = 0; waited < 200 && (!n0 || !n1); waited++) {
		n0 = gfx_report_index_ns(r0);
		n1 = gfx_report_index_ns(r1);
		if (!n0 || !n1)
			usleep(1000);
	}
	if (!t1 || !t2) {
		/* the RSX stopped at the reports: nothing more may go to it (the app ends, as after a self-test step) */
		log_where("the clock check's tiles and reports did not finish");
		log_line("RSX time stamps: FAILED - the RSX did not get past them; the app ends");
		st.selftest_failed = 1;
		st.ready = 0;
		return;
	}
	if (n0 && n1)
		gfx_reports_on();
	ppu_us = (double)(t2 - t1) * tb_ns / 1000.0;
	rsx_us = n1 > n0 && n0 ? (double)(n1 - n0) / 1000.0 : 0.0;
	st.clock_ratio = ppu_us > 0.0 ? rsx_us / ppu_us : 0.0;
	st.reports_ok = st.probe_ok && rsx_us > 0.0 && st.clock_ratio > 0.8 && st.clock_ratio < 1.25;
	st.timed = st.reports_ok;
	log_line("RSX time stamps: %d shader tiles took %.0f us by the RSX's own clock, %.0f us by the PPU watching labels "
	         "(ratio %.3f; reports %s) - %s", k, rsx_us, ppu_us, st.clock_ratio, n0 && n1 ? "written" : "NOT written",
	         st.reports_ok ? "the load is paced by them" : !st.probe_ok ? "not used (no plausible timings: an emulator?)" :
	         "they do not keep time: the load is set by the probe's timings");
}

/* V1.0: the RSX did not finish something at the start: the app ends (as after a failed self-test step) */
static int stuck_end(void)
{
	if (!rsx_stuck && gfx_rewind_stalls() > 0) {
		log_line("RSX: it did not reach the command buffer's rewind in 0.5 s");
		rsx_stuck = 1;
	}
	if (!rsx_stuck)
		return 0;
	log_line("RSX: it did not finish what it was given at the start - nothing more goes to it, the app ends so the "
	         "system can reset the RSX");
	st.ready = 0;
	st.selftest_failed = 1;
	return 1;
}

void rsxload_probe(void)
{
	gcmContextData *ctx = gfx_context();
	int best = -1;

	if (!st.ready)
		return;
	if (selftest(ctx) != 0) {
		st.ready = 0;
		st.selftest_failed = 1;
		return;
	}
	*(vu32 *)gcmGetLabelAddress(LABEL_PROBE) = 0;   /* the steps left 100 + n there */
	/* each layout: its check pixel against the PPU, then its rate alone */
	for (int v = 0; v < RSXLOAD_VARIANTS; v++) {
		if (check_twice(ctx, v) != 0 || rsx_stuck) {
			stuck_end();
			return;
		}
		st.variant_ok[v] = compare_pixel(v, slot_of(&chk_alu, chk_alu.no));
		st.variant_gmad[v] = time_variant(ctx, v);
		log_line("RSX shader, %s: %s, %s", VARIANT_NAME[v], st.variant_ok[v] ? "check pixel as the PPU has it" :
		         "check pixel DIFFERENT from the PPU's", st.variant_gmad[v] > 0.0 ? "" : "rate not measured");
		if (st.variant_gmad[v] > 0.0)
			log_line("  %.2f G multiply-adds a second on four floats = %.1f GFLOP/s alone (%.1f a clock at 500 MHz)",
			         st.variant_gmad[v], st.variant_gmad[v] * 8.0, st.variant_gmad[v] / 0.5);
		if (st.variant_ok[v] && (best < 0 || st.variant_gmad[v] > st.variant_gmad[best]))
			best = v;
	}
	st.variant = best < 0 ? 0 : best;
	/* V1.0: the RSX's core clock from each layout's rate, against its multiply-adds a clock on a stock RSX at
	   500 MHz (the shader is arithmetic alone: its rate goes with the clock); the median of the layouts */
	{
		static const double PER_GHZ[RSXLOAD_VARIANTS] = { 15.60, 23.50, 46.34, 46.34 };
		double f[RSXLOAD_VARIANTS];
		int n = 0;

		for (int v = 0; v < RSXLOAD_VARIANTS; v++) {
			st.variant_mhz[v] = st.variant_gmad[v] > 0.0 ? st.variant_gmad[v] / PER_GHZ[v] * 1000.0 : 0.0;
			if (st.variant_mhz[v] > 0.0)
				f[n++] = st.variant_mhz[v];
		}
		for (int i = 1; i < n; i++)   /* sort the few */
			for (int j = i; j > 0 && f[j] < f[j - 1]; j--) {
				double t = f[j];

				f[j] = f[j - 1];
				f[j - 1] = t;
			}
		st.rsx_mhz = n ? (n % 2 ? f[n / 2] : (f[n / 2 - 1] + f[n / 2]) / 2.0) : 0.0;
		if (st.rsx_mhz < 250.0 || st.rsx_mhz > 1500.0)
			st.rsx_mhz = 0.0;
		if (st.rsx_mhz > 0.0)
			log_line("RSX core clock: %.0f MHz, measured (the layouts: %.0f, %.0f, %.0f, %.0f MHz - each one's rate alone "
			         "against its multiply-adds a clock on a stock RSX at 500 MHz)", st.rsx_mhz, st.variant_mhz[0],
			         st.variant_mhz[1], st.variant_mhz[2], st.variant_mhz[3]);
		else
			log_line("RSX core clock: not measured (no plausible shader rate)");
	}
	log_line("RSX shader: the load uses %s%s", VARIANT_NAME[st.variant], best < 0 ? " (no layout agreed with the PPU)" :
	         st.variant_gmad[st.variant] > 0.0 ? ", the fastest" : " (none was timed)");
	st.probe_gmad = st.variant_gmad[st.variant];
	st.probe_ok = st.probe_gmad > 0.0;
	st.probe_load_gmad = var_load_gmad[st.variant];
	st.probe_tile_us = var_tile_us[st.variant];
	st.probe_long_us = var_long_us[st.variant];
	st.probe_short_us = var_short_us[st.variant];
	st.probe_tiles = var_tiles[st.variant];

	/* the reference of the layout the load uses */
	if (check_twice(ctx, st.variant) != 0 || rsx_stuck) {
		stuck_end();
		return;
	}
	st.first_differed = checksum(slot_of(&chk_alu, chk_alu.no - 1)) != checksum(slot_of(&chk_alu, chk_alu.no));
	st.reference = checksum(slot_of(&chk_alu, chk_alu.no));
	chk_alu.reference = st.reference;
	memcpy(chk_alu.ref, slot_of(&chk_alu, chk_alu.no), sizeof(chk_alu.ref));
	memcpy(chk_alu.start, chk_alu.ref, sizeof(chk_alu.start));
	chk_alu.have_start = 1;
	compare_pixel(st.variant, slot_of(&chk_alu, chk_alu.no));

	gfx_rewind();
	if (st.fill_ok)
		probe_fill(ctx);
	if (stuck_end())
		return;
	gfx_rewind();
	if (st.fill_ok && st.copy_ok) {
		guard_part(GUARD_COPY);
		probe_copy(ctx);
		if (stuck_end())
			return;   /* the note stays on the VRAM copy: the next start leaves it out */
		guard_part(GUARD_RSX);
	} else {
		st.copy_ok = 0;
	}
	if (stuck_end())
		return;
	gfx_rewind();
	if (st.fill_ok && st.vert_ok)
		probe_vert(ctx);
	else
		st.vert_ok = 0;
	if (stuck_end())
		return;
	gfx_rewind();
	probe_clock(ctx);
	if (st.selftest_failed || stuck_end())
		return;
	/* V1.0: no plausible shader rate and a fill impossibly fast: an emulator */
	st.emulator = !st.probe_ok && fill_implausible;
	for (int m = 0; m < RSXMODES; m++)   /* each mode's start, for the log */
		set_mode(m, 1);
	set_mode(RSXMODE_CORE, 0);
}

/* V1.0: mode m's load - its programs, the copy's share of the second part's rows, a row's time there - and,
   without the time stamps, a picture's rows by the probe's timings: FRAME_LOAD of its two frames less the
   interface's time, split by the mode's shares (without timings - an emulator - fixed rows) */
static void set_mode(int m, int log)
{
	double share = MODE_FILL_SHARE[m], cshare = st.copy_ok ? MODE_COPY_SHARE[m] : 0.0;
	double row_f = m == RSXMODE_VRAM ? fill0_row_us : st.layer_us / RSXLOAD_FILL_H, row_c, row_m;

	run_mode = m;
	st.mode = m;
	fill_prog = m == RSXMODE_VRAM ? P_FILL(0) : fill_best;   /* VRAM without the copy: the plain fill */
	copy_prog = m == RSXMODE_BOTH ? copy_best : P_COPY(0);
	row_c = m == RSXMODE_BOTH ? st.copy_mad_row_us : st.copy_row_us;
	if (!st.fill_ok)
		share = 0.0;
	run_share = share;
	/* the copy's rows: its share of the part's time, by the rows' times (half the rows without timings) */
	if (cshare >= 1.0)
		copy_frac = 1.0;
	else if (cshare <= 0.0)
		copy_frac = 0.0;
	else if (row_f > 0.0 && row_c > 0.0) {
		double r = cshare / (1.0 - cshare) * row_f / row_c;   /* copy rows per fill row */

		copy_frac = r / (1.0 + r);
	} else {
		copy_frac = 0.5;
	}
	row_m = (1.0 - copy_frac) * row_f + copy_frac * row_c;
	st.copy_frac = copy_frac;
	st.mix_row_us = row_m;
	st.unit_s_us = st.probe_tile_us / RSXLOAD_TILE;
	st.unit_f_us = row_m;
	if (st.probe_ok && row_m > 0.0) {
		double budget = FRAME_LOAD * RSXLOAD_FRAMES * gfx_period_us() - UI_RESERVE_US;
		double rs = budget * (1.0 - share) / st.unit_s_us, rf = budget * share / row_m;
		int min_s = share < 1.0 ? RSXLOAD_TILE : 0;

		st.target_rows_s = rs < min_s ? min_s : rs > ROWS_S_MAX ? ROWS_S_MAX : (int)rs;
		st.target_rows_f = rf > ROWS_C_MAX ? ROWS_C_MAX : (int)rf;
	} else {
		st.target_rows_s = share < 1.0 ? 8 * RSXLOAD_TILE : 0;
		st.target_rows_f = !st.fill_ok ? 0 : copy_frac >= 1.0 ? COPY_W : copy_frac > 0.0 ? 2 * RSXLOAD_FILL_H :
		                   RSXLOAD_FILL_H;
	}
	st.tiles_per_picture = st.target_rows_s / (double)RSXLOAD_TILE;
	st.layers_per_picture = st.target_rows_f * (1.0 - copy_frac) / (double)RSXLOAD_FILL_H;
	if (log && st.probe_ok)
		log_line("RSX load %s, a picture (%d TV frames) by the probe's timings: %.1f shader tiles (%.1f ms), %.2f fill "
		         "layers and %.2f copy passes (%.1f ms; %.0f %% of the rows copy rows, a row %.3f us)%s", MODE_NAME[m],
		         RSXLOAD_FRAMES, st.tiles_per_picture, st.target_rows_s * st.unit_s_us / 1000.0, st.layers_per_picture,
		         st.target_rows_f * copy_frac / COPY_W, st.target_rows_f * row_m / 1000.0, 100.0 * copy_frac, row_m,
		         st.reports_ok ? " - the start; the RSX's time stamps set it from then on" : "");
}

/* ---- every picture --------------------------------------------------------------------------------------- */

static void poll(uint64_t now)
{
	u32 done = label(LABEL_LOAD);

	while ((int32_t)(done - counted) > 0) {
		int k, rs, rf, rc;

		counted++;
		k = (int)(counted % RING);
		rs = ring_rs[k];
		rf = ring_rf[k];
		rc = ring_rc[k];
		st.tiles += (uint64_t)ring_ds[k];
		st.layers += (uint64_t)ring_df[k];
		st.copy_passes += (uint64_t)ring_dc[k];
		st.rows_s += (uint64_t)rs;
		st.rows_f += (uint64_t)rf;
		st.copy_rows += (uint64_t)rc;
		st.mads += (uint64_t)rs * RSXLOAD_TILE * MADS;
		st.pixels += (uint64_t)rf * RSXLOAD_FILL_W;
		st.fmads += (uint64_t)rf * RSXLOAD_FILL_W * (uint64_t)ring_fm[k];
		st.copy_pixels += (uint64_t)rc * COPY_W;
		st.cmads += (uint64_t)rc * COPY_W * (uint64_t)ring_cm[k];
		st.vbatches += (uint64_t)ring_vb[k];
		st.vmads += (uint64_t)ring_vb[k] * VLOAD_N * VMADS;
		if (st.probe_ok)   /* only by timings that were plausible (not in an emulator) */
			st.busy_tile_us += rs * st.probe_tile_us / RSXLOAD_TILE;
		st.busy_fill_us += ring_b2[k];
		st.frames++;
		st.progress_tb = now;
	}
	verify(&chk_alu);
	if (st.fill_ok)
		verify(&chk_fill);
	if (st.vert_ok)
		verify(&chk_vert);
	if (st.vram_ok)
		verify_vram();
	if (st.copy_ok)
		verify_copy();
	st.pending = (int)(frame_no - counted);
}

static double gap_us(uint64_t from, uint64_t to)
{
	return (double)(to - from) / 1000.0;
}

/* the screen alone, before any test: how the RSX waits (logged once - which command holds it: in RPCS3 the
   flip itself, on the console the wait for the flip, V0.8's first run: 1.06 ms at the flip, 14.17 at the wait) */
static void idle_seen(double flip_wait, double wait, double draw)
{
	if (st.idle_pictures)
		return;
	idle_sum[0] += flip_wait;
	idle_sum[1] += wait;
	idle_sum[2] += draw;
	if (++idle_n < 120)
		return;
	st.idle_flip_us = idle_sum[0] / idle_n;
	st.idle_wait_us = idle_sum[1] / idle_n;
	st.idle_draw_us = idle_sum[2] / idle_n;
	st.idle_pictures = idle_n;
	log_line("RSX with the screen alone (%d pictures, by its time stamps): it waits %.2f ms at the flip command and "
	         "%.2f ms at the wait for the flip, and draws the screen in %.2f ms - %s", idle_n, st.idle_flip_us / 1000.0,
	         st.idle_wait_us / 1000.0, st.idle_draw_us / 1000.0, st.idle_flip_us > st.idle_wait_us ?
	         "the flip command holds it until the vertical blank (as in RPCS3)" :
	         "the wait for the flip holds it until the vertical blank (as on the console in V0.8)");
}

/* Flip j's time stamps, EVAL_LAG flips after it (all written by then): from reaching flip j (BEFORE) to
   reaching flip j+1 is one picture's span. In it the RSX waits at the flip (until AFTER: on the console the
   vertical blank) and between the load's end and WAITED (the wait for the flip; or, if the main thread came
   late, for its next commands); the rest it works. Its shader and fill times are the load's parts. */
static void evaluate(u32 now_cycle)
{
	const double period = gfx_period_us();
	flip_plan *f, *before;
	uint64_t a, b, sh, fi, e, w, a1, b1;
	double flip_wait, wait, work;
	u32 j;

	if (now_cycle < EVAL_LAG + 1)
		return;
	j = now_cycle - EVAL_LAG;
	f = &plans[j % CRING];
	before = &plans[(j - 1) % CRING];
	a = gfx_report_ns(j, GFX_REP_BEFORE);
	b = gfx_report_ns(j, GFX_REP_AFTER);
	sh = gfx_report_ns(j, GFX_REP_LOAD);
	fi = gfx_report_ns(j, GFX_REP_LOAD + 1);
	e = gfx_report_ns(j, GFX_REP_LOAD + 2);
	w = gfx_report_ns(j, GFX_REP_WAITED);
	a1 = gfx_report_ns(j + 1, GFX_REP_BEFORE);
	b1 = gfx_report_ns(j + 1, GFX_REP_AFTER);
	if (!a || !b || !sh || !fi || !e || !w || !a1 || !b1 || b < a || sh < b || fi < sh || e < fi || w < e || a1 < w ||
	    b1 < a1 || (double)(b1 - b) > 10000.0 * period) {
		if (f->active) {
			st.ts_bad++;
			if (st.timed && st.ts_bad >= 30 && st.ts_bad > st.ts_seen) {
				st.timed = 0;
				log_line("RSX time stamps: %llu of %llu pictures had none or impossible ones - the load is set by the "
				         "probe's timings from now on", (unsigned long long)st.ts_bad,
				         (unsigned long long)(st.ts_bad + st.ts_seen));
			}
		}
		return;
	}
	flip_wait = gap_us(a, b);
	wait = gap_us(e, w);
	work = gap_us(b, a1) - wait;
	if (!f->active) {
		if (!before->active)
			idle_seen(flip_wait, wait, gap_us(w, a1));
		return;
	}
	st.ts_seen++;
	if (!before->active)   /* its flip waited behind a picture without the load */
		return;
	st.ts_pictures++;
	st.ts_span_us += gap_us(a, a1);
	st.ts_idle_us += flip_wait + wait;
	st.ts_flip_wait_us += flip_wait;
	st.ts_tile_us += gap_us(b, sh);
	st.ts_fill_us += gap_us(sh, fi);
	/* a picture whose next commands came late from the main thread says nothing about the RSX */
	if (f->paced && st.timed && wait < 300.0) {
		pace_cycle m;

		m.phase = f->phase;
		m.checks = f->checks;
		m.rows_s = f->rows_s;
		m.rows_f = f->rows_f;
		m.target_us = f->target;
		m.work_us = work;
		m.shader_us = gap_us(b, sh);
		m.fill_us = gap_us(sh, fi);
		m.interval_us = gap_us(b, b1);
		if (pace_measured(&pc, &m))
			st.missed++;
	}
}

/* Without the time stamps: the load the probe's timings allow; when a picture came late (gfx_late_flips:
   later than its two frames, not because of the main thread) 15 % less and a ceiling 5 % under where it
   was; after CALM_PICTURES pictures without one, 5 % back up to the ceiling */
static void adapt(uint64_t now)
{
	uint32_t late = gfx_late_flips();

	if (last_hook && late != seen_late) {
		st.missed += late - seen_late;
		st.ceiling = st.scale - 0.05 < 0.3 ? 0.3 : st.scale - 0.05;
		st.scale = st.scale - 0.15 < 0.25 ? 0.25 : st.scale - 0.15;
		calm = 0;
	} else if (st.scale < st.ceiling && ++calm >= CALM_PICTURES) {
		st.scale = st.scale + 0.05 > st.ceiling ? st.ceiling : st.scale + 0.05;
		calm = 0;
	}
	seen_late = late;
	last_hook = now;
}

/* this picture's rows: by the pacing (time stamps), else by the probe's timings, else fixed */
static void plan(flip_plan *f, uint64_t now)
{
	if (st.timed) {
		f->target = pace_plan(&pc, f->phase, &f->rows_s, &f->rows_f);
		f->paced = 1;
		st.plan_us = f->rows_s * pc.unit_s + f->rows_f * pc.unit_f;
		st.margin_us = pc.margin;
		st.unit_s_us = pc.unit_s;
		st.unit_f_us = pc.unit_f;
		return;
	}
	if (st.probe_ok)
		adapt(now);
	f->rows_s = floor(st.target_rows_s * st.scale);
	f->rows_f = floor(st.target_rows_f * st.scale);
	if (run_share < 1.0 && f->rows_s < RSXLOAD_TILE)   /* V1.0: VRAM runs no shader tiles */
		f->rows_s = RSXLOAD_TILE;
	st.plan_us = f->rows_s * st.unit_s_us + f->rows_f * st.unit_f_us;
}

/* called by gfx between the flip and the wait for it: the RSX works through this after the flip, while
   the next picture is drawn */
static void frame_hook(void)
{
	gcmContextData *ctx = gfx_context();
	uint64_t now = __gettime();
	u32 cyc = gfx_cycle();
	flip_plan *f = &plans[cyc % CRING];
	int k, rs, r2, ds, df = 0, dc = 0, vb = 0, rc = 0, core;

	poll(now);
	evaluate(cyc);
	memset(f, 0, sizeof(*f));
	if (!want) {
		if (st.active)
			gfx_set_flip_frames(1);
		st.active = 0;
		last_hook = 0;
		/* the load's time stamps all at one place: the screen's own timing still reads */
		gfx_report(ctx, GFX_REP_LOAD);
		gfx_report(ctx, GFX_REP_LOAD + 1);
		gfx_report(ctx, GFX_REP_LOAD + 2);
		rsxFlushBuffer(ctx);
		return;
	}
	if (!st.active) {
		st.active = 1;
		set_mode(want_mode, 0);   /* V1.0: RSX core, VRAM or both */
		calm = 0;
		st.scale = st.ceiling = 1.0;   /* each test starts from the full load */
		st.missed = 0;
		seen_late = gfx_late_flips();
		st.progress_tb = now;
		chk_alu.rebase = chk_fill.rebase = chk_vert.rebase = 1;
		chk_alu.logged = chk_fill.logged = chk_vert.logged = 0;
		vram_logged = copy_logged = 0;
		for (int k = 0; k < PATS; k++) {   /* V1.0 (6th build): each test's own error pattern */
			const char *name = pat[k].name;

			memset(&pat[k], 0, sizeof(pat[k]));
			pat[k].name = name;
		}
		gfx_set_flip_frames(RSXLOAD_FRAMES);
		if (st.timed)
			pace_start(&pc, gfx_period_us(), RSXLOAD_FRAMES, run_share, st.unit_s_us, st.unit_f_us, ROWS_S_MAX,
			           st.fill_ok ? ROWS_C_MAX : 0);
	}
	/* V1.0: VRAM runs the copy alone - no shader tiles, no vertices, the fill only without the copy */
	core = run_mode != RSXMODE_VRAM;
	f->active = 1;
	f->phase = (int)((frame_no + 1) % CHECK_EVERY);
	f->checks = (f->phase == 0 && core) || (f->phase == 2 && st.fill_ok && copy_frac < 1.0) ||
	            (f->phase == 1 && st.vert_ok && core);
	plan(f, now);
	rs = (int)f->rows_s;
	r2 = st.fill_ok ? (int)f->rows_f : 0;

	state_common(ctx);
	set_target(ctx, tile_offset, RSXLOAD_TILE, GCM_SURFACE_X8R8G8B8);
	load_fp(ctx, P_LONG(st.variant));
	ds = shader_rows(ctx, rs);
	frame_no++;
	if (core && frame_no % CHECK_EVERY == 0)
		check_tile(ctx, P_LONG(st.variant));
	gfx_report(ctx, GFX_REP_LOAD);
	if (st.fill_ok) {
		if (r2 > 0)
			df = part2_rows(ctx, r2, &vb, &rc, &dc);
		if (copy_frac < 1.0 && frame_no % CHECK_EVERY == 2)
			fill_check(ctx);
	}
	if (st.vert_ok && core && frame_no % CHECK_EVERY == 1)
		vert_check(ctx);
	gfx_report(ctx, GFX_REP_LOAD + 1);
	vram_copy(ctx);
	if (st.copy_ok && rc > 0)
		copy_readback(ctx);
	k = (int)(frame_no % RING);
	ring_rs[k] = rs;
	ring_rf[k] = r2 - rc;
	ring_rc[k] = rc;
	ring_fm[k] = 2 * FILL_PAIRS[fill_prog - P_FILL0];
	ring_cm[k] = 2 * COPY_PAIRS[copy_prog - P_COPY0];
	ring_ds[k] = ds;
	ring_df[k] = df;
	ring_dc[k] = dc;
	ring_vb[k] = vb;
	/* by the probe's timings, if they were plausible: the fill rows' and the copy rows' time alone */
	ring_b2[k] = (st.probe_gpix > 0.0 ? (r2 - rc) * (fill_prog == fill_best ? st.probe_layer_us / RSXLOAD_FILL_H :
	                                                 fill0_row_us) : 0.0) +
	             rc * (copy_prog == P_COPY(0) ? st.copy_row_us : st.copy_mad_row_us);
	rsxSetWriteBackendLabel(ctx, LABEL_LOAD, frame_no);
	gfx_report(ctx, GFX_REP_LOAD + 2);
	rsxFlushBuffer(ctx);
	st.tiles_per_picture = rs / (double)RSXLOAD_TILE;
	st.layers_per_picture = (r2 - rc) / (double)RSXLOAD_FILL_H;
	st.copies_per_picture = rc / (double)COPY_W;
	st.pending = (int)(frame_no - counted);
}

void rsxload_set_active(int on, int mode)
{
	if (!st.ready)
		return;
	want = on;
	if (mode >= 0 && mode < RSXMODES)
		want_mode = mode;
	gfx_set_frame_hook(frame_hook);
}

void rsxload_shutdown(void)
{
	if (tfill_pitch)
		gcmSetInvalidateTile(TILE_INDEX);
	tfill_pitch = 0;
}

const rsxload_status *rsxload_get(void)
{
	return &st;
}
