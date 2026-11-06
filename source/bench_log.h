/* The bench log (the folder keeps its name from TEE SPE Bench): /dev_hdd0/tmp/teebench/bench-YYYYMMDD-HHMMSS.log, one file per start, every
   line flushed at once so a crash keeps what came before it. Lines carry the console's local time
   and the time since start. Nothing is ever written anywhere else. */
#ifndef BENCH_LOG_H
#define BENCH_LOG_H

#define TEE_LOG_DIR  "/dev_hdd0/tmp/teebench"

int  log_open(void);
void log_close(void);
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* "HH:MM:SS" of the console's local time */
void log_clock(char *buf, int len);

#endif
