/* The bench log (the folder keeps its name from TEE SPE Bench): /dev_hdd0/tmp/teebench/bench-YYYYMMDD-HHMMSS.log, one file per start, every
   line handed to the system at once - and during the start also written to the disk at once (V1.0), so even a
   console that freezes keeps what came before. Lines carry the console's local time and the time since start.
   Nothing is ever written anywhere else. */
#ifndef BENCH_LOG_H
#define BENCH_LOG_H

#define TEE_LOG_DIR  "/dev_hdd0/tmp/teebench"

int  log_open(void);
void log_close(void);
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* 1: every line on the disk at once (log_open turns it on, guard_done off) */
void log_durable(int on);
/* "HH:MM:SS" of the console's local time */
void log_clock(char *buf, int len);

/* V1.0: the start guard. Before each part of the start the app notes it on the disk (start.txt next to the log);
   once the start is through the note goes. A note still there at the next start names the part that start did
   not get through - the console froze, or a self-test ended the app there. When that part can be left out, this
   build leaves it out from then on (skip.txt); a new build tries it again. */
#define GUARD_GRAPHICS "graphics"
#define GUARD_RSX      "RSX load"        /* can be left out: no RSX loads */
#define GUARD_COPY     "VRAM copy"       /* can be left out: no VRAM loads, no VRAM figures */
#define GUARD_UI       "interface"
#define GUARD_SPU      "SPU threads"
#define GUARD_NET      "network"         /* can be left out: no webMAN, no clock check */
#define GUARD_LV1      "clock read"      /* can be left out: no clocks from lv1 (V1.0, 5th build) */
#define GUARD_FIRST    "first pictures"

void guard_open(void);                    /* after log_open: reads what the last start left */
int  guard_part(const char *part);        /* notes `part` as running; 1: this build leaves it out */
int  guard_can_skip(const char *part);
void guard_done(void);                    /* the start is through */
const char *guard_left_out(void);         /* "" or the parts left out at this start, for the screen */

#endif
