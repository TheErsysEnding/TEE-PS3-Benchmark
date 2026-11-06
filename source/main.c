/* TEE PS3 Benchmark (TEE SPE Bench until V9.0): how much the Cell can do - floating point (Mandelbrot), integer (ChaCha20) and memory
   (DMA) - with the PPU and with 1 to 6 SPEs, scored in points; a full-load stress test of the whole CPU
   (mixed, the maximum load the Cell can make, or that and the RAM at full speed) for overclocked consoles
   and for the cooling; and the Cell's real clock and the RAM's latency, measured.

   The main menu shows the tests as a grid of cards (V0.6): the arrows choose, X starts, TRIANGLE explains
   the chosen one, CIRCLE goes back (and stops a test), L1/R1 set PPU or 1-6 SPEs. SELECT saves the
   screen as a 1280x720 picture next to the log and says so for a few seconds, START quits.
   The tests run in their own thread (runner), the main thread only draws and reads the pad. Every result
   goes to /dev_hdd0/tmp/teebench/bench-*.log. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <ppu-asm.h>
#include <ppu-types.h>
#include <io/pad.h>
#include <lv2/systime.h>
#include <sys/process.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sysutil/sysutil.h>

#include "bench.h"
#include "bench_log.h"
#include "gfx.h"
#include "info.h"
#include "ntp.h"
#include "rsx_load.h"
#include "scene.h"
#include "stress.h"
#include "ui.h"
#include "webman.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define VERSION  "V0.9 BETA"   /* the app's own numbering since V0.5 (the work before was V1-V9.0) */
#define PADS     7
#define CONFIGS  (BENCH_MAX_SPUS + 1)   /* 0 = PPU, 1..6 SPEs */
#define NOTE_US  4000000                /* how long the note after a screenshot stays */
#define STRESS_DRAW_EVERY  8             /* the pixel font: while the stress test runs, every 8th frame */
#define STRESS_DRAW_UI     2             /* the textured interface: every 2nd frame (30 a second) */

enum { PAD_LEFT = 1, PAD_RIGHT = 2, PAD_UP = 4, PAD_DOWN = 8, PAD_CROSS = 16, PAD_TRIANGLE = 32, PAD_CIRCLE = 64,
       PAD_START = 128, PAD_SELECT = 256, PAD_L1 = 512, PAD_R1 = 1024 };
#define PAD_REPEATS  (PAD_LEFT | PAD_RIGHT | PAD_UP | PAD_DOWN)

enum { REQ_NONE, REQ_TEST, REQ_ALL, REQ_SERIES, REQ_ENDURANCE, REQ_FULLLOAD };
enum { ST_PREPARING, ST_IDLE, ST_RUNNING, ST_ENDURANCE, ST_FULLLOAD };
enum { SCR_MENU, SCR_INFO, SCR_RESULTS, SCR_STRESS };

/* the stress test's running times; 0 runs until CIRCLE */
#ifdef BENCH_AUTOTEST
static const uint32_t DURATIONS[] = { 20, 300, 900, 3600, 0 };
#define DURATION_DEFAULT  1
#else
static const uint32_t DURATIONS[] = { 60, 300, 900, 3600, 0 };
#define DURATION_DEFAULT  1
#endif
#define DURATION_COUNT  ((int)(sizeof(DURATIONS) / sizeof(DURATIONS[0])))

static volatile int running = 1;

/* shared with the runner thread; the screen reads them without a lock, a torn number lasts one frame.
   A request's parameters are in memory before the request itself (post). */
static volatile int state = ST_PREPARING, request = REQ_NONE, request_spus, request_test, request_load, stop_requested,
                    quit;
static volatile uint32_t request_secs;
static volatile int cur_test = -1, cur_spus = -1;
static volatile uint32_t endurance_rounds, total_errors;
static volatile uint64_t endurance_start;
/* measured once at the start: the Cell's clock against the timebase and the RAM's latency (0: not yet) */
static volatile double start_mhz, start_latency_ns;
static bench_score scores[CONFIGS][BENCH_TESTS];

/* the screen's own state, main thread only */
static int screen = SCR_MENU, cursor, selected, info_entry, info_back = SCR_MENU, duration_pick = DURATION_DEFAULT,
           load_pick = STRESS_MIXED;
static char note[192];          /* what the last screenshot did, shown for NOTE_US */
static uint64_t note_until;
static int note_bad;

/* the PS button's menu, or the system drawing over the picture: the RSX load pauses meanwhile, so the
   system's overlay gets the RSX (under the full load it flickered) */
static volatile int system_menu, system_draw;

static void on_sysutil(u64 status, u64 param, void *user)
{
	(void)param;
	(void)user;
	if (status == SYSUTIL_EXIT_GAME)
		running = 0;
	else if (status == SYSUTIL_MENU_OPEN)
		system_menu = 1;
	else if (status == SYSUTIL_MENU_CLOSE)
		system_menu = 0;
	else if (status == SYSUTIL_DRAW_BEGIN)
		system_draw = 1;
	else if (status == SYSUTIL_DRAW_END)
		system_draw = 0;
}

static int pad_buttons(void)
{
	static padData data[PADS];
	padInfo info;
	int held = 0;

	ioPadGetInfo(&info);
	for (int i = 0; i < PADS; i++) {
		if (!info.status[i] || ioPadGetData(i, &data[i]) != 0)
			continue;
		held |= (data[i].BTN_LEFT ? PAD_LEFT : 0) | (data[i].BTN_RIGHT ? PAD_RIGHT : 0) |
		        (data[i].BTN_L1 ? PAD_L1 : 0) | (data[i].BTN_R1 ? PAD_R1 : 0) |
		        (data[i].BTN_UP ? PAD_UP : 0) | (data[i].BTN_DOWN ? PAD_DOWN : 0) |
		        (data[i].BTN_CROSS ? PAD_CROSS : 0) | (data[i].BTN_TRIANGLE ? PAD_TRIANGLE : 0) |
		        (data[i].BTN_CIRCLE ? PAD_CIRCLE : 0) | (data[i].BTN_START ? PAD_START : 0) |
		        (data[i].BTN_SELECT ? PAD_SELECT : 0);
	}
	return held;
}

/* an arrow held down repeats: after 0.4 s, then every 0.12 s */
static int key_repeat(int held, int pressed, uint64_t now)
{
	static int key;
	static uint64_t next;

	if (pressed & PAD_REPEATS) {
		key = pressed & PAD_REPEATS;
		next = now + 400000;
		return 0;
	}
	if (!(held & key)) {
		key = 0;
		return 0;
	}
	if (now < next)
		return 0;
	next = now + 120000;
	return held & key;
}

static const char *config_name(int spus)
{
	static const char *const NAMES[CONFIGS] = { "PPU", "1 SPE", "2 SPES", "3 SPES", "4 SPES", "5 SPES", "6 SPES" };
	return spus >= 0 && spus < CONFIGS ? NAMES[spus] : "?";
}

static uint32_t total_points(int spus)
{
	uint32_t sum = 0;

	for (int t = 0; t < BENCH_TESTS; t++) {
		if (!scores[spus][t].ran)
			return 0;
		sum += scores[spus][t].points;
	}
	return sum;
}

static void rate_text(char *buf, size_t len, int test, double rate)
{
	if (test == BENCH_MEMORY)
		snprintf(buf, len, "%.1f GB/S", rate / 1024.0);
	else if (test == BENCH_MANDEL)
		snprintf(buf, len, "%.0f M ITER/S", rate);
	else
		snprintf(buf, len, "%.0f MB/S", rate);
}

static void clock_text(char *buf, size_t len, uint64_t secs)
{
	if (secs >= 3600)
		snprintf(buf, len, "%u:%02u:%02u", (unsigned)(secs / 3600), (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
	else
		snprintf(buf, len, "%02u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
}

static int busy(void)
{
	return state != ST_IDLE || request != REQ_NONE;
}

/* hands the runner a request whose parameters are already set */
static void post(int req)
{
	__asm__ volatile("sync" ::: "memory");
	request = req;
}

/* ---- the runner thread -------------------------------------------------------------------------- */

static void run_test(int t, int spus)
{
	bench_score s;
	char rate[32];

	cur_test = t;
	cur_spus = spus;
	if (bench_run(t, spus, &s) != 0) {
		if (!stop_requested && !quit)
			log_line("%s with %s: NO ANSWER from an SPU, run discarded", bench_name(t), config_name(spus));
		return;
	}
	scores[spus][t] = s;
	total_errors += s.errors;
	rate_text(rate, sizeof(rate), t, s.rate);
	log_line("%-11s %-6s %6u points  %s (%.3f %s), %u jobs in %.2f s, %s", bench_name(t),
	         config_name(spus), s.points, rate, s.rate, bench_unit(t), s.jobs, s.seconds,
	         !s.checked ? "nothing to compare with" : s.errors ? "ERRORS" : "checksums OK");
	if (s.errors)
		log_line("  -> %u of %u results WRONG", s.errors, s.jobs);
}

static void run_set(int spus)
{
	for (int t = 0; t < BENCH_TESTS && !stop_requested && !quit; t++)
		run_test(t, spus);
	cur_test = -1;
	if (total_points(spus))
		log_line("Total %s: %u points", config_name(spus), total_points(spus));
}

static void runner(void *arg)
{
	(void)arg;
	bench_prepare();
	log_line("Prepared: references for ChaCha20 and memory worked out");
	if (bench_spus()) {
		bench_probe_result pr;

		if (bench_probe(&pr) == 0) {
			start_latency_ns = pr.latency_ns;
			start_mhz = pr.mhz;
			log_line("Cell clock %.1f MHz against the timebase (%d SPEs, %.1f to %.1f, fastest of %d blocks each; SPU 0 "
			         "on average %.1f, a loop end %.1f cycles), RAM latency %.0f ns", pr.mhz, pr.spus, pr.mhz_min, pr.mhz_max,
			         PROBE_BLOCKS, pr.mhz_avg, pr.loop_end_cycles, pr.latency_ns);
		} else {
			log_line("Cell clock: the measurement failed");
		}
	}
	state = ST_IDLE;
	while (!quit) {
		int req = request, spus;

		if (req == REQ_NONE) {
			sysUsleep(10000);
			continue;
		}
		__asm__ volatile("sync" ::: "memory");   /* the parameters as the screen wrote them */
		spus = request_spus;
		request = REQ_NONE;
		stop_requested = 0;
		if (req == REQ_TEST) {
			state = ST_RUNNING;
			run_test(request_test, spus);
			cur_test = -1;
		} else if (req == REQ_ALL) {
			state = ST_RUNNING;
			run_set(spus);
		} else if (req == REQ_SERIES) {
			state = ST_RUNNING;
			log_line("Series PPU to %d SPEs", spus);
			for (int n = 0; n <= spus && !stop_requested && !quit; n++)
				run_set(n);
#ifdef BENCH_AUTOTEST
			{
				uint64_t spu, ppu;
				if (bench_mandel_compare(&spu, &ppu))
					log_line("Mandelbrot check: SPU %llu, PPU %llu iterations, difference %.4f %%",
					         (unsigned long long)spu, (unsigned long long)ppu,
					         100.0 * ((double)spu - (double)ppu) / (double)ppu);
			}
#endif
		} else if (req == REQ_ENDURANCE) {
			uint32_t errors_before = total_errors;
			state = ST_ENDURANCE;
			endurance_rounds = 0;
			endurance_start = (uint64_t)sysGetSystemTime();
			log_line("Endurance run with %s started", config_name(spus));
			while (!stop_requested && !quit) {
				run_set(spus);
				if (!stop_requested)
					endurance_rounds++;
			}
			log_line("Endurance run ended after %u rounds, %.0f s, %u errors", endurance_rounds,
			         ((uint64_t)sysGetSystemTime() - endurance_start) / 1e6, total_errors - errors_before);
		} else if (req == REQ_FULLLOAD) {
			state = ST_FULLLOAD;
			stress_run(request_load, request_secs, &quit);
			total_errors += stress_get()->errors_total;
		}
		state = ST_IDLE;
	}
	sysThreadExit(0);
}

/* ---- drawing (V0.6) --------------------------------------------------------------------------------- */

/* The interface draws with ui.h: Roboto and round shapes on a 1920x1080 layout, every frame anew (a few
   thousand quads, ~0.2 ms of the PPU), so something on the screen always moves. Every text is measured
   before it is placed, and none runs over a shape. The colours are opaque, so the pixel font's stand-in
   (ui.h's fallback) shows them too. */
#define K_BACK     0xFF0B0E13
#define K_CARD     0xFF151A22
#define K_CARD_HI  0xFF1D232D
#define K_LINE     0xFF2A313C
#define K_TEXT     0xFFEDF0F4
#define K_SOFT     0xFFC5CCD6
#define K_DIM      0xFF8B95A3
#define K_FAINT    0xFF59626F
#define K_ACCENT   0xFFFF9A3C
#define K_HOT      0xFFFF5A36
#define K_GOOD     0xFF4CD964
#define K_BAD      0xFFFF453A
#define K_RSX      0xFF4FC3F7
#define K_RAM      0xFFB18CFF
#define K_WHITE    0xFFFFFFFF
/* the pad's own symbols */
#define B_CROSS    0xFF8FB8FF
#define B_CIRCLE   0xFFFF7A7A
#define B_TRIANGLE 0xFF5EE0B0

#define LEFT     96.0f      /* the margins, inside a TV's overscan */
#define RIGHT    1824.0f
#define WIDE     (RIGHT - LEFT)
#define BAR_Y    876.0f     /* the status bar */
#define BAR_H    64.0f
#define FOOT_Y   990.0f     /* the middle of the key help */
#define PI2      6.2831853f

static float anim;          /* seconds for what moves, wraps every 600 s */
static int stress_result_open;   /* the finished stress test stays on the screen until CIRCLE */

/* 0..1 over a period, and a soft 0..1..0 pulse */
static float cycle(float period)
{
	float t = anim / period;

	return t - floorf(t);
}

static float pulse(float period)
{
	return 0.5f - 0.5f * cosf(cycle(period) * PI2);
}

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
	uint32_t out = 0;

	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	for (int sh = 0; sh < 32; sh += 8) {
		float ca = (float)((a >> sh) & 0xFF), cb = (float)((b >> sh) & 0xFF);

		out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << sh;
	}
	return out;
}

/* a colour with another alpha, for glows */
static uint32_t alpha(uint32_t argb, float a)
{
	return (uint32_t)(a * 255.0f + 0.5f) << 24 | (argb & 0xFFFFFF);
}

/* text centred on the line cy */
static float text_mid(float x, float cy, int style, uint32_t argb, const char *s)
{
	return ui_text(x, cy - ui_cap(style) / 2.0f, style, argb, s);
}

/* the largest style from style down to UI_SMALL in which s fits w */
static int fit(int style, float w, const char *s)
{
	while (style > UI_SMALL && ui_text_w(style, s) > w)
		style--;
	return style;
}

/* fit(), and where even UI_SMALL is too wide, s cut short with "..." - a text never runs into the next */
static int clip(char *s, int style, float w)
{
	size_t n = strlen(s);

	style = fit(style, w, s);
	while (n > 3 && ui_text_w(style, s) > w) {
		n--;
		while (n > 0 && s[n - 1] == ' ')
			n--;
		memcpy(s + n, "...", 4);
	}
	return style;
}

static float text_wn(int style, const char *s, size_t n)
{
	char buf[256];

	if (n >= sizeof(buf))
		n = sizeof(buf) - 1;
	memcpy(buf, s, n);
	buf[n] = 0;
	return ui_text_w(style, buf);
}

/* a paragraph in lines of at most w, `lines` apart, at most max_lines of them (0: all); returns its
   height (draw 0: only measures) */
static float para(float x, float y, float w, int style, float lines, uint32_t argb, const char *s, int draw,
                  int max_lines)
{
	float h = 0.0f;
	int n = 0;

	while (*s == ' ')
		s++;
	while (*s && (!max_lines || n++ < max_lines)) {
		const char *q = s, *end = s;
		float lw = 0.0f;

		while (*q) {
			const char *word = q;
			float ww;

			while (*q == ' ')
				q++;
			while (*q && *q != ' ')
				q++;
			ww = text_wn(style, word, (size_t)(q - word));
			if (end != s && lw + ww > w)
				break;
			lw += ww;
			end = q;
		}
		if (draw) {
			char buf[256];
			size_t n = (size_t)(end - s);

			if (n >= sizeof(buf))
				n = sizeof(buf) - 1;
			memcpy(buf, s, n);
			buf[n] = 0;
			ui_text(x, y + h, style, argb, buf);
		}
		h += lines;
		s = end;
		while (*s == ' ')
			s++;
	}
	return h;
}

/* a short text in at most n lines: Roboto 28, or 22 where 28 needs more lines (the pixel font is wider) */
static void para_fit(float x, float y, float w, int n, uint32_t argb, const char *s)
{
	int style = para(0.0f, 0.0f, w, UI_BODY, 1.0f, 0, s, 0, 0) <= (float)n ? UI_BODY : UI_SMALL;

	para(x, y, w, style, style == UI_BODY ? 36.0f : 30.0f, argb, s, 1, n);
}

/* a turning arc: something is being worked on */
static void spinner(float cx, float cy, float r, float width, uint32_t argb)
{
	float t = cycle(1.1f);

	ui_arc(cx, cy, r, width, 0.0f, 1.0f, K_LINE);
	ui_arc(cx, cy, r, width, t, t + 0.3f, argb);
}

/* a key's name in a little box; returns its width */
static float key_pill(float x, float cy, const char *key)
{
	float w = ui_text_w(UI_SMALL, key) + 22.0f;

	ui_round(x, cy - 16.0f, w, 32.0f, 8.0f, K_LINE);
	text_mid(x + 11.0f, cy, UI_SMALL, K_SOFT, key);
	return w;
}

/* the key help: a pad symbol or a key's name, then what it does; returns where the next one goes */
static float hint_button(float x, int icon, uint32_t argb, const char *what)
{
	ui_icon(icon, x, FOOT_Y - 18.0f, 36.0f, argb);
	x += 46.0f;
	x += text_mid(x, FOOT_Y, UI_BODY, K_DIM, what);
	return x + 44.0f;
}

static float hint_key(float x, const char *key, const char *what)
{
	x += key_pill(x, FOOT_Y, key) + 12.0f;
	x += text_mid(x, FOOT_Y, UI_BODY, K_DIM, what);
	return x + 44.0f;
}

/* a coloured tag; returns where the next one goes */
static float tag(float x, float cy, const char *s, uint32_t argb)
{
	float w = ui_text_w(UI_SMALL, s) + 20.0f;

	ui_round(x, cy - 15.0f, w, 30.0f, 8.0f, mix(K_CARD, argb, 0.2f));
	text_mid(x + 10.0f, cy, UI_SMALL, argb, s);
	return x + w + 8.0f;
}

static float tag_w(const char *s)
{
	return ui_text_w(UI_SMALL, s) + 28.0f;
}

/* the title top left, a pill after it and a line under it */
static void header(const char *title, const char *pill, uint32_t pill_rgb, const char *sub)
{
	float x = LEFT + 30.0f;

	ui_round(LEFT, 58.0f, 8.0f, 50.0f, 4.0f, K_ACCENT);
	x += ui_text(x, 62.0f, UI_TITLE, K_TEXT, title) + 20.0f;
	if (pill) {
		float w = ui_text_w(UI_LABEL, pill) + 32.0f;

		ui_round(x, 64.0f, w, 38.0f, 19.0f, mix(K_BACK, pill_rgb, 0.22f));
		text_mid(x + 16.0f, 83.0f, UI_LABEL, pill_rgb, pill);
	}
	if (sub)
		ui_text(LEFT + 30.0f, 124.0f, UI_BODY, K_DIM, sub);
}

/* a chip in the header, right-aligned at right; returns the next one's right edge */
static float chip(float right, const char *s, uint32_t dot, uint32_t text_rgb)
{
	float w = ui_text_w(UI_LABEL, s) + (dot ? 58.0f : 36.0f), x = right - w;

	ui_round(x, 61.0f, w, 44.0f, 22.0f, K_CARD);
	if (dot)
		ui_circle(x + 26.0f, 83.0f, 7.0f, dot);
	text_mid(x + (dot ? 42.0f : 18.0f), 83.0f, UI_LABEL, text_rgb, s);
	return x - 12.0f;
}

static uint32_t heat_rgb(int c)
{
	return c >= STRESS_TEMP_STOP - 5 ? K_BAD : c >= 75 ? K_ACCENT : K_GOOD;
}

/* webMAN's temperatures and fan, top right */
static void temp_chips(void)
{
	int cpu = -1, rsx = -1, fan = -1;
	float r = RIGHT;
	char buf[32];

	if (!webman_temps(&cpu, &rsx, &fan)) {
		chip(r, "No temperatures from webMAN", 0, K_FAINT);
		return;
	}
	if (fan >= 0) {
		snprintf(buf, sizeof(buf), "Fan %d %%", fan);
		r = chip(r, buf, 0, K_SOFT);
	}
	if (rsx >= 0) {
		snprintf(buf, sizeof(buf), "RSX %d`C", rsx);
		r = chip(r, buf, heat_rgb(rsx), K_TEXT);
	}
	if (cpu >= 0) {
		snprintf(buf, sizeof(buf), "CPU %d`C", cpu);
		chip(r, buf, heat_rgb(cpu), K_TEXT);
	}
}

static const char *config_label(int spus)
{
	static const char *const NAMES[CONFIGS] = { "PPU", "1 SPE", "2 SPEs", "3 SPEs", "4 SPEs", "5 SPEs", "6 SPEs" };
	return spus >= 0 && spus < CONFIGS ? NAMES[spus] : "?";
}

static const char *test_title(int t)
{
	static const char *const NAMES[BENCH_TESTS] = { "Float", "Integer", "Memory" };
	return t >= 0 && t < BENCH_TESTS ? NAMES[t] : "?";
}

static void rate_label(char *buf, size_t len, int test, double rate)
{
	if (test == BENCH_MEMORY)
		snprintf(buf, len, "%.1f GB/s", rate / 1024.0);
	else if (test == BENCH_MANDEL)
		snprintf(buf, len, "%.0f M iter/s", rate);
	else
		snprintf(buf, len, "%.0f MB/s", rate);
}

static void duration_label(char *buf, size_t len, uint32_t secs)
{
	if (!secs)
		snprintf(buf, len, "Endless");
	else if (secs < 60)
		snprintf(buf, len, "%u s", (unsigned)secs);
	else
		snprintf(buf, len, "%u min", (unsigned)(secs / 60));
}

/* what the runner is doing, at the foot of the menu and the table */
static void status_bar(uint64_t now)
{
	char line[160], err[32];
	float x = LEFT + 30.0f, cy = BAR_Y + BAR_H / 2.0f;
	int idle = !busy();

	ui_round(LEFT, BAR_Y, WIDE, BAR_H, 18.0f, K_CARD);
	if (state == ST_PREPARING) {
		snprintf(line, sizeof(line), "Preparing: working out the references");
	} else if (state == ST_ENDURANCE) {
		uint32_t secs = (uint32_t)((now - endurance_start) / 1000000);
		snprintf(line, sizeof(line), "Endurance run with %s: round %u, %02u:%02u", config_label(cur_spus),
		         endurance_rounds + 1, secs / 60, secs % 60);
	} else if (state == ST_RUNNING && cur_test >= 0) {
		snprintf(line, sizeof(line), "Running: %s with %s", test_title(cur_test), config_label(cur_spus));
	} else if (state == ST_FULLLOAD) {
		snprintf(line, sizeof(line), "The stress test is running");
	} else if (!idle) {
		snprintf(line, sizeof(line), "Starting ...");
	} else {
		snprintf(line, sizeof(line), "Ready");
	}
	if (idle)
		ui_circle(x + 10.0f, cy, 8.0f, K_GOOD);
	else
		spinner(x + 10.0f, cy, 11.0f, 5.0f, K_ACCENT);
	text_mid(x + 38.0f, cy, clip(line, UI_LABEL, 880.0f), K_TEXT, line);
	if (!idle && state != ST_FULLLOAD) {
		float p = (float)bench_progress() / 1000.0f;
		char pct[16];

		p = p < 0.0f ? 0.0f : p > 1.0f ? 1.0f : p;
		ui_round(1010.0f, cy - 5.0f, 480.0f, 10.0f, 5.0f, K_LINE);
		ui_round(1010.0f, cy - 5.0f, 480.0f * p, 10.0f, 5.0f, K_ACCENT);
		snprintf(pct, sizeof(pct), "%d %%", (int)(p * 100.0f));
		text_mid(1508.0f, cy, UI_SMALL, K_DIM, pct);
	}
	if (total_errors == 1)
		snprintf(err, sizeof(err), "1 error");
	else
		snprintf(err, sizeof(err), "%u errors", total_errors);
	ui_text_right(RIGHT - 30.0f, cy - ui_cap(UI_LABEL) / 2.0f, UI_LABEL, total_errors ? K_BAD : K_GOOD, err);
}

/* ---- the menu ----------------------------------------------------------------------------------- */

#define GRID_COLS  4
#define CARD_W     414.0f
#define CARD_H     294.0f
#define CARD_GAP   24.0f
#define GRID_Y     244.0f

static const int MENU_ICON[M_ENTRIES] = { UI_ICON_WAVE, UI_ICON_HASH, UI_ICON_CHIP, UI_ICON_LIST, UI_ICON_BARS,
                                          UI_ICON_REPEAT, UI_ICON_FLAME, UI_ICON_TABLE };

/* a card's last result, bottom right */
static void menu_result(int entry, char *buf, size_t len, uint32_t *rgb)
{
	const stress_status *s = stress_get();
	int spus = bench_spus(), n = 0;

	*rgb = K_ACCENT;
	switch (entry) {
	case M_FLOAT:
	case M_INT:
	case M_MEM: {
		const bench_score *sc = &scores[selected][entry - M_FLOAT];
		if (sc->ran) {
			snprintf(buf, len, "%u points", sc->points);
			if (sc->errors)
				*rgb = K_BAD;
			return;
		}
		break;
	}
	case M_ALL:
		if (total_points(selected)) {
			snprintf(buf, len, "%u points", total_points(selected));
			return;
		}
		break;
	case M_SERIES:
		if (total_points(spus)) {
			snprintf(buf, len, "%u points", total_points(spus));
			return;
		}
		break;
	case M_ENDURANCE:
		if (endurance_rounds) {
			snprintf(buf, len, "%u round%s", endurance_rounds, endurance_rounds == 1 ? "" : "s");
			return;
		}
		snprintf(buf, len, "No time limit");
		*rgb = K_FAINT;
		return;
	case M_FULLLOAD:
		if (state == ST_FULLLOAD) {
			snprintf(buf, len, "Running");
			return;
		}
		if (s->phase == STRESS_DONE) {
			snprintf(buf, len, "%s", s->result == STRESS_PASSED ? "Passed" : s->result == STRESS_FAILED ? "Not passed" :
			         "Cancelled");
			*rgb = s->result == STRESS_PASSED ? K_GOOD : s->result == STRESS_FAILED ? K_BAD : K_ACCENT;
			return;
		}
		snprintf(buf, len, "6 loads");
		*rgb = K_FAINT;
		return;
	default:
		for (int c = 0; c < CONFIGS; c++)
			for (int t = 0; t < BENCH_TESTS; t++)
				n += scores[c][t].ran;
		if (n) {
			snprintf(buf, len, "%d result%s", n, n == 1 ? "" : "s");
			return;
		}
		snprintf(buf, len, "Empty");
		*rgb = K_FAINT;
		return;
	}
	snprintf(buf, len, "Not run yet");
	*rgb = K_FAINT;
}

static void menu_card(int i, float x, float y)
{
	char res[48], who[24];
	int sel = i == cursor, dim = busy() && i != M_RESULTS, spus = bench_spus();
	uint32_t tint = i == M_FULLLOAD ? K_HOT : K_ACCENT, res_rgb;
	float tx = x + 28.0f, ty = y + 26.0f, foot = y + CARD_H - 38.0f, room, tw = 0.0f;

	if (sel)
		ui_card(x, y, CARD_W, CARD_H, 22.0f, K_CARD_HI, mix(tint, 0xFFFFE6CC, 0.5f * pulse(1.6f)), 3.0f);
	else
		ui_card(x, y, CARD_W, CARD_H, 22.0f, K_CARD, K_LINE, 2.0f);
	/* the icon on its tile, with a light that breathes on the chosen card */
	if (sel)
		ui_glow(tx + 38.0f, ty + 38.0f, 120.0f, alpha(tint, 0.18f + 0.22f * pulse(1.6f)));
	ui_round(tx, ty, 76.0f, 76.0f, 20.0f, mix(K_CARD, tint, sel ? 0.28f : 0.14f));
	ui_icon(MENU_ICON[i], tx + 12.0f, ty + 12.0f, 52.0f, dim ? K_FAINT : tint);
	if (sel)
		ui_icon(UI_ICON_TRIANGLE, x + CARD_W - 62.0f, y + 30.0f, 32.0f, B_TRIANGLE);   /* it has an info page */
	ui_text(x + 28.0f, y + 124.0f, UI_SUB, dim ? K_DIM : K_TEXT, menu_name(i));
	para_fit(x + 28.0f, y + 170.0f, CARD_W - 56.0f, 2, K_DIM, menu_desc(i));

	/* the foot: for what it runs on the left, its last result on the right - the tags only where both fit */
	menu_result(i, res, sizeof(res), &res_rgb);
	room = CARD_W - 56.0f - ui_text_w(UI_LABEL, res) - 16.0f;
	ui_text_right(x + CARD_W - 28.0f, foot - ui_cap(UI_LABEL) / 2.0f, UI_LABEL, res_rgb, res);
	if (i == M_FULLLOAD) {
		tw = tag_w("CPU") + tag_w("RAM") + tag_w("RSX");
		if (tw <= room) {
			float tx2 = tag(x + 28.0f, foot, "CPU", K_ACCENT);
			tx2 = tag(tx2, foot, "RAM", K_RAM);
			tag(tx2, foot, "RSX", K_RSX);
		}
		return;
	}
	if (i == M_RESULTS)
		return;
	if (i == M_SERIES && spus)
		snprintf(who, sizeof(who), "PPU to %d SPEs", spus);
	else if (i == M_SERIES)
		snprintf(who, sizeof(who), "PPU only");
	else
		snprintf(who, sizeof(who), "%s", config_label(selected));
	if (tag_w(who) <= room)
		tag(x + 28.0f, foot, who, dim ? K_FAINT : K_SOFT);
}

static void draw_menu(uint64_t now)
{
	char buf[64];
	int spus = bench_spus();
	float r = RIGHT, x = LEFT, cy = 190.0f;

	header("TEE PS3 Benchmark", VERSION, K_ACCENT, NULL);
	if (start_latency_ns > 0.0) {
		snprintf(buf, sizeof(buf), "RAM %.0f ns", start_latency_ns);
		r = chip(r, buf, 0, K_SOFT);
	}
	if (start_mhz > 0.0) {
		snprintf(buf, sizeof(buf), "Cell %.0f MHz", start_mhz);
		r = chip(r, buf, 0, K_SOFT);
	}
	if (spus)
		snprintf(buf, sizeof(buf), "%d SPEs ready", spus);
	else
		snprintf(buf, sizeof(buf), "SPEs not started");
	chip(r, buf, spus ? K_GOOD : K_BAD, K_TEXT);

	/* PPU or 1..6 SPEs for the tests */
	x += text_mid(x, cy, UI_BODY, K_DIM, "Run on") + 24.0f;
	for (int i = 0; i <= spus; i++) {
		float w = i ? 64.0f : 92.0f;
		int on = i == selected;

		if (i)
			snprintf(buf, sizeof(buf), "%d", i);
		else
			snprintf(buf, sizeof(buf), "PPU");
		ui_round(x, cy - 24.0f, w, 48.0f, 14.0f, on ? K_ACCENT : K_CARD);
		ui_text_center(x + w / 2.0f, cy - ui_cap(UI_LABEL) / 2.0f, UI_LABEL, on ? K_BACK : K_SOFT, buf);
		x += w + 10.0f;
	}
	x += 16.0f;
	x += key_pill(x, cy, "L1") + 6.0f;
	x += key_pill(x, cy, "R1") + 14.0f;
	text_mid(x, cy, UI_BODY, K_DIM, selected ? "SPEs for the tests" : "The PPU alone");
	snprintf(buf, sizeof(buf), "Every test %d s", BENCH_SECONDS);
	ui_text_right(RIGHT, cy - ui_cap(UI_BODY) / 2.0f, UI_BODY, K_DIM, buf);

	for (int i = 0; i < M_ENTRIES; i++)
		menu_card(i, LEFT + (float)(i % GRID_COLS) * (CARD_W + CARD_GAP),
		          GRID_Y + (float)(i / GRID_COLS) * (CARD_H + CARD_GAP));

	status_bar(now);
	x = hint_key(LEFT, "ARROWS", "Choose");
	x = hint_button(x, UI_ICON_CROSS, B_CROSS, "Start");
	x = hint_button(x, UI_ICON_TRIANGLE, B_TRIANGLE, "Info");
	x = hint_key(x, "L1 / R1", "PPU or SPEs");
	x = hint_key(x, "SELECT", "Photo");
	hint_key(x, "START", "Quit");
}

/* ---- the info pages ------------------------------------------------------------------------------- */

static float info_height(const char *const *lines, float w, int style, float line, float gap, float head)
{
	float h = 0.0f;

	for (; *lines; lines++)
		h += (*lines)[0] == '#' ? head : para(0.0f, 0.0f, w, style, line, 0, *lines, 0, 0) + gap;
	return h;
}

static void draw_info(void)
{
	const char *const *lines = info_lines(info_entry);
	const float top = 172.0f, bottom = 948.0f, pad = 40.0f, w = WIDE - 2 * pad;
	float y, line = 38.0f, gap = 22.0f, head = 42.0f;
	int body = UI_BODY;
	char sub[48];
	float x;

	snprintf(sub, sizeof(sub), "Info %d of %d", info_entry + 1, M_ENTRIES);
	header(menu_title(info_entry), NULL, 0, sub);
	ui_round(RIGHT - 80.0f, 50.0f, 80.0f, 80.0f, 20.0f, mix(K_CARD, K_ACCENT, 0.18f));
	ui_icon(MENU_ICON[info_entry], RIGHT - 66.0f, 64.0f, 52.0f, K_ACCENT);

	/* the largest text that fits the card: Roboto 28 with air, then tighter, then 22 */
	if (info_height(lines, w, body, line, gap, head) > bottom - top - 2 * pad) {
		line = 35.0f;
		gap = 14.0f;
		head = 38.0f;
		if (info_height(lines, w, body, line, gap, head) > bottom - top - 2 * pad) {
			body = UI_SMALL;
			line = 30.0f;
			gap = 12.0f;
			head = 36.0f;
		}
	}
	ui_round(LEFT, top, WIDE, bottom - top, 24.0f, K_CARD);
	y = top + pad;
	for (; *lines; lines++) {
		if ((*lines)[0] == '#') {
			ui_text(LEFT + pad, y, UI_LABEL, K_ACCENT, *lines + 1);
			y += head;
		} else {
			y += para(LEFT + pad, y, w, body, line, K_SOFT, *lines, 1, 0) + gap;
		}
	}
	x = hint_button(LEFT, UI_ICON_CIRCLE_BTN, B_CIRCLE, "Back");
	x = hint_key(x, "ARROWS", "Other test");
	hint_key(x, "SELECT", "Photo");
}

static void open_info(int entry, int back)
{
	info_entry = entry;
	info_back = back;
	screen = SCR_INFO;
}

/* ---- the table of results ------------------------------------------------------------------------- */

static void draw_results(uint64_t now)
{
	static const float COL[BENCH_TESTS] = { 430.0f, 800.0f, 1170.0f };
	static const char *const KIND[BENCH_TESTS] = { "Mandelbrot", "ChaCha20", "DMA read" };
	const float top = 170.0f, head_h = 78.0f, row_h = 84.0f, tot_x = 1500.0f, bar_x = 1640.0f;
	const float bar_w = RIGHT - 40.0f - bar_x;
	char sub[64], buf[48];
	uint32_t best = 1;
	int spus = bench_spus(), rows = spus + 1;
	float x;

	snprintf(sub, sizeof(sub), "Points and throughput - every test %d s", BENCH_SECONDS);
	header("Results", NULL, 0, sub);

	ui_round(LEFT, top, WIDE, head_h + rows * row_h + 12.0f, 24.0f, K_CARD);
	ui_text(LEFT + 40.0f, top + 30.0f, UI_LABEL, K_DIM, "Run");
	for (int t = 0; t < BENCH_TESTS; t++) {
		ui_text(COL[t], top + 22.0f, UI_LABEL, K_SOFT, test_title(t));
		ui_text(COL[t], top + 50.0f, UI_SMALL, K_FAINT, KIND[t]);
	}
	ui_text(tot_x, top + 30.0f, UI_LABEL, K_SOFT, "Total");
	for (int n = 0; n <= spus; n++)
		if (total_points(n) > best)
			best = total_points(n);

	for (int n = 0; n < rows; n++) {
		float ry = top + head_h + n * row_h, mid = ry + row_h / 2.0f;
		int run = (state == ST_RUNNING || state == ST_ENDURANCE) && cur_spus == n;
		uint32_t errors = 0, total = total_points(n);

		ui_rect(LEFT + 24.0f, ry, WIDE - 48.0f, 2.0f, K_LINE);
		if (run) {
			ui_round(LEFT + 12.0f, ry + 6.0f, WIDE - 24.0f, row_h - 12.0f, 16.0f, K_CARD_HI);
			ui_round(LEFT + 12.0f, ry + 22.0f, 5.0f, row_h - 44.0f, 2.5f, K_ACCENT);
		}
		text_mid(LEFT + 40.0f, mid, UI_SUB, run ? K_ACCENT : K_TEXT, config_label(n));
		for (int t = 0; t < BENCH_TESTS; t++) {
			const bench_score *s = &scores[n][t];

			if (run && cur_test == t) {
				float p = (float)bench_progress() / 1000.0f;

				p = p < 0.0f ? 0.0f : p > 1.0f ? 1.0f : p;
				ui_text(COL[t], ry + 18.0f, UI_LABEL, K_ACCENT, "Running");
				ui_round(COL[t], ry + 54.0f, 230.0f, 8.0f, 4.0f, K_LINE);
				ui_round(COL[t], ry + 54.0f, 230.0f * p, 8.0f, 4.0f, K_ACCENT);
				continue;
			}
			if (!s->ran) {
				text_mid(COL[t], mid, UI_SUB, K_FAINT, "-");
				continue;
			}
			errors += s->errors;
			snprintf(buf, sizeof(buf), "%u", s->points);
			ui_text(COL[t], ry + 16.0f, UI_SUB, s->errors ? K_BAD : K_TEXT, buf);
			rate_label(buf, sizeof(buf), t, s->rate);
			ui_text(COL[t], ry + 54.0f, UI_SMALL, K_DIM, buf);
		}
		if (total) {
			snprintf(buf, sizeof(buf), "%u", total);
			text_mid(tot_x, mid, UI_SUB, errors ? K_BAD : K_GOOD, buf);
			ui_round(bar_x, mid - 7.0f, bar_w, 14.0f, 7.0f, K_LINE);
			ui_round(bar_x, mid - 7.0f, bar_w * (float)total / (float)best, 14.0f, 7.0f, errors ? K_BAD : K_ACCENT);
		}
	}

	status_bar(now);
	x = hint_button(LEFT, UI_ICON_CIRCLE_BTN, B_CIRCLE, busy() ? "Stop" : "Back");
	x = hint_button(x, UI_ICON_TRIANGLE, B_TRIANGLE, "Info");
	x = hint_key(x, "SELECT", "Photo");
	hint_key(x, "START", "Quit");
}

/* ---- the stress test ------------------------------------------------------------------------------ */

#define LOAD_W  560.0f
#define LOAD_H  206.0f
#define LOAD_Y  246.0f
#define RING_X  400.0f
#define RING_Y  520.0f
#define RING_R  236.0f
#define RING_W  26.0f
#define RX      760.0f      /* the right column of the running test */
#define RW      (RIGHT - RX)

static const int LOAD_ICON[STRESS_LOADS] = { UI_ICON_REPEAT, UI_ICON_FLAME, UI_ICON_CHIP, UI_ICON_GPU, UI_ICON_GPU,
                                             UI_ICON_FLAME };
static const uint32_t LOAD_RGB[STRESS_LOADS] = { K_ACCENT, K_ACCENT, K_RAM, K_RSX, K_RSX, K_HOT };
static const char *const LOAD_DESC[STRESS_LOADS] = {
	"All three benchmarks on the Cell, moving on every 10 s",
	"Maximum heat: every SPE and both PPU threads",
	"CPU max while the RAM is written and read back",
	"The graphics chip alone: shader, fill, vertices and VRAM",
	"CPU max and the RSX load together",
	"CPU, RAM and RSX at once - the whole console"
};

static float load_tags(float x, float cy, int load)
{
	if (stress_load_cpu(load))
		x = tag(x, cy, "CPU", K_ACCENT);
	if (stress_load_ram(load))
		x = tag(x, cy, "RAM", K_RAM);
	if (stress_load_rsx(load))
		x = tag(x, cy, "RSX", K_RSX);
	return x;
}

/* what runs where under a load */
static void load_what(char *buf, size_t len, int load, int spus)
{
	const char *rsx = stress_load_rsx(load) ? " - RSX: shader, fill, vertices, VRAM check" : "";

	if (load == STRESS_RSX)
		snprintf(buf, len, "RSX: multiply-add shader, texture and blend layers, vertices, the VRAM read back - all "
		         "checked, the Cell only feeds it");
	else if (!spus)
		snprintf(buf, len, "SPEs not started - PPU only: %s%s", load == STRESS_MIXED ?
		         "thread 1 ChaCha20, thread 2 VMX Mandelbrot" : "2 threads VMX heat kernel", rsx);
	else if (load == STRESS_MIXED)
		snprintf(buf, len, "%d SPEs: %d each on Mandelbrot, ChaCha20 and memory - PPU: thread 1 ChaCha20, thread 2 "
		         "VMX Mandelbrot", spus, spus / 3);
	else if (stress_load_ram(load) && stress_load_rsx(load))
		snprintf(buf, len, "%d SPEs: heat kernel + RAM by DMA - PPU: 2 threads VMX heat%s", spus, rsx);
	else
		snprintf(buf, len, "%d SPEs: heat kernel (FMA + shuffle)%s - PPU: 2 threads VMX heat%s", spus,
		         stress_load_ram(load) ? " + RAM written and read back by DMA" : "", rsx);
}

static void draw_stress_setup(const stress_status *s)
{
	char buf[192];
	float x = LEFT, cy = 194.0f;
	uint32_t rgb;

	header("Stress test", NULL, 0, "Full load for the cooling and for overclocked consoles - every result checked");
	temp_chips();

	x += text_mid(x, cy, UI_BODY, K_DIM, "Duration") + 24.0f;
	for (int i = 0; i < DURATION_COUNT; i++) {
		float w;
		int on = i == duration_pick;

		duration_label(buf, sizeof(buf), DURATIONS[i]);
		w = ui_text_w(UI_LABEL, buf) + 40.0f;
		ui_round(x, cy - 24.0f, w, 48.0f, 14.0f, on ? K_ACCENT : K_CARD);
		ui_text_center(x + w / 2.0f, cy - ui_cap(UI_LABEL) / 2.0f, UI_LABEL, on ? K_BACK : K_SOFT, buf);
		x += w + 10.0f;
	}
	x += 16.0f;
	x += key_pill(x, cy, "L1") + 6.0f;
	key_pill(x, cy, "R1");

	for (int i = 0; i < STRESS_LOADS; i++) {
		float cx = LEFT + (float)(i % 3) * (LOAD_W + 24.0f), cy0 = LOAD_Y + (float)(i / 3) * (LOAD_H + 24.0f);
		int sel = i == load_pick;
		uint32_t tint = LOAD_RGB[i];

		if (sel)
			ui_card(cx, cy0, LOAD_W, LOAD_H, 22.0f, K_CARD_HI, mix(tint, K_WHITE, 0.45f * pulse(1.6f)), 3.0f);
		else
			ui_card(cx, cy0, LOAD_W, LOAD_H, 22.0f, K_CARD, K_LINE, 2.0f);
		if (sel)
			ui_glow(cx + 60.0f, cy0 + 60.0f, 110.0f, alpha(tint, 0.18f + 0.22f * pulse(1.6f)));
		ui_round(cx + 24.0f, cy0 + 24.0f, 72.0f, 72.0f, 18.0f, mix(K_CARD, tint, sel ? 0.28f : 0.14f));
		ui_icon(LOAD_ICON[i], cx + 36.0f, cy0 + 36.0f, 48.0f, tint);
		ui_text(cx + 116.0f, cy0 + 30.0f, UI_SUB, K_TEXT, stress_load_title(i));
		load_tags(cx + 116.0f, cy0 + 80.0f, i);
		para_fit(cx + 24.0f, cy0 + 122.0f, LOAD_W - 48.0f, 2, K_DIM, LOAD_DESC[i]);
	}

	/* what runs where, and what the start measured */
	ui_round(LEFT, 706.0f, WIDE, 146.0f, 22.0f, K_CARD);
	ui_text(LEFT + 32.0f, 730.0f, UI_SMALL, K_DIM, "What runs");
	if (start_mhz > 0.0) {
		snprintf(buf, sizeof(buf), "Measured at the start: Cell %.1f MHz, RAM latency %.0f ns", start_mhz,
		         start_latency_ns);
		ui_text_right(RIGHT - 32.0f, 730.0f, UI_SMALL, K_DIM, buf);
	}
	load_what(buf, sizeof(buf), load_pick, bench_spus());
	para_fit(LEFT + 32.0f, 768.0f, WIDE - 64.0f, 2, K_SOFT, buf);

	/* ready, and how the last test ended */
	ui_round(LEFT, BAR_Y, WIDE, BAR_H, 18.0f, K_CARD);
	ui_circle(LEFT + 40.0f, BAR_Y + BAR_H / 2.0f, 8.0f, K_GOOD);
	{
		char line[96], d[24];

		duration_label(d, sizeof(d), DURATIONS[duration_pick]);
		if (DURATIONS[duration_pick])
			snprintf(line, sizeof(line), "Ready - X starts %s for %s", stress_load_title(load_pick), d);
		else
			snprintf(line, sizeof(line), "Ready - X starts %s until circle", stress_load_title(load_pick));
		text_mid(LEFT + 68.0f, BAR_Y + BAR_H / 2.0f, UI_LABEL, K_TEXT, line);
	}
	if (s->phase == STRESS_DONE) {
		rgb = s->result == STRESS_PASSED ? K_GOOD : s->result == STRESS_FAILED ? K_BAD : K_ACCENT;
		snprintf(buf, sizeof(buf), "Last: %s (%s)", s->result == STRESS_PASSED ? "passed" : s->result == STRESS_FAILED ?
		         "not passed" : "cancelled", stress_load_title(s->load));
		ui_text_right(RIGHT - 30.0f, BAR_Y + BAR_H / 2.0f - ui_cap(UI_LABEL) / 2.0f, UI_LABEL, rgb, buf);
	}

	x = hint_button(LEFT, UI_ICON_CROSS, B_CROSS, "Start");
	x = hint_key(x, "ARROWS", "Load");
	x = hint_key(x, "L1 / R1", "Duration");
	x = hint_button(x, UI_ICON_CIRCLE_BTN, B_CIRCLE, "Back");
	x = hint_button(x, UI_ICON_TRIANGLE, B_TRIANGLE, "Info");
	hint_key(x, "SELECT", "Photo");
}

/* a number in a card with its name over it and a line under it */
static void metric(float x, float y, float w, const char *label, const char *value, uint32_t rgb, const char *sub)
{
	ui_round(x, y, w, 148.0f, 20.0f, K_CARD);
	ui_text(x + 26.0f, y + 24.0f, UI_SMALL, K_DIM, label);
	ui_text(x + 26.0f, y + 56.0f, fit(UI_NUM, w - 52.0f, value), rgb, value);
	if (sub)
		ui_text(x + 26.0f, y + 110.0f, UI_SMALL, K_DIM, sub);
}

static void draw_stress_run(const stress_status *s, int run)
{
	int done = !run && s->phase == STRESS_DONE;
	int live = done || (run && (s->phase == STRESS_RUNNING || s->phase == STRESS_STOPPING));
	int load = live ? s->load : request_load, spus = bench_spus(), row = 0;
	uint32_t rc = !done ? K_ACCENT : s->result == STRESS_PASSED ? K_GOOD : s->result == STRESS_FAILED ? K_BAD : K_ACCENT;
	uint32_t secs = live ? s->duration_s : request_secs;
	char a[32], b[48], buf[192];
	float inner = RING_R - RING_W / 2.0f - 24.0f, f = 0.0f, x;

	if (load < 0 || load >= STRESS_LOADS)
		load = STRESS_MIXED;
	duration_label(a, sizeof(a), secs);
	if (done)
		snprintf(buf, sizeof(buf), "Finished - %s", secs ? a : "endless");
	else if (secs)
		snprintf(buf, sizeof(buf), "Running for %s - every result is checked", a);
	else
		snprintf(buf, sizeof(buf), "Running until circle - every result is checked");
	header("Stress test", stress_load_title(load), LOAD_RGB[load], buf);
	temp_chips();

	/* the ring: the time so far, and a dot that always goes round while the test runs */
	if (live && secs)
		f = (float)((double)s->elapsed_us / (secs * 1e6));
	f = f > 1.0f ? 1.0f : f;
	ui_arc(RING_X, RING_Y, RING_R, RING_W, 0.0f, 1.0f, K_LINE);
	if (!live) {
		float t = cycle(1.4f);
		ui_arc(RING_X, RING_Y, RING_R, RING_W, t, t + 0.22f, K_ACCENT);
	} else {
		if (secs)
			ui_arc(RING_X, RING_Y, RING_R, RING_W, 0.0f, f, rc);
		else
			ui_arc(RING_X, RING_Y, RING_R, RING_W, 0.0f, 1.0f, done ? rc : mix(K_LINE, K_ACCENT, 0.3f));
		if (!done) {
			float t = cycle(3.0f), dx = RING_X + RING_R * sinf(t * PI2), dy = RING_Y - RING_R * cosf(t * PI2);

			ui_arc(RING_X, RING_Y, RING_R, RING_W * 0.44f, t - 0.08f, t, 0xFFFFC890);
			ui_glow(dx, dy, 44.0f, 0xA0FFB060);
			ui_circle(dx, dy, RING_W * 0.46f, K_WHITE);
		}
	}

	/* inside the ring; the time's width is measured against the circle at its top line */
	if (!live) {
		ui_text_center(RING_X, RING_Y - 44.0f, UI_SUB, K_TEXT, "Preparing");
		if (stress_load_cpu(load))
			snprintf(buf, sizeof(buf), "VMX references %d %%", bench_progress() / 10);
		else
			snprintf(buf, sizeof(buf), "Measuring the Cell's clock");
		ui_text_center(RING_X, RING_Y + 8.0f, UI_BODY, K_DIM, buf);
	} else {
		int big = UI_HERO;

		clock_text(b, sizeof(b), s->elapsed_us / 1000000);
		if (ui_text_w(big, b) > 2.0f * sqrtf(inner * inner - 64.0f * 64.0f))
			big = UI_TITLE;
		ui_text_center(RING_X, RING_Y - 104.0f, UI_SMALL, K_DIM, done ? "Ran for" : "Elapsed");
		ui_text_center(RING_X, RING_Y - 62.0f, big, K_TEXT, b);
		if (secs) {
			clock_text(a, sizeof(a), secs);
			snprintf(buf, sizeof(buf), "of %s", a);
		} else {
			snprintf(buf, sizeof(buf), "endless");
		}
		ui_text_center(RING_X, RING_Y + 30.0f, UI_BODY, K_DIM, buf);
		if (done)
			snprintf(buf, sizeof(buf), "%s", s->result == STRESS_PASSED ? "Passed" : s->result == STRESS_FAILED ?
			         "Not passed" : "Cancelled");
		else if (s->phase == STRESS_STOPPING)
			snprintf(buf, sizeof(buf), "Stopping");
		else if (secs)
			snprintf(buf, sizeof(buf), "%d %%", (int)(f * 100.0f));
		else
			buf[0] = 0;
		ui_text_center(RING_X, RING_Y + 76.0f, UI_LABEL, rc, buf);
	}
	if (live && !done) {
		float w = ui_text_w(UI_BODY, "Live - every second anew"), lx = RING_X - (w + 26.0f) / 2.0f;

		ui_circle(lx + 7.0f, RING_Y + RING_R + 60.0f, 4.0f + 3.0f * pulse(1.0f), K_GOOD);
		text_mid(lx + 26.0f, RING_Y + RING_R + 60.0f, UI_BODY, K_DIM, "Live - every second anew");
	}

	/* three numbers */
	{
		float mw = (RW - 40.0f) / 3.0f;
		uint64_t checked = s->rsx_checks;   /* the RSX's check tiles and VRAM slices; every Cell job is checked */

		for (int p = 0; p < PART_RSX; p++)
			checked += s->jobs[p];
		if (live) {
			snprintf(a, sizeof(a), "%u", s->errors_total);
			snprintf(b, sizeof(b), "%llu results checked", (unsigned long long)checked);
			metric(RX, 170.0f, mw, "Errors", a, s->errors_total ? K_BAD : K_GOOD, b);
		} else {
			metric(RX, 170.0f, mw, "Errors", "-", K_FAINT, "nothing checked yet");
		}

		if (stress_load_cpu(load) && spus) {
			if (live && s->spu_busy_pct > 0.0)
				snprintf(a, sizeof(a), "%.1f %%", s->spu_busy_pct);
			else
				snprintf(a, sizeof(a), "-");
			if (stress_load_rsx(load) && live && s->rsx_pct > 0.0)
				snprintf(b, sizeof(b), "RSX busy %.1f %%", s->rsx_pct);
			else
				snprintf(b, sizeof(b), "the SPEs' share computing");
			metric(RX + mw + 20.0f, 170.0f, mw, "SPU busy", a, live ? K_TEXT : K_FAINT, b);
		} else if (stress_load_rsx(load)) {
			/* the share of the time the RSX is busy: by its own time stamps (V0.8), else by the probe's timings */
			if (live && s->rsx_pct > 0.0) {
				snprintf(a, sizeof(a), s->rsx_timed ? "%.1f %%" : "%.0f %%", s->rsx_pct);
				/* V0.9: what each unit does against its rate alone */
				if (s->rsx_alu_pct > 0.0 && s->rsx_vert_pct > 0.0) {
					snprintf(b, sizeof(b), "ALU %.0f, blend %.0f, vertex %.0f %%", s->rsx_alu_pct, s->rsx_rop_pct,
					         s->rsx_vert_pct);
					if (ui_text_w(UI_SMALL, b) > mw - 52.0f)
						snprintf(b, sizeof(b), "ALU %.0f, blend %.0f, vtx %.0f", s->rsx_alu_pct, s->rsx_rop_pct,
						         s->rsx_vert_pct);
				} else if (s->rsx_alu_pct > 0.0) {
					snprintf(b, sizeof(b), "ALU %.0f %%, blend %.0f %%", s->rsx_alu_pct, s->rsx_rop_pct);
				} else {
					snprintf(b, sizeof(b), "shader %.0f %% + fill %.0f %%", s->rsx_tile_pct, s->rsx_fill_pct);
				}
				clip(b, UI_SMALL, mw - 52.0f);
			} else {
				snprintf(a, sizeof(a), "-");
				snprintf(b, sizeof(b), s->rsx_probe_gflops > 0.0 ? "shader and fill" : "its timing not measured");
			}
			metric(RX + mw + 20.0f, 170.0f, mw, "RSX busy", a, live ? K_TEXT : K_FAINT, b);
		} else {
			metric(RX + mw + 20.0f, 170.0f, mw, "PPU", live && s->threads_ok ? "2 threads" : "-",
			       live ? K_TEXT : K_FAINT, "SPEs not started");
		}

		if (!live || s->clock_state == NTP_OFF || s->clock_state == NTP_NO_SERVER) {
			metric(RX + 2 * (mw + 20.0f), 170.0f, mw, "Clock", "-", K_FAINT,
			       !live ? "against a time server" : "no time server answered");
		} else if (!s->clock_valid) {
			metric(RX + 2 * (mw + 20.0f), 170.0f, mw, "Clock", "Measuring", K_SOFT,
			       s->clock_state == NTP_ASKING ? "asking the time server" : "first value after 15 s");
		} else {
			double dev = s->clock_dev_pct, mag = dev < 0 ? -dev : dev;

			snprintf(a, sizeof(a), "%+.4f %%", dev);
			snprintf(b, sizeof(b), "%+.1f ms/min - %s", s->clock_ms_per_min, mag <= 0.01 ? "correct" :
			         dev > 0 ? "fast" : "slow");
			metric(RX + 2 * (mw + 20.0f), 170.0f, mw, "Clock", a,
			       mag > STRESS_CLOCK_LIMIT ? K_BAD : mag > 0.01 ? K_ACCENT : K_GOOD, b);
		}
	}

	/* the parts */
	{
		const float py = 338.0f, c_who = RX + 420.0f, c_rate = RX + 600.0f, c_jobs = RX + 830.0f;
		float pitch = 44.0f;
		int rows = 0;

		for (int p = 0; p < STRESS_PARTS; p++)
			rows += stress_part_used(p, load);
		if (rows > 5)
			pitch = 196.0f / (float)(rows - 1);   /* six rows (Everything) still inside the card */

		ui_round(RX, py, RW, 290.0f, 20.0f, K_CARD);
		ui_text(RX + 28.0f, py + 22.0f, UI_SMALL, K_DIM, "Part");
		ui_text(c_who, py + 22.0f, UI_SMALL, K_DIM, "Runs on");
		ui_text(c_rate, py + 22.0f, UI_SMALL, K_DIM, done ? "Average" : "Throughput");
		ui_text(c_jobs, py + 22.0f, UI_SMALL, K_DIM, "Jobs");
		ui_text_right(RX + RW - 28.0f, py + 22.0f, UI_SMALL, K_DIM, "Errors");
		for (int p = 0; p < STRESS_PARTS; p++) {
			float ry;

			if (!stress_part_used(p, load))
				continue;
			ry = py + 70.0f + (float)row++ * pitch;
			text_mid(RX + 28.0f, ry, fit(UI_BODY, c_who - RX - 44.0f, stress_part_name(p, load)), K_TEXT,
			         stress_part_name(p, load));
			if (p == PART_RSX || p == PART_RSX_FILL || p == PART_RSX_VERT)
				snprintf(a, sizeof(a), "RSX");
			else if (p >= PART_PPU_1)
				snprintf(a, sizeof(a), "PPU thread %d", p - PART_PPU_1 + 1);
			else if (!spus)
				snprintf(a, sizeof(a), "-");
			else
				snprintf(a, sizeof(a), "%s", config_label(live ? s->part_spus[p] : load == STRESS_MIXED ? spus / 3 : spus));
			text_mid(c_who, ry, UI_BODY, K_DIM, a);
			if (!live || (p < PART_PPU_1 && !spus)) {
				text_mid(c_rate, ry, UI_BODY, K_FAINT, "-");
				continue;
			}
			stress_rate_text(b, sizeof(b), stress_part_unit(p, load), done ? stress_part_average(s, p) : s->rate[p]);
			text_mid(c_rate, ry, fit(UI_LABEL, c_jobs - c_rate - 16.0f, b), K_TEXT, b);
			snprintf(a, sizeof(a), "%llu", (unsigned long long)s->jobs[p]);
			text_mid(c_jobs, ry, fit(UI_BODY, 150.0f, a), K_SOFT, a);
			snprintf(a, sizeof(a), "%u", s->errors[p]);
			ui_text_right(RX + RW - 28.0f, ry - ui_cap(UI_LABEL) / 2.0f, UI_LABEL, s->errors[p] ? K_BAD : K_GOOD, a);
		}
	}

	/* the load over time */
	{
		const float gy = 648.0f, gh = 200.0f, x0 = RX + 28.0f, gw = RW - 56.0f, base = gy + gh - 26.0f, hmax = 108.0f;

		ui_round(RX, gy, RW, gh, 20.0f, K_CARD);
		ui_text(RX + 28.0f, gy + 22.0f, UI_SMALL, K_DIM, "Load over time - points a second, red with errors");
		if (live && s->history_len > 0) {
			float best = 1.0f, pitch = gw / STRESS_HISTORY;
			int len = s->history_len;

			for (int i = 0; i < len; i++)
				if (s->history[i] > best)
					best = s->history[i];
			snprintf(buf, sizeof(buf), "Peak %.0f - a bar is %d s", best, s->history_step_s);
			ui_text_right(RX + RW - 28.0f, gy + 22.0f, UI_SMALL, K_DIM, buf);
			for (int i = 0; i < len; i++) {
				float h = hmax * s->history[i] / best;
				uint32_t c = s->history_bad[i] ? K_BAD : i == len - 1 && !done ? mix(K_ACCENT, K_WHITE, 0.4f) : K_ACCENT;

				h = h < 3.0f ? 3.0f : h;
				ui_rect(x0 + i * pitch, base - h, pitch * 0.68f, h, c);
			}
		} else {
			ui_rect(x0, base - 2.0f, gw, 2.0f, K_LINE);
			ui_text_center(RX + RW / 2.0f, base - 64.0f, UI_BODY, K_FAINT, live ? "The first bar comes after a second" :
			               "Starts with the load");
		}
	}

	/* how it ended - or what runs where */
	if (done) {
		char verdict[sizeof(s->verdict)];
		int st;

		snprintf(verdict, sizeof(verdict), "%s", s->verdict);
		st = clip(verdict, UI_SUB, WIDE - 130.0f);

		ui_card(LEFT, BAR_Y, WIDE, BAR_H, 18.0f, mix(K_BACK, rc, 0.16f), rc, 2.0f);
		ui_icon(s->result == STRESS_PASSED ? UI_ICON_CHECK : UI_ICON_CROSS, LEFT + 24.0f, BAR_Y + 12.0f, 40.0f, rc);
		text_mid(LEFT + 84.0f, BAR_Y + BAR_H / 2.0f, st, K_TEXT, verdict);
	} else {
		float right_w = 0.0f;

		ui_round(LEFT, BAR_Y, WIDE, BAR_H, 18.0f, K_CARD);
		if (live && s->probe_ok) {
			double mhz = s->clock_valid ? s->cell_mhz * (1.0 + s->clock_dev_pct / 100.0) : s->cell_mhz;

			snprintf(b, sizeof(b), "Cell %.1f MHz - RAM %.0f ns", mhz, s->ram_latency_ns);
			right_w = ui_text_right(RIGHT - 30.0f, BAR_Y + BAR_H / 2.0f - ui_cap(UI_LABEL) / 2.0f, UI_LABEL, K_TEXT, b);
		}
		load_what(buf, sizeof(buf), load, spus);
		text_mid(LEFT + 30.0f, BAR_Y + BAR_H / 2.0f, clip(buf, UI_BODY, WIDE - 90.0f - right_w), K_SOFT, buf);
	}

	if (done) {
		x = hint_button(LEFT, UI_ICON_CROSS, B_CROSS, "Run again");
		x = hint_button(x, UI_ICON_CIRCLE_BTN, B_CIRCLE, "Setup");
	} else {
		x = hint_button(LEFT, UI_ICON_CIRCLE_BTN, B_CIRCLE, "Cancel");
	}
	x = hint_button(x, UI_ICON_TRIANGLE, B_TRIANGLE, "Info");
	hint_key(x, "SELECT", "Photo");
}

static void draw_stress(void)
{
	const stress_status *s = stress_get();
	int run = state == ST_FULLLOAD || request == REQ_FULLLOAD;

	if (run || (stress_result_open && s->phase == STRESS_DONE))
		draw_stress_run(s, run);
	else
		draw_stress_setup(s);
}

/* ---- the buttons ---------------------------------------------------------------------------------- */

static void start_entry(int entry)
{
	switch (entry) {
	case M_FLOAT:
	case M_INT:
	case M_MEM:
		request_test = entry - M_FLOAT;
		request_spus = selected;
		post(REQ_TEST);
		screen = SCR_RESULTS;
		break;
	case M_ALL:
		request_spus = selected;
		post(REQ_ALL);
		screen = SCR_RESULTS;
		break;
	case M_SERIES:
		request_spus = bench_spus();
		post(REQ_SERIES);
		screen = SCR_RESULTS;
		break;
	case M_ENDURANCE:
		request_spus = selected;
		post(REQ_ENDURANCE);
		screen = SCR_RESULTS;
		break;
	case M_FULLLOAD:
		screen = SCR_STRESS;
		webman_watch_temps(1);
		break;
	default:
		screen = SCR_RESULTS;
		break;
	}
}

static void start_stress(void)
{
	request_secs = DURATIONS[duration_pick];
	request_load = load_pick;
	stress_result_open = 1;
	post(REQ_FULLLOAD);
}

static void keys(int pressed)
{
	switch (screen) {
	case SCR_MENU:
		/* a grid of 4 x 2: LEFT/RIGHT go on through the rows, UP/DOWN change the row */
		if (pressed & PAD_LEFT)
			cursor = (cursor + M_ENTRIES - 1) % M_ENTRIES;
		if (pressed & PAD_RIGHT)
			cursor = (cursor + 1) % M_ENTRIES;
		if (pressed & (PAD_UP | PAD_DOWN))
			cursor = (cursor + GRID_COLS) % M_ENTRIES;
		if ((pressed & PAD_L1) && selected > 0)
			selected--;
		if ((pressed & PAD_R1) && selected < bench_spus())
			selected++;
		if (pressed & PAD_TRIANGLE)
			open_info(cursor, SCR_MENU);
		else if ((pressed & PAD_CROSS) && (cursor == M_RESULTS || !busy()))
			start_entry(cursor);
		break;
	case SCR_INFO:
		if (pressed & (PAD_UP | PAD_LEFT | PAD_L1))
			info_entry = (info_entry + M_ENTRIES - 1) % M_ENTRIES;
		if (pressed & (PAD_DOWN | PAD_RIGHT | PAD_R1))
			info_entry = (info_entry + 1) % M_ENTRIES;
		if ((pressed & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_L1 | PAD_R1)) && info_back == SCR_MENU)
			cursor = info_entry;
		if (pressed & (PAD_CIRCLE | PAD_TRIANGLE))
			screen = info_back;
		break;
	case SCR_RESULTS:
		if (pressed & PAD_TRIANGLE) {
			open_info(M_RESULTS, SCR_RESULTS);
		} else if (pressed & PAD_CIRCLE) {
			if (busy()) {
				stop_requested = 1;
				bench_cancel();
			} else {
				screen = SCR_MENU;
			}
		}
		break;
	case SCR_STRESS:
		if (pressed & PAD_TRIANGLE) {
			open_info(M_FULLLOAD, SCR_STRESS);
		} else if (state == ST_FULLLOAD || request == REQ_FULLLOAD) {
			if (pressed & PAD_CIRCLE)
				stress_cancel();
		} else if (stress_result_open && stress_get()->phase == STRESS_DONE) {
			/* the finished test: CIRCLE back to the setup, X the same again */
			if (pressed & PAD_CIRCLE)
				stress_result_open = 0;
			else if ((pressed & PAD_CROSS) && !busy())
				start_stress();
		} else {
			if ((pressed & PAD_L1) && duration_pick > 0)
				duration_pick--;
			if ((pressed & PAD_R1) && duration_pick < DURATION_COUNT - 1)
				duration_pick++;
			/* the loads, 3 x 2 */
			if (pressed & PAD_LEFT)
				load_pick = (load_pick + STRESS_LOADS - 1) % STRESS_LOADS;
			if (pressed & PAD_RIGHT)
				load_pick = (load_pick + 1) % STRESS_LOADS;
			if (pressed & (PAD_UP | PAD_DOWN))
				load_pick = (load_pick + 3) % STRESS_LOADS;
			if ((pressed & PAD_CROSS) && !busy())
				start_stress();
			if (pressed & PAD_CIRCLE) {
				screen = SCR_MENU;
				webman_watch_temps(0);
			}
		}
		break;
	}
}

/* ---- screenshots ---------------------------------------------------------------------------------- */

#define SHOT_W  1280
#define SHOT_H  720

/* the note on the screen (note already written) and the same as a webMAN message */
static void tell(uint64_t now, int bad, const char *popup)
{
	note_bad = bad;
	note_until = now + NOTE_US;
	webman_popup(popup);
}

/* the capture buffer as a PPM next to the log, named after the console's clock; the note says how it went */
static void save_screen(uint64_t now, int fail_on_purpose)
{
	static uint8_t row[SHOT_W * 3];
	const uint32_t *p = gfx_capture_buffer(0);
	const char *dir = fail_on_purpose ? TEE_LOG_DIR "-missing" : TEE_LOG_DIR;   /* the autotest's failure */
	char clock[16], name[48], path[128], popup[96];
	FILE *f;
	int ok = 1;

	log_clock(clock, sizeof(clock));
	for (char *c = clock; *c; c++)
		if (*c == ':')
			*c = '-';
	/* two pictures in one second: the second one gets -2, and so on */
	for (int k = 1; k <= 9; k++) {
		FILE *t;

		if (k == 1)
			snprintf(name, sizeof(name), "screen-%s.ppm", clock);
		else
			snprintf(name, sizeof(name), "screen-%s-%d.ppm", clock, k);
		snprintf(path, sizeof(path), "%s/%s", dir, name);
		t = fopen(path, "rb");
		if (!t)
			break;
		fclose(t);
	}
	f = fopen(path, "wb");
	if (!f) {
		log_line("Screenshot: %s cannot be created", path);
		snprintf(note, sizeof(note), "Photo not saved: no file can be created in %s/", dir);
		tell(now, 1, "TEE PS3 Benchmark: photo NOT saved");
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", SHOT_W, SHOT_H);
	for (int y = 0; y < SHOT_H && ok; y++) {
		for (int x = 0; x < SHOT_W; x++) {
			uint32_t v = p[y * SHOT_W + x];
			row[x * 3] = (uint8_t)(v >> 16);
			row[x * 3 + 1] = (uint8_t)(v >> 8);
			row[x * 3 + 2] = (uint8_t)v;
		}
		if (fwrite(row, 1, sizeof(row), f) != sizeof(row))
			ok = 0;
	}
	if (fclose(f) != 0)
		ok = 0;
	if (!ok) {
		log_line("Screenshot: write error in %s", path);
		snprintf(note, sizeof(note), "Photo not saved: write error in %s", path);
		tell(now, 1, "TEE PS3 Benchmark: photo NOT saved (write error)");
		return;
	}
	log_line("Screenshot: %s", path);
	snprintf(note, sizeof(note), "Photo saved: %s in %s/", name, dir);
	snprintf(popup, sizeof(popup), "TEE PS3 Benchmark: photo saved - %s", name);
	tell(now, 0, popup);
}

/* the note over the status bar */
static void draw_note(uint64_t now)
{
	uint32_t rgb = note_bad ? K_BAD : K_GOOD;
	char shown[sizeof(note)];

	if (now >= note_until)
		return;
	ui_card(LEFT, BAR_Y, WIDE, BAR_H, 18.0f, mix(K_BACK, rgb, 0.2f), rgb, 2.0f);
	ui_icon(note_bad ? UI_ICON_CROSS : UI_ICON_CHECK, LEFT + 24.0f, BAR_Y + 12.0f, 40.0f, rgb);
	snprintf(shown, sizeof(shown), "%s", note);
	text_mid(LEFT + 84.0f, BAR_Y + BAR_H / 2.0f, clip(shown, UI_LABEL, WIDE - 120.0f), K_TEXT, shown);
}

/* ---- the autotest ----------------------------------------------------------------------------------- */

#ifdef BENCH_AUTOTEST
/* RPCS3 runs this build: it presses the buttons like a player would - menu, info page, the series from
   PPU to 6 SPEs, a 20 s stress test with each of the six loads (CPU max, CPU + RAM, RSX, CPU + RSX,
   Everything, Mixed) - takes pictures on the way and quits. Each step waits for its moment. */
enum { AT_IDLE, AT_PAUSE, AT_PRESS, AT_SHOT, AT_SHOT_NOTE, AT_SHOT_FAIL, AT_SHOT_SHOWN, AT_STRESS_RUNNING, AT_STRESS_DONE,
       AT_QUIT };

/* AT_SHOT_SHOWN: a picture of what is on screen while the stress test runs, without drawing anew - the
   frames in between only show the last picture again, and this proves they show the right one */
static int at_shot_shown;

typedef struct {
	uint8_t op;
	uint16_t arg;   /* AT_PAUSE: ms, AT_PRESS: buttons */
} at_step;

/* one load of the stress test: start it, two pictures under the load, one of its result, back to the
   setup and on to the next load */
#define AT_LOAD_RUN \
	{ AT_PRESS, PAD_CROSS }, \
	{ AT_STRESS_RUNNING, 0 }, { AT_PAUSE, 11000 }, \
	{ AT_SHOT_SHOWN, 0 }, { AT_PAUSE, 1500 },      /* what the screen shows, nothing drawn for it */ \
	{ AT_SHOT, 0 },                                 /* under the load */ \
	{ AT_STRESS_DONE, 0 }, { AT_PAUSE, 1000 }, \
	{ AT_SHOT, 0 }, { AT_PAUSE, 1500 },             /* its result */ \
	{ AT_PRESS, PAD_CIRCLE }, { AT_PAUSE, 300 }, \
	{ AT_PRESS, PAD_RIGHT }, { AT_PAUSE, 300 }

static const at_step SCRIPT[] = {
	{ AT_IDLE, 0 }, { AT_PAUSE, 1500 },
	{ AT_SHOT, 0 }, { AT_PAUSE, 700 },                       /* 1: the menu */
	{ AT_SHOT_NOTE, 0 }, { AT_PAUSE, 1200 },                 /* 2: the menu with the note about picture 1 */
	{ AT_SHOT_FAIL, 0 }, { AT_PAUSE, 700 },                  /* a picture that cannot be saved ... */
	{ AT_SHOT_NOTE, 0 }, { AT_PAUSE, 1200 },                 /* 3: ... and its red note */
	{ AT_PRESS, PAD_RIGHT }, { AT_PAUSE, 300 }, { AT_PRESS, PAD_RIGHT }, { AT_PAUSE, 300 },
	{ AT_PRESS, PAD_L1 }, { AT_PAUSE, 300 }, { AT_PRESS, PAD_L1 }, { AT_PAUSE, 800 },
	{ AT_SHOT, 0 }, { AT_PAUSE, 1200 },                      /* 4: Memory chosen, 4 SPEs */
	{ AT_PRESS, PAD_TRIANGLE }, { AT_PAUSE, 800 },
	{ AT_SHOT, 0 }, { AT_PAUSE, 1200 },                      /* 5: its info page */
	{ AT_PRESS, PAD_CIRCLE }, { AT_PAUSE, 300 },
	{ AT_PRESS, PAD_R1 }, { AT_PAUSE, 300 }, { AT_PRESS, PAD_R1 }, { AT_PAUSE, 300 },
	{ AT_PRESS, PAD_RIGHT }, { AT_PAUSE, 300 }, { AT_PRESS, PAD_RIGHT }, { AT_PAUSE, 300 },
	{ AT_PRESS, PAD_CROSS }, { AT_PAUSE, 4000 },             /* the series PPU to 6 SPEs */
	{ AT_SHOT, 0 },                                          /* 6a: the table while it runs */
	{ AT_IDLE, 0 }, { AT_PAUSE, 1000 },
	{ AT_SHOT, 0 }, { AT_PAUSE, 1200 },                      /* 6: the table */
	{ AT_PRESS, PAD_CIRCLE }, { AT_PAUSE, 300 },
	{ AT_SHOT, 0 }, { AT_PAUSE, 1200 },                      /* 6b: the menu with the scores */
	{ AT_PRESS, PAD_RIGHT }, { AT_PAUSE, 300 }, { AT_PRESS, PAD_RIGHT }, { AT_PAUSE, 300 },
	{ AT_PRESS, PAD_TRIANGLE }, { AT_PAUSE, 800 },
	{ AT_SHOT, 0 }, { AT_PAUSE, 1200 },                      /* 7: the stress test's info page */
	{ AT_PRESS, PAD_CIRCLE }, { AT_PAUSE, 300 },
	{ AT_PRESS, PAD_CROSS }, { AT_PAUSE, 400 },
	{ AT_PRESS, PAD_L1 }, { AT_PAUSE, 300 },                 /* 20 s */
	{ AT_PRESS, PAD_RIGHT }, { AT_PAUSE, 2000 },             /* CPU max */
	{ AT_SHOT, 0 }, { AT_PAUSE, 1200 },                      /* 8: the setup, 20 s and CPU max chosen */
	AT_LOAD_RUN,                                             /* CPU max */
	AT_LOAD_RUN,                                             /* CPU + RAM */
	AT_LOAD_RUN,                                             /* RSX */
	AT_LOAD_RUN,                                             /* CPU + RSX */
	{ AT_SHOT, 0 }, { AT_PAUSE, 1200 },                      /* the setup with Everything chosen */
	AT_LOAD_RUN,                                             /* Everything */
	AT_LOAD_RUN,                                             /* Mixed */
	{ AT_SHOT, 0 }, { AT_PAUSE, 1200 },                      /* the setup after the last one */
	{ AT_QUIT, 0 },
};

/* the buttons the script presses this frame; asks for pictures through *shot */
static int autotest(uint64_t now, int shot_pending, int *shot, int *keep_note, int *fail)
{
	static int step;
	static uint64_t until;

	while (step < (int)(sizeof(SCRIPT) / sizeof(SCRIPT[0]))) {
		const at_step *s = &SCRIPT[step];

		switch (s->op) {
		case AT_IDLE:
			if (busy())
				return 0;
			break;
		case AT_PAUSE:
			if (!until)
				until = now + s->arg * 1000ull;
			if (now < until)
				return 0;
			until = 0;
			break;
		case AT_PRESS:
			step++;
			return s->arg;
		case AT_SHOT:
		case AT_SHOT_NOTE:
		case AT_SHOT_FAIL:
			if (shot_pending)
				return 0;
			*shot = 1;
			*keep_note = s->op == AT_SHOT_NOTE;
			*fail = s->op == AT_SHOT_FAIL;
			break;
		case AT_SHOT_SHOWN:
			if (shot_pending)
				return 0;
			*shot = 1;
			*keep_note = *fail = 0;
			at_shot_shown = 1;
			break;
		case AT_STRESS_RUNNING:
			if (stress_get()->phase != STRESS_RUNNING)
				return 0;
			break;
		case AT_STRESS_DONE:
			if (busy() || stress_get()->phase != STRESS_DONE)
				return 0;
			break;
		case AT_QUIT:
			log_line("Autotest done");
			printf("AUTOTEST DONE\n");
			running = 0;
			break;
		}
		step++;
		if (*shot)
			return 0;
	}
	return 0;
}
#endif

/* ---- the program -------------------------------------------------------------------------------- */

/* the RSX load's programs and buffers, its self-test and its probe: the shader alone on the RSX, timed on
   the PPU's clock, and one pixel of the check tile against the PPU's sum - before the first frame.
   -1 when a self-test step hung the RSX: then nothing more may go to it */
static int rsx_start(void)
{
	const rsxload_status *r;

	if (rsxload_init() != 0) {
		log_line("RSX load: could not be set up (no RSX memory for it)");
		return 0;
	}
	rsxload_probe();
	r = rsxload_get();
	if (r->selftest_failed)
		return -1;
	if (r->probe_ok)
		log_line("RSX shader alone: %.2f G multiply-adds a second on four floats = %.1f GFLOP/s (%.1f a clock at 500 MHz), "
		         "as the load runs it %.1f GFLOP/s, a %dx%d tile in %.0f us (%d tiles with 256 and with 64 a pixel: %.2f "
		         "and %.2f ms)", r->probe_gmad, r->probe_gmad * 8.0, r->probe_gmad / 0.5, r->probe_load_gmad * 8.0,
		         RSXLOAD_TILE, RSXLOAD_TILE, r->probe_tile_us, r->probe_tiles, r->probe_long_us / 1000.0,
		         r->probe_short_us / 1000.0);
	else if (r->probe_gmad > 0.0)
		log_line("RSX shader alone: %.0f GFLOP/s is not plausible (the RSX's 24 pixel pipes make 192 at 500 MHz): the "
		         "labels came before the work was done - an emulator? (%d tiles: %.2f and %.2f ms)", r->probe_gmad * 8.0,
		         r->probe_tiles, r->probe_long_us / 1000.0, r->probe_short_us / 1000.0);
	else
		log_line("RSX shader alone: not measured (%d tiles: %.2f and %.2f ms)", r->probe_tiles, r->probe_long_us / 1000.0,
		         r->probe_short_us / 1000.0);
	if (r->reference)
		log_line("RSX check tile: pixel (%d, %d) %08x on the RSX (alpha, red, green, blue), the PPU's value %.6f gives red "
		         "%02x green %02x - %s (rows counted from the %s), checksum %016llx%s", r->check_x, r->check_y,
		         (unsigned)r->check_bits, (double)r->check_ppu, (unsigned)(r->check_expect >> 16) & 0xFF,
		         (unsigned)(r->check_expect >> 8) & 0xFF, r->check_ok ? "the program runs as meant" : "THEY DIFFER",
		         r->check_from_top ? "top" : "bottom", (unsigned long long)r->reference, r->first_differed ? "; the very "
		         "first one differed (RPCS3 runs a new shader in its interpreter at first)" : "");
	return 0;
}

int main(void)
{
	sys_ppu_thread_t thread;
	u64 ret;
	int last = 0, spus_ok, want_shot = 0, shot_pending = 0, keep_note = 0, fail_shot = 0, shot_fail = 0;
	int since_draw = 0, note_drawn = 0;
	uint32_t shot_ticket = 0;
	uint64_t shot_at = 0;

	if (gfx_init() != 0)
		return 1;
	ioPadInit(PADS);
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, on_sysutil, NULL);
	log_open();
	log_line("TEE PS3 Benchmark " VERSION " started: picture %dx%d, %s", gfx_width(), gfx_height(), gfx_selftest());
	if (rsx_start() != 0) {
		/* a hung RSX takes every later frame, and the app with it once the command buffer is full (V9.0's
		   second try froze the console): end at once, so the system resets the RSX */
		log_line("The RSX did not pass its self-test: the app ends here, so the system can reset the RSX");
		log_close();
		return 1;
	}
	if (ui_init() < 0) {
		log_line("The interface's self-test hung the RSX: the app ends here, so the system can reset the RSX");
		log_close();
		return 1;
	}
	gfx_reset_target();
	spus_ok = bench_start() == 0;
	if (spus_ok) {
		char how[96];

		bench_layout(how, sizeof(how));
		log_line("%d SPU threads started %s", bench_spus(), how);
	} else {
		log_line("SPU threads NOT started (%d), PPU only", bench_spus());
	}
	selected = bench_spus();
	if (webman_start() == 0) {
		int ntp = ntp_start() == 0;
		log_line("Network ready: messages, temperatures and fan from webMAN if it runs; %s",
		         ntp ? "time server for the clock check" : "NO time server thread, the clock is not checked");
	} else {
		log_line("Network not started: no webMAN messages, temperatures or fan, no clock check");
	}

	if (sysThreadCreate(&thread, runner, NULL, 1000, 0x10000, THREAD_JOINABLE, "bench_runner") != 0) {
		log_line("Test thread could not be started");
		running = 0;
	}

	while (running) {
		uint64_t frame_top = __gettime();   /* timebase: the frame's busy time, for the stress test's log */
		uint64_t now = (uint64_t)sysGetSystemTime();
		int held = pad_buttons(), pressed = held & ~last, draw, note_on, shot_shown = 0;

		anim = (float)(now % 600000000ull) / 1e6f;

		last = held;
		pressed |= key_repeat(held, pressed, now);
#ifdef BENCH_AUTOTEST
		pressed |= autotest(now, shot_pending, &want_shot, &keep_note, &fail_shot);
#endif
		sysUtilCheckCallback();

		if (pressed & PAD_START)
			running = 0;
		if ((pressed & PAD_SELECT) && !shot_pending)
			want_shot = 1;
		keys(pressed);

		/* While the stress test runs, only every STRESS_DRAW_UI-th frame is drawn anew (the pixel font:
		   STRESS_DRAW_EVERY-th) - at once for a key, a photo or the note coming or going - and the others
		   show the last picture again: the drawing's time comes out of the PPU load threads (V0.6 on the
		   console: 1.26 ms a frame, the PPU at 89 % instead of 91 %; the pixel font's thousands of
		   rectangles took 1.45 ms in V8.3). At 30 frames a second the ring's dot still goes round smoothly. */
#ifdef BENCH_AUTOTEST
		shot_shown = want_shot && at_shot_shown;
#endif
		note_on = now < note_until;
		/* the RSX load alone leaves the PPU idle: there every frame is drawn. V0.8: while the RSX load runs a
		   picture stays two TV frames anyway (30 a second): every one is drawn */
		draw = screen != SCR_STRESS || stress_get()->phase != STRESS_RUNNING || pressed ||
		       (want_shot && !shot_shown) || note_on != note_drawn ||
		       ++since_draw >= (!ui_textured() ? STRESS_DRAW_EVERY : stress_load_cpu(stress_get()->load) &&
		                        !stress_load_rsx(stress_get()->load) ? STRESS_DRAW_UI : 1);
		if (draw) {
			since_draw = 0;
			note_drawn = note_on;
			ui_begin(K_BACK);
			if (screen == SCR_MENU)
				draw_menu(now);
			else if (screen == SCR_INFO)
				draw_info();
			else if (screen == SCR_RESULTS)
				draw_results(now);
			else
				draw_stress();
			/* a new picture shows the screen, not the note about the last one */
			if (!want_shot || keep_note)
				draw_note(now);
			ui_end();   /* the shapes are queued before the capture copies the picture */
			if (want_shot && !shot_shown) {
				shot_ticket = gfx_capture(0, SHOT_W, SHOT_H);
				shot_at = now;
				shot_fail = fail_shot;
				want_shot = keep_note = fail_shot = 0;
				shot_pending = 1;
			}
		}
		if (shot_shown) {
			shot_ticket = gfx_capture_shown(0, SHOT_W, SHOT_H);
			shot_at = now;
			shot_fail = 0;
			want_shot = keep_note = fail_shot = 0;
			shot_pending = 1;
#ifdef BENCH_AUTOTEST
			at_shot_shown = 0;
#endif
		}
		stress_screen_frame(__gettime() - frame_top, gfx_sleeps(), draw);
		rsxload_set_active(stress_rsx_wanted() && !system_menu && !system_draw);
		if (draw)
			gfx_flip();
		else
			gfx_reflip();
		if (shot_pending) {
			if (gfx_capture_done(shot_ticket)) {
				save_screen(now, shot_fail);
				shot_pending = 0;
			} else if (now - shot_at > 2000000) {
				log_line("Screenshot: the RSX did not copy the picture");
				snprintf(note, sizeof(note), "Photo not saved: the RSX did not copy the picture");
				tell(now, 1, "TEE PS3 Benchmark: photo NOT saved");
				shot_pending = 0;
			}
		}
	}

	quit = 1;
	stop_requested = 1;
	bench_cancel();
	stress_cancel();
	sysThreadJoin(thread, &ret);
	log_line("End, %u errors in total", total_errors);
	ntp_stop();
	webman_stop();
	bench_stop();
	log_close();
	gfx_shutdown();
	rsxload_shutdown();
	return 0;
}
