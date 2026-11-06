#include "webman.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <arpa/inet.h>
#include <sys/time.h>     /* PSL1GHT's socket header uses struct timeval without declaring it */
#include <net/net.h>
#include <lv2/systime.h>
#include <sys/mutex.h>
#include <sys/systime.h>
#include <sys/thread.h>

/* the autotest build asks a stand-in on the Mac instead (RPCS3 passes 127.0.0.1 through), where port 80
   would need root: see the Makefile */
#ifndef WEBMAN_PORT
#define WEBMAN_PORT    80
#endif
#define TEXT_MAX       160
#define REPLY_MAX      (32 * 1024)   /* the status page is about 12 KB */
#define TEMP_EVERY_US  5000000
#define TEMP_FRESH_US  15000000
#define THREAD_PRIO    1100          /* below the screen (1001) and the tests (1000), above the load */

static sys_mutex_t lock;
static sys_ppu_thread_t thread;
static char waiting[TEXT_MAX];
static char reply[REPLY_MAX];
static volatile int has_waiting, running, watching, net_up;
static volatile int temp_cpu = -1, temp_rsx = -1, fan = -1;
static volatile uint64_t temp_at;

/* webMAN takes the text from the URL: everything but letters, digits, '-' and '.' is %-escaped */
static void url_encode(char *out, size_t cap, const char *in)
{
	static const char HEX[] = "0123456789ABCDEF";
	size_t n = 0;

	for (; *in && n + 4 < cap; in++) {
		unsigned char ch = (unsigned char)*in;
		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.') {
			out[n++] = (char)ch;
		} else {
			out[n++] = '%';
			out[n++] = HEX[ch >> 4];
			out[n++] = HEX[ch & 15];
		}
	}
	out[n] = 0;
}

/* GET path from webMAN; the answer (as much as fits) lands in reply. Its length, -1 without an answer. */
static int http_get(const char *path)
{
	static char drain[1024];
	struct timeval timeout = { 2, 0 };
	struct sockaddr_in sa;
	char request[TEXT_MAX * 3 + 96];
	int fd, n = 0, len, ok;

	snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\nHost: localhost\r\n\r\n", path);
	len = (int)strlen(request);
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(WEBMAN_PORT);
	sa.sin_addr.s_addr = htonl(0x7F000001u);

	ok = connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && send(fd, request, len, 0) == (ssize_t)len;
	while (ok) {
		int full = n >= REPLY_MAX - 1;
		ssize_t got = recv(fd, full ? drain : reply + n, full ? sizeof(drain) : (size_t)(REPLY_MAX - 1 - n), 0);

		if (got <= 0)
			break;
		if (!full)
			n += (int)got;
	}
	close(fd);
	if (!ok)
		return -1;
	reply[n] = 0;
	return n;
}

/* webMAN writes "CPU: 50°C (MAX: 72°C)<br>RSX: 56°C" and then the same in Fahrenheit: the number after
   "NAME:" whose unit is C (the degree sign may be UTF-8, Latin-1 or &deg;). -1 if there is none. */
static int find_temp(const char *s, int len, const char *name)
{
	int nl = (int)strlen(name), fahrenheit = -1;

	for (int i = 0; i + nl + 1 < len; i++) {
		int k, v = 0, digits = 0;

		if (memcmp(s + i, name, (size_t)nl) != 0 || s[i + nl] != ':')
			continue;
		for (k = i + nl + 1; k < len && s[k] == ' '; k++)
			;
		for (; k < len && s[k] >= '0' && s[k] <= '9' && digits < 4; k++, digits++)
			v = v * 10 + (s[k] - '0');
		if (!digits)
			continue;
		for (int u = k; u < len && u < k + 8; u++) {
			if (s[u] == 'C')
				return v;
			if (s[u] == 'F') {
				if (fahrenheit < 0)
					fahrenheit = (v - 32) * 5 / 9;
				break;
			}
		}
	}
	return fahrenheit;
}

/* webMAN writes the fan speed as "32% (0x54)" after a label in the console's language (and links it to
   its fan settings, which are never asked for): the number before "% (0x". -1 if there is none. */
static int find_fan(const char *s, int len)
{
	for (int i = 1; i + 5 <= len; i++) {
		int k, v = 0, unit = 1;

		if (memcmp(s + i, "% (0x", 5) != 0)
			continue;
		for (k = i - 1; k >= 0 && k >= i - 3 && s[k] >= '0' && s[k] <= '9'; k--, unit *= 10)
			v += (s[k] - '0') * unit;
		if (k < i - 1)
			return v;
	}
	return -1;
}

static void webman_main(void *arg)
{
	char text[TEXT_MAX], path[TEXT_MAX * 3 + 16];
	uint64_t next_temp = 0;

	(void)arg;
	while (running) {
		int take = 0;

		sysMutexLock(lock, 0);
		if (has_waiting) {
			memcpy(text, waiting, sizeof(text));
			has_waiting = 0;
			take = 1;
		}
		sysMutexUnlock(lock);
		if (take) {
			strcpy(path, "/popup.ps3?");
			url_encode(path + strlen(path), sizeof(path) - strlen(path), text);
			http_get(path);
		}

		if (watching && (uint64_t)sysGetSystemTime() >= next_temp) {
			int n = http_get("/cpursx.ps3");

			next_temp = (uint64_t)sysGetSystemTime() + TEMP_EVERY_US;
			if (n > 0) {
				int cpu = find_temp(reply, n, "CPU"), rsx = find_temp(reply, n, "RSX");

				if (cpu > 0 && cpu < 150) {
					int f = find_fan(reply, n);

					temp_cpu = cpu;
					temp_rsx = rsx > 0 && rsx < 150 ? rsx : -1;
					fan = f >= 0 && f <= 100 ? f : -1;
					temp_at = (uint64_t)sysGetSystemTime();
				}
			}
		}
		sysUsleep(50000);
	}
	sysThreadExit(0);
}

int webman_start(void)
{
	sys_mutex_attr_t attr;

	if (netInitialize() < 0)
		return -1;
	net_up = 1;
	memset(&attr, 0, sizeof(attr));
	attr.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO;
	attr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
	attr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
	attr.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
	strcpy(attr.name, "teewman");   /* 7 letters: the field holds 8 bytes */
	if (sysMutexCreate(&lock, &attr) != 0)
		return -1;
	running = 1;
	if (sysThreadCreate(&thread, webman_main, NULL, THREAD_PRIO, 0x8000, THREAD_JOINABLE, "tee_webman") != 0) {
		running = 0;
		sysMutexDestroy(lock);
		return -1;
	}
	return 0;
}

void webman_stop(void)
{
	u64 ret;

	if (running) {
		running = 0;
		sysThreadJoin(thread, &ret);
		sysMutexDestroy(lock);
	}
	if (net_up) {
		netDeinitialize();
		net_up = 0;
	}
}

void webman_popup(const char *text)
{
	if (!running)
		return;
	sysMutexLock(lock, 0);
	strncpy(waiting, text, sizeof(waiting) - 1);
	waiting[sizeof(waiting) - 1] = 0;
	has_waiting = 1;
	sysMutexUnlock(lock);
}

void webman_watch_temps(int on)
{
	watching = on;
}

int webman_temps(int *cpu, int *rsx, int *fan_pct)
{
	uint64_t at = temp_at;

	if (!at || (uint64_t)sysGetSystemTime() - at > TEMP_FRESH_US)
		return 0;
	*cpu = temp_cpu;
	*rsx = temp_rsx;
	*fan_pct = fan;
	return 1;
}
