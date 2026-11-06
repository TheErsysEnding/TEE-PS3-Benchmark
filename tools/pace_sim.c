/* Tries source/rsx_pace.c against a simulated RSX on the Mac:
   cc -O2 -I source tools/pace_sim.c source/rsx_pace.c -lm -o /tmp/pace_sim && /tmp/pace_sim
   The RSX as the console showed it: the flip command holds it until the next vertical blank after it
   got there; the plan for picture c is made when the PPU saw flip c-1, from the pictures up to c-3. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "rsx_pace.h"

#define P 16683.0
#define N 2
#define CYCLES 9000   /* 5 minutes at 30 pictures a second */

static double gauss(void)
{
	double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);

	return sqrt(-2.0 * log(u)) * cos(6.283185307 * v);
}

static void run(const char *name, double sigma, double us_true, double uf_true, double us_est, double uf_est,
                double step_at, double step_us, double spike_rate)
{
	pace p;
	static double rs_of[CYCLES], rf_of[CYCLES], tgt_of[CYCLES], x_of[CYCLES], sh_of[CYCLES], fi_of[CYCLES], iv_of[CYCLES];
	double busy = 0, span = 0, busy_last = 0, span_last = 0;
	int late = 0, late_last = 0;

	srand(7);
	pace_start(&p, P, N, 0.5, us_est, uf_est, 256 * 256, 1080 * 64);
	for (int c = 0; c < CYCLES; c++) {
		int phase = c % 4;
		double other, x, iv;

		/* the plan: from what was measured up to c - 3 */
		if (c >= 3) {
			pace_cycle m = { (c - 3) % 4, ((c - 3) % 4) == 0 || ((c - 3) % 4) == 2, rs_of[c - 3], rf_of[c - 3],
			                 tgt_of[c - 3], x_of[c - 3], sh_of[c - 3], fi_of[c - 3], iv_of[c - 3] };
			pace_measured(&p, &m);
		}
		tgt_of[c] = pace_plan(&p, phase, &rs_of[c], &rf_of[c]);
		/* the RSX */
		other = 900.0 + (phase == 0 ? 250.0 : phase == 2 ? 400.0 : 0.0) + (c >= step_at ? step_us : 0.0) + sigma * gauss();
		if (spike_rate > 0 && rand() < spike_rate * RAND_MAX)
			other += 800.0;   /* now and then a slow picture (the PPU late, a photo) */
		sh_of[c] = rs_of[c] * us_true * (1.0 + 0.002 * gauss()) + (phase == 0 ? 180.0 : 0.0);
		fi_of[c] = rf_of[c] * uf_true * (1.0 + 0.002 * gauss()) + (phase == 2 ? 300.0 : 0.0);
		x = sh_of[c] + fi_of[c] + other - (phase == 0 ? 180.0 : 0.0) - (phase == 2 ? 300.0 : 0.0);
		x_of[c] = x;
		iv = ceil(x / P) * P;
		iv = iv < P ? P : iv;
		iv_of[c] = iv;
		busy += x > iv ? iv : x;
		span += iv;
		late += iv > (N + 0.5) * P;
		if (c >= CYCLES - 1800) {   /* the last minute */
			busy_last += x > iv ? iv : x;
			span_last += iv;
			late_last += iv > (N + 0.5) * P;
		}
	}
	printf("%-34s busy %.2f %% (last minute %.2f %%), late %d (last minute %d), margin %.0f us, floor %.0f, "
	       "jitter %.0f, units %.3f/%.3f\n", name, 100 * busy / span, 100 * busy_last / span_last, late, late_last,
	       p.margin, p.floor, p.jitter, p.unit_s, p.unit_f);
}

int main(void)
{
	run("steady, sigma 20 us", 20, 2.85, 4.27, 2.9, 4.0, 1e9, 0, 0);
	run("steady, sigma 50 us", 50, 2.85, 4.27, 2.9, 4.0, 1e9, 0, 0);
	run("steady, sigma 150 us", 150, 2.85, 4.27, 2.9, 4.0, 1e9, 0, 0);
	run("probe 30 % optimistic", 50, 2.85, 4.27, 2.0, 3.0, 1e9, 0, 0);
	run("probe 30 % pessimistic", 50, 2.85, 4.27, 3.7, 5.5, 1e9, 0, 0);
	run("screen +600 us at 2 min", 50, 2.85, 4.27, 2.9, 4.0, 3600, 600, 0);
	run("1 % slow pictures (+0.8 ms)", 50, 2.85, 4.27, 2.9, 4.0, 1e9, 0, 0.01);
	run("fill 3x faster (swizzle)", 50, 2.85, 1.4, 2.9, 1.4, 1e9, 0, 0);
	return 0;
}
