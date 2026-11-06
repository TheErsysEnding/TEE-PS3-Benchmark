/* The benchmark itself: six SPU threads that stay up, three tests, and the PPU doing the same work for
   comparison. Every test runs for BENCH_SECONDS and hands its jobs round to the SPUs it may use, so the
   result is work per second however many SPUs there are. Every job's checksum is compared with a
   reference; a wrong one counts as an error.
   The CPU full-load stress test (stress.c) uses the same jobs: bench_mix_* keeps every SPU busy with a
   mix of them - or, for its maximum loads, with BENCH_HEAT or BENCH_HEAT_RAM - and bench_ppu_*_job
   gives the two PPU threads theirs. bench_probe measures the SPUs' clock and the RAM's latency. */
#ifndef BENCH_H
#define BENCH_H

#include <stddef.h>
#include <stdint.h>

#include "bench_job.h"

#define BENCH_MAX_SPUS   6
/* bench_mix_counts.unseen_hist, in microseconds: below -50 | -50..50 | 50..200 | 200..500 | 500..1000 |
   1000..2000 | 2000..5000 | 5000..10000 | 10000..50000 | 50000 and more */
#define BENCH_UNSEEN_BUCKETS 10
#define BENCH_SECONDS    3

typedef struct {
	int ran;
	double seconds;
	uint64_t work;          /* iterations, bytes */
	uint32_t jobs, errors;  /* jobs done; of those, wrong checksums */
	int checked;            /* 0: nothing to compare with (the PPU's floats round differently) */
	double rate;            /* million iterations/s, MB/s, MB/s */
	uint32_t points;
} bench_score;

/* SPU threads and the work buffers; 0 if all is well. Without SPUs the PPU tests still run. */
int  bench_start(void);
void bench_stop(void);
int  bench_spus(void);
/* how the SPU threads are grouped, for the log ("in two groups: ...") */
void bench_layout(char *buf, size_t len);

/* the references: ChaCha20 keystream and memory sums worked out on the PPU (about a second) */
void bench_prepare(void);

/* one test with spus SPUs (0: the PPU), blocking for BENCH_SECONDS. 0 if it ran. */
int bench_run(int test, int spus, bench_score *out);

/* 0..1000 through the test that is running */
int  bench_progress(void);
/* makes a running test end early; its score is not counted */
void bench_cancel(void);

/* the whole Mandelbrot picture as the SPUs counted it (once every job ran on an SPU) and as the PPU
   counts it - they differ by rounding only. 0 if the SPUs have not done every job yet. Takes the PPU
   several seconds. */
int bench_mandel_compare(uint64_t *spu, uint64_t *ppu);

const char *bench_name(int test);
const char *bench_unit(int test);
int bench_jobs(int test);

/* 1 once an SPU stopped answering, in a test or in the stress test: at the end its group is then
   terminated instead of asked to stop */
int bench_spu_hung(void);

/* 1 if the RAM test's 36 MB could be had (without them the stress test has no MAX+RAM) */
int bench_ram_ready(void);

/* ---- the probe ------------------------------------------------------------------------------------- */

typedef struct {
	int spus;                       /* SPUs that measured */
	double mhz, mhz_min, mhz_max;   /* the SPU clock against the timebase from each SPU's fastest blocks:
	                                   mean, lowest, highest SPU */
	double mhz_avg;                 /* SPU 0 from all its blocks: lower if something stopped it now and then */
	double loop_end_cycles;         /* SPU 0: what one loop end of the chain costs, taken out of mhz */
	double latency_ns;              /* one 128-byte read from the RAM (SPU 0) */
} bench_probe_result;

/* every SPU in turn times a chain of fma against the timebase (fastest of PROBE_BLOCKS blocks), SPU 0
   also 4096 single reads from the RAM; about 0.2 s. Only while no test runs. 0 if it worked. */
int bench_probe(bench_probe_result *out);

/* ---- for the stress test --------------------------------------------------------------------------- */

typedef struct {
	uint64_t work[BENCH_KINDS];               /* iterations, bytes of keystream, bytes read, flops, RAM bytes */
	uint32_t jobs[BENCH_KINDS], errors[BENCH_KINDS];
	int bad_spu, bad_test, bad_job;           /* the latest wrong answer (BENCH_HEAT_RAM: bad_job is the region) */
	/* the SPUs' decrementer, all SPUs together: working on jobs, waiting for the next, answering (result
	   out, event thrown) - not before an SPU's first job of this run */
	uint64_t ticks_work, ticks_wait, ticks_answer;
	/* the PPU's timebase (same rate): from each SPU's first job to its latest answer, summed. What the
	   three above leave of it, the SPU timer did not see. */
	uint64_t ticks_span;
	uint64_t spu_work[BENCH_MAX_SPUS], spu_span[BENCH_MAX_SPUS];   /* the same per SPU */
	uint32_t spu_jobs[BENCH_MAX_SPUS];
	/* per answer after an SPU's first: the time the PPU's clock saw go by since that SPU's answer before,
	   less what the SPU timer accounted for in between - the time the SPU stood still, give or take how
	   late the PPU got the two answers - counted in BENCH_UNSEEN_BUCKETS; and the largest, in timebase ticks */
	uint32_t unseen_hist[BENCH_UNSEEN_BUCKETS];
	int64_t unseen_max;
	int hung_spu;                             /* the SPU that stopped answering */
} bench_mix_counts;

/* hands every SPU its first job; SPU k works on test bench_mix_test(k): with all < 0 the three tests
   mixed, else every SPU on that one (BENCH_HEAT, BENCH_HEAT_RAM: the first result per job becomes its
   reference; BENCH_HEAT_RAM counts its rotations under BENCH_HEAT and its RAM under BENCH_HEAT_RAM).
   -1 without SPUs (or, for BENCH_HEAT_RAM, without the RAM test's memory). */
int  bench_mix_start(int all);
/* mixed: from its next job on, SPU k works on test (k + shift) % BENCH_TESTS */
void bench_mix_shift(int shift);
int  bench_mix_test(int spu);
/* waits up to timeout_us for an answer, checks and counts it into c and gives that SPU its next job.
   1: an answer was counted, 0: none came, -1: an SPU has not answered for 10 s (c->hung_spu) */
int  bench_mix_wait(uint32_t timeout_us, bench_mix_counts *c);
/* hands out no more jobs and collects the ones still out; -1 if an SPU does not answer */
int  bench_mix_finish(bench_mix_counts *c);

/* The PPU's VMX unit rounds in its own way, so its Mandelbrot needs references of its own: every job
   once on the idle PPU (about a second; bench_progress() counts). -1 if *stop ended it early. */
int      bench_vmx_prepare(const volatile int *stop);
uint64_t bench_vmx_total(void);          /* iterations of the whole picture on VMX */
/* one job on the PPU, checked against its reference: returns the work (bytes, iterations), *ok = 1 if
   the checksum was right */
uint64_t bench_ppu_chacha_job(int index, int *ok);
uint64_t bench_ppu_vmx_job(int index, int *ok);
/* the maximum loads' PPU work for hardware thread 0 or 1: rotations on VMX, as hot as it gets; the first
   result of each job becomes its reference. Returns floating-point operations. */
uint64_t bench_ppu_heat_job(int thread, int index, int *ok);

#endif
