/* SPU side of TEE SPE Bench. Started once per SPU and kept running, like TEE PS3 Live's encoder: every
   word in its inbound mailbox is the address of a bench_job, the SPU does that work, puts a bench_result
   back and answers with an event; it also tells how long it waited for the job and worked on it. An
   address of 0 ends it.

   All tests are written for the SPU's 128-bit registers and keep several independent chains in
   flight, so the pipelines stay full:
     - Mandelbrot: 16 points at once (four vectors of four floats); every lane takes the next point as
       soon as its own is done, so no lane idles while its neighbours still iterate.
     - ChaCha20: 4 blocks at once, one block per vector lane, 20 rounds of add/xor/rotate; the 16- and
       8-bit rotates are byte shuffles in the odd pipeline.
     - Memory: 16 KB DMA pieces, four in flight, added up as they arrive.
     - Heat (the stress test's maximum load): fused multiply-adds in the even pipeline and byte
       shuffles in the odd one, both every cycle, without touching the local store; with RAM, the DMA
       unit meanwhile writes a region of main memory and reads it back, every quadword checked.
     - Probe: the SPU's clock from a chain of instructions whose cycles are known, the RAM's latency.
   Every checksum folds with xor and add only, so it does not depend on how the work was split. */
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_event.h>
#include <sys/spu_thread.h>

#include "bench_job.h"

#define TAG_JOB     0
#define TAG_MEM     1          /* +0..+3: the four memory pieces */
#define MEM_CHUNK   16384      /* the largest single DMA */
#define MEM_PIECES  4

static bench_job job;
static bench_result result;
static vec_uint4 mem_buf[MEM_PIECES][MEM_CHUNK / 16] __attribute__((aligned(128)));

static void wait_tag(int tag)
{
	mfc_write_tag_mask(1 << tag);
	spu_mfcstat(MFC_TAG_UPDATE_ALL);
}

static uint32_t sum_words(vec_uint4 v)
{
	return spu_extract(v, 0) + spu_extract(v, 1) + spu_extract(v, 2) + spu_extract(v, 3);
}

static uint32_t xor_words(vec_uint4 v)
{
	return spu_extract(v, 0) ^ spu_extract(v, 1) ^ spu_extract(v, 2) ^ spu_extract(v, 3);
}

/* ---- floating point ---------------------------------------------------------------------------- */

#define STEP_ITER  16                 /* iterations between two looks at which points are done */

/* One iteration of one vector, 9 instructions, all in the even pipeline:
     ii = zi*zi,  |z|^2 = zr*zr + ii,  t = zi*zi - cr,  2zr,  inside &= |z|^2 < 4,  n += inside,
     zi = 2zr*zi + ci,  zr = zr*zr - t
   "inside" stays 0 once a point is out: after a few more steps its z saturates (the SPU has no
   infinity) and zr*zr - t could come back small. */
#define ITER(ZR, ZI, CR, CI, N, IN)                                 \
	do {                                                             \
		vec_float4 ii = spu_mul(ZI, ZI);                             \
		vec_float4 mag = spu_madd(ZR, ZR, ii);                       \
		vec_float4 t = spu_msub(ZI, ZI, CR);                         \
		vec_float4 x2 = spu_add(ZR, ZR);                             \
		IN = spu_and(IN, spu_cmpgt(four, mag));                      \
		N = spu_sub(N, IN);                                          \
		ZI = spu_madd(x2, ZI, CI);                                   \
		ZR = spu_msub(ZR, ZR, t);                                    \
	} while (0)

/* the lanes of one vector that are done (out, or at max_iter) get their count added up and the
   next point; once none are left, a filler that is out at once. Written as a macro on plain
   variables: with arrays indexed in a loop the compiler kept all 16 points on the stack and every
   iteration paid loads and stores. */
#define REFILL(ZR, ZI, CR, CI, N, IN, REAL)                                     \
	do {                                                                         \
		vec_uint4 done = spu_or(spu_xor(IN, ones), spu_cmpeq(N, maxv));          \
		if (spu_extract(spu_orx(done), 0) == 0)                                  \
			break;                                                               \
		for (int l = 0; l < 4; l++) {                                            \
			float fcr = 100.0f, fci = 100.0f;                                    \
			uint32_t inside = 0;                                                 \
			if (spu_extract(done, l) == 0)                                       \
				continue;                                                        \
			if (REAL[l]) {                                                       \
				iterations += spu_extract(N, l);                                 \
				busy--;                                                          \
			}                                                                    \
			REAL[l] = left > 0;                                                  \
			if (left > 0) {                                                      \
				fcr = job.x0 + (float)col * job.dx;                              \
				fci = job.y0 + (float)(job.row0 + row) * job.dy;                 \
				inside = ~0u;                                                    \
				busy++;                                                          \
				left--;                                                          \
				if (++col == job.width) {                                        \
					col = 0;                                                     \
					row++;                                                       \
				}                                                                \
			}                                                                    \
			CR = spu_insert(fcr, CR, l);                                         \
			CI = spu_insert(fci, CI, l);                                         \
			ZR = spu_insert(fcr, ZR, l);                                         \
			ZI = spu_insert(fci, ZI, l);                                         \
			N = spu_insert(0u, N, l);                                            \
			IN = spu_insert(inside, IN, l);                                      \
		}                                                                        \
	} while (0)

/* 16 points in four vectors. Every lane works on its own point and takes the next one as soon as
   it is done - max_iter is a multiple of STEP_ITER, so a point reaches it exactly at a look. No lane
   waits for its neighbours (version 1 kept 8 points together until the last was out). */
static void mandel(void)
{
	const vec_float4 four = spu_splats(4.0f), away = spu_splats(100.0f);
	const vec_uint4 maxv = spu_splats(job.max_iter), ones = spu_splats(~0u), zero = spu_splats(0u);
	vec_float4 zr0 = away, zi0 = away, cr0 = away, ci0 = away, zr1 = away, zi1 = away, cr1 = away, ci1 = away;
	vec_float4 zr2 = away, zi2 = away, cr2 = away, ci2 = away, zr3 = away, zi3 = away, cr3 = away, ci3 = away;
	vec_uint4 n0 = zero, n1 = zero, n2 = zero, n3 = zero, in0 = zero, in1 = zero, in2 = zero, in3 = zero;
	int real0[4] = { 0 }, real1[4] = { 0 }, real2[4] = { 0 }, real3[4] = { 0 };
	uint32_t col = 0, row = 0, left = job.rows * job.width, busy = 0;
	uint64_t iterations = 0;

	for (;;) {
		REFILL(zr0, zi0, cr0, ci0, n0, in0, real0);
		REFILL(zr1, zi1, cr1, ci1, n1, in1, real1);
		REFILL(zr2, zi2, cr2, ci2, n2, in2, real2);
		REFILL(zr3, zi3, cr3, ci3, n3, in3, real3);
		if (busy == 0)
			break;
#pragma GCC unroll 16
		for (int k = 0; k < STEP_ITER; k++) {
			ITER(zr0, zi0, cr0, ci0, n0, in0);
			ITER(zr1, zi1, cr1, ci1, n1, in1);
			ITER(zr2, zi2, cr2, ci2, n2, in2);
			ITER(zr3, zi3, cr3, ci3, n3, in3);
		}
	}
	result.work = iterations;
	result.checksum = iterations;
}

/* ---- integer ----------------------------------------------------------------------------------- */

/* One quarter round on four blocks at once. Rotating a word by 16 or 8 bits only moves whole bytes,
   so shufb does it - in the odd pipeline, next to the adds, xors and the two real rotates (12, 7)
   in the even one: 10 even and 2 odd instructions instead of 12 even. */
#define QR(a, b, c, d)                                                  \
	do {                                                                 \
		vec_uint4 t_;                                                    \
		a = spu_add(a, b); t_ = spu_xor(d, a); d = spu_shuffle(t_, t_, rot16); \
		c = spu_add(c, d); b = spu_rl(spu_xor(b, c), 12);                \
		a = spu_add(a, b); t_ = spu_xor(d, a); d = spu_shuffle(t_, t_, rot8);  \
		c = spu_add(c, d); b = spu_rl(spu_xor(b, c), 7);                 \
	} while (0)

/* The state as sixteen plain variables, not an array: an array indexed in a loop puts it on the
   stack, and version 3 paid about two loads or stores per rotate for that. */
static void chacha(void)
{
	const vec_uchar16 rot16 = { 2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13 };
	const vec_uchar16 rot8 = { 1, 2, 3, 0, 5, 6, 7, 4, 9, 10, 11, 8, 13, 14, 15, 12 };
	const vec_uint4 lane = { 0, 1, 2, 3 };
	const vec_uint4 s0 = spu_splats(0x61707865u), s1 = spu_splats(0x3320646eu);   /* "expand 32-byte k" */
	const vec_uint4 s2 = spu_splats(0x79622d32u), s3 = spu_splats(0x6b206574u);
	const vec_uint4 s4 = spu_splats(job.key[0]), s5 = spu_splats(job.key[1]), s6 = spu_splats(job.key[2]);
	const vec_uint4 s7 = spu_splats(job.key[3]), s8 = spu_splats(job.key[4]), s9 = spu_splats(job.key[5]);
	const vec_uint4 s10 = spu_splats(job.key[6]), s11 = spu_splats(job.key[7]);
	const vec_uint4 s13 = spu_splats(job.nonce[0]), s14 = spu_splats(job.nonce[1]), s15 = spu_splats(job.nonce[2]);
	vec_uint4 acc_xor = spu_splats(0u), acc_add = spu_splats(0u);

	for (uint32_t b = 0; b < job.blocks; b += 4) {
		const vec_uint4 s12 = spu_add(spu_splats(job.block0 + b), lane);   /* one block counter per lane */
		vec_uint4 x0 = s0, x1 = s1, x2 = s2, x3 = s3, x4 = s4, x5 = s5, x6 = s6, x7 = s7;
		vec_uint4 x8 = s8, x9 = s9, x10 = s10, x11 = s11, x12 = s12, x13 = s13, x14 = s14, x15 = s15;

		for (int r = 0; r < 10; r++) {
			QR(x0, x4, x8, x12);
			QR(x1, x5, x9, x13);
			QR(x2, x6, x10, x14);
			QR(x3, x7, x11, x15);
			QR(x0, x5, x10, x15);
			QR(x1, x6, x11, x12);
			QR(x2, x7, x8, x13);
			QR(x3, x4, x9, x14);
		}
		x0 = spu_add(x0, s0); x1 = spu_add(x1, s1); x2 = spu_add(x2, s2); x3 = spu_add(x3, s3);
		x4 = spu_add(x4, s4); x5 = spu_add(x5, s5); x6 = spu_add(x6, s6); x7 = spu_add(x7, s7);
		x8 = spu_add(x8, s8); x9 = spu_add(x9, s9); x10 = spu_add(x10, s10); x11 = spu_add(x11, s11);
		x12 = spu_add(x12, s12); x13 = spu_add(x13, s13); x14 = spu_add(x14, s14); x15 = spu_add(x15, s15);
		/* xor and add every output word into the checksum, as trees (short chains) */
		acc_xor = spu_xor(acc_xor,
		                  spu_xor(spu_xor(spu_xor(spu_xor(x0, x1), spu_xor(x2, x3)), spu_xor(spu_xor(x4, x5), spu_xor(x6, x7))),
		                          spu_xor(spu_xor(spu_xor(x8, x9), spu_xor(x10, x11)), spu_xor(spu_xor(x12, x13), spu_xor(x14, x15)))));
		acc_add = spu_add(acc_add,
		                  spu_add(spu_add(spu_add(spu_add(x0, x1), spu_add(x2, x3)), spu_add(spu_add(x4, x5), spu_add(x6, x7))),
		                          spu_add(spu_add(spu_add(x8, x9), spu_add(x10, x11)), spu_add(spu_add(x12, x13), spu_add(x14, x15)))));
	}
	result.work = (uint64_t)job.blocks * 64;
	result.checksum = (uint64_t)xor_words(acc_xor) << 32 | sum_words(acc_add);
}

/* ---- memory ------------------------------------------------------------------------------------ */

static void memory(void)
{
	uint32_t pieces = job.mem_bytes / MEM_CHUNK;
	vec_uint4 a0 = spu_splats(0u), a1 = a0, a2 = a0, a3 = a0, x0 = a0, x1 = a0;

	for (uint32_t i = 0; i < MEM_PIECES && i < pieces; i++)
		mfc_get(mem_buf[i], job.mem_ea + (uint64_t)i * MEM_CHUNK, MEM_CHUNK, TAG_MEM + i, 0, 0);
	for (uint32_t p = 0; p < pieces; p++) {
		int b = (int)(p % MEM_PIECES);
		const vec_uint4 *v = mem_buf[b];

		wait_tag(TAG_MEM + b);
		for (int k = 0; k < MEM_CHUNK / 16; k += 4) {
			a0 = spu_add(a0, v[k]);
			a1 = spu_add(a1, v[k + 1]);
			a2 = spu_add(a2, v[k + 2]);
			a3 = spu_add(a3, v[k + 3]);
			x0 = spu_xor(x0, spu_xor(v[k], v[k + 1]));
			x1 = spu_xor(x1, spu_xor(v[k + 2], v[k + 3]));
		}
		if (p + MEM_PIECES < pieces)
			mfc_get(mem_buf[b], job.mem_ea + (uint64_t)(p + MEM_PIECES) * MEM_CHUNK, MEM_CHUNK, TAG_MEM + b, 0, 0);
	}
	result.work = job.mem_bytes;
	result.checksum = (uint64_t)xor_words(spu_xor(x0, x1)) << 32 |
	                  sum_words(spu_add(spu_add(a0, a1), spu_add(a2, a3)));
}

/* ---- heat ------------------------------------------------------------------------------------- */

/* As much work per cycle as an SPU can issue, for the stress test's maximum load.
   Even pipeline: 16 independent rotations, x += a*y and then y -= a*x - two fused multiply-adds each,
   32 a step, so the FPU (latency 6) always has more than enough independent work. The rotation keeps
   x and y on an ellipse: no value runs away or dies out, and the bits keep changing.
   Odd pipeline: 32 byte shuffles a step, each mixing the newest x or y into one of eight registers.
   Everything lives in registers - plain variables, no arrays - so the loop never loads or stores.
   The same job gives the same checksum on every SPU; a different one means a wrong result.
   HEAT_UNROLL steps make one pass of the loop. The pass's own instructions (the count, the branch and
   its hints) go to the odd pipeline next to the 32 shuffles a step, so each pass costs a few cycles
   without an fma: at 16 steps 519 cycles for 512 (98.65 %, V8.2 on the console 98.5 %), at 64 steps
   they are spread over 2048. */
#define HEAT_UNROLL  BENCH_HEAT_PASS
_Static_assert(HEAT_UNROLL == 64, "the unroll pragma in heat() says 64");

#define ROT(X, Y, A, P, Q)                                            \
	do {                                                               \
		X = spu_madd(A, Y, X);                                         \
		P = spu_shuffle(P, (vec_uchar16)Y, mix);                       \
		Y = spu_nmsub(A, X, Y);                                        \
		Q = spu_shuffle(Q, (vec_uchar16)X, mix);                       \
	} while (0)

/* one step: all 16 rotations */
#define STEP16()                                                                                   \
	do {                                                                                            \
		ROT(x0, y0, a0, p0, p1);   ROT(x1, y1, a1, p2, p3);   ROT(x2, y2, a2, p4, p5);             \
		ROT(x3, y3, a3, p6, p7);   ROT(x4, y4, a0, p1, p0);   ROT(x5, y5, a1, p3, p2);             \
		ROT(x6, y6, a2, p5, p4);   ROT(x7, y7, a3, p7, p6);   ROT(x8, y8, a0, p0, p2);             \
		ROT(x9, y9, a1, p1, p3);   ROT(x10, y10, a2, p4, p6); ROT(x11, y11, a3, p5, p7);           \
		ROT(x12, y12, a0, p2, p0); ROT(x13, y13, a1, p3, p1); ROT(x14, y14, a2, p6, p4);           \
		ROT(x15, y15, a3, p7, p5);                                                                  \
	} while (0)

/* the state of the rotations, made from the job's seed */
#define HEAT_STATE()                                                                                          \
	const vec_uchar16 mix = { 1, 17, 3, 19, 5, 21, 7, 23, 9, 25, 11, 27, 13, 29, 15, 31 };                   \
	const vec_float4 a0 = { 0.0101f, 0.0113f, 0.0127f, 0.0131f }, a1 = { 0.0137f, 0.0149f, 0.0151f, 0.0163f }; \
	const vec_float4 a2 = { 0.0167f, 0.0173f, 0.0179f, 0.0181f }, a3 = { 0.0191f, 0.0193f, 0.0197f, 0.0199f }; \
	vec_float4 x0 = start_value(0), y0 = start_value(1), x1 = start_value(2), y1 = start_value(3);           \
	vec_float4 x2 = start_value(4), y2 = start_value(5), x3 = start_value(6), y3 = start_value(7);           \
	vec_float4 x4 = start_value(8), y4 = start_value(9), x5 = start_value(10), y5 = start_value(11);         \
	vec_float4 x6 = start_value(12), y6 = start_value(13), x7 = start_value(14), y7 = start_value(15);       \
	vec_float4 x8 = start_value(16), y8 = start_value(17), x9 = start_value(18), y9 = start_value(19);       \
	vec_float4 x10 = start_value(20), y10 = start_value(21), x11 = start_value(22), y11 = start_value(23);   \
	vec_float4 x12 = start_value(24), y12 = start_value(25), x13 = start_value(26), y13 = start_value(27);   \
	vec_float4 x14 = start_value(28), y14 = start_value(29), x15 = start_value(30), y15 = start_value(31);   \
	vec_uchar16 p0 = (vec_uchar16)start_value(32), p1 = (vec_uchar16)start_value(33);                         \
	vec_uchar16 p2 = (vec_uchar16)start_value(34), p3 = (vec_uchar16)start_value(35);                         \
	vec_uchar16 p4 = (vec_uchar16)start_value(36), p5 = (vec_uchar16)start_value(37);                         \
	vec_uchar16 p6 = (vec_uchar16)start_value(38), p7 = (vec_uchar16)start_value(39)

/* every bit of the final state folded into the checksum, as trees */
#define HEAT_FOLD()                                                                                         \
	do {                                                                                                     \
		vec_uint4 fx, fa;                                                                                    \
		fx = spu_xor(spu_xor(spu_xor(spu_xor((vec_uint4)x0, (vec_uint4)y0), spu_xor((vec_uint4)x1, (vec_uint4)y1)),   \
		                     spu_xor(spu_xor((vec_uint4)x2, (vec_uint4)y2), spu_xor((vec_uint4)x3, (vec_uint4)y3))),  \
		             spu_xor(spu_xor(spu_xor((vec_uint4)x4, (vec_uint4)y4), spu_xor((vec_uint4)x5, (vec_uint4)y5)),   \
		                     spu_xor(spu_xor((vec_uint4)x6, (vec_uint4)y6), spu_xor((vec_uint4)x7, (vec_uint4)y7)))); \
		fx = spu_xor(fx, spu_xor(spu_xor(spu_xor(spu_xor((vec_uint4)x8, (vec_uint4)y8), spu_xor((vec_uint4)x9, (vec_uint4)y9)), \
		                                 spu_xor(spu_xor((vec_uint4)x10, (vec_uint4)y10), spu_xor((vec_uint4)x11, (vec_uint4)y11))), \
		                         spu_xor(spu_xor(spu_xor((vec_uint4)x12, (vec_uint4)y12), spu_xor((vec_uint4)x13, (vec_uint4)y13)), \
		                                 spu_xor(spu_xor((vec_uint4)x14, (vec_uint4)y14), spu_xor((vec_uint4)x15, (vec_uint4)y15))))); \
		fa = spu_add(spu_add(spu_add((vec_uint4)p0, (vec_uint4)p1), spu_add((vec_uint4)p2, (vec_uint4)p3)),        \
		             spu_add(spu_add((vec_uint4)p4, (vec_uint4)p5), spu_add((vec_uint4)p6, (vec_uint4)p7)));       \
		result.checksum = (uint64_t)xor_words(fx) << 32 | sum_words(fa);                                     \
	} while (0)

/* 16 rotations, two fused multiply-adds each, on four floats, two operations each */
#define HEAT_FLOPS_PER_STEP  (16 * 2 * 4 * 2)

static uint32_t hash32(uint32_t v)
{
	v ^= v >> 16;
	v *= 0x7feb352du;
	v ^= v >> 15;
	v *= 0x846ca68bu;
	v ^= v >> 16;
	return v;
}

/* four floats in [0.5, 1), made from the job's seed and n */
static vec_float4 start_value(uint32_t n)
{
	vec_uint4 v = spu_splats(0u);

	for (int l = 0; l < 4; l++)
		v = spu_insert(0x3F000000u | (hash32(job.heat_seed + n * 4 + (uint32_t)l) >> 9), v, l);
	return (vec_float4)v;
}

/* PSL1GHT's spu_rules turn on -fmodulo-sched. With the pass count in a register the loop qualifies for
   it, and its prologue, epilogue and a plain copy put the 64 steps into the program four times - too
   much for the local store, and the pass needs no pipelining: its 64 steps are all independent work. */
static void __attribute__((noinline, optimize("no-modulo-sched"))) heat(void)
{
	HEAT_STATE();

	/* the passes counted down in a register: V8.2's loop read job.heat_steps from the local store on
	   every pass (two more odd-pipeline instructions) */
	for (uint32_t pass = job.heat_steps / HEAT_UNROLL; pass != 0; pass--) {
#pragma GCC unroll 64
		for (int k = 0; k < HEAT_UNROLL; k++)
			STEP16();
	}
	HEAT_FOLD();
	result.work = (uint64_t)job.heat_steps * HEAT_FLOPS_PER_STEP;
}

/* ---- heat with RAM ---------------------------------------------------------------------------- */

/* The heat kernel's rotations, and with every step RAM_PER_STEP quadwords for the RAM. The region at
   job.mem_ea is first written - a pattern made up as it goes, put out piece by piece by the DMA unit
   while the rotations run - then read back piece by piece and compared with the same pattern. The DMA
   unit works next to the pipelines, so the SPU heats and the RAM is busy at the same time.
   The pattern: per lane a Weyl sequence (start + n * an odd step), so every bit keeps changing; it is
   new for every job (ram_seed), so a write that never arrived cannot hide behind the pass before.
   The loads and stores here are the pieces of the region on purpose - the rotations stay in registers. */
#define RAM_PER_STEP  3
#define RAM_Q         (BENCH_RAM_CHUNK / 16)             /* quadwords a piece: 768 */
#define TAG_RAM       8                                   /* +0, +1: writing; +2 .. +4: reading */

static vec_uint4 ram_buf[5][RAM_Q] __attribute__((aligned(128)));

/* the next three values of the pattern into Q[O..O+2] */
#define PUT3(Q, O)                                                       \
	do {                                                                  \
		Q[O] = spu_add(v, i1);                                            \
		Q[O + 1] = spu_add(v, i2);                                        \
		v = spu_add(v, i3);                                               \
		Q[O + 2] = v;                                                     \
	} while (0)

/* Q[O..O+2] against the next three values; any difference stays in acc */
#define CHECK3(Q, O)                                                                                      \
	do {                                                                                                   \
		acc = spu_or(acc, spu_or(spu_xor(Q[O], spu_add(e, i1)),                                            \
		                         spu_or(spu_xor(Q[O + 1], spu_add(e, i2)), spu_xor(Q[O + 2], spu_add(e, i3))))); \
		e = spu_add(e, i3);                                                                                \
	} while (0)

static vec_uint4 ram_start(void)
{
	vec_uint4 v = spu_splats(0u);

	for (int l = 0; l < 4; l++)
		v = spu_insert(hash32(job.ram_seed * 4u + (uint32_t)l + 0x51ED270Bu), v, l);
	return v;
}

static void __attribute__((noinline)) heat_ram(void)
{
	HEAT_STATE();
	const vec_uint4 i1 = { 0x9E3779B9u, 0x7F4A7C15u, 0xF39CC061u, 0x5CEDC835u };   /* odd steps */
	const vec_uint4 i2 = spu_add(i1, i1), i3 = spu_add(i2, i1);
	const uint32_t pieces = job.mem_bytes / BENCH_RAM_CHUNK;
	vec_uint4 v = ram_start(), e = v;
	uint32_t bad = 0;

	/* write: piece c is made in buffer c & 1 while the put of the piece before goes out */
	for (uint32_t c = 0; c < pieces; c++) {
		vec_uint4 *q = ram_buf[c & 1];

		wait_tag(TAG_RAM + (int)(c & 1));       /* this buffer's put from two pieces ago is out */
		for (int s = 0; s < RAM_Q; s += 8 * RAM_PER_STEP, q += 8 * RAM_PER_STEP) {
			STEP16(); PUT3(q, 0);
			STEP16(); PUT3(q, 3);
			STEP16(); PUT3(q, 6);
			STEP16(); PUT3(q, 9);
			STEP16(); PUT3(q, 12);
			STEP16(); PUT3(q, 15);
			STEP16(); PUT3(q, 18);
			STEP16(); PUT3(q, 21);
		}
		mfc_put(ram_buf[c & 1], job.mem_ea + (uint64_t)c * BENCH_RAM_CHUNK, BENCH_RAM_CHUNK, TAG_RAM + (c & 1), 0, 0);
	}
	mfc_write_tag_mask(3u << TAG_RAM);
	spu_mfcstat(MFC_TAG_UPDATE_ALL);                /* all of it written before anything is read back */

	/* read back: three pieces on the way, each compared as it arrives */
	for (uint32_t c = 0; c < 3 && c < pieces; c++)
		mfc_get(ram_buf[2 + c], job.mem_ea + (uint64_t)c * BENCH_RAM_CHUNK, BENCH_RAM_CHUNK, TAG_RAM + 2 + c, 0, 0);
	for (uint32_t c = 0; c < pieces; c++) {
		const uint32_t b = c % 3;
		const vec_uint4 *q = ram_buf[2 + b];
		vec_uint4 acc = spu_splats(0u);

		wait_tag(TAG_RAM + 2 + (int)b);
		for (int s = 0; s < RAM_Q; s += 8 * RAM_PER_STEP, q += 8 * RAM_PER_STEP) {
			STEP16(); CHECK3(q, 0);
			STEP16(); CHECK3(q, 3);
			STEP16(); CHECK3(q, 6);
			STEP16(); CHECK3(q, 9);
			STEP16(); CHECK3(q, 12);
			STEP16(); CHECK3(q, 15);
			STEP16(); CHECK3(q, 18);
			STEP16(); CHECK3(q, 21);
		}
		bad += spu_extract(spu_orx(acc), 0) != 0;
		if (c + 3 < pieces)
			mfc_get(ram_buf[2 + b], job.mem_ea + (uint64_t)(c + 3) * BENCH_RAM_CHUNK, BENCH_RAM_CHUNK,
			        TAG_RAM + 2 + b, 0, 0);
	}
	HEAT_FOLD();
	result.work = (uint64_t)pieces * 2 * (RAM_Q / RAM_PER_STEP) * HEAT_FLOPS_PER_STEP;
	result.bytes = (uint64_t)job.mem_bytes * 2;
	result.ram_errors = bad;
}

/* ---- probe: the SPU's clock and the RAM's latency --------------------------------------------- */

/* A chain of dependent fused multiply-adds takes exactly 6 cycles each. The SPU times it with its
   decrementer (the timebase) in blocks of PROBE_BLOCK_CYCLES chain cycles: once as PROBE_LONG_LOOPS
   loops of PROBE_LONG fma, once as PROBE_SHORT_LOOPS loops of PROBE_SHORT - the same chain cycles,
   but eight times as many loop ends, so the PPU can take out whatever a loop end costs. Of
   PROBE_BLOCKS blocks each, the fastest counts: anything that stops the SPU for a moment only ever
   makes a block slower. Each loop is assembler, so the compiler can neither shorten nor reorder the
   chain, and its branch is hinted the way GCC does it - 16 instructions ahead, aiming at the loop's
   first fma. */
#define TAG_PROBE  13
#define R2(S)      S S
#define R4(S)      R2(R2(S))
#define R16(S)     R4(R4(S))
#define R64(S)     R4(R16(S))
#define R256(S)    R4(R64(S))
#define R1024(S)   R4(R256(S))
#define FMA        "\tfma\t%0,%0,%2,%3\n"
#define HINT_END   "\thbrr\t2f,1b\n" R16(FMA) "\tai\t%1,%1,-1\n2:\tbrnz\t%1,1b\n"

static vec_uint4 probe_line[8] __attribute__((aligned(128)));

/* n loops of 2048 chained fma: 2032, the hint, 16 more, the decrement and the branch back. Not inlined:
   the probe's block loop would otherwise be unrolled with 8 KB of chain in every copy. */
static uint32_t __attribute__((noinline)) loops_long(uint32_t n, vec_float4 *f, vec_float4 m, vec_float4 c)
{
	vec_float4 x = *f;
	uint32_t t0, t1;

	t0 = spu_read_decrementer();
	__asm__ volatile("1:\n" R1024(FMA) R256(FMA) R256(FMA) R256(FMA) R64(FMA) R64(FMA) R64(FMA) R16(FMA) R16(FMA)
	                 R16(FMA) HINT_END
	                 : "+r"(x), "+r"(n)
	                 : "r"(m), "r"(c));
	t1 = spu_read_decrementer();
	*f = x;
	return t0 - t1;
}

/* n loops of 256 chained fma: 240, the hint, 16 more, the decrement and the branch back */
static uint32_t __attribute__((noinline)) loops_short(uint32_t n, vec_float4 *f, vec_float4 m, vec_float4 c)
{
	vec_float4 x = *f;
	uint32_t t0, t1;

	t0 = spu_read_decrementer();
	__asm__ volatile("1:\n" R64(FMA) R64(FMA) R64(FMA) R16(FMA) R16(FMA) R16(FMA) HINT_END
	                 : "+r"(x), "+r"(n)
	                 : "r"(m), "r"(c));
	t1 = spu_read_decrementer();
	*f = x;
	return t0 - t1;
}

static void __attribute__((noinline)) probe(void)
{
	const vec_float4 m = spu_splats(0.5f), c = spu_splats(0.5f);
	vec_float4 f = spu_splats(1.0f);
	uint32_t best_long = ~0u, best_short = ~0u, sum_long = 0, sum_short = 0;

	spu_write_decrementer(0xFFFFFFFFu);
	for (int b = 0; b < PROBE_BLOCKS; b++) {
		uint32_t t = loops_long(PROBE_LONG_LOOPS, &f, m, c);

		best_long = t < best_long ? t : best_long;
		sum_long += t;
		t = loops_short(PROBE_SHORT_LOOPS, &f, m, c);
		best_short = t < best_short ? t : best_short;
		sum_short += t;
	}
	result.ticks_long = best_long;
	result.ticks_short = best_short;
	result.sum_long = sum_long;
	result.sum_short = sum_short;
	result.checksum = spu_extract((vec_uint4)f, 0);

	/* single 128-byte reads from scattered places, each waiting for the one before */
	if (job.probe_flags & PROBE_LATENCY) {
		uint32_t x = job.ram_seed | 1u, lines = job.mem_bytes / 128, t0, t1;

		t0 = spu_read_decrementer();
		for (uint32_t i = 0; i < PROBE_RAM_READS; i++) {
			x = x * 1664525u + 1013904223u;
			mfc_get(probe_line, job.mem_ea + (uint64_t)((x >> 7) & (lines - 1)) * 128, 128, TAG_PROBE, 0, 0);
			wait_tag(TAG_PROBE);
		}
		t1 = spu_read_decrementer();
		result.ticks_ram = t0 - t1;
		result.ram_reads = PROBE_RAM_READS;
	}
}

int main(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
	uint32_t done, answer = 0;   /* answer: the decrementer ticks the last answer took */

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	spu_write_decrementer(0xFFFFFFFFu);
	done = spu_read_decrementer();
	for (;;) {
		uint32_t ea = spu_read_in_mbox(), start = spu_read_decrementer(), end;

		if (ea == 0)
			break;
		mfc_get(&job, ea, sizeof(job), TAG_JOB, 0, 0);
		wait_tag(TAG_JOB);
		result.checksum = result.work = result.bytes = 0;
		result.ram_errors = result.ticks_long = result.ticks_short = result.sum_long = result.sum_short = 0;
		result.ticks_ram = result.ram_reads = 0;
		result.status = BENCH_OK;
		if (job.type == BENCH_MANDEL && job.max_iter % STEP_ITER == 0)
			mandel();
		else if (job.type == BENCH_CHACHA && job.blocks % 4 == 0)
			chacha();
		else if (job.type == BENCH_MEMORY && job.mem_bytes % MEM_CHUNK == 0 && job.mem_ea % 128 == 0)
			memory();
		else if (job.type == BENCH_HEAT && job.heat_steps % HEAT_UNROLL == 0)
			heat();
		else if (job.type == BENCH_HEAT_RAM && job.mem_bytes % BENCH_RAM_CHUNK == 0 && job.mem_ea % 128 == 0)
			heat_ram();
		else if (job.type == BENCH_PROBE && job.mem_bytes >= 128 && (job.mem_bytes & (job.mem_bytes - 1)) == 0)
			probe();
		else
			result.status = BENCH_BAD_JOB;
		end = spu_read_decrementer();
		result.ticks_wait = done - start;              /* the decrementer counts down */
		result.ticks_work = start - end;
		result.ticks_answer = answer;
		result.seq = job.seq;
		mfc_put(&result, job.result_ea, sizeof(result), TAG_JOB, 0, 0);
		wait_tag(TAG_JOB);
		/* thrown, not sent: send_event would wait for lv2's reply in the inbound mailbox, and refuse while
		   the next job already waits there */
		spu_thread_throw_event(BENCH_EVENT_PORT, BENCH_ANSWER(job.seq, result.status), (uint32_t)(result.work >> 10));
		done = spu_read_decrementer();
		answer = end - done;
	}
	spu_thread_exit(0);
	return 0;
}
