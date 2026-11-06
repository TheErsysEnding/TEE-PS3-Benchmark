/* V0.8: how much load a picture, from the RSX's own clock. Plain C without the PS3's headers, so it
   can be tried on a computer against a simulated RSX (tools/pace_sim.c).

   With one picture a frame the RSX idles from the moment it reaches the flip until the TV's vertical
   blank (on the console at the wait for the flip, in RPCS3 at the flip command itself). While the RSX
   load runs, a picture stays for `frames` TV frames (2), and the load is set so that the RSX reaches the
   next flip just a margin
   before the blank `frames` frames on: its work from the blank to the next flip command aims at
   frames * period - margin. That work is the load's rows (each kind learnt in microseconds a row) and
   everything else (the screen, check tiles, copies), learnt for each of PACE_PHASES kinds of picture
   (the check tiles come every fourth). A picture that came a frame late widens the margin; calm ones
   narrow it again, down to what the work's jitter allows. On the console V0.8's first run reached 99.2 %
   busy this way, with no late picture. */
#ifndef RSX_PACE_H
#define RSX_PACE_H

#define PACE_PHASES 4

typedef struct {
	double period_us;            /* the TV's frame */
	int frames;                  /* TV frames a picture */
	double fill_share;           /* the fill's share of the load's time */
	double unit_s, unit_f;       /* us a shader row (256 pixels), a fill row (1920 pixels) */
	double rows_s_max, rows_f_max;
	double other[PACE_PHASES];   /* us of work a picture besides the rows, by phase; < 0: not seen yet */
	double margin, floor, jitter;
	unsigned measured, misses;
} pace;

/* one picture as the RSX's time stamps tell it */
typedef struct {
	int phase;
	int checks;                  /* it had a check tile: its parts' times say nothing about the rows */
	double rows_s, rows_f;       /* drawn */
	double target_us;            /* the work its plan aimed at */
	double work_us;              /* from going on after the flip (the blank) to reaching the next flip */
	double shader_us, fill_us;   /* the parts */
	double interval_us;          /* from its flip to the next one */
} pace_cycle;

void pace_start(pace *p, double period_us, int frames, double fill_share, double unit_s, double unit_f,
                double rows_s_max, double rows_f_max);
/* returns 1 when the picture came late (a frame after it was meant to) */
int pace_measured(pace *p, const pace_cycle *c);
/* rows for the next picture of that phase; returns the work it aims at (us) */
double pace_plan(const pace *p, int phase, double *rows_s, double *rows_f);

#endif
