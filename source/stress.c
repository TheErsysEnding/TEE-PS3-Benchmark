#include "stress.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ppu-asm.h>
#include <ppu-types.h>
#include <lv2/systime.h>
#include <sys/systime.h>
#include <sys/thread.h>

#include "bench.h"
#include "bench_log.h"
#include "ntp.h"
#include "rsx_load.h"
#include "webman.h"

#define LOAD_PRIO       3000      /* below everything else, webMAN's threads too: the load gets what is left */
#define PROBE_PRIO      500       /* above everything of this app and webMAN's: the PPU probe runs alone */
#define PROBE_JOBS      16        /* heat jobs a PPU probe thread does, each one timed */
#define HEAT_BUCKETS    8         /* a load thread's heat job against the probe's: see heat_time */
#define LOAD_STACK      0x10000
#define SHIFT_EVERY_S   10        /* mixed: the SPU tests move on one SPU this often */
#define LOG_EVERY_S     60
#define LOG_ERRORS      20        /* wrong results logged one by one; after that only counted */
#define NTP_WAIT_US     10000000  /* the longest wait for the time server's last answers */
#define RSX_HANG_US     3000000   /* the RSX load finished nothing this long: it stopped */

_Static_assert((int)PART_SPU_HEAT == (int)BENCH_HEAT && (int)PART_SPU_RAM == (int)BENCH_HEAT_RAM &&
               (int)PART_PPU_1 == (int)BENCH_KINDS, "the SPU parts follow the bench tests");
_Static_assert(STRESS_SPUS == BENCH_MAX_SPUS, "one entry per SPU");
_Static_assert(STRESS_UNSEEN_BUCKETS == BENCH_UNSEEN_BUCKETS, "the same buckets");

static stress_status st;
static uint64_t rsx_base_mads, rsx_base_tiles, rsx_base_errors, rsx_base_checks, rsx_base_pixels, rsx_base_layers,
                rsx_base_ferrors, rsx_base_fchecks, rsx_base_slices, rsx_base_verrors, rsx_base_pictures, rsx_base_fmads,
                rsx_base_vmads, rsx_base_vbatches, rsx_base_vchecks, rsx_base_vert_errors;
static double rsx_base_tile_us, rsx_base_fill_us, rsx_base_span, rsx_base_idle, rsx_base_flip_wait, rsx_base_ts_tile,
              rsx_base_ts_fill;

/* the loads that keep the Cell busy, those whose SPUs and PPU threads run the heat kernels, those with the
   RAM and those with the RSX */
static int load_cpu(int load)
{
	return load != STRESS_RSX;
}

static int load_heat(int load)
{
	return load == STRESS_MAX || load == STRESS_MAX_RAM || load == STRESS_MAX_RSX || load == STRESS_ALL;
}

static int load_ram(int load)
{
	return load == STRESS_MAX_RAM || load == STRESS_ALL;
}

static int load_rsx(int load)
{
	return load == STRESS_RSX || load == STRESS_MAX_RSX || load == STRESS_ALL;
}

int stress_load_cpu(int load)
{
	return load_cpu(load);
}

int stress_load_ram(int load)
{
	return load_ram(load);
}

int stress_load_rsx(int load)
{
	return load_rsx(load);
}

static int ppu_part(int part)
{
	return part == PART_PPU_1 || part == PART_PPU_2;
}

int stress_rsx_wanted(void)
{
	return load_rsx(st.load) && st.phase == STRESS_RUNNING;
}

/* the screen's frames while the load runs: the main thread adds, finish() reads once the load is off */
static volatile uint64_t screen_busy, screen_busy_drawn;
static volatile uint32_t screen_frames, screen_drawn, screen_sleeps_first, screen_sleeps_last;

void stress_screen_frame(uint64_t busy_ticks, uint32_t sleeps, int drawn)
{
	if (st.phase != STRESS_RUNNING)
		return;
	if (!screen_frames)
		screen_sleeps_first = sleeps;
	screen_busy += busy_ticks;
	if (drawn) {
		screen_busy_drawn += busy_ticks;
		screen_drawn++;
	}
	screen_sleeps_last = sleeps;
	screen_frames++;
}
static volatile int stop, load_stop;

/* written by the two PPU load threads only */
static volatile uint64_t load_work[2], load_jobs[2];
static volatile uint32_t load_errors[2];

/* the maximum loads' PPU heat jobs on the timebase, per load thread (written by that thread only), and a
   job of the PPU probe's with both threads and nothing else on the PPU (0 if the probe did not measure) */
static volatile uint64_t heat_ticks[2], heat_min[2], heat_max[2];
static volatile uint32_t heat_hist[2][HEAT_BUCKETS];
static uint64_t heat_ref;

const stress_status *stress_get(void)
{
	return &st;
}

void stress_cancel(void)
{
	stop = 1;
}

/* ---- the parts -------------------------------------------------------------------------------------- */

/* what a PPU thread runs: in the mixed load thread 1 ChaCha20 and thread 2 Mandelbrot on VMX, in the
   maximum loads both the VMX heat kernel */
enum { PPU_CHACHA, PPU_MANDEL, PPU_HEAT };

static int ppu_kind(int part, int load)
{
	if (load != STRESS_MIXED)
		return PPU_HEAT;
	return part == PART_PPU_2 ? PPU_MANDEL : PPU_CHACHA;
}

const char *stress_load_name(int load)
{
	static const char *const NAMES[STRESS_LOADS] = { "MIXED", "MAX", "MAX+RAM", "RSX", "MAX+RSX", "ALL" };

	return load >= 0 && load < STRESS_LOADS ? NAMES[load] : "?";
}

const char *stress_load_title(int load)
{
	static const char *const NAMES[STRESS_LOADS] = { "Mixed", "CPU max", "CPU + RAM", "RSX", "CPU + RSX", "Everything" };

	return load >= 0 && load < STRESS_LOADS ? NAMES[load] : "?";
}

int stress_part_used(int part, int load)
{
	if (part == PART_RSX)
		return load_rsx(load);
	if (part == PART_RSX_FILL)
		return load_rsx(load) && rsxload_get()->fill_ok;
	if (part == PART_RSX_VERT)
		return load_rsx(load) && rsxload_get()->vert_ok;
	if (!load_cpu(load))
		return 0;
	if (ppu_part(part))
		return 1;
	if (load_heat(load))
		return part == PART_SPU_HEAT || (part == PART_SPU_RAM && load_ram(load));
	return part >= 0 && part < PART_SPU_HEAT;
}

const char *stress_part_name(int part, int load)
{
	static const char *const NAMES[PART_PPU_1] = {
		"Float (Mandelbrot)", "Integer (ChaCha20)", "Memory (DMA)", "Heat (FMA + shuffle)", "RAM (write + read back)"
	};

	if (part == PART_RSX)
		return "Heat (shader MAD)";
	if (part == PART_RSX_FILL)
		return "Fill (texture + blend)";
	if (part == PART_RSX_VERT)
		return "Vertices (MAD)";
	if (ppu_part(part)) {
		int kind = ppu_kind(part, load);
		return kind == PPU_HEAT ? "Heat (VMX FMA)" : kind == PPU_MANDEL ? "Float (VMX Mandelbrot)" :
		       "Integer (ChaCha20)";
	}
	return part >= 0 && part < PART_PPU_1 ? NAMES[part] : "?";
}

static void log_name(char *buf, size_t len, int part, int load)
{
	static const char *const NAMES[PART_PPU_1] = {
		"SPU float (Mandelbrot)", "SPU integer (ChaCha20)", "SPU memory (DMA)", "SPU heat (FMA + shuffle)",
		"SPU RAM (DMA write + read back)"
	};

	if (part == PART_RSX) {
		snprintf(buf, len, "RSX heat (pixel multiply-adds)");
	} else if (part == PART_RSX_FILL) {
		snprintf(buf, len, "RSX fill (texture + blend, VRAM check)");
	} else if (part == PART_RSX_VERT) {
		snprintf(buf, len, "RSX vertices (vertex multiply-adds)");
	} else if (ppu_part(part)) {
		int kind = ppu_kind(part, load);
		snprintf(buf, len, "PPU thread %d %s", part - PART_PPU_1 + 1, kind == PPU_HEAT ? "heat (VMX FMA)" :
		         kind == PPU_MANDEL ? "float (Mandelbrot VMX)" : "integer (ChaCha20 scalar)");
	}
	else
		snprintf(buf, len, "%s", NAMES[part]);
}

int stress_part_unit(int part, int load)
{
	switch (part) {
	case PART_SPU_FLOAT:
		return UNIT_ITER;
	case PART_SPU_INT:
		return UNIT_MB;
	case PART_SPU_MEM:
	case PART_SPU_RAM:
		return UNIT_GB;
	case PART_SPU_HEAT:
	case PART_RSX:
	case PART_RSX_VERT:
		return UNIT_GFLOP;
	case PART_RSX_FILL:
		return UNIT_GPIX;
	default:
		return ppu_kind(part, load) == PPU_HEAT ? UNIT_GFLOP : ppu_kind(part, load) == PPU_MANDEL ? UNIT_ITER : UNIT_MB;
	}
}

/* work over us microseconds in a unit's rate (UNIT_GB as MB/s) */
static double rate_of(int unit, double work, double us)
{
	if (us <= 0.0)
		return 0.0;
	if (unit == UNIT_ITER)
		return work / us;
	if (unit == UNIT_GFLOP || unit == UNIT_GPIX)
		return work / us / 1000.0;
	return work / 1.048576 / us;
}

double stress_part_average(const stress_status *s, int part)
{
	return rate_of(stress_part_unit(part, s->load), (double)s->work[part], (double)s->elapsed_us);
}

void stress_rate_text(char *buf, int len, int unit, double rate)
{
	if (unit == UNIT_GB)
		snprintf(buf, (size_t)len, "%.1f GB/s", rate / 1024.0);
	else if (unit == UNIT_GFLOP)
		snprintf(buf, (size_t)len, "%.1f GFLOP/s", rate);
	else if (unit == UNIT_GPIX)
		snprintf(buf, (size_t)len, "%.2f Gpix/s", rate);
	else if (unit == UNIT_ITER)
		snprintf(buf, (size_t)len, "%.0f M iter/s", rate);
	else
		snprintf(buf, (size_t)len, rate < 100.0 ? "%.1f MB/s" : "%.0f MB/s", rate);
}

/* points per second as the benchmark counts them; heat: 1 per 10 MFLOP/s, fill: 1 per 0.25 M pixels/s */
static double points_of(int unit, double rate)
{
	return unit == UNIT_GB ? rate / 10.0 : unit == UNIT_GFLOP ? rate * 100.0 : unit == UNIT_GPIX ? rate * 4000.0 : rate;
}

static void clock_text(char *buf, size_t len, uint64_t secs)
{
	if (secs >= 3600)
		snprintf(buf, len, "%u:%02u:%02u", (unsigned)(secs / 3600), (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
	else
		snprintf(buf, len, "%02u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
}

/* ---- the PPU's two hardware threads ------------------------------------------------------------- */

/* A heat job's time: everything a load thread does not get of the PPU - the screen, the system, every
   wakeup of another thread - makes its jobs longer than the probe's. Buckets: faster than the probe's
   by over 2 %, within 2 %, longer by 2-5 %, 5-10 %, 10-25 %, 25-50 %, 50-100 %, twice or more. */
static void heat_time(int which, uint64_t dt)
{
	heat_ticks[which] += dt;
	if (!heat_min[which] || dt < heat_min[which])
		heat_min[which] = dt;
	if (dt > heat_max[which])
		heat_max[which] = dt;
	if (heat_ref) {
		int64_t over = (int64_t)(dt * 1000 / heat_ref) - 1000;   /* thousandths */

		heat_hist[which][over < -20 ? 0 : over <= 20 ? 1 : over <= 50 ? 2 : over <= 100 ? 3 : over <= 250 ? 4 :
		                 over <= 500 ? 5 : over < 1000 ? 6 : 7]++;
	}
}

/* ---- the PPU's ceiling ---------------------------------------------------------------------------------- */

/* The VMX heat kernel with nothing else on the PPU, before the load begins: one thread, then both, at
   PROBE_PRIO above every other thread of this app and webMAN's, PROBE_JOBS jobs each, each one timed.
   It answers what V8.3 could not: whether the PPU's two threads reach the VMX unit's peak at all (then
   the load threads lose the rest to others during the test) or not. The jobs are the load threads'
   own, so their results are checked like the load's. */
typedef struct {
	int thread, wrong;
	uint64_t work, start[PROBE_JOBS], end[PROBE_JOBS];
} ppu_probe_run;

static void probe_main(void *arg)
{
	ppu_probe_run *p = arg;

	for (int i = 0; i < PROBE_JOBS; i++) {
		int ok = 0;

		p->start[i] = __gettime();
		p->work = bench_ppu_heat_job(p->thread, i, &ok);
		p->end[i] = __gettime();
		if (!ok)
			p->wrong++;
	}
	sysThreadExit(0);
}

/* n (1 or 2) probe threads at once; 0 when all of them ran */
static int probe_run(ppu_probe_run *runs, int n)
{
	sys_ppu_thread_t thread[2];
	int have[2] = { 0, 0 };
	u64 ret;

	for (int i = 0; i < n; i++) {
		memset(&runs[i], 0, sizeof(runs[i]));
		runs[i].thread = i;
	}
	for (int i = 0; i < n; i++)
		have[i] = sysThreadCreate(&thread[i], probe_main, &runs[i], PROBE_PRIO, LOAD_STACK, THREAD_JOINABLE,
		                          i ? "tee_probe_2" : "tee_probe_1") == 0;
	for (int i = 0; i < n; i++)
		if (have[i])
			sysThreadJoin(thread[i], &ret);
	for (int i = 0; i < n; i++)
		if (!have[i])
			return -1;
	return 0;
}

/* the fastest (and the slowest) of a run's jobs that lie wholly inside [from, to] */
static uint64_t probe_fastest(const ppu_probe_run *p, uint64_t from, uint64_t to, uint64_t *slowest)
{
	uint64_t best = 0, worst = 0;

	for (int i = 0; i < PROBE_JOBS; i++) {
		uint64_t d = p->end[i] - p->start[i];

		if (p->start[i] < from || p->end[i] > to)
			continue;
		if (!best || d < best)
			best = d;
		if (d > worst)
			worst = d;
	}
	*slowest = worst;
	return best;
}

static void ppu_probe(void)
{
	static ppu_probe_run one[1], two[2];
	double tb = st.timebase_mhz * 1e6, peak = (st.probe_ok ? st.cell_mhz : 3192.0) * 8.0 / 1000.0, r1, r2;
	uint64_t f1, fa, fb, s1, sa, sb, from, to;

	heat_ref = 0;
	if (probe_run(one, 1) != 0 || probe_run(two, 2) != 0) {
		log_line("PPU heat with nothing else on the PPU: a probe thread could not be started");
		return;
	}
	for (int i = 0; i < 2; i++) {
		uint32_t wrong = (uint32_t)two[i].wrong + (i ? 0u : (uint32_t)one[0].wrong);

		if (wrong) {
			load_errors[i] += wrong;
			log_line("ERROR in the stress test: PPU thread %d heat, %u of the probe's jobs wrong", i + 1, (unsigned)wrong);
		}
	}
	/* with both: only jobs while the other thread ran too - the first started alone, the last may end alone */
	from = two[0].start[0] > two[1].start[0] ? two[0].start[0] : two[1].start[0];
	to = two[0].end[PROBE_JOBS - 1] < two[1].end[PROBE_JOBS - 1] ? two[0].end[PROBE_JOBS - 1] : two[1].end[PROBE_JOBS - 1];
	f1 = probe_fastest(&one[0], 0, ~0ull, &s1);
	fa = probe_fastest(&two[0], from, to, &sa);
	fb = probe_fastest(&two[1], from, to, &sb);
	if (!f1 || !fa || !fb || tb <= 0.0) {
		log_line("PPU heat with nothing else on the PPU: not measured, the two probe threads did not run side by side");
		return;
	}
	r1 = (double)one[0].work / ((double)f1 / tb) / 1e9;
	r2 = (double)two[0].work / ((double)fa / tb) / 1e9 + (double)two[1].work / ((double)fb / tb) / 1e9;
	heat_ref = (fa + fb) / 2;
	log_line("PPU heat with nothing else on the PPU: one thread alone %.2f GFLOP/s (%.1f %% of the PPU's %.2f peak), "
	         "both threads %.2f GFLOP/s together (%.1f %%) - the fastest of %d jobs each; with both a job takes "
	         "%.3f ms, the slowest of them %.1f %% longer", r1, 100.0 * r1 / peak, peak, r2, 100.0 * r2 / peak,
	         PROBE_JOBS, 1000.0 * (double)heat_ref / tb,
	         100.0 * ((double)(sa > sb ? sa : sb) / (double)(fa < fb ? fa : fb) - 1.0));
}

/* arg: 0 PPU thread 1, 1 PPU thread 2. Each goes round its jobs until told to stop; every result is
   checked against its reference. */
static void load_main(void *arg)
{
	int which = (int)(intptr_t)arg, logged = 0, kind = ppu_kind(PART_PPU_1 + which, st.load);
	int n = kind == PPU_HEAT ? 16 : bench_jobs(kind == PPU_MANDEL ? BENCH_MANDEL : BENCH_CHACHA);
	char name[64];

	log_name(name, sizeof(name), PART_PPU_1 + which, st.load);
	for (int i = kind == PPU_HEAT ? 0 : which * n / 2; !load_stop; i = (i + 1) % n) {
		int ok = 0;
		uint64_t t0 = __gettime();
		uint64_t w = kind == PPU_HEAT ? bench_ppu_heat_job(which, i, &ok) : kind == PPU_MANDEL ?
		             bench_ppu_vmx_job(i, &ok) : bench_ppu_chacha_job(i, &ok);

		if (kind == PPU_HEAT)
			heat_time(which, __gettime() - t0);
		load_work[which] += w;
		load_jobs[which]++;
		if (!ok) {
			load_errors[which]++;
			if (logged++ < LOG_ERRORS)
				log_line("ERROR in the stress test: %s, job %d wrong", name, i);
		}
	}
	sysThreadExit(0);
}

/* ---- the clock: the time server's view, for the screen ---------------------------------------------- */

static void clock_update(void)
{
	ntp_status n;

	ntp_get(&n);
	st.clock_state = n.state;
	st.clock_valid = n.valid;
	st.clock_samples = n.samples;
	st.clock_span_s = n.span_s;
	st.clock_dev_pct = n.dev_pct;
	st.clock_ms_per_min = n.dev_pct * 600.0;   /* that share of 60000 ms */
	st.clock_delay_ms = n.delay_ms;
	snprintf(st.clock_server, sizeof(st.clock_server), "%s", n.server);
}

/* ---- the history of the load ----------------------------------------------------------------------- */

static float bucket_sum;
static int bucket_n, bucket_bad;

static void history_add(float points, int bad)
{
	bucket_sum += points;
	bucket_n++;
	bucket_bad |= bad;
	if (bucket_n < st.history_step_s)
		return;
	st.history[st.history_len] = bucket_sum / (float)bucket_n;
	st.history_bad[st.history_len] = (uint8_t)bucket_bad;
	st.history_len++;
	bucket_sum = 0.0f;
	bucket_n = bucket_bad = 0;
	if (st.history_len == STRESS_HISTORY) {
		/* full: two bars become one, and from now on a bar stands for twice the time */
		for (int i = 0; i < STRESS_HISTORY / 2; i++) {
			st.history[i] = (st.history[2 * i] + st.history[2 * i + 1]) / 2.0f;
			st.history_bad[i] = st.history_bad[2 * i] | st.history_bad[2 * i + 1];
		}
		st.history_len = STRESS_HISTORY / 2;
		st.history_step_s *= 2;
	}
}

/* ---- the test --------------------------------------------------------------------------------------- */

static void publish(const bench_mix_counts *c)
{
	uint32_t errors = 0;

	for (int t = 0; t < BENCH_KINDS; t++) {
		st.work[t] = c->work[t];
		st.jobs[t] = c->jobs[t];
		st.errors[t] = c->errors[t];
	}
	for (int i = 0; i < 2; i++) {
		st.work[PART_PPU_1 + i] = load_work[i];
		st.jobs[PART_PPU_1 + i] = load_jobs[i];
		st.errors[PART_PPU_1 + i] = load_errors[i];
	}
	if (load_rsx(st.load)) {
		const rsxload_status *r = rsxload_get();

		/* V0.9: the pixel units' multiply-adds - the shader tiles' and the fill pixels' (four floats, two
		   operations each) */
		st.work[PART_RSX] = ((r->mads - rsx_base_mads) + (r->fmads - rsx_base_fmads)) * 8;
		st.jobs[PART_RSX] = r->tiles - rsx_base_tiles;
		st.errors[PART_RSX] = (uint32_t)(r->check_errors - rsx_base_errors);
		st.work[PART_RSX_FILL] = r->pixels - rsx_base_pixels;
		st.jobs[PART_RSX_FILL] = r->layers - rsx_base_layers;
		st.errors[PART_RSX_FILL] = (uint32_t)((r->fill_errors - rsx_base_ferrors) + (r->vram_errors - rsx_base_verrors));
		st.work[PART_RSX_VERT] = (r->vmads - rsx_base_vmads) * 8;
		st.jobs[PART_RSX_VERT] = r->vbatches - rsx_base_vbatches;
		st.errors[PART_RSX_VERT] = (uint32_t)(r->vert_errors - rsx_base_vert_errors);
		st.vram_slices = r->vram_slices - rsx_base_slices;
		st.rsx_checks = (r->checks - rsx_base_checks) + (r->fill_checks - rsx_base_fchecks) + st.vram_slices +
		                (r->vert_checks - rsx_base_vchecks);
		if (st.elapsed_us > 0) {
			double s_ = (double)st.elapsed_us * 1e-6;

			st.rsx_alu_pct = r->probe_gmad > 0.0 ? 100.0 * (double)((r->mads - rsx_base_mads) + (r->fmads - rsx_base_fmads)) /
			                 s_ / (r->probe_gmad * 1e9) : 0.0;
			st.rsx_rop_pct = r->blend_gpix > 0.0 ? 100.0 * (double)(r->pixels - rsx_base_pixels) / s_ / (r->blend_gpix * 1e9) :
			                 0.0;
			st.rsx_vert_pct = r->probe_vert_gmad > 0.0 && r->vert_ok ? 100.0 * (double)(r->vmads - rsx_base_vmads) / s_ /
			                  (r->probe_vert_gmad * 1e9) : 0.0;
		}
		st.rsx_tiles_per_picture = r->tiles_per_picture;
		st.rsx_layers_per_picture = r->layers_per_picture;
		{
			/* V0.8: by the RSX's own time stamps (all its work: the load, the screen, the copies) once a few
			   pictures were timed; else by the probe's timings (the load's two parts only) */
			double span = r->ts_span_us - rsx_base_span;

			st.rsx_timed = r->reports_ok && r->ts_pictures - rsx_base_pictures >= 8 && span > 0.0;
			if (st.rsx_timed) {
				st.rsx_pct = 100.0 * (1.0 - (r->ts_idle_us - rsx_base_idle) / span);
				st.rsx_flip_wait_pct = 100.0 * (r->ts_flip_wait_us - rsx_base_flip_wait) / span;
				st.rsx_tile_pct = 100.0 * (r->ts_tile_us - rsx_base_ts_tile) / span;
				st.rsx_fill_pct = 100.0 * (r->ts_fill_us - rsx_base_ts_fill) / span;
			} else if (st.elapsed_us > 0) {
				st.rsx_tile_pct = 100.0 * (r->busy_tile_us - rsx_base_tile_us) / (double)st.elapsed_us;
				st.rsx_fill_pct = 100.0 * (r->busy_fill_us - rsx_base_fill_us) / (double)st.elapsed_us;
				st.rsx_pct = st.rsx_tile_pct + st.rsx_fill_pct;
				st.rsx_flip_wait_pct = 0.0;
			}
		}
	}
	for (int p = 0; p < STRESS_PARTS; p++)
		errors += st.errors[p];
	st.errors_total = errors;
	/* against the time that went by on the PPU (timebase and decrementer count at the same rate) */
	if (c->ticks_span) {
		double span = (double)c->ticks_span;

		st.spu_busy_pct = 100.0 * (double)c->ticks_work / span;
		st.spu_answer_pct = 100.0 * (double)c->ticks_answer / span;
		st.spu_wait_pct = 100.0 * (double)c->ticks_wait / span;
		st.spu_unseen_pct = 100.0 - st.spu_busy_pct - st.spu_answer_pct - st.spu_wait_pct;
	}
	for (int k = 0; k < STRESS_SPUS; k++) {
		st.spu_busy_each[k] = c->spu_span[k] ? 100.0 * (double)c->spu_work[k] / (double)c->spu_span[k] : 0.0;
		st.spu_jobs_each[k] = c->spu_jobs[k];
	}
	st.spu_work_ticks = c->ticks_work;
	memcpy(st.unseen_hist, c->unseen_hist, sizeof(st.unseen_hist));
	st.unseen_max_us = st.timebase_mhz > 0.0 ? (double)c->unseen_max / st.timebase_mhz : 0.0;
}

static void count_part_spus(int spus)
{
	memset(st.part_spus, 0, sizeof(st.part_spus));
	for (int k = 0; k < spus; k++) {
		int t = bench_mix_test(k);

		st.part_spus[t]++;
		if (t == BENCH_HEAT_RAM)
			st.part_spus[BENCH_HEAT]++;   /* the same SPU heats too */
	}
}

static double average(int part, uint64_t us)
{
	return rate_of(stress_part_unit(part, st.load), (double)st.work[part], (double)us);
}

static void clock_log(char *buf, size_t len)
{
	if (st.clock_valid)
		snprintf(buf, len, "%+.4f %%", st.clock_dev_pct);
	else
		snprintf(buf, len, "%s", st.clock_state == NTP_NO_SERVER ? "no time server" : "not measured yet");
}

static void log_progress(uint64_t us)
{
	char t[16], parts[224], clock[32], tail[64] = "";
	int cpu, rsx, fan;

	clock_text(t, sizeof(t), us / 1000000);
	if (load_heat(st.load)) {
		int n = snprintf(parts, sizeof(parts), "SPU %.1f GFLOP/s", average(PART_SPU_HEAT, us));

		if (load_ram(st.load))
			n += snprintf(parts + n, sizeof(parts) - (size_t)n, ", RAM %.1f GB/s", average(PART_SPU_RAM, us) / 1024.0);
		n += snprintf(parts + n, sizeof(parts) - (size_t)n, " (busy %.1f %%, unseen %.1f %%); PPU %.1f + %.1f GFLOP/s",
		              st.spu_busy_pct, st.spu_unseen_pct, average(PART_PPU_1, us), average(PART_PPU_2, us));
		if (load_rsx(st.load))
			snprintf(parts + n, sizeof(parts) - (size_t)n, "; RSX %.1f GFLOP/s + fill %.2f Gpix/s, busy %.1f %%, %.1f tiles + "
			         "%.2f layers a picture", average(PART_RSX, us), average(PART_RSX_FILL, us), st.rsx_pct,
			         st.rsx_tiles_per_picture, st.rsx_layers_per_picture);
	} else if (st.load == STRESS_RSX)
		snprintf(parts, sizeof(parts), "RSX %.1f GFLOP/s + fill %.2f Gpix/s, busy %.1f %% (shader %.0f + fill %.0f), %.1f "
		         "tiles + %.2f layers a picture", average(PART_RSX, us), average(PART_RSX_FILL, us), st.rsx_pct,
		         st.rsx_tile_pct, st.rsx_fill_pct, st.rsx_tiles_per_picture, st.rsx_layers_per_picture);
	else
		snprintf(parts, sizeof(parts), "SPU %.0f M iter/s, %.0f MB/s, %.1f GB/s; PPU %.1f MB/s, %.0f M iter/s",
		         average(PART_SPU_FLOAT, us), average(PART_SPU_INT, us), average(PART_SPU_MEM, us) / 1024.0,
		         average(PART_PPU_1, us), average(PART_PPU_2, us));
	clock_log(clock, sizeof(clock));
	if (webman_temps(&cpu, &rsx, &fan)) {
		int n = snprintf(tail, sizeof(tail), "; CPU %d C, RSX %d C", cpu, rsx);

		if (fan >= 0)
			snprintf(tail + n, sizeof(tail) - (size_t)n, ", fan %d %%", fan);
	}
	log_line("Stress test %s: %s (average); %u errors; clock %s%s", t, parts, st.errors_total, clock, tail);
}

static void duration_log(char *buf, size_t len, uint32_t secs)
{
	if (!secs)
		snprintf(buf, len, "endless");
	else if (secs < 60)
		snprintf(buf, len, "%u s", (unsigned)secs);
	else
		snprintf(buf, len, "%u min", (unsigned)(secs / 60));
}

/* the verdict and the summary in the log */
static void finish(int hang, int hung_spu, uint32_t duration_s)
{
	char a[24], name[64], note[96];
	int judged, cancelled;

	clock_text(a, sizeof(a), st.elapsed_us / 1000000);
	judged = st.clock_valid && st.clock_span_s >= STRESS_CLOCK_JUDGE;
	cancelled = duration_s && st.elapsed_us < duration_s * 1000000ull;
	if (hang && st.rsx_hang) {
		st.result = STRESS_FAILED;
		snprintf(st.verdict, sizeof(st.verdict), "Not passed: the RSX stopped finishing its work (after %s)", a);
	} else if (hang) {
		st.result = STRESS_FAILED;
		snprintf(st.verdict, sizeof(st.verdict), "Not passed: SPU %d stopped answering (after %s)", hung_spu, a);
	} else if (st.errors_total) {
		st.result = STRESS_FAILED;
		snprintf(st.verdict, sizeof(st.verdict), "Not passed: %u error%s in %s", st.errors_total,
		         st.errors_total == 1 ? "" : "s", a);
	} else if (!st.threads_ok) {
		st.result = STRESS_FAILED;
		snprintf(st.verdict, sizeof(st.verdict), "Not passed: a PPU load thread did not run");
	} else if (st.too_hot) {
		st.result = STRESS_FAILED;
		snprintf(st.verdict, sizeof(st.verdict), "Not passed: the %s reached %d`C after %s - too hot",
		         st.too_hot == 1 ? "CPU" : "RSX", st.too_hot == 1 ? st.temp_cpu_max : st.temp_rsx_max, a);
	} else if (judged && fabs(st.clock_dev_pct) > STRESS_CLOCK_LIMIT) {
		st.result = STRESS_FAILED;
		snprintf(st.verdict, sizeof(st.verdict), "Not passed: the timer is off by %+.3f %%", st.clock_dev_pct);
	} else if (cancelled) {
		st.result = STRESS_CANCELLED;
		snprintf(st.verdict, sizeof(st.verdict), "Cancelled after %s - no errors until then", a);
	} else {
		st.result = STRESS_PASSED;
		snprintf(st.verdict, sizeof(st.verdict), "Passed: %s of the %s load without errors%s", a,
		         stress_load_title(st.load), judged ? ", clock correct" : "");
	}
	if (st.probe_ok && judged)
		st.cell_mhz_real = st.cell_mhz * (1.0 + st.clock_dev_pct / 100.0);

	log_line("Stress test result: %s", st.verdict);
	for (int p = 0; p < STRESS_PARTS; p++) {
		int unit = stress_part_unit(p, st.load);

		if (!stress_part_used(p, st.load))
			continue;
		log_name(name, sizeof(name), p, st.load);
		log_line("  %-40s %8llu jobs, %9.1f %s average, %u errors", name, (unsigned long long)st.jobs[p],
		         average(p, st.elapsed_us), unit == UNIT_ITER ? "M iter/s" : unit == UNIT_GFLOP ? "GFLOP/s" :
		         unit == UNIT_GPIX ? "Gpix/s" : "MB/s",
		         st.errors[p]);
	}
	if (st.clock_state == NTP_NO_SERVER || st.clock_state == NTP_OFF)
		log_line("  Clock check: no time server answered (%s) - the clock was not checked",
		         st.clock_server[0] ? st.clock_server : "network not started");
	else if (st.clock_valid)
		log_line("  Clock check against %s: the timer runs %+.4f %% (%+.1f ms/min) over %.1f s, %d answers, "
		         "shortest round trip %.1f ms%s, timebase %.3f MHz", st.clock_server, st.clock_dev_pct,
		         st.clock_ms_per_min, st.clock_span_s, st.clock_samples, st.clock_delay_ms,
		         judged ? "" : " - too short to judge", st.timebase_mhz);
	else
		log_line("  Clock check: too short, %d answer%s from %s", st.clock_samples, st.clock_samples == 1 ? "" : "s",
		         st.clock_server);
	if (st.spus && st.spu_work_ticks) {
		double spu_s = (double)st.spu_work_ticks / (st.timebase_mhz * 1e6);
		char speed[96] = "", each[192] = "";
		size_t n = 0;

		/* the heat kernel's speed while an SPU works on it, against the peak: one fma on four floats a cycle */
		if (load_heat(st.load) && spu_s > 0.0) {
			double per_spe = (double)st.work[PART_SPU_HEAT] / spu_s / 1e9;
			double peak = (st.probe_ok ? st.cell_mhz : 3192.0) * 8.0 / 1000.0;
			snprintf(speed, sizeof(speed), "; the heat kernel runs %.2f GFLOP/s per SPE while it works, %.1f %% of the "
			         "%.2f peak", per_spe, 100.0 * per_spe / peak, peak);
		}
		/* unseen: time that went by on the PPU's clock but on no SPU timer - the SPUs stood still with it */
		log_line("  SPU time on their own timer against the PPU's clock: working %.2f %%, answering %.2f %%, waiting "
		         "for the next job %.2f %%, unseen by the SPU timer %.2f %%%s", st.spu_busy_pct, st.spu_answer_pct,
		         st.spu_wait_pct, st.spu_unseen_pct, speed);
		for (int k = 0; k < st.spus && k < STRESS_SPUS && n < sizeof(each); k++) {
			int w = snprintf(each + n, sizeof(each) - n, "%sSPU %d %u jobs %.2f %%", k ? ", " : "", k,
			                 (unsigned)st.spu_jobs_each[k], st.spu_busy_each[k]);

			if (w < 0)
				break;
			n += (size_t)w;
		}
		log_line("  Working per SPU: %s", each);
	}
	if (st.spus) {
		static const char *const NAMES[STRESS_UNSEEN_BUCKETS] = {
			"below -0.05 ms", "about 0", "0.05-0.2 ms", "0.2-0.5 ms", "0.5-1 ms", "1-2 ms", "2-5 ms", "5-10 ms",
			"10-50 ms", "50 ms or more"
		};
		char hist[320] = "";
		size_t n = 0;
		uint32_t answers = 0;

		for (int b = 0; b < STRESS_UNSEEN_BUCKETS; b++)
			answers += st.unseen_hist[b];
		for (int b = 0; b < STRESS_UNSEEN_BUCKETS && n < sizeof(hist); b++) {
			int w = snprintf(hist + n, sizeof(hist) - n, "%s%s %u", b ? ", " : "", NAMES[b], (unsigned)st.unseen_hist[b]);

			if (w < 0)
				break;
			n += (size_t)w;
		}
		/* how long the SPUs stood still between two answers (the PPU's clock less the SPU timer) */
		if (answers)
			log_line("  Unseen per answer (%u answers): %s; the longest %.2f ms", (unsigned)answers, hist,
			         st.unseen_max_us / 1000.0);
	}
	/* the PPU's VMX unit: one fused multiply-add on four floats a cycle, shared by both hardware threads */
	if (load_heat(st.load) && st.threads_ok) {
		double ppu = average(PART_PPU_1, st.elapsed_us) + average(PART_PPU_2, st.elapsed_us);
		double peak = (st.probe_ok ? st.cell_mhz : 3192.0) * 8.0 / 1000.0;

		log_line("  PPU heat: both threads together %.1f GFLOP/s, %.1f %% of the PPU's %.2f peak (one VMX fused "
		         "multiply-add on four floats a cycle for both threads)", ppu, 100.0 * ppu / peak, peak);
	}
	/* the heat jobs' own times: how much of the PPU the load threads lost, and in what pieces */
	if (load_heat(st.load) && st.threads_ok && load_jobs[0] + load_jobs[1] && st.timebase_mhz > 0.0) {
		static const char *const NAMES[HEAT_BUCKETS] = {
			"faster by over 2 %", "within 2 %", "2-5 % longer", "5-10 %", "10-25 %", "25-50 %", "50-100 %",
			"twice or more"
		};
		double ms = 1000.0 / (st.timebase_mhz * 1e6);
		uint64_t jobs = load_jobs[0] + load_jobs[1], ticks = heat_ticks[0] + heat_ticks[1];
		uint64_t fast = heat_min[0] && (!heat_min[1] || heat_min[0] < heat_min[1]) ? heat_min[0] : heat_min[1];
		uint64_t slow = heat_max[0] > heat_max[1] ? heat_max[0] : heat_max[1];
		double avg = (double)ticks / (double)jobs;

		if (heat_ref) {
			char hist[320] = "";
			size_t n = 0;

			log_line("  PPU heat jobs during the test: %llu, on average %.3f ms, %.1f %% longer than with nothing else "
			         "on the PPU (%.3f ms); the fastest %.3f ms, the longest %.3f ms", (unsigned long long)jobs,
			         avg * ms, 100.0 * (avg / (double)heat_ref - 1.0), (double)heat_ref * ms, (double)fast * ms,
			         (double)slow * ms);
			for (int b = 0; b < HEAT_BUCKETS && n < sizeof(hist); b++) {
				int w = snprintf(hist + n, sizeof(hist) - n, "%s%s %u", b ? ", " : "", NAMES[b],
				                 (unsigned)(heat_hist[0][b] + heat_hist[1][b]));

				if (w < 0)
					break;
				n += (size_t)w;
			}
			log_line("  PPU heat jobs against the probe's: %s", hist);
		} else {
			log_line("  PPU heat jobs during the test: %llu, on average %.3f ms, the fastest %.3f ms, the longest %.3f ms",
			         (unsigned long long)jobs, avg * ms, (double)fast * ms, (double)slow * ms);
		}
	}
	/* the RSX: its rate against the probe's, and the check tiles */
	if (load_rsx(st.load)) {
		const rsxload_status *r = rsxload_get();
		double gf = average(PART_RSX, st.elapsed_us);

		if (st.rsx_probe_gflops > 0.0)
			log_line("  RSX heat: %.1f GFLOP/s on average in the pixel units (shader tiles and fill pixels), %.1f %% of the "
			         "%.1f the shader did alone at the start (counting the multiply-adds only); %.1f tiles of %dx%d pixels a "
			         "picture at the end", gf,
			         100.0 * gf / st.rsx_probe_gflops, st.rsx_probe_gflops, st.rsx_tiles_per_picture, RSXLOAD_TILE,
			         RSXLOAD_TILE);
		else
			log_line("  RSX heat: %.1f GFLOP/s on average (no rate alone to compare: the probe did not measure it); %.1f "
			         "tiles a picture", gf, st.rsx_tiles_per_picture);
		if (st.rsx_timed) {
			double pics = (double)(r->ts_pictures - rsx_base_pictures), span = r->ts_span_us - rsx_base_span;

			log_line("  RSX busy: %.2f %% of the time by its own time stamps (%.0f pictures timed, %.1f a second, each "
			         "staying %d TV frames) - shader %.1f %%, fill %.1f %%, screen and checks %.1f %%; it waited %.2f %% at "
			         "the flips (%.2f ms a picture) and %.2f %% elsewhere", st.rsx_pct, pics, pics * 1e6 / span,
			         RSXLOAD_FRAMES, st.rsx_tile_pct, st.rsx_fill_pct, st.rsx_pct - st.rsx_tile_pct - st.rsx_fill_pct,
			         st.rsx_flip_wait_pct, st.rsx_flip_wait_pct / 100.0 * span / pics / 1000.0,
			         100.0 - st.rsx_pct - st.rsx_flip_wait_pct);
			log_line("  RSX pacing: %u pictures came a frame late%s; at the end a margin of %.2f ms before the blank, the "
			         "load %.1f ms a picture, a shader row %.3f us, a fill row %.3f us (the probe: %.3f and %.3f)",
			         (unsigned)r->missed, r->timed ? "" : " (the time stamps stopped: set by the probe's timings at the end)",
			         r->margin_us / 1000.0, r->plan_us / 1000.0, r->unit_s_us, r->unit_f_us,
			         r->probe_tile_us / RSXLOAD_TILE, r->probe_layer_us / RSXLOAD_FILL_H);
		} else if (r->probe_ok) {
			log_line("  RSX busy: %.1f %% of the time by the probe's timings - shader %.1f %%, fill %.1f %% (its time "
			         "stamps %s); %u pictures came late, the load at %.0f %% of its plan at the end", st.rsx_pct,
			         st.rsx_tile_pct, st.rsx_fill_pct, r->reports_ok ? "came too seldom" : "did not keep time",
			         (unsigned)r->missed, 100.0 * r->scale);
		}
		if (st.rsx_alu_pct > 0.0 || st.rsx_rop_pct > 0.0)
			log_line("  RSX units against what each managed alone at the start: pixel arithmetic %.1f %%, blending %.1f %%, "
			         "vertex units %s", st.rsx_alu_pct, st.rsx_rop_pct, r->vert_ok && st.rsx_vert_pct > 0.0 ?
			         "" : "(not measured)");
		if (st.rsx_vert_pct > 0.0)
			log_line("  RSX vertex units: %.1f %% of their rate alone", st.rsx_vert_pct);
		if (r->vert_ok)
			log_line("  RSX vertices: %.1f GFLOP/s on average, %d batches of 4096 points behind each fill layer; %llu check "
			         "tiles (a point a pixel, every fourth picture), %u of them different from the test's first",
			         average(PART_RSX_VERT, st.elapsed_us), r->vert_batches,
			         (unsigned long long)(r->vert_checks - rsx_base_vchecks), st.errors[PART_RSX_VERT]);
		log_line("  RSX shader check tiles (%dx%d pixels, every fourth picture): %llu, %u of them different from the test's "
		         "first%s", RSXLOAD_CHECK, RSXLOAD_CHECK, (unsigned long long)(r->checks - rsx_base_checks),
		         st.errors[PART_RSX], r->pending ? "; some work was still queued at the end" : "");
		if (r->fill_ok) {
			double gp = average(PART_RSX_FILL, st.elapsed_us);

			log_line("  RSX fill: %.2f G pixels a second on average%s, %.2f layers of %dx%d a picture at the end; %llu "
			         "check tiles, %llu different from the test's first", gp, r->probe_gpix > 0.0 ? "" : " (its rate alone "
			         "was not measured)", st.rsx_layers_per_picture, RSXLOAD_FILL_W, RSXLOAD_FILL_H,
			         (unsigned long long)(r->fill_checks - rsx_base_fchecks),
			         (unsigned long long)(r->fill_errors - rsx_base_ferrors));
			if (r->probe_gpix > 0.0)
				log_line("  RSX fill against its rate alone (%.2f G pixels a second): %.1f %%", r->probe_gpix,
				         100.0 * gp / r->probe_gpix);
			if (r->vram_ok)
				log_line("  VRAM check: %llu slices of 64 KB read back (%.0f MB, the %d MB texture %.1f times), %llu wrong",
				         (unsigned long long)st.vram_slices, (double)st.vram_slices / 16.0,
				         RSXLOAD_TEX * RSXLOAD_TEX * 4 >> 20, (double)st.vram_slices / 256.0,
				         (unsigned long long)(r->vram_errors - rsx_base_verrors));
			else
				log_line("  VRAM check: off (its first slice did not come back right at the start)");
		}
	}
	/* what the screen takes from the PPU while the load runs: its busy time comes out of the load
	   threads' (they run below everything else), and every sleep in a wait on the RSX wakes it again */
	if (screen_frames > 1 && st.elapsed_us > 0 && st.timebase_mhz > 0.0) {
		double span_s = (double)st.elapsed_us / 1e6, tb = st.timebase_mhz * 1e6;
		double busy_s = (double)screen_busy / tb, drawn_s = (double)screen_busy_drawn / tb;

		log_line("  The screen during the test: %.1f pictures/s, %.1f of them drawn anew (each busy %.2f ms on the PPU), "
		         "on average %.2f ms busy a picture (%.1f %% of one PPU thread), %.1f sleeps a picture waiting for the RSX",
		         screen_frames / span_s, screen_drawn / span_s, screen_drawn ? 1000.0 * drawn_s / screen_drawn : 0.0,
		         1000.0 * busy_s / screen_frames, 100.0 * busy_s / span_s,
		         (double)(screen_sleeps_last - screen_sleeps_first) / (screen_frames - 1));
	}
	if (st.probe_ok) {
		if (st.cell_mhz_real > 0.0)
			log_line("  Cell clock: %.1f MHz against the timebase, %.1f MHz against the time server (the real one)",
			         st.cell_mhz, st.cell_mhz_real);
		else
			log_line("  Cell clock: %.1f MHz against the timebase (the time server's correction is missing)",
			         st.cell_mhz);
	}
	if (st.temp_cpu_max >= 0) {
		char fan[32] = "";

		if (st.fan_min >= 0)
			snprintf(fan, sizeof(fan), "; fan %d-%d %%", st.fan_min, st.fan_max);
		log_line("  Hottest (webMAN): CPU %d C, RSX %d C%s", st.temp_cpu_max, st.temp_rsx_max, fan);
	} else {
		log_line("  Temperature: webMAN did not answer");
	}

	snprintf(note, sizeof(note), "TEE PS3 Benchmark: stress test %s (%s, %u errors)", st.result == STRESS_PASSED ?
	         "PASSED" : st.result == STRESS_FAILED ? "NOT passed" : "cancelled", a, st.errors_total);
	webman_popup(note);
}

void stress_run(int load, uint32_t duration_s, const volatile int *quit)
{
	bench_mix_counts c;
	sys_ppu_thread_t thread[2];
	uint64_t t0, now, end, next_second, next_log, second_at, prev_work[STRESS_PARTS];
	uint32_t seen_errors[BENCH_KINDS] = { 0, 0, 0, 0, 0 }, last_errors = 0;
	int have[2] = { 0, 0 }, spus = bench_spus(), hang = 0, shift = 0, logged = 0, fresh;
	char a[24], name[64];
	u64 ret;

	memset(&st, 0, sizeof(st));
	memset(&c, 0, sizeof(c));
	memset(prev_work, 0, sizeof(prev_work));
	for (int i = 0; i < 2; i++) {
		load_work[i] = load_jobs[i] = 0;
		load_errors[i] = 0;
		heat_ticks[i] = heat_min[i] = heat_max[i] = 0;
		for (int b = 0; b < HEAT_BUCKETS; b++)
			heat_hist[i][b] = 0;
	}
	heat_ref = 0;
	bucket_sum = 0.0f;
	bucket_n = bucket_bad = 0;
	st.load = load > STRESS_MIXED && load < STRESS_LOADS ? load : STRESS_MIXED;
	st.duration_s = duration_s;
	st.spus = spus;
	st.history_step_s = 1;
	st.temp_cpu_max = st.temp_rsx_max = st.fan_min = st.fan_max = -1;
	st.timebase_mhz = (double)sysGetTimebaseFrequency() / 1e6;
	stop = load_stop = 0;
	st.phase = STRESS_PREPARING;

	duration_log(a, sizeof(a), duration_s);
	if (st.load == STRESS_MAX)
		log_line("CPU full-load stress test, %s, MAX load: %d SPEs on the heat kernel (fused multiply-adds and "
		         "shuffles, both pipelines every cycle) and both PPU threads on the VMX heat kernel", a, spus);
	else if (st.load == STRESS_MAX_RAM)
		log_line("CPU full-load stress test, %s, MAX+RAM load: %d SPEs on the heat kernel while their DMA units "
		         "write 3 MB regions of the RAM and read them back, checked, and both PPU threads on the VMX "
		         "heat kernel", a, spus);
	else if (st.load == STRESS_RSX)
		log_line("Stress test, %s, RSX load: the RSX's pixel pipelines on a shader of multiply-adds over %dx%d tiles "
		         "(%s), on %dx%d layers of texture reads, multiply-adds and blending, on vertices, the VRAM read back - as "
		         "much a picture as it gets through (a picture stays two TV frames), all checked; the Cell only feeds it",
		         a, RSXLOAD_TILE, RSXLOAD_TILE,
		         rsxload_variant_name(rsxload_get()->variant), RSXLOAD_FILL_W, RSXLOAD_FILL_H);
	else if (st.load == STRESS_MAX_RSX)
		log_line("Full-load stress test, %s, MAX+RSX load: %d SPEs and both PPU threads on their heat kernels, and the "
		         "RSX's load (shader tiles, texture and blend layers, VRAM check) on top", a, spus);
	else if (st.load == STRESS_ALL)
		log_line("Full-load stress test, %s, ALL: %d SPEs on the heat kernel while their DMA units write the RAM and "
		         "read it back, both PPU threads on the VMX heat kernel, and the RSX's load (shader, fill, VRAM) - the whole "
		         "console", a, spus);
	else
		log_line("CPU full-load stress test, %s, MIXED load: %d SPEs (%d each on Mandelbrot, ChaCha20, memory, "
		         "moving on every 10 s) and both PPU threads (ChaCha20 scalar, Mandelbrot VMX)", a, spus, spus / 3);

	/* the VMX references, worked out on the idle PPU before the load begins */
	fresh = bench_vmx_total() == 0 && load_cpu(st.load);
	t0 = (uint64_t)sysGetSystemTime();
	if ((load_cpu(st.load) && bench_vmx_prepare(&stop) != 0) || *quit) {
		st.result = STRESS_CANCELLED;
		snprintf(st.verdict, sizeof(st.verdict), "Cancelled while preparing");
		log_line("Stress test cancelled while preparing");
		__asm__ volatile("sync" ::: "memory");
		st.phase = STRESS_DONE;
		return;
	}
	if (fresh)
		log_line("VMX references worked out: %llu iterations in %.2f s", (unsigned long long)bench_vmx_total(),
		         ((uint64_t)sysGetSystemTime() - t0) / 1e6);
	if (spus && load_ram(st.load) && !bench_ram_ready()) {
		st.result = STRESS_CANCELLED;
		snprintf(st.verdict, sizeof(st.verdict), "Not started: no memory for the RAM test");
		log_line("Stress test not started: the 36 MB for the RAM test could not be had");
		__asm__ volatile("sync" ::: "memory");
		st.phase = STRESS_DONE;
		return;
	}

	/* the Cell's clock and the RAM's latency, measured while the SPUs are still free */
	if (spus) {
		bench_probe_result pr;

		if (bench_probe(&pr) == 0) {
			st.probe_ok = 1;
			st.cell_mhz = pr.mhz;
			st.cell_mhz_min = pr.mhz_min;
			st.cell_mhz_max = pr.mhz_max;
			st.ram_latency_ns = pr.latency_ns;
			log_line("Cell clock %.1f MHz against the timebase (%d SPEs, %.1f to %.1f, fastest of %d blocks each; SPU 0 "
			         "on average %.1f, a loop end %.1f cycles), RAM latency %.0f ns", pr.mhz, pr.spus, pr.mhz_min, pr.mhz_max,
			         PROBE_BLOCKS, pr.mhz_avg, pr.loop_end_cycles, pr.latency_ns);
		} else {
			log_line("Cell clock: the measurement failed");
		}
	}
	/* the PPU's ceiling, for the maximum loads' VMX heat kernel */
	if (load_heat(st.load))
		ppu_probe();
	if (load_rsx(st.load)) {
		const rsxload_status *r = rsxload_get();

		if (!r->ready) {
			st.result = STRESS_CANCELLED;
			snprintf(st.verdict, sizeof(st.verdict), "Not started: the RSX load could not be set up");
			log_line("Stress test not started: the RSX load could not be set up");
			__asm__ volatile("sync" ::: "memory");
			st.phase = STRESS_DONE;
			return;
		}
		st.rsx_probe_gflops = r->probe_ok ? r->probe_load_gmad * 8.0 : 0.0;
	}

	/* load on, and the first question to the time server */
	if (load_cpu(st.load)) {
		for (int i = 0; i < 2; i++)
			have[i] = sysThreadCreate(&thread[i], load_main, (void *)(intptr_t)i, LOAD_PRIO, LOAD_STACK,
			                          THREAD_JOINABLE, i ? "tee_load_2" : "tee_load_1") == 0;
		st.threads_ok = have[0] && have[1];
		if (!st.threads_ok)
			log_line("ERROR in the stress test: PPU load thread %d could not be started", have[0] ? 2 : 1);
		if (spus)
			bench_mix_start(load_ram(st.load) ? BENCH_HEAT_RAM : load_heat(st.load) ? BENCH_HEAT : -1);
	} else {
		st.threads_ok = 1;   /* none needed: the main thread feeds the RSX */
	}
	if (load_rsx(st.load)) {
		const rsxload_status *r = rsxload_get();

		rsx_base_mads = r->mads;
		rsx_base_tiles = r->tiles;
		rsx_base_errors = r->check_errors;
		rsx_base_checks = r->checks;
		rsx_base_pixels = r->pixels;
		rsx_base_layers = r->layers;
		rsx_base_fmads = r->fmads;
		rsx_base_vmads = r->vmads;
		rsx_base_vbatches = r->vbatches;
		rsx_base_vchecks = r->vert_checks;
		rsx_base_vert_errors = r->vert_errors;
		rsx_base_tile_us = r->busy_tile_us;
		rsx_base_fill_us = r->busy_fill_us;
		rsx_base_pictures = r->ts_pictures;
		rsx_base_span = r->ts_span_us;
		rsx_base_idle = r->ts_idle_us;
		rsx_base_flip_wait = r->ts_flip_wait_us;
		rsx_base_ts_tile = r->ts_tile_us;
		rsx_base_ts_fill = r->ts_fill_us;
		rsx_base_ferrors = r->fill_errors;
		rsx_base_fchecks = r->fill_checks;
		rsx_base_slices = r->vram_slices;
		rsx_base_verrors = r->vram_errors;
	}
	count_part_spus(load_cpu(st.load) ? spus : 0);
	ntp_begin();
	clock_update();
	t0 = end = second_at = (uint64_t)sysGetSystemTime();
	next_second = t0 + 1000000;
	next_log = t0 + LOG_EVERY_S * 1000000ull;
	screen_busy = screen_busy_drawn = 0;
	screen_frames = screen_drawn = 0;
	__asm__ volatile("sync" ::: "memory");   /* the screen's counts start from 0 */
	st.phase = STRESS_RUNNING;

	while (!stop && !*quit) {
		int r = 0;

		if (spus && load_cpu(st.load))
			r = bench_mix_wait(20000, &c);
		else
			sysUsleep(20000);
		now = end = (uint64_t)sysGetSystemTime();
		if (r < 0) {
			hang = 1;
			log_line("ERROR in the stress test: SPU %d has not answered for 10 s", c.hung_spu);
			break;
		}
		if (load_rsx(st.load)) {
			const rsxload_status *r2 = rsxload_get();
			uint64_t tb = __gettime(), since = r2->progress_tb;

			if (r2->active && tb > since && (double)(tb - since) / st.timebase_mhz > RSX_HANG_US) {
				hang = st.rsx_hang = 1;
				log_line("ERROR in the stress test: the RSX has not finished a picture's load for %.1f s (%d pictures queued)",
				         (double)(tb - since) / st.timebase_mhz / 1e6, r2->pending);
				break;
			}
		}
		for (int t = 0; t < BENCH_KINDS; t++)
			if (c.errors[t] != seen_errors[t]) {
				seen_errors[t] = c.errors[t];
				if (logged++ < LOG_ERRORS) {
					log_name(name, sizeof(name), t, st.load);
					if (t == BENCH_HEAT_RAM)
						log_line("ERROR in the stress test: %s on SPU %d, region %d came back different (%u bad "
						         "12 KB pieces so far)", name, c.bad_spu, c.bad_job, c.errors[t]);
					else
						log_line("ERROR in the stress test: %s on SPU %d, job %d wrong", name, c.bad_spu, c.bad_job);
				}
			}
		publish(&c);
		st.elapsed_us = now - t0;

		if (now >= next_second) {
			double dt = (double)(now - second_at);
			float points = 0.0f;
			int cpu, rsx, fan, secs = (int)((now - t0) / 1000000);

			for (int p = 0; p < STRESS_PARTS; p++) {
				int unit = stress_part_unit(p, st.load);
				double rate = rate_of(unit, (double)(st.work[p] - prev_work[p]), dt);

				st.rate[p] = (float)rate;
				if (stress_part_used(p, st.load))
					points += (float)points_of(unit, rate);
				prev_work[p] = st.work[p];
			}
			history_add(points, st.errors_total != last_errors);
			last_errors = st.errors_total;
			second_at = now;
			while (next_second <= now)
				next_second += 1000000;
			if (spus && st.load == STRESS_MIXED && secs / SHIFT_EVERY_S != shift) {
				shift = secs / SHIFT_EVERY_S;
				bench_mix_shift(shift);
			}
			count_part_spus(load_cpu(st.load) ? spus : 0);
			clock_update();
			if (webman_temps(&cpu, &rsx, &fan)) {
				if (cpu > st.temp_cpu_max)
					st.temp_cpu_max = cpu;
				if (rsx > st.temp_rsx_max)
					st.temp_rsx_max = rsx;
				if (fan >= 0) {
					if (st.fan_min < 0 || fan < st.fan_min)
						st.fan_min = fan;
					if (fan > st.fan_max)
						st.fan_max = fan;
				}
				if (cpu >= STRESS_TEMP_STOP || rsx >= STRESS_TEMP_STOP) {
					st.too_hot = cpu >= STRESS_TEMP_STOP ? 1 : 2;
					log_line("Stress test stopped: the %s is at %d C (the limit is %d C)", st.too_hot == 1 ? "CPU" : "RSX",
					         st.too_hot == 1 ? cpu : rsx, STRESS_TEMP_STOP);
					break;
				}
			}
		}
		if (now >= next_log) {
			log_progress(now - t0);
			next_log += LOG_EVERY_S * 1000000ull;
		}
		if (duration_s && now - t0 >= duration_s * 1000000ull)
			break;
	}

	/* load off: the SPUs finish the jobs they have, the PPU threads the one they are on; meanwhile the
	   time server is asked for the last time */
	st.phase = STRESS_STOPPING;
	load_stop = 1;
	ntp_finish();
	/* after the RSX stopped, the SPUs are fine: their jobs come back as always (V0.6 - skipped before, the
	   next test then took the old answers for its own and reported SPU 0 as hung) */
	if (spus && load_cpu(st.load) && (!hang || st.rsx_hang) && bench_mix_finish(&c) != 0) {
		hang = 1;
		log_line("ERROR in the stress test: SPU %d stopped answering while finishing", c.hung_spu);
	}
	/* the RSX finishes what it has queued (the main thread counts it) */
	for (int k = 0; load_rsx(st.load) && !hang && rsxload_get()->pending && k < 100; k++)
		sysUsleep(10000);
	for (int i = 0; i < 2; i++)
		if (have[i])
			sysThreadJoin(thread[i], &ret);
	publish(&c);
	st.elapsed_us = end - t0;
	{
		uint64_t until = (uint64_t)sysGetSystemTime() + NTP_WAIT_US;

		while (ntp_busy() && !*quit && (uint64_t)sysGetSystemTime() < until)
			sysUsleep(50000);
	}
	clock_update();

	finish(hang, c.hung_spu, duration_s);
	__asm__ volatile("sync" ::: "memory");   /* the verdict is in place before the screen sees DONE */
	st.phase = STRESS_DONE;
}
