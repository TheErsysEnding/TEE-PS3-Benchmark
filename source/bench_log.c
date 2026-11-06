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

#include <lv2/systime.h>

static FILE *file;
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
	t = local_now();
	gmtime_r(&t, &tm);
	snprintf(path, sizeof(path), TEE_LOG_DIR "/bench-%04d%02d%02d-%02d%02d%02d.log", tm.tm_year + 1900,
	         tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	file = fopen(path, "w");
	start_us = (uint64_t)sysGetSystemTime();
	return file ? 0 : -1;
}

void log_close(void)
{
	if (!file)
		return;
	sysMutexLock(lock, 0);
	fclose(file);
	file = NULL;
	sysMutexUnlock(lock);
	sysMutexDestroy(lock);
}

void log_line(const char *fmt, ...)
{
	char clock[16];
	uint64_t up = (uint64_t)sysGetSystemTime() - start_us;
	va_list ap;

	if (!file)
		return;
	log_clock(clock, sizeof(clock));
	sysMutexLock(lock, 0);
	fprintf(file, "%s  +%02u:%02u:%02u  ", clock, (unsigned)(up / 3600000000ULL),
	        (unsigned)(up / 60000000ULL % 60), (unsigned)(up / 1000000ULL % 60));
	va_start(ap, fmt);
	vfprintf(file, fmt, ap);
	va_end(ap);
	fputc('\n', file);
	fflush(file);
	sysMutexUnlock(lock);
}
