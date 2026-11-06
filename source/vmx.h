/* Mandelbrot and the heat kernel on the PPU's VMX (AltiVec) unit, for the stress test's PPU threads. In a file of its own
   because altivec.h redefines "vector", "pixel" and "bool". */
#ifndef VMX_H
#define VMX_H

#include "bench_job.h"

/* a Mandelbrot job as on the SPU, 16 points at once in four vectors. Checksum: the bits of every
   point's last z (high word) and the iterations (low word); work: the iterations. */
void vmx_mandel(const bench_job *j, bench_result *r);

/* the maximum loads' PPU work: steps (a multiple of 4) of fourteen rotations on VMX from start values made
   out of seed. Checksum: the bits of the final state; work: floating-point operations. */
void vmx_heat(uint32_t seed, uint32_t steps, bench_result *r);

#endif
