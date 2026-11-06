/* The CPU full-load stress test, for overclocked consoles and for the cooling: all six SPUs and both PPU
   hardware threads busy at once and without a break, every result checked, the PPU's timer compared
   with a time server (ntp.h). Three loads:
     - MIXED: two SPUs each on Mandelbrot, ChaCha20 and memory; every 10 s the tests move on one SPU,
       so over time every SPU runs every test. PPU thread 1: ChaCha20 in scalar integer code, PPU
       thread 2: Mandelbrot on the VMX unit.
     - MAX: as much heat as the Cell makes: every SPU on the heat kernel (fused multiply-adds and
       shuffles, both pipelines every cycle), both PPU threads on a heat kernel of their own (VMX).
     - MAX+RAM: the same, while the SPUs' DMA units write regions of the RAM and read them back at
       full speed, every quadword checked - for the memory controller and the RAM.
     - RSX (V9, V0.7): the graphics chip alone - a shader of multiply-adds over tiles off the screen,
       full-screen texture and blend layers, the VRAM read back - as much as it gets through between
       two flips, everything checked (rsx_load.h). The Cell only feeds it.
     - MAX+RSX and ALL (V0.6): MAX, or MAX+RAM, with the RSX load on top - the whole console.
   Before the load starts, every SPU measures the Cell's clock and SPU 0 the RAM's latency.
   Should webMAN report the CPU or the RSX at STRESS_TEMP_STOP or hotter, the test stops at once.
   stress_run() blocks for the whole test, so the bench runner thread calls it; the screen reads
   stress_get() while it runs (a torn number lasts one frame). */
#ifndef STRESS_H
#define STRESS_H

#include <stdint.h>

enum { STRESS_IDLE, STRESS_PREPARING, STRESS_RUNNING, STRESS_STOPPING, STRESS_DONE };
enum { STRESS_NONE, STRESS_PASSED, STRESS_FAILED, STRESS_CANCELLED };
enum { STRESS_MIXED, STRESS_MAX, STRESS_MAX_RAM, STRESS_RSX, STRESS_MAX_RSX, STRESS_ALL, STRESS_LOADS };
/* the SPU parts in the order of the bench tests (BENCH_MANDEL .. BENCH_HEAT_RAM), then the PPU threads,
   then the RSX: its shader tiles, and (V0.7) its fill layers with the VRAM check */
enum { PART_SPU_FLOAT, PART_SPU_INT, PART_SPU_MEM, PART_SPU_HEAT, PART_SPU_RAM, PART_PPU_1, PART_PPU_2, PART_RSX,
       PART_RSX_FILL, PART_RSX_VERT, STRESS_PARTS };
/* what a part's rate counts: million iterations/s, MB/s, MB/s shown as GB/s, GFLOP/s, G pixels/s */
enum { UNIT_ITER, UNIT_MB, UNIT_GB, UNIT_GFLOP, UNIT_GPIX };

#define STRESS_HISTORY      120   /* bars; once full, two become one and a bar stands for twice the time */
#define STRESS_CLOCK_LIMIT  0.1   /* % the timer may differ from the time server and still pass */
#define STRESS_CLOCK_JUDGE  15    /* s between the first and the last answer before the clock counts */
#define STRESS_SPUS         6     /* = BENCH_MAX_SPUS */
#define STRESS_UNSEEN_BUCKETS 10  /* = BENCH_UNSEEN_BUCKETS */
#define STRESS_TEMP_STOP    85    /* degrees C, CPU or RSX: the test stops */

typedef struct {
	volatile int phase, result;
	int load;                            /* STRESS_MIXED or STRESS_MAX */
	uint32_t duration_s;                 /* 0: endless */
	uint64_t elapsed_us;                 /* under load, on the PPU's timer */
	int spus;                            /* SPUs taking part (0: SPUs not started) */
	int part_spus[PART_PPU_1];           /* of those, on each SPU part right now */
	int threads_ok;                      /* both PPU load threads running */
	uint64_t work[STRESS_PARTS];         /* iterations, bytes or floating-point operations */
	uint64_t jobs[STRESS_PARTS];
	uint32_t errors[STRESS_PARTS], errors_total;
	float rate[STRESS_PARTS];            /* over the last second, in the part's unit (GB/s as MB/s) */
	float history[STRESS_HISTORY];       /* the load in points per second, one bar per history_step_s */
	uint8_t history_bad[STRESS_HISTORY]; /* a wrong result in that time */
	int history_len, history_step_s;
	/* the PPU's timer against a time server: NTP_OFF .. NTP_RUNNING of ntp.h */
	int clock_state, clock_valid, clock_samples;
	double clock_span_s, clock_dev_pct, clock_ms_per_min, clock_delay_ms, timebase_mhz;
	char clock_server[48];
	/* measured before the load: the Cell's clock against the timebase (mean, lowest and highest SPU),
	   against the time server once the clock check counts (else 0), and the RAM's latency */
	int probe_ok;
	double cell_mhz, cell_mhz_min, cell_mhz_max, cell_mhz_real, ram_latency_ns;
	/* the SPUs' own timer against the PPU's clock, in % of the time: working (SPU BUSY), answering a job,
	   waiting for the next, and the rest, which their timer did not see; the work in decrementer ticks;
	   per SPU the share working and the jobs done */
	double spu_busy_pct, spu_answer_pct, spu_wait_pct, spu_unseen_pct;
	uint64_t spu_work_ticks;
	double spu_busy_each[STRESS_SPUS];
	uint32_t spu_jobs_each[STRESS_SPUS];
	/* per answer: the time an SPU stood still since its answer before (bench_mix_counts.unseen_hist), and
	   the longest, in microseconds */
	uint32_t unseen_hist[STRESS_UNSEEN_BUCKETS];
	double unseen_max_us;
	/* from webMAN: the hottest temperatures and the fan's range, -1 if it never answered */
	int temp_cpu_max, temp_rsx_max, fan_min, fan_max;
	int too_hot;                         /* stopped at STRESS_TEMP_STOP: 1 the CPU, 2 the RSX */
	/* the RSX load: its shader's rate alone at the start (rsx_load's probe); how much of the time the RSX
	   was busy (V0.8: by its own time stamps when rsx_timed - all its work, the screen's too; else by the
	   probe's timings, shader + fill), the shader's and the fill's shares, the share it waited at the flip;
	   tiles and fill layers a picture; check tiles of both kinds and VRAM slices checked */
	double rsx_probe_gflops, rsx_pct, rsx_tile_pct, rsx_fill_pct, rsx_flip_wait_pct;
	double rsx_tiles_per_picture, rsx_layers_per_picture;
	/* V0.9: each of the RSX's units against what it managed alone at the start (rsx_load's probe), %; 0 when
	   not measured: the pixel units' arithmetic (shader tiles and fill pixels), the blending, the vertex units */
	double rsx_alu_pct, rsx_rop_pct, rsx_vert_pct;
	int rsx_timed, rsx_hang;
	uint64_t rsx_checks, vram_slices;
	char verdict[112];
} stress_status;

const stress_status *stress_get(void);

/* for the main thread, every frame: whether the RSX load is to run (it feeds it, see rsx_load.h) */
int stress_rsx_wanted(void);

/* the whole test; ends after duration_s (0: never), with stress_cancel(), at STRESS_TEMP_STOP or once
   *quit is set */
void stress_run(int load, uint32_t duration_s, const volatile int *quit);
void stress_cancel(void);

/* the screen's own cost while the load runs, for the log: the main thread calls this once a frame with
   the timebase ticks it was busy (everything before the flip's wait), gfx_sleeps()' running count and
   whether it drew the frame anew (1) or showed the last one again (0) */
void stress_screen_frame(uint64_t busy_ticks, uint32_t sleeps, int drawn);

/* the load's name for the log and the verdict: MIXED, MAX, MAX+RAM, RSX, MAX+RSX, ALL */
const char *stress_load_name(int load);
/* its name on the screen and in the verdict: Mixed, CPU max, CPU + RAM, RSX, CPU + RSX, Everything */
const char *stress_load_title(int load);
/* what a load keeps busy */
int stress_load_cpu(int load);
int stress_load_ram(int load);
int stress_load_rsx(int load);
/* for the screen: which parts a load has, what they are called and what they count */
int  stress_part_used(int part, int load);
const char *stress_part_name(int part, int load);
int  stress_part_unit(int part, int load);
/* a part's rate over the whole test, in its unit */
double stress_part_average(const stress_status *s, int part);
/* a rate in a unit as text, e.g. "1857 M iter/s" or "20.1 GB/s" */
void stress_rate_text(char *buf, int len, int unit, double rate);

#endif
