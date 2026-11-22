#include "err_info.h"

#include <stdio.h>

#include "lv1_clock.h"
#include "webman.h"

void err_context(char *b, size_t n, int rsx)
{
	int cpu = -1, t_rsx = -1, fan = -1, core = 0, vram = 0, k = 0;
	int have_t = webman_temps(&cpu, &t_rsx, &fan);   /* the cached ones: no request from here */

	b[0] = 0;
	if (!rsx) {
		if (have_t)
			snprintf(b, n, " - at CPU %d C", cpu);
		return;
	}
	lv1_clock_last(&core, &vram);
	if (have_t && t_rsx >= 0)
		k = snprintf(b, n, " - at RSX %d C", t_rsx);
	if (core > 0 && k >= 0 && (size_t)k < n)
		snprintf(b + k, n - (size_t)k, "%s core %d MHz, VRAM %d MHz", k ? "," : " - at", core, vram);
}
