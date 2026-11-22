#include "bench_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <ppu-types.h>
#include <sys/file.h>
#include <sys/mutex.h>
#include <sys/systime.h>
#include <sysutil/sysutil.h>

#include <lv2/sysfs.h>
#include <lv2/systime.h>

/* build.sh writes the sources' hash into build_stamp.h: a part that stopped a start is left out by the same
   build only, a new build tries it again */
#if __has_include("build_stamp.h")
#include "build_stamp.h"
#endif
#ifndef BUILD_STAMP
#define BUILD_STAMP "unstamped"
#endif

#ifndef GUARD_DIR
#define GUARD_DIR TEE_LOG_DIR   /* the test build moves it: RPCS3 empties /dev_hdd0/tmp at every start */
#endif
#define START_FILE GUARD_DIR "/start.txt"   /* the part of the start running now */
#define SKIP_FILE  GUARD_DIR "/skip.txt"    /* the parts this build leaves out */

static s32 fd = -1;
static int durable;   /* V1.0: every line on the disk at once (fsync) - the start */
static sys_mutex_t lock;
static uint64_t start_us;
static int offset_s;   /* local time = UTC + this */

/* the console's clock is UTC; time zone and summer time come from its settings */
static time_t local_now(void)
{
	u64 sec = 0, nsec = 0;

	sysGetCurrentTime(&sec, &nsec);
	return (time_t)sec + offset_s;
}

void log_clock(char *buf, int len)
{
	time_t t = local_now();
	struct tm tm;

	gmtime_r(&t, &tm);
	snprintf(buf, (size_t)len, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* V1.0: the log through the system's own file calls: a line is in the system's hands at once, and while
   `durable` is on also on the disk (fsync) - the 3rd V1.0 build froze a PS3 so early that the system never
   wrote its log out, the file was gone after the restart */
int log_open(void)
{
	sys_mutex_attr_t attr;
	s32 zone = 0, summer = 0;
	char path[96];
	time_t t;
	struct tm tm;

	if (sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_TIMEZONE, &zone) == 0)
		offset_s = zone * 60;
	if (sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME, &summer) == 0 && summer)
		offset_s += 3600;

	memset(&attr, 0, sizeof(attr));
	attr.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO;
	attr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
	attr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
	attr.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
	strcpy(attr.name, "teebnch");   /* 7 letters: the field holds 8 bytes */
	if (sysMutexCreate(&lock, &attr) != 0)
		return -1;
	sysLv2FsMkdir(TEE_LOG_DIR, 0777);   /* fails harmlessly when it is already there */
	if (strcmp(GUARD_DIR, TEE_LOG_DIR) != 0)
		sysLv2FsMkdir(GUARD_DIR, 0777);
	t = local_now();
	gmtime_r(&t, &tm);
	snprintf(path, sizeof(path), TEE_LOG_DIR "/bench-%04d%02d%02d-%02d%02d%02d.log", tm.tm_year + 1900,
	         tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd, 0644, NULL, 0) != 0)
		fd = -1;
	start_us = (uint64_t)sysGetSystemTime();
	durable = 1;
	return fd >= 0 ? 0 : -1;
}

void log_durable(int on)
{
	durable = on;
}

void log_close(void)
{
	if (fd < 0)
		return;
	sysMutexLock(lock, 0);
	sysLv2FsFsync(fd);
	sysLv2FsClose(fd);
	fd = -1;
	sysMutexUnlock(lock);
	sysMutexDestroy(lock);
}

void log_line(const char *fmt, ...)
{
	char clock[16], buf[1024];
	uint64_t up = (uint64_t)sysGetSystemTime() - start_us, written = 0;
	va_list ap;
	int n;

	if (fd < 0)
		return;
	log_clock(clock, sizeof(clock));
	n = snprintf(buf, sizeof(buf), "%s  +%02u:%02u:%02u  ", clock, (unsigned)(up / 3600000000ULL),
	             (unsigned)(up / 60000000ULL % 60), (unsigned)(up / 1000000ULL % 60));
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 2)
		n = (int)sizeof(buf) - 2;   /* a cut line still ends */
	buf[n++] = '\n';
	sysMutexLock(lock, 0);
	sysLv2FsWrite(fd, buf, (u64)n, &written);
	if (durable)
		sysLv2FsFsync(fd);
	sysMutexUnlock(lock);
}

/* ---- V1.0: the start guard --------------------------------------------------------------------------------- */

#define GUARD_MAX 8

static char skipped[GUARD_MAX][24];   /* the parts this build leaves out */
static int n_skipped, guard_on;
static char left_out[160];            /* the parts left out at this start, for the screen */

/* a small file at once on the disk; 0 when written */
static int put_file(const char *path, const char *text)
{
	s32 f;
	u64 written = 0;
	int ok;

	if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &f, 0644, NULL, 0) != 0)
		return -1;
	ok = sysLv2FsWrite(f, text, strlen(text), &written) == 0 && written == strlen(text);
	sysLv2FsFsync(f);
	sysLv2FsClose(f);
	return ok ? 0 : -1;
}

/* a small file into buf (cut to its size); its length, -1 when it is not there */
static int get_file(const char *path, char *buf, int size)
{
	s32 f;
	u64 got = 0;

	if (sysLv2FsOpen(path, SYS_O_RDONLY, &f, 0, NULL, 0) != 0)
		return -1;
	if (sysLv2FsRead(f, buf, (u64)size - 1, &got) != 0)
		got = 0;
	sysLv2FsClose(f);
	buf[got] = 0;
	return (int)got;
}

/* "stamp\nline\nline..." - the first line against this build's; then the lines one by one */
static const char *after_stamp(char *text)
{
	size_t n = strlen(BUILD_STAMP);

	if (strncmp(text, BUILD_STAMP, n) != 0 || text[n] != '\n')
		return NULL;
	return text + n + 1;
}

static int is_skipped(const char *part)
{
	for (int i = 0; i < n_skipped; i++)
		if (strcmp(skipped[i], part) == 0)
			return 1;
	return 0;
}

void guard_open(void)
{
	char text[512], last[32] = "";
	const char *rest;
	int n;

	/* the parts this build already leaves out */
	if (get_file(SKIP_FILE, text, sizeof(text)) > 0 && (rest = after_stamp(text)) != NULL) {
		while (*rest && n_skipped < GUARD_MAX) {
			const char *end = strchr(rest, '\n');
			size_t len = end ? (size_t)(end - rest) : strlen(rest);

			if (len > 0 && len < sizeof(skipped[0])) {
				memcpy(skipped[n_skipped], rest, len);
				skipped[n_skipped++][len] = 0;
			}
			rest = end ? end + 1 : rest + len;
		}
	}
	/* the last start's part, still there: that start did not get through it */
	n = get_file(START_FILE, text, sizeof(text));
	if (n > 0) {
		rest = after_stamp(text);
		snprintf(last, sizeof(last), "%.31s", rest ? rest : strchr(text, '\n') ? strchr(text, '\n') + 1 : "");
		if (strchr(last, '\n'))
			*strchr(last, '\n') = 0;
		if (!rest) {
			log_line("Start guard: the last start of an earlier build did not get through \"%s\" - this build tries "
			         "everything", last);
		} else if (!guard_can_skip(last)) {
			log_line("Start guard: the last start of this build did not get through \"%s\" (the console froze or the "
			         "app ended there) - it cannot be left out, this start tries it again", last);
		} else if (!is_skipped(last) && n_skipped < GUARD_MAX) {
			char list[512];
			int len = snprintf(list, sizeof(list), "%s\n", BUILD_STAMP);

			snprintf(skipped[n_skipped++], sizeof(skipped[0]), "%s", last);
			for (int i = 0; i < n_skipped && len < (int)sizeof(list); i++)
				len += snprintf(list + len, sizeof(list) - (size_t)len, "%s\n", skipped[i]);
			put_file(SKIP_FILE, list);
			log_line("Start guard: the last start of this build did not get through \"%s\" (the console froze or the "
			         "app ended there) - this build leaves it out from now on (a new build, or deleting skip.txt next to "
			         "this log, tries it again)", last);
		}
	}
	guard_on = 1;
}

int guard_can_skip(const char *part)
{
	return strcmp(part, GUARD_RSX) == 0 || strcmp(part, GUARD_COPY) == 0 || strcmp(part, GUARD_NET) == 0 ||
	       strcmp(part, GUARD_LV1) == 0;
}

int guard_part(const char *part)
{
	char text[96];

	if (!guard_on)
		return 0;
	if (guard_can_skip(part) && is_skipped(part)) {
		if (!strstr(left_out, part)) {
			size_t len = strlen(left_out);

			snprintf(left_out + len, sizeof(left_out) - len, "%s%s", len ? ", " : "", part);
			log_line("Start guard: \"%s\" is left out - it stopped an earlier start of this build (a new build tries "
			         "it again)", part);
		}
		return 1;
	}
	snprintf(text, sizeof(text), "%s\n%s\n", BUILD_STAMP, part);
	put_file(START_FILE, text);
	return 0;
}

void guard_done(void)
{
	if (!guard_on)
		return;
	guard_on = 0;
	sysLv2FsUnlink(START_FILE);
	log_durable(0);
}

const char *guard_left_out(void)
{
	return left_out;
}
