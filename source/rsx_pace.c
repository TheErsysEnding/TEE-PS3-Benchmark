/* See rsx_pace.h. */
#include "rsx_pace.h"

#include <math.h>
#include <string.h>

#define OTHER_GUESS   1500.0   /* us besides the rows before a picture of that phase was measured */
#define MARGIN_START  1500.0   /* us before the flip's blank at the start: safe, then it narrows */
#define MARGIN_MIN    150.0
#define MARGIN_MAX    3000.0
#define MARGIN_LATE   150.0    /* wider by this for each late picture */
#define MARGIN_CALM   4.0      /* narrower by this for each one on time (about 0.12 ms a second) */

static double clampd(double v, double lo, double hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* a learnt time: up quickly (too little of it makes pictures late), down slowly */
static double learn(double old, double seen, double up, double down)
{
	return old + (seen > old ? up : down) * (seen - old);
}

void pace_start(pace *p, double period_us, int frames, double fill_share, double unit_s, double unit_f,
                double rows_s_max, double rows_f_max)
{
	memset(p, 0, sizeof(*p));
	p->period_us = period_us;
	p->frames = frames < 1 ? 1 : frames;
	p->fill_share = rows_f_max > 0.0 && unit_f > 0.0 ? fill_share : 0.0;
	p->unit_s = unit_s > 0.0 ? unit_s : 1.0;
	p->unit_f = unit_f > 0.0 ? unit_f : 1.0;
	p->rows_s_max = rows_s_max;
	p->rows_f_max = p->fill_share > 0.0 ? rows_f_max : 0.0;
	for (int i = 0; i < PACE_PHASES; i++)
		p->other[i] = -1.0;
	p->margin = MARGIN_START;
	p->floor = MARGIN_MIN;
}

static double other_of(const pace *p, int phase)
{
	double most = -1.0;

	if (p->other[phase] >= 0.0)
		return p->other[phase];
	for (int i = 0; i < PACE_PHASES; i++)
		most = p->other[i] > most ? p->other[i] : most;
	return most >= 0.0 ? most : OTHER_GUESS;
}

int pace_measured(pace *p, const pace_cycle *c)
{
	double span = p->frames * p->period_us, rows_us, other;
	int k = c->phase % PACE_PHASES, late;

	/* numbers no picture of this pace can have: not counted */
	if (c->work_us <= 0.0 || c->work_us > 4.0 * span || c->interval_us <= 0.0)
		return 0;
	late = c->interval_us > (p->frames + 0.5) * p->period_us;
	p->measured++;
	/* the rows' times from pictures without check tiles (an observation off by more than 4x is not one) */
	if (!c->checks) {
		if (c->rows_s >= 64.0 && c->shader_us > 0.0) {
			double u = c->shader_us / c->rows_s;

			if (u > p->unit_s * 0.25 && u < p->unit_s * 4.0)
				p->unit_s = learn(p->unit_s, u, 0.5, 0.15);
		}
		if (c->rows_f >= 64.0 && c->fill_us > 0.0) {
			double u = c->fill_us / c->rows_f;

			if (u > p->unit_f * 0.25 && u < p->unit_f * 4.0)
				p->unit_f = learn(p->unit_f, u, 0.5, 0.15);
		}
	}
	rows_us = c->rows_s * p->unit_s + c->rows_f * p->unit_f;
	other = c->work_us - rows_us;
	other = other < 0.0 ? 0.0 : other;
	p->other[k] = p->other[k] < 0.0 ? other : learn(p->other[k], other, 0.5, 0.1);
	/* how far the work lands from where it was aimed: the margin may not go below three times that */
	p->jitter += 0.05 * (fabs(c->work_us - c->target_us) - p->jitter);
	p->floor = clampd(100.0 + 3.0 * p->jitter, MARGIN_MIN, 2500.0);
	if (late) {
		p->misses++;
		p->margin = clampd(p->margin + MARGIN_LATE, p->floor, MARGIN_MAX);
	} else {
		p->margin = clampd(p->margin - MARGIN_CALM, p->floor, MARGIN_MAX);
	}
	return late;
}

double pace_plan(const pace *p, int phase, double *rows_s, double *rows_f)
{
	double span = p->frames * p->period_us, other = other_of(p, phase % PACE_PHASES);
	double load = clampd(span - p->margin - other, 0.1 * span, span), rs, rf;

	rs = load * (1.0 - p->fill_share) / p->unit_s;
	rf = p->fill_share > 0.0 ? load * p->fill_share / p->unit_f : 0.0;
	rs = floor(clampd(rs, 0.0, p->rows_s_max));
	rf = floor(clampd(rf, 0.0, p->rows_f_max));
	*rows_s = rs;
	*rows_f = rf;
	return rs * p->unit_s + rf * p->unit_f + other;
}
