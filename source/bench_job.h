/* One piece of work for an SPU and its answer. Shared by the PPU side (source/bench.c) and the SPU
   program (spu/source/main.c); both are big-endian with the same alignment, so the layout matches. */
#ifndef BENCH_JOB_H
#define BENCH_JOB_H

#include <stdint.h>

#define BENCH_EVENT_PORT   10

enum {
	BENCH_MANDEL,     /* single-precision floating point: Mandelbrot iterations */
	BENCH_CHACHA,     /* 32-bit integer: ChaCha20 blocks (add, xor, rotate) */
	BENCH_MEMORY,     /* DMA: read main memory and add it up */
	BENCH_TESTS,      /* the benchmark's three tests; the stress test's maximum loads add: */
	BENCH_HEAT = BENCH_TESTS,   /* fused multiply-adds and shuffles, both SPU pipelines every cycle */
	BENCH_HEAT_RAM,   /* the same, while the SPU's DMA unit writes a RAM region and reads it back, checked */
	BENCH_KINDS,
	BENCH_PROBE = 16  /* no load: the SPU's clock (instruction chains) and the RAM's latency */
};

/* BENCH_HEAT_RAM moves the RAM in pieces of this size; its region is a multiple of it */
#define BENCH_RAM_CHUNK   12288
/* BENCH_HEAT: the SPU's heat loop does this many steps a pass, so heat_steps is a multiple of it */
#define BENCH_HEAT_PASS   64
/* BENCH_PROBE: a chain of dependent fma (6 cycles each), timed in PROBE_BLOCKS blocks of
   PROBE_BLOCK_CYCLES chain cycles, once as long loops and once as short ones; then PROBE_RAM_READS
   single 128-byte reads from scattered places of mem_ea */
#define PROBE_FMA_CYCLES   6
#define PROBE_LONG         2048     /* fma a long loop */
#define PROBE_SHORT        256      /* fma a short loop */
#define PROBE_LONG_LOOPS   16       /* loops a block: the same chain cycles either way */
#define PROBE_SHORT_LOOPS  128
#define PROBE_BLOCK_CYCLES (PROBE_LONG * PROBE_LONG_LOOPS * PROBE_FMA_CYCLES)
#define PROBE_BLOCKS       128
#define PROBE_RAM_READS    4096
#define PROBE_LATENCY     1      /* probe_flags: measure the RAM's latency too */

enum { BENCH_OK, BENCH_BAD_JOB };

typedef struct {
	uint32_t type, seq;
	uint64_t result_ea;           /* bench_result */
	/* BENCH_MANDEL: rows row0.. of a width-wide grid, c = (x0 + x*dx, y0 + y*dy) */
	float x0, y0, dx, dy;
	uint32_t width, row0, rows, max_iter;
	/* BENCH_CHACHA: blocks block0..block0+blocks-1 of this key and nonce */
	uint32_t key[8], nonce[3];
	uint32_t block0, blocks;
	/* BENCH_MEMORY: bytes from mem_ea, both multiples of 16 KB. BENCH_HEAT_RAM: the region it writes
	   and reads back (a multiple of BENCH_RAM_CHUNK). BENCH_PROBE: where it reads (a power of two) */
	uint32_t mem_bytes;
	uint64_t mem_ea;
	/* BENCH_HEAT: heat_steps steps (a multiple of BENCH_HEAT_PASS) from start values made out of heat_seed;
	   BENCH_HEAT_RAM: as many steps as the RAM work takes, from the same start values */
	uint32_t heat_seed, heat_steps;
	/* BENCH_HEAT_RAM: the pattern's seed, new for every job; BENCH_PROBE: PROBE_LATENCY or 0 */
	uint32_t ram_seed, probe_flags;
} __attribute__((aligned(128))) bench_job;

typedef struct {
	uint64_t checksum;            /* the same for the same work, however it is split */
	uint64_t work;                /* iterations, bytes of keystream, bytes read, floating-point operations */
	uint32_t seq, status;
	uint64_t bytes;               /* BENCH_HEAT_RAM: bytes written and read back */
	uint32_t ram_errors;          /* BENCH_HEAT_RAM: pieces that came back different */
	uint32_t ticks_long, ticks_short;   /* BENCH_PROBE: decrementer ticks of the fastest block of each kind */
	uint32_t sum_long, sum_short;       /* BENCH_PROBE: ticks of all blocks of each kind */
	uint32_t ticks_ram, ram_reads;      /* BENCH_PROBE: ticks for ram_reads single reads (0: not measured) */
	/* every job, on the SPU's decrementer: how long it waited for this job after answering the one
	   before, and how long it worked on it */
	uint32_t ticks_wait, ticks_work;
	/* how long answering the job before took (writing its result, the event): that came after its
	   result had gone, so it rides along with this one */
	uint32_t ticks_answer;
	uint32_t pad[12];
} __attribute__((aligned(128))) bench_result;

/* A job reaches its SPU as its address in the SPU's inbound mailbox (4 words deep, the stress test keeps
   two jobs in it); an address of 0 ends the SPU program. The SPU answers with spu_thread_throw_event:
   spu_thread_send_event waits for lv2's reply in that same mailbox and refuses (EBUSY) while a job waits
   there.
   The answer event's data0 (24 bits): (seq & 0xFFFF) << 8 | status */
#define BENCH_ANSWER(seq, status)  ((((seq) & 0xFFFF) << 8) | ((status) & 0xFF))

#endif
