/* The VMX unit rounds differently from the SPU (and from the PPU's scalar FPU), so its results are only
   compared with themselves: bench.c works out every job once on the idle PPU and checks every later run
   against that. */
#include "vmx.h"

#include <altivec.h>

/* GCC's first scheduling pass interleaves the 16 unrolled iterations so eagerly that 20 vectors of
   state plus their temporaries no longer fit the 32 vector registers: the loop then spilled to the
   stack (about 125 lvx/stvx per 16 iterations). Without it the loop keeps everything in registers -
   check with ppu-objdump -d after every change. */
#pragma GCC optimize ("no-schedule-insns")

#define STEP_ITER  16   /* iterations between two looks whether all 16 points are out */

/* One iteration of one vector, VMX has no plain multiply, so a*b is a*b + (-0):
     ii = zi*zi,  |z|^2 = zr*zr + ii,  t = cr - zi*zi,  2zr,  inside &= |z|^2 < 4,  n += inside,
     zi = 2zr*zi + ci,  zr = zr*zr + t   - only where inside: a point that is out keeps its last z
   (vec_sel), so its count stops, its z stays finite, and that z goes into the checksum. */
#define VMX_ITER(ZR, ZI, CR, N, IN)                                          \
	do {                                                                     \
		vector float ii_ = vec_madd(ZI, ZI, nzero);                          \
		vector float mag_ = vec_madd(ZR, ZR, ii_);                           \
		vector float t_ = vec_nmsub(ZI, ZI, CR);                             \
		vector float x2_ = vec_add(ZR, ZR);                                  \
		IN = vec_and(IN, (vector unsigned int)vec_cmpgt(four, mag_));        \
		N = vec_sub(N, IN);                                                  \
		ZI = vec_sel(ZI, vec_madd(x2_, ZI, ci), IN);                         \
		ZR = vec_sel(ZR, vec_madd(ZR, ZR, t_), IN);                          \
	} while (0)

/* the bits of a final z, zi turned so that zr == zi does not cancel out */
#define ZBITS(ZR, ZI)  vec_xor((vector unsigned int)(ZR), vec_rl((vector unsigned int)(ZI), r13))

static vector float splat(float f)
{
	return (vector float){ f, f, f, f };
}

static uint32_t rotl(uint32_t v, int n)
{
	return (v << n) | (v >> (32 - n));
}

void vmx_mandel(const bench_job *j, bench_result *r)
{
	const vector float four = splat(4.0f), eight = splat(8.0f), twelve = splat(12.0f), nzero = splat(-0.0f);
	const vector float lane = (vector float){ 0.0f, 1.0f, 2.0f, 3.0f };
	const vector float x0 = splat(j->x0), dx = splat(j->dx);
	const vector unsigned int zero = vec_splat_u32(0), ones = vec_splat_u32(-1);
	const vector unsigned int r1 = vec_splat_u32(1), r13 = vec_splat_u32(13);
	vector unsigned int acc_n = zero, acc_z = zero;
	union { vector unsigned int v; uint32_t u[4]; } n, z;
	uint64_t iterations;

	/* non-Java mode (denormals count as 0) in whatever thread runs this, so every run rounds alike */
	vec_mtvscr(vec_or(vec_mfvscr(), (vector unsigned short){ 0, 0, 0, 0, 0, 0, 1, 0 }));

	for (uint32_t y = 0; y < j->rows; y++) {
		const vector float ci = splat(j->y0 + (float)(j->row0 + y) * j->dy);

		for (uint32_t x = 0; x + 16 <= j->width; x += 16) {
			const vector float fx = vec_add(splat((float)x), lane);
			const vector float cr0 = vec_madd(fx, dx, x0), cr1 = vec_madd(vec_add(fx, four), dx, x0);
			const vector float cr2 = vec_madd(vec_add(fx, eight), dx, x0), cr3 = vec_madd(vec_add(fx, twelve), dx, x0);
			vector float zr0 = cr0, zr1 = cr1, zr2 = cr2, zr3 = cr3, zi0 = ci, zi1 = ci, zi2 = ci, zi3 = ci;
			vector unsigned int n0 = zero, n1 = zero, n2 = zero, n3 = zero;
			vector unsigned int in0 = ones, in1 = ones, in2 = ones, in3 = ones;

			for (uint32_t it = 0; it < j->max_iter; it += STEP_ITER) {
#pragma GCC unroll 16
				for (int k = 0; k < STEP_ITER; k++) {
					VMX_ITER(zr0, zi0, cr0, n0, in0);
					VMX_ITER(zr1, zi1, cr1, n1, in1);
					VMX_ITER(zr2, zi2, cr2, n2, in2);
					VMX_ITER(zr3, zi3, cr3, n3, in3);
				}
				if (vec_all_eq(vec_or(vec_or(in0, in1), vec_or(in2, in3)), zero))
					break;
			}
			acc_n = vec_add(acc_n, vec_add(vec_add(n0, n1), vec_add(n2, n3)));
			acc_z = vec_add(vec_rl(acc_z, r1),
			                vec_add(vec_add(ZBITS(zr0, zi0), ZBITS(zr1, zi1)), vec_add(ZBITS(zr2, zi2), ZBITS(zr3, zi3))));
		}
	}
	n.v = acc_n;
	z.v = acc_z;
	iterations = (uint64_t)n.u[0] + n.u[1] + n.u[2] + n.u[3];
	r->status = BENCH_OK;
	r->work = iterations;
	r->checksum = (uint64_t)(z.u[0] ^ rotl(z.u[1], 8) ^ rotl(z.u[2], 16) ^ rotl(z.u[3], 24)) << 32 |
	              (uint32_t)iterations;
}

/* ---- heat on VMX ------------------------------------------------------------------------------ */

/* The PPU's share of the maximum loads: rotations x += a*y, y -= a*x like the SPU's heat kernel,
   fourteen of them in 28 of the 32 vector registers. Each rotation is a chain - y needs the new x, the
   next x the new y - so only the chains side by side keep the VMX unit's fused multiply-add pipeline
   full. V8.2's ten chains a thread reached 81 % of the peak with both hardware threads on it, about
   what ten chains give if the pipeline is 12 deep and the other thread hides none of it; fourteen
   leave room. A step first moves every x, then every y, so each result is needed 14 instructions
   later. Everything stays in the vector registers: 28 for the chains, 2 for the factors - which only
   holds if each value is updated in place, see vmx_heat. */
#define VX(X, Y, A)  X = vec_madd(A, Y, X)
#define VY(X, Y, A)  Y = vec_nmsub(A, X, Y)
#define VSTEP()                                                                                   \
	do {                                                                                           \
		VX(x0, y0, a0); VX(x1, y1, a1); VX(x2, y2, a0); VX(x3, y3, a1); VX(x4, y4, a0);             \
		VX(x5, y5, a1); VX(x6, y6, a0); VX(x7, y7, a1); VX(x8, y8, a0); VX(x9, y9, a1);             \
		VX(x10, y10, a0); VX(x11, y11, a1); VX(x12, y12, a0); VX(x13, y13, a1);                     \
		VY(x0, y0, a0); VY(x1, y1, a1); VY(x2, y2, a0); VY(x3, y3, a1); VY(x4, y4, a0);             \
		VY(x5, y5, a1); VY(x6, y6, a0); VY(x7, y7, a1); VY(x8, y8, a0); VY(x9, y9, a1);             \
		VY(x10, y10, a0); VY(x11, y11, a1); VY(x12, y12, a0); VY(x13, y13, a1);                     \
	} while (0)

static uint32_t mix32(uint32_t v)
{
	v ^= v >> 16;
	v *= 0x7feb352du;
	v ^= v >> 15;
	v *= 0x846ca68bu;
	v ^= v >> 16;
	return v;
}

/* four floats in [0.5, 1) from seed and n */
static vector float heat_start(uint32_t seed, uint32_t n)
{
	union {
		vector float v;
		uint32_t u[4];
	} x;

	for (int l = 0; l < 4; l++)
		x.u[l] = 0x3F000000u | (mix32(seed + n * 4 + (uint32_t)l) >> 9);
	return x.v;
}

/* Without the instruction scheduler: the order above already puts 14 instructions between a result and
   its use. Its first pass moved the steps into each other, kept old and new values alive at once and
   ran out of the 32 registers - one spilled to the stack and loaded straight back, which the PPU
   punishes (load-hit-store); its second pass moved a few uses up to 4 instructions behind their
   results at the loop's seam. */
void __attribute__((optimize("no-schedule-insns", "no-schedule-insns2"))) vmx_heat(uint32_t seed, uint32_t steps,
                                                                                bench_result *r)
{
	const vector float a0 = { 0.0101f, 0.0113f, 0.0127f, 0.0131f }, a1 = { 0.0137f, 0.0149f, 0.0151f, 0.0163f };
	vector float x0 = heat_start(seed, 0), y0 = heat_start(seed, 1), x1 = heat_start(seed, 2), y1 = heat_start(seed, 3);
	vector float x2 = heat_start(seed, 4), y2 = heat_start(seed, 5), x3 = heat_start(seed, 6), y3 = heat_start(seed, 7);
	vector float x4 = heat_start(seed, 8), y4 = heat_start(seed, 9), x5 = heat_start(seed, 10), y5 = heat_start(seed, 11);
	vector float x6 = heat_start(seed, 12), y6 = heat_start(seed, 13), x7 = heat_start(seed, 14), y7 = heat_start(seed, 15);
	vector float x8 = heat_start(seed, 16), y8 = heat_start(seed, 17), x9 = heat_start(seed, 18), y9 = heat_start(seed, 19);
	vector float x10 = heat_start(seed, 20), y10 = heat_start(seed, 21), x11 = heat_start(seed, 22), y11 = heat_start(seed, 23);
	vector float x12 = heat_start(seed, 24), y12 = heat_start(seed, 25), x13 = heat_start(seed, 26), y13 = heat_start(seed, 27);
	vector unsigned int f;
	union {
		vector unsigned int v;
		uint32_t u[4];
	} out;

	/* denormals as zero, as the SPU has them */
	vec_mtvscr(vec_or(vec_mfvscr(), (vector unsigned short){ 0, 0, 0, 0, 0, 0, 1, 0 }));
	for (uint32_t s = 0; s < steps; s += 4) {
#pragma GCC unroll 4
		for (int k = 0; k < 4; k++)
			VSTEP();
	}
	f = vec_xor(vec_xor(vec_xor((vector unsigned int)x0, (vector unsigned int)y0), vec_xor((vector unsigned int)x1, (vector unsigned int)y1)),
	            vec_xor(vec_xor((vector unsigned int)x2, (vector unsigned int)y2), vec_xor((vector unsigned int)x3, (vector unsigned int)y3)));
	f = vec_xor(f, vec_xor(vec_xor(vec_xor((vector unsigned int)x4, (vector unsigned int)y4), vec_xor((vector unsigned int)x5, (vector unsigned int)y5)),
	                       vec_xor(vec_xor((vector unsigned int)x6, (vector unsigned int)y6), vec_xor((vector unsigned int)x7, (vector unsigned int)y7))));
	f = vec_xor(f, vec_xor(vec_xor((vector unsigned int)x8, (vector unsigned int)y8), vec_xor((vector unsigned int)x9, (vector unsigned int)y9)));
	f = vec_xor(f, vec_xor(vec_xor(vec_xor((vector unsigned int)x10, (vector unsigned int)y10), vec_xor((vector unsigned int)x11, (vector unsigned int)y11)),
	                       vec_xor(vec_xor((vector unsigned int)x12, (vector unsigned int)y12), vec_xor((vector unsigned int)x13, (vector unsigned int)y13))));
	out.v = f;
	r->checksum = (uint64_t)(out.u[0] ^ rotl(out.u[1], 8)) << 32 | (out.u[2] ^ rotl(out.u[3], 16));
	/* fourteen rotations, two fused multiply-adds each, on four floats, two operations each */
	r->work = (uint64_t)steps * 14 * 2 * 4 * 2;
	r->status = BENCH_OK;
}
