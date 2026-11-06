/* SPU thread setup follows TEE PS3 Live's spu_enc.c (and PSL1GHT's samples/spu/sputhread). */
#include "bench.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <ppu-asm.h>
#include <ppu-types.h>
#include <lv2/systime.h>
#include <sys/event_queue.h>
#include <sys/systime.h>
#include <sys/spu.h>

#include "bench_log.h"
#include "spu_bin.h"
#include "vmx.h"

#define WAIT_US        2000000   /* one job never takes this long: an SPU that does not answer is lost */
#define JOBS           64        /* per test; a run goes round them as often as BENCH_SECONDS allow */

/* Mandelbrot: 2048 x 1024 over -2.2..1.0 x -1.2..1.2, 16 rows per job */
#define MANDEL_W       2048
#define MANDEL_H       1024
#define MANDEL_ITER    1024
/* ChaCha20: 1 MB of keystream per job */
#define CHACHA_BLOCKS  16384
/* memory: 8 jobs of 8 MB over a 64 MB buffer */
#define MEM_JOB        (8u * 1024 * 1024)
#define MEM_JOBS       8
#define MEM_BYTES      (MEM_JOB * MEM_JOBS)
/* heat (stress test only): 16 jobs of 2^23 steps, about 85 ms each on an SPU (the next one is already
   waiting in its mailbox, see MIX_DEPTH). RPCS3 takes about 70 times as long, so the autotest build keeps
   them at 2^20. */
#define HEAT_JOBS      16
#ifdef BENCH_AUTOTEST
#define HEAT_STEPS     (1u << 20)
#else
#define HEAT_STEPS     (1u << 23)
#endif
_Static_assert(HEAT_STEPS % BENCH_HEAT_PASS == 0, "the SPU's heat loop does whole passes");
/* heat with RAM: 12 regions of 3 MB (256 pieces of 12 KB), each job writes one and reads it back (about
   2 ms); the other five SPUs hold up to two each (MIX_DEPTH), so one is always free */
#define RAM_REGION     (256u * BENCH_RAM_CHUNK)
#define RAM_REGIONS    12
/* the PPU's heat (VMX): 16 jobs a thread of 2^17 steps, about 2.5 ms each */
#define PPU_HEAT_JOBS  16
#define PPU_HEAT_STEPS (1u << 17)
#define RAM_BYTES      (RAM_REGION * RAM_REGIONS)

/* The SPUs run in two groups: the first GROUP_A_SPUS together, the last one alone at a lower priority. A
   group only ever runs as a whole, and V8.1 found all six SPUs standing still 4.2 % of the time, their
   timers with them. Split (V8.2 on the console), the five worked 100 % of the time and only the single
   one stopped, once per job for about 2 ms: the system needs one SPU now and then, and with one group of
   six it had to stop them all. */
#define GROUP_A_SPUS   5
#define GROUP_A_PRIO   100
#define GROUP_B_PRIO   101   /* a larger number is a lower priority: this group gives way first */
_Static_assert(GROUP_A_SPUS > 0 && GROUP_A_SPUS < BENCH_MAX_SPUS, "two groups");

static sysSpuImage image;
static sys_spu_group_t groups[2];
static int ngroups;              /* groups running: 2, or 1 if the second could not be had */
static s32 group_b_err;          /* why not */
static sys_spu_thread_t threads[BENCH_MAX_SPUS];
static double tb_mhz;            /* the timebase, which the SPU decrementers count at too */
static sys_event_queue_t queue;
static bench_job jobs[BENCH_MAX_SPUS];
static bench_result results[BENCH_MAX_SPUS];
static int started, hung;

static uint32_t *mem, *ram;
static uint64_t mem_ref[MEM_JOBS], chacha_ref[JOBS], mandel_ref[JOBS], vmx_ref[JOBS], vmx_total, heat_ref[HEAT_JOBS];
static uint64_t heatram_ref[HEAT_JOBS], ppu_heat_ref[2 * PPU_HEAT_JOBS];
static int chacha_ready, mandel_known[JOBS], vmx_ready, heat_known[HEAT_JOBS], heatram_known[HEAT_JOBS];
static int ppu_heat_known[2 * PPU_HEAT_JOBS];

static volatile int progress, cancel;
static uint32_t seq;

static const uint32_t KEY[8] = { 0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c,
                                 0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c };
static const uint32_t NONCE[3] = { 0x09000000, 0x4a000000, 0x00000000 };

const char *bench_name(int test)
{
	static const char *const NAMES[BENCH_KINDS] = { "FLOAT", "INTEGER", "MEMORY", "HEAT", "RAM" };
	return test >= 0 && test < BENCH_KINDS ? NAMES[test] : "?";
}

const char *bench_unit(int test)
{
	static const char *const UNITS[BENCH_KINDS] = { "M ITER/S", "MB/S", "MB/S", "GFLOP/S", "MB/S" };
	return test >= 0 && test < BENCH_KINDS ? UNITS[test] : "";
}

/* ---- the work, described once --------------------------------------------------------------------- */

static void make_job(bench_job *j, int test, int index)
{
	memset(j, 0, sizeof(*j));
	j->type = (uint32_t)test;
	if (test == BENCH_MANDEL) {
		j->x0 = -2.2f;
		j->y0 = -1.2f;
		j->dx = 3.2f / MANDEL_W;
		j->dy = 2.4f / MANDEL_H;
		j->width = MANDEL_W;
		j->rows = MANDEL_H / JOBS;
		j->row0 = (uint32_t)index * j->rows;
		j->max_iter = MANDEL_ITER;
	} else if (test == BENCH_CHACHA) {
		memcpy(j->key, KEY, sizeof(KEY));
		memcpy(j->nonce, NONCE, sizeof(NONCE));
		j->block0 = (uint32_t)index * CHACHA_BLOCKS;
		j->blocks = CHACHA_BLOCKS;
	} else if (test == BENCH_HEAT) {
		j->heat_seed = 0x9E3779B9u * (uint32_t)(index + 1);
		j->heat_steps = HEAT_STEPS;
	} else if (test == BENCH_HEAT_RAM) {
		/* the region and the pattern's seed come from the stress test's dispatcher (mix_send) */
		j->heat_seed = 0x9E3779B9u * (uint32_t)(index % HEAT_JOBS + 1);
		j->mem_ea = (uint64_t)(uintptr_t)ram + (uint64_t)(index % RAM_REGIONS) * RAM_REGION;
		j->mem_bytes = RAM_REGION;
	} else {
		j->mem_ea = (uint64_t)(uintptr_t)mem + (uint64_t)(index % MEM_JOBS) * MEM_JOB;
		j->mem_bytes = MEM_JOB;
	}
}

static int jobs_of(int test)
{
	return test == BENCH_MEMORY ? MEM_JOBS : test == BENCH_HEAT || test == BENCH_HEAT_RAM ? HEAT_JOBS : JOBS;
}

/* ---- the same work on the PPU ---------------------------------------------------------------------- */

static void ppu_mandel(const bench_job *j, bench_result *r)
{
	uint64_t n = 0;

	for (uint32_t y = 0; y < j->rows; y++) {
		float ci = j->y0 + (float)(j->row0 + y) * j->dy;
		for (uint32_t x = 0; x < j->width; x++) {
			float cr = j->x0 + (float)x * j->dx, zr = cr, zi = ci;
			uint32_t i;
			for (i = 0; i < j->max_iter; i++) {
				float rr = zr * zr, ii = zi * zi;
				if (rr + ii >= 4.0f)
					break;
				zi = 2.0f * zr * zi + ci;
				zr = rr - ii + cr;
			}
			n += i;
		}
	}
	r->work = r->checksum = n;
}

#define ROTL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d)                                   \
	do {                                                  \
		a += b; d = ROTL(d ^ a, 16);                      \
		c += d; b = ROTL(b ^ c, 12);                      \
		a += b; d = ROTL(d ^ a, 8);                       \
		c += d; b = ROTL(b ^ c, 7);                       \
	} while (0)

static void ppu_chacha(const bench_job *j, bench_result *r)
{
	uint32_t s[16], acc_xor = 0, acc_add = 0;

	s[0] = 0x61707865u;
	s[1] = 0x3320646eu;
	s[2] = 0x79622d32u;
	s[3] = 0x6b206574u;
	memcpy(s + 4, j->key, 32);
	memcpy(s + 13, j->nonce, 12);
	for (uint32_t b = 0; b < j->blocks; b++) {
		uint32_t x[16];
		s[12] = j->block0 + b;
		memcpy(x, s, sizeof(x));
		for (int k = 0; k < 10; k++) {
			QR(x[0], x[4], x[8], x[12]);
			QR(x[1], x[5], x[9], x[13]);
			QR(x[2], x[6], x[10], x[14]);
			QR(x[3], x[7], x[11], x[15]);
			QR(x[0], x[5], x[10], x[15]);
			QR(x[1], x[6], x[11], x[12]);
			QR(x[2], x[7], x[8], x[13]);
			QR(x[3], x[4], x[9], x[14]);
		}
		for (int k = 0; k < 16; k++) {
			uint32_t out = x[k] + s[k];
			acc_xor ^= out;
			acc_add += out;
		}
	}
	r->work = (uint64_t)j->blocks * 64;
	r->checksum = (uint64_t)acc_xor << 32 | acc_add;
}

static void ppu_memory(const bench_job *j, bench_result *r)
{
	const uint32_t *p = (const uint32_t *)(uintptr_t)j->mem_ea;
	uint32_t acc_xor = 0, acc_add = 0;

	for (uint32_t i = 0; i < j->mem_bytes / 4; i += 4) {
		acc_add += p[i] + p[i + 1] + p[i + 2] + p[i + 3];
		acc_xor ^= p[i] ^ p[i + 1] ^ p[i + 2] ^ p[i + 3];
	}
	r->work = j->mem_bytes;
	r->checksum = (uint64_t)acc_xor << 32 | acc_add;
}

static void ppu_job(const bench_job *j, bench_result *r)
{
	r->status = BENCH_OK;
	if (j->type == BENCH_MANDEL)
		ppu_mandel(j, r);
	else if (j->type == BENCH_CHACHA)
		ppu_chacha(j, r);
	else
		ppu_memory(j, r);
}

/* ---- set-up ----------------------------------------------------------------------------------------- */

/* SPU thread group g with n threads, threads[first] on, connected to the queue and started; -1 with the
   error in *err (and nothing left of the group) if any of that fails */
static int start_group(int g, int first, int n, u32 prio, const char *name, s32 *err)
{
	sysSpuThreadGroupAttribute gattr = { 8 + 1, name, 0, { 0 } };
	sysSpuThreadAttribute tattr = { "teebench", 8 + 1, SPU_THREAD_ATTR_NONE };
	sysSpuThreadArgument arg = { 0, 0, 0, 0 };
	int connected = 0;
	s32 rc = sysSpuThreadGroupCreate(&groups[g], (u32)n, prio, &gattr);

	if (rc != 0) {
		*err = rc;
		return -1;
	}
	for (int i = 0; i < n && rc == 0; i++) {
		sys_spu_thread_t *t = &threads[first + i];

		rc = sysSpuThreadInitialize(t, groups[g], (u32)i, &image, &tattr, &arg);   /* numbered within its group */
		if (rc != 0)
			break;
		sysSpuThreadSetConfiguration(*t, SPU_SIGNAL1_OVERWRITE | SPU_SIGNAL2_OVERWRITE);
		rc = sysSpuThreadConnectEvent(*t, queue, SPU_THREAD_EVENT_USER, BENCH_EVENT_PORT);
		if (rc == 0)
			connected++;
	}
	if (rc == 0)
		rc = sysSpuThreadGroupStart(groups[g]);
	if (rc != 0) {
		for (int i = 0; i < connected; i++)
			sysSpuThreadDisconnectEvent(threads[first + i], SPU_THREAD_EVENT_USER, BENCH_EVENT_PORT);
		sysSpuThreadGroupDestroy(groups[g]);
		*err = rc;
		return -1;
	}
	return 0;
}

int bench_start(void)
{
	sys_event_queue_attr_t qattr = { SYS_EVENT_QUEUE_FIFO, SYS_EVENT_QUEUE_PPU, "teebench" };
	s32 err = 0;

	/* a known pattern, so the PPU can tell what the SPUs must add up to */
	mem = memalign(128, MEM_BYTES);
	if (!mem)
		return -1;
	ram = memalign(128, RAM_BYTES);   /* without it the stress test has no MAX+RAM, the rest runs */
	for (uint32_t i = 0; i < MEM_BYTES / 4; i++)
		mem[i] = i * 2654435761u ^ (i >> 7);

	tb_mhz = (double)sysGetTimebaseFrequency() / 1e6;
	sysSpuInitialize(BENCH_MAX_SPUS, 0);
	if (sysSpuImageImport(&image, spu_bin, 0) != 0)
		return -1;
	if (sysEventQueueCreate(&queue, &qattr, SYS_EVENT_QUEUE_KEY_LOCAL, 16) != 0)
		return -1;
	if (start_group(0, 0, GROUP_A_SPUS, GROUP_A_PRIO, "teebench", &err) != 0)
		return -1;
	ngroups = 1;
	started = GROUP_A_SPUS;
	/* without the second group the first runs on its own */
	if (start_group(1, GROUP_A_SPUS, BENCH_MAX_SPUS - GROUP_A_SPUS, GROUP_B_PRIO, "teebenc2", &err) == 0) {
		ngroups = 2;
		started = BENCH_MAX_SPUS;
	} else {
		group_b_err = err;
	}
	return 0;
}

void bench_layout(char *buf, size_t len)
{
	if (ngroups == 2)
		snprintf(buf, len, "in two groups: %d together, %d alone at a lower priority", GROUP_A_SPUS,
		         BENCH_MAX_SPUS - GROUP_A_SPUS);
	else
		snprintf(buf, len, "in one group - the second group could not be had (error 0x%08x)", (unsigned)group_b_err);
}

void bench_stop(void)
{
	u32 cause, status;

	if (started) {
		/* an SPU that stopped answering would never read the 0 in its mailbox, and the join would wait
		   for ever: then the groups are terminated */
		if (hung)
			for (int g = 0; g < ngroups; g++)
				sysSpuThreadGroupTerminate(groups[g], 0);
		else
			for (int i = 0; i < started; i++)
				sysSpuThreadWriteMb(threads[i], 0);
		for (int g = 0; g < ngroups; g++)
			sysSpuThreadGroupJoin(groups[g], &cause, &status);
		for (int i = 0; i < started; i++)
			sysSpuThreadDisconnectEvent(threads[i], SPU_THREAD_EVENT_USER, BENCH_EVENT_PORT);
		for (int g = 0; g < ngroups; g++)
			sysSpuThreadGroupDestroy(groups[g]);
		sysEventQueueDestroy(queue, 0);
		sysSpuImageClose(&image);
		started = ngroups = 0;
	}
	free(mem);
	free(ram);
	mem = ram = NULL;
}

int bench_spus(void)
{
	return started;
}

int bench_jobs(int test)
{
	return jobs_of(test);
}

int bench_spu_hung(void)
{
	return hung;
}

int bench_ram_ready(void)
{
	return ram != NULL;
}

void bench_prepare(void)
{
	bench_job j;
	bench_result r;

	if (!mem)
		return;
	for (int i = 0; i < MEM_JOBS; i++) {
		make_job(&j, BENCH_MEMORY, i);
		ppu_memory(&j, &r);
		mem_ref[i] = r.checksum;
	}
	for (int i = 0; i < JOBS; i++) {
		make_job(&j, BENCH_CHACHA, i);
		ppu_chacha(&j, &r);
		chacha_ref[i] = r.checksum;
		progress = (i + 1) * 1000 / JOBS;
	}
	chacha_ready = 1;
}

int bench_mandel_compare(uint64_t *spu, uint64_t *ppu)
{
	bench_job j;
	bench_result r;

	*spu = *ppu = 0;
	for (int i = 0; i < JOBS; i++) {
		if (!mandel_known[i])
			return 0;
		*spu += mandel_ref[i];
	}
	for (int i = 0; i < JOBS; i++) {
		make_job(&j, BENCH_MANDEL, i);
		ppu_mandel(&j, &r);
		*ppu += r.checksum;
	}
	return 1;
}

/* ---- running ---------------------------------------------------------------------------------------- */

int bench_progress(void)
{
	return progress;
}

void bench_cancel(void)
{
	cancel = 1;
}

/* the first SPU result of a job becomes its reference; every later one must be the same */
static int learn(uint64_t *ref, int *known, uint64_t checksum)
{
	if (!*known) {
		*ref = checksum;
		*known = 1;
		return 1;
	}
	return checksum == *ref;
}

/* 1 right, 0 wrong, -1 nothing to compare with */
static int verify(int test, int index, uint64_t checksum, int on_spu)
{
	if (test == BENCH_MEMORY)
		return checksum == mem_ref[index % MEM_JOBS];
	if (test == BENCH_CHACHA)
		return chacha_ready ? checksum == chacha_ref[index] : -1;
	/* floats: the SPU rounds towards zero, the PPU to nearest - only SPU results are compared,
	   with the first SPU result for that job */
	if (!on_spu)
		return -1;
	if (test == BENCH_HEAT)
		return learn(&heat_ref[index], &heat_known[index], checksum);
	if (test == BENCH_HEAT_RAM)
		return learn(&heatram_ref[index % HEAT_JOBS], &heatram_known[index % HEAT_JOBS], checksum);
	return learn(&mandel_ref[index], &mandel_known[index], checksum);
}

static void count(bench_score *s, int test, int index, const bench_result *r, int on_spu)
{
	int ok = r->status == BENCH_OK ? verify(test, index, r->checksum, on_spu) : 0;

	s->work += r->work;
	s->jobs++;
	if (ok == 0)
		s->errors++;
	if (ok >= 0)
		s->checked = 1;
}

static void finish(bench_score *s, int test, uint64_t t0)
{
	s->seconds = (double)(sysGetSystemTime() - t0) / 1e6;
	if (s->seconds <= 0)
		s->seconds = 1e-6;
	s->rate = (double)s->work / s->seconds / (test == BENCH_MANDEL ? 1e6 : 1048576.0);
	/* points: Mandelbrot 1 per million iterations/s, ChaCha20 1 per MB/s, memory 1 per 10 MB/s */
	s->points = (uint32_t)(test == BENCH_MEMORY ? s->rate / 10.0 : s->rate);
	s->ran = 1;
}

static int run_ppu(int test, bench_score *s)
{
	uint64_t t0 = sysGetSystemTime(), end = t0 + BENCH_SECONDS * 1000000ull;
	int n = jobs_of(test);
	bench_job j;
	bench_result r;

	for (int i = 0; !cancel; i = (i + 1) % n) {
		make_job(&j, test, i);
		ppu_job(&j, &r);
		count(s, test, i, &r, 0);
		uint64_t now = sysGetSystemTime();
		progress = (int)((now - t0) * 1000 / (end - t0));
		if (now >= end)
			break;
	}
	finish(s, test, t0);
	return cancel ? -1 : 0;
}

static int run_spus(int test, int spus, bench_score *s)
{
	uint64_t t0 = sysGetSystemTime(), end = t0 + BENCH_SECONDS * 1000000ull;
	int n = jobs_of(test), next = 0, busy = 0, index[BENCH_MAX_SPUS];
	sys_event_t event;

	/* answers from a run that was given up must not count for this one */
	while (sysEventQueueReceive(queue, &event, 1) == 0)
		;
	seq++;
	for (int k = 0; k < spus; k++) {
		index[k] = next;
		make_job(&jobs[k], test, next);
		next = (next + 1) % n;
		jobs[k].seq = seq;
		jobs[k].result_ea = (uint64_t)(uintptr_t)&results[k];
	}
	__asm__ volatile("sync" ::: "memory");   /* the jobs are in memory before an SPU reads them */
	for (int k = 0; k < spus; k++)
		sysSpuThreadWriteMb(threads[k], (u32)(uintptr_t)&jobs[k]);
	busy = spus;

	while (busy > 0) {
		int k;

		if (sysEventQueueReceive(queue, &event, WAIT_US) != 0) {
			hung = 1;
			return -1;   /* an SPU stopped answering: this run is lost */
		}
		for (k = 0; k < spus; k++)
			if (threads[k] == (sys_spu_thread_t)event.data_1)
				break;
		if (k == spus || ((event.data_2 & 0xFFFFFF) >> 8) != (seq & 0xFFFF))
			continue;
		__asm__ volatile("sync" ::: "memory");
		count(s, test, index[k], &results[k], 1);

		uint64_t now = sysGetSystemTime();
		progress = (int)((now - t0) * 1000 / (end - t0));
		if (now >= end || cancel) {
			busy--;
			continue;
		}
		index[k] = next;
		make_job(&jobs[k], test, next);
		next = (next + 1) % n;
		jobs[k].seq = seq;
		jobs[k].result_ea = (uint64_t)(uintptr_t)&results[k];
		__asm__ volatile("sync" ::: "memory");
		sysSpuThreadWriteMb(threads[k], (u32)(uintptr_t)&jobs[k]);
	}
	finish(s, test, t0);
	return cancel ? -1 : 0;
}

int bench_run(int test, int spus, bench_score *out)
{
	memset(out, 0, sizeof(*out));
	cancel = 0;
	progress = 0;
	if (test < 0 || test >= BENCH_TESTS || !mem || spus < 0 || spus > started)
		return -1;
	return spus == 0 ? run_ppu(test, out) : run_spus(test, spus, out);
}

/* ---- the stress test's SPU side: every SPU busy, the three tests mixed or all on heat ------------------- */

/* Every SPU has MIX_DEPTH jobs out: when it finishes one, the next is already in its mailbox, and the PPU
   refills while it works. (V8 tried two and lost answers: the SPU answered with send_event, which refuses
   while a word waits in the inbound mailbox. Now it throws its answers - see bench_job.h.) The inbound
   mailbox holds 4 words; with RAM_REGIONS and the event queue (16) sized for it, any depth up to 2 works. */
#define MIX_DEPTH    2
/* in the stress test an SPU counts as hung after this long without an answer: far beyond any job on the
   console, and long enough for RPCS3, which translates a big SPU function on its first call */
#define MIX_WAIT_US  10000000

static bench_job mjobs[BENCH_MAX_SPUS][MIX_DEPTH];
static bench_result mresults[BENCH_MAX_SPUS][MIX_DEPTH];
static int mtest[BENCH_MAX_SPUS][MIX_DEPTH], mindex[BENCH_MAX_SPUS][MIX_DEPTH], mregion[BENCH_MAX_SPUS][MIX_DEPTH];
static uint32_t mseq[BENCH_MAX_SPUS][MIX_DEPTH];
static int mhead[BENCH_MAX_SPUS], mcount[BENCH_MAX_SPUS], mfirst[BENCH_MAX_SPUS];
static uint64_t mlast[BENCH_MAX_SPUS];   /* when the SPU last answered, or got its first jobs */
static uint64_t mtb[BENCH_MAX_SPUS];     /* the same on the timebase, for bench_mix_counts.ticks_span */
static int mix_next[BENCH_KINDS], mix_shift, mix_out, mix_stopping, mix_all = -1, ram_next;
static uint32_t ram_pass, mix_seq;
static int mix_reports;   /* dispatcher troubles logged so far (only the first few) */

int bench_mix_test(int spu)
{
	return mix_all >= 0 ? mix_all : (spu + mix_shift) % BENCH_TESTS;
}

void bench_mix_shift(int shift)
{
	mix_shift = shift;
}

/* the next RAM region no other SPU has a job on (jobs of the same SPU run one after the other) */
static int free_region(int k)
{
	for (int tries = 0; tries < RAM_REGIONS; tries++) {
		int r = ram_next, used = 0;

		ram_next = (ram_next + 1) % RAM_REGIONS;
		for (int o = 0; o < started; o++)
			for (int q = 0; q < mcount[o]; q++) {
				int s = (mhead[o] + q) % MIX_DEPTH;
				if (o != k && mtest[o][s] == BENCH_HEAT_RAM && mregion[o][s] == r)
					used = 1;
			}
		if (!used)
			return r;
	}
	return k;   /* not reached: there are more regions than the other SPUs can hold */
}

/* one more job into SPU k's mailbox, of the test it works on now */
static void mix_send(int k)
{
	int s = (mhead[k] + mcount[k]) % MIX_DEPTH, test = bench_mix_test(k);
	bench_job *j = &mjobs[k][s];

	mtest[k][s] = test;
	mindex[k][s] = mix_next[test];
	mix_next[test] = (mix_next[test] + 1) % jobs_of(test);
	make_job(j, test, mindex[k][s]);
	if (test == BENCH_HEAT_RAM) {
		mregion[k][s] = free_region(k);
		j->mem_ea = (uint64_t)(uintptr_t)ram + (uint64_t)mregion[k][s] * RAM_REGION;
		j->ram_seed = ++ram_pass;   /* a new pattern every time */
	}
	j->seq = mseq[k][s] = ++mix_seq;
	j->result_ea = (uint64_t)(uintptr_t)&mresults[k][s];
	__asm__ volatile("sync" ::: "memory");   /* the job is in memory before the SPU reads it */
	mcount[k]++;
	mix_out++;
	{
		s32 rc = sysSpuThreadWriteMb(threads[k], (u32)(uintptr_t)j);

		if (rc != 0 && mix_reports++ < 8)
			log_line("Stress dispatcher: the mailbox of SPU %d refused job %u (error 0x%08x, %d queued)", k,
			         (unsigned)mseq[k][s], (unsigned)rc, mcount[k]);
	}
}

int bench_mix_start(int all)
{
	sys_event_t event;
	uint64_t now = (uint64_t)sysGetSystemTime();

	if (!started || !mem || (all == BENCH_HEAT_RAM && !ram))
		return -1;
	/* a run that ended early (an SPU did not answer in time) may have left jobs in the mailboxes: they
	   come back first, or the new run would take their answers for its own */
	if (mix_out > 0) {
		bench_mix_counts left;

		memset(&left, 0, sizeof(left));
		mix_stopping = 1;
		while (mix_out > 0)
			if (bench_mix_wait(MIX_WAIT_US, &left) < 0)
				return -1;
	}
	while (sysEventQueueReceive(queue, &event, 1) == 0)   /* nothing left over from a run given up */
		;
	mix_shift = mix_out = mix_stopping = 0;
	mix_reports = 0;
	mix_all = all;
	memset(mix_next, 0, sizeof(mix_next));
	for (int k = 0; k < started; k++) {
		mhead[k] = mcount[k] = 0;
		mfirst[k] = 1;
		mlast[k] = now;
		mtb[k] = __gettime();
	}
	for (int d = 0; d < MIX_DEPTH; d++)
		for (int k = 0; k < started; k++)
			mix_send(k);
	return 0;
}

/* -1 once an SPU with jobs has not answered for longer than any job takes */
static int mix_hang(bench_mix_counts *c)
{
	uint64_t now = (uint64_t)sysGetSystemTime();

	for (int k = 0; k < started; k++)
		if (mcount[k] && now - mlast[k] > MIX_WAIT_US) {
			c->hung_spu = k;
			hung = 1;
			return -1;
		}
	return 0;
}

int bench_mix_wait(uint32_t timeout_us, bench_mix_counts *c)
{
	sys_event_t event;
	const bench_result *r;
	int k, s, test, ok;

	if (mix_out == 0)
		return 0;
	if (sysEventQueueReceive(queue, &event, timeout_us) != 0)
		return mix_hang(c);
	for (k = 0; k < started; k++)
		if (threads[k] == (sys_spu_thread_t)event.data_1)
			break;
	if (k == started || !mcount[k] || ((event.data_2 & 0xFFFFFF) >> 8) != (mseq[k][mhead[k]] & 0xFFFF)) {
		if (mix_reports++ < 8)
			log_line("Stress dispatcher: unexpected answer from SPU %d: number %u, expected %u (%d queued)",
			         k < started ? k : -1, (unsigned)((event.data_2 & 0xFFFFFF) >> 8),
			         k < started ? (unsigned)(mseq[k][mhead[k]] & 0xFFFF) : 0u, k < started ? mcount[k] : -1);
		return mix_hang(c);
	}
	__asm__ volatile("sync" ::: "memory");
	s = mhead[k];
	r = &mresults[k][s];
	mhead[k] = (s + 1) % MIX_DEPTH;
	mcount[k]--;
	mix_out--;
	mlast[k] = (uint64_t)sysGetSystemTime();

	/* how long the SPU worked, waited and answered (not before its first job: that wait was the start,
	   and the answer it carries belongs to whatever ran before) - and how much time went by meanwhile */
	{
		static const double EDGES_US[BENCH_UNSEEN_BUCKETS - 1] = { -50, 50, 200, 500, 1000, 2000, 5000, 10000, 50000 };
		uint64_t tb = __gettime(), gone = tb - mtb[k];

		c->ticks_span += gone;
		c->spu_span[k] += gone;
		mtb[k] = tb;
		if (!mfirst[k]) {
			int64_t unseen = (int64_t)gone - (int64_t)r->ticks_wait - (int64_t)r->ticks_work - (int64_t)r->ticks_answer;
			double us = (double)unseen / tb_mhz;
			int b = 0;

			while (b < BENCH_UNSEEN_BUCKETS - 1 && us >= EDGES_US[b])
				b++;
			c->unseen_hist[b]++;
			if (unseen > c->unseen_max)
				c->unseen_max = unseen;
		}
	}
	c->ticks_work += r->ticks_work;
	c->spu_work[k] += r->ticks_work;
	c->spu_jobs[k]++;
	if (!mfirst[k]) {
		c->ticks_wait += r->ticks_wait;
		c->ticks_answer += r->ticks_answer;
	}
	mfirst[k] = 0;

	test = mtest[k][s];
	ok = r->status == BENCH_OK ? verify(test, mindex[k][s], r->checksum, 1) : 0;
	if (test == BENCH_HEAT_RAM) {
		/* the rotations count as heat, the region as RAM - each with its own errors */
		c->jobs[BENCH_HEAT]++;
		c->work[BENCH_HEAT] += r->work;
		c->jobs[BENCH_HEAT_RAM]++;
		c->work[BENCH_HEAT_RAM] += r->bytes;
		if (ok == 0) {
			c->errors[BENCH_HEAT]++;
			c->bad_spu = k;
			c->bad_test = BENCH_HEAT;
			c->bad_job = mindex[k][s];
		}
		if (r->status == BENCH_OK && r->ram_errors) {
			c->errors[BENCH_HEAT_RAM] += r->ram_errors;
			c->bad_spu = k;
			c->bad_test = BENCH_HEAT_RAM;
			c->bad_job = mregion[k][s];
		}
	} else {
		c->jobs[test]++;
		c->work[test] += r->work;
		if (ok == 0) {
			c->errors[test]++;
			c->bad_spu = k;
			c->bad_test = test;
			c->bad_job = mindex[k][s];
		}
	}
	if (!mix_stopping)
		mix_send(k);
	return mix_hang(c) < 0 ? -1 : 1;
}

int bench_mix_finish(bench_mix_counts *c)
{
	mix_stopping = 1;
	while (mix_out > 0)
		if (bench_mix_wait(MIX_WAIT_US, c) < 0)
			return -1;
	return 0;
}

/* ---- the probe: the SPUs' clock and the RAM's latency ----------------------------------------------- */

int bench_probe(bench_probe_result *out)
{
	sys_event_t event;
	const double tb = (double)sysGetTimebaseFrequency();
	double sum = 0.0;

	memset(out, 0, sizeof(*out));
	if (!started || !mem || tb <= 0.0)
		return -1;
	while (sysEventQueueReceive(queue, &event, 1) == 0)   /* nothing left over from a run given up */
		;
	/* one SPU after the other: each measures undisturbed by the rest */
	for (int k = 0; k < started; k++) {
		seq++;
		memset(&jobs[k], 0, sizeof(jobs[k]));
		jobs[k].type = BENCH_PROBE;
		jobs[k].seq = seq;
		jobs[k].result_ea = (uint64_t)(uintptr_t)&results[k];
		jobs[k].mem_ea = (uint64_t)(uintptr_t)mem;
		jobs[k].mem_bytes = MEM_BYTES;                 /* 64 MB: a power of two */
		jobs[k].ram_seed = 0x2545F491u + (uint32_t)k;
		jobs[k].probe_flags = k == 0 ? PROBE_LATENCY : 0;
		__asm__ volatile("sync" ::: "memory");
		sysSpuThreadWriteMb(threads[k], (u32)(uintptr_t)&jobs[k]);
		for (;;) {
			if (sysEventQueueReceive(queue, &event, WAIT_US) != 0) {
				hung = 1;
				return -1;
			}
			if (threads[k] == (sys_spu_thread_t)event.data_1 && ((event.data_2 & 0xFFFFFF) >> 8) == (seq & 0xFFFF))
				break;
		}
	}
	__asm__ volatile("sync" ::: "memory");

	/* a block is PROBE_BLOCK_CYCLES chain cycles plus PROBE_LONG_LOOPS or PROBE_SHORT_LOOPS loop ends (e
	   cycles each); in ticks: tl = (C + nl e) tb / f, ts = (C + ns e) tb / f, so
	   f = C tb / (tl - (ts - tl) nl / (ns - nl)), whatever a loop end costs */
	for (int k = 0; k < started; k++) {
		const bench_result *r = &results[k];
		const double nl = PROBE_LONG_LOOPS, ns = PROBE_SHORT_LOOPS;
		double tl = r->ticks_long, ts = r->ticks_short, d = tl - (ts - tl) * nl / (ns - nl), mhz;

		if (r->status != BENCH_OK || tl <= 0.0 || d <= 0.0)
			continue;
		mhz = PROBE_BLOCK_CYCLES * tb / d / 1e6;
		if (!out->spus || mhz < out->mhz_min)
			out->mhz_min = mhz;
		if (!out->spus || mhz > out->mhz_max)
			out->mhz_max = mhz;
		sum += mhz;
		out->spus++;
		if (k == 0) {
			double al = (double)r->sum_long / PROBE_BLOCKS, as = (double)r->sum_short / PROBE_BLOCKS;
			double da = al - (as - al) * nl / (ns - nl);

			out->mhz_avg = da > 0.0 ? PROBE_BLOCK_CYCLES * tb / da / 1e6 : 0.0;
			out->loop_end_cycles = (ts - tl) / (ns - nl) * mhz * 1e6 / tb;
			if (r->ram_reads)
				out->latency_ns = (double)r->ticks_ram / r->ram_reads / tb * 1e9;
		}
	}
	if (!out->spus)
		return -1;
	out->mhz = sum / out->spus;
	return 0;
}

/* ---- the stress test's PPU side ----------------------------------------------------------------------- */

int bench_vmx_prepare(const volatile int *stop)
{
	bench_job j;
	bench_result r;
	uint64_t total = 0;

	if (vmx_ready)
		return 0;
	for (int i = 0; i < JOBS; i++) {
		if (*stop)
			return -1;
		make_job(&j, BENCH_MANDEL, i);
		vmx_mandel(&j, &r);
		vmx_ref[i] = r.checksum;
		total += r.work;
		progress = (i + 1) * 1000 / JOBS;
	}
	vmx_total = total;
	vmx_ready = 1;
	return 0;
}

uint64_t bench_vmx_total(void)
{
	return vmx_total;
}

uint64_t bench_ppu_chacha_job(int index, int *ok)
{
	bench_job j;
	bench_result r;

	make_job(&j, BENCH_CHACHA, index);
	ppu_chacha(&j, &r);
	*ok = chacha_ready && r.checksum == chacha_ref[index];
	return r.work;
}

uint64_t bench_ppu_vmx_job(int index, int *ok)
{
	bench_job j;
	bench_result r;

	make_job(&j, BENCH_MANDEL, index);
	vmx_mandel(&j, &r);
	*ok = vmx_ready && r.checksum == vmx_ref[index];
	return r.work;
}

/* the maximum loads' PPU work: thread t (0, 1) goes round its own PPU_HEAT_JOBS jobs, so the references
   it learns are its own */
uint64_t bench_ppu_heat_job(int thread, int index, int *ok)
{
	bench_result r;
	int i = (thread & 1) * PPU_HEAT_JOBS + index % PPU_HEAT_JOBS;

	vmx_heat(0x85EBCA6Bu * (uint32_t)(i + 1), PPU_HEAT_STEPS, &r);
	*ok = learn(&ppu_heat_ref[i], &ppu_heat_known[i], r.checksum);
	return r.work;
}
