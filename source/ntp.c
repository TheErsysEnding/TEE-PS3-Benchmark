#include "ntp.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <arpa/inet.h>
#include <sys/time.h>     /* PSL1GHT's socket header uses struct timeval without declaring it */
#include <net/net.h>
#include <net/netdb.h>
#include <lv2/systime.h>
#include <sys/mutex.h>
#include <sys/systime.h>
#include <sys/thread.h>

#ifndef NTP_PORT
#define NTP_PORT       123
#endif
/* above the screen (1001) and the tests (1000): the moment an answer comes in is read at once. It
   sleeps or waits for the network nearly all the time. */
#define THREAD_PRIO    900
#define BURST          4                 /* requests at the start and at the end */
#define BURST_GAP_US   500000
#define ANSWER_US      1000000           /* how long one request waits for its answer */
#define FALLBACK_IP    0xD8EF2300u       /* 216.239.35.0, time.google.com, should no name resolve */

#ifndef NTP_TEST_IP
static const char *const SERVERS[] = { "pool.ntp.org", "time.google.com" };
#endif
/* after the first answer: one request at these seconds, then every 60 s */
static const int SCHEDULE_S[] = { 15, 30, 60 };
#define SCHEDULE_COUNT  ((int)(sizeof(SCHEDULE_S) / sizeof(SCHEDULE_S[0])))

typedef struct {
	int64_t local_us;     /* the PPU's timer halfway through the round trip */
	int64_t offset_us;    /* the server's clock minus the timer */
	int64_t delay_us;     /* the round trip, without the time the server took */
} sample;

static sys_mutex_t lock;
static sys_ppu_thread_t thread;
static volatile int running, want_begin, want_finish, busy;
static ntp_status status;                /* under lock */

/* the thread's own */
static uint32_t server_ip;
static int resolved, measuring, have_base, next_slot;
static sample base, last;
static int64_t min_delay, next_at;

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* an NTP timestamp (seconds since 1900, 32.32 fixed point) in microseconds */
static int64_t ntp_us(const uint8_t *p)
{
	uint64_t sec = get32(p), frac = get32(p + 4);

	return (int64_t)(sec * 1000000u + ((frac * 1000000u) >> 32));
}

/* the server's address, once */
static void resolve(void)
{
	char name[48];
	uint32_t ip = FALLBACK_IP;

#ifdef NTP_TEST_IP
	ip = NTP_TEST_IP;
	snprintf(name, sizeof(name), "%u.%u.%u.%u:%d", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255, NTP_PORT);
#else
	snprintf(name, sizeof(name), "216.239.35.0");
	for (int i = 0; i < (int)(sizeof(SERVERS) / sizeof(SERVERS[0])); i++) {
		struct hostent *h = gethostbyname(SERVERS[i]);

		if (h && h->h_addrtype == AF_INET && h->h_length == 4 && h->h_addr_list && h->h_addr_list[0]) {
			memcpy(&ip, h->h_addr_list[0], 4);
			ip = ntohl(ip);
			snprintf(name, sizeof(name), "%s", SERVERS[i]);
			break;
		}
	}
#endif
	server_ip = ip;
	resolved = 1;
	sysMutexLock(lock, 0);
	snprintf(status.server, sizeof(status.server), "%s", name);
	sysMutexUnlock(lock);
}

/* one request; 1 and *out with the answer, 0 without one */
static int ask(sample *out)
{
	static uint32_t serial;
	uint8_t pkt[48];
	struct sockaddr_in sa, from;
	struct timeval timeout = { ANSWER_US / 1000000, ANSWER_US % 1000000 };
	uint32_t cookie = ++serial;
	int64_t t1;
	int fd, ok = 0;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	memset(pkt, 0, sizeof(pkt));
	pkt[0] = 0x23;                  /* no leap second announced, version 4, mode 3: a client */
	put32(pkt + 40, 0x54454521u);   /* the transmit time: any value, the server sends it back */
	put32(pkt + 44, cookie);
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(NTP_PORT);
	sa.sin_addr.s_addr = htonl(server_ip);

	t1 = (int64_t)sysGetSystemTime();
	if (sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&sa, sizeof(sa)) == (ssize_t)sizeof(pkt)) {
		for (;;) {
			socklen_t from_len = sizeof(from);
			ssize_t n = recvfrom(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &from_len);
			int64_t t4 = (int64_t)sysGetSystemTime(), t2, t3;

			if (n < 48)
				break;                  /* nothing within ANSWER_US */
			/* the answer to this request, from a server that has the time (stratum 1..15) */
			if (get32(pkt + 24) != 0x54454521u || get32(pkt + 28) != cookie || (pkt[0] & 7) != 4 ||
			    pkt[1] == 0 || pkt[1] > 15) {
				if (t4 - t1 > ANSWER_US)
					break;
				continue;
			}
			t2 = ntp_us(pkt + 32);      /* the server received it */
			t3 = ntp_us(pkt + 40);      /* the server sent the answer */
			out->offset_us = ((t2 - t1) + (t3 - t4)) / 2;
			out->delay_us = (t4 - t1) - (t3 - t2);
			out->local_us = t1 + (t4 - t1) / 2;
			ok = out->delay_us >= 0;
			break;
		}
	}
	close(fd);
	return ok;
}

/* BURST requests, the answer with the shortest round trip in *best */
static int burst(sample *best)
{
	int got = 0;

	for (int i = 0; i < BURST && running; i++) {
		sample s;

		if (i)
			sysUsleep(BURST_GAP_US);
		if (ask(&s) && (!got || s.delay_us < best->delay_us)) {
			*best = s;
			got = 1;
		}
	}
	return got;
}

/* the deviation from the first answer to the latest, for the screen */
static void publish(int state)
{
	sysMutexLock(lock, 0);
	status.state = state;
	if (have_base) {
		int64_t span = last.local_us - base.local_us;

		status.delay_ms = (double)min_delay / 1000.0;
		if (span > 0) {
			/* server minus timer falls when the timer runs fast */
			status.dev_pct = -100.0 * (double)(last.offset_us - base.offset_us) / (double)span;
			status.span_s = (double)span / 1e6;
			status.valid = 1;
		}
	}
	sysMutexUnlock(lock);
}

/* a further answer counts unless its round trip is far longer than the best: then the path was busy
   and the offset is unsure */
static void use(const sample *s)
{
	if (s->delay_us > 2 * min_delay + 20000)
		return;
	if (s->delay_us < min_delay)
		min_delay = s->delay_us;
	last = *s;
	sysMutexLock(lock, 0);
	status.samples++;
	sysMutexUnlock(lock);
}

static void schedule_next(void)
{
	int64_t at = next_slot < SCHEDULE_COUNT ? SCHEDULE_S[next_slot] : SCHEDULE_S[SCHEDULE_COUNT - 1] +
	             60 * (next_slot - SCHEDULE_COUNT + 1);

	next_at = base.local_us + at * 1000000;
	next_slot++;
}

static void ntp_main(void *arg)
{
	(void)arg;
	while (running) {
		if (want_begin) {
			want_begin = 0;
			have_base = measuring = 0;
			next_slot = 0;
			if (!resolved)
				resolve();
			if (burst(&base)) {
				have_base = measuring = 1;
				last = base;
				min_delay = base.delay_us;
				sysMutexLock(lock, 0);
				status.samples = 1;
				sysMutexUnlock(lock);
				schedule_next();
				publish(NTP_RUNNING);
			} else {
				publish(NTP_NO_SERVER);
			}
			if (!want_finish)
				busy = 0;
		}
		if (want_finish) {
			sample s;

			want_finish = 0;
			if (measuring && burst(&s)) {
				use(&s);
				publish(NTP_RUNNING);
			}
			measuring = 0;
			busy = 0;
		}
		if (measuring && (int64_t)sysGetSystemTime() >= next_at) {
			sample s;

			if (ask(&s)) {
				use(&s);
				publish(NTP_RUNNING);
			}
			schedule_next();
		}
		sysUsleep(50000);
	}
	sysThreadExit(0);
}

int ntp_start(void)
{
	sys_mutex_attr_t attr;

	memset(&attr, 0, sizeof(attr));
	attr.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO;
	attr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
	attr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
	attr.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
	strcpy(attr.name, "teentp");   /* at most 7 letters: the field holds 8 bytes */
	if (sysMutexCreate(&lock, &attr) != 0)
		return -1;
	running = 1;
	if (sysThreadCreate(&thread, ntp_main, NULL, THREAD_PRIO, 0x8000, THREAD_JOINABLE, "tee_ntp") != 0) {
		running = 0;
		sysMutexDestroy(lock);
		return -1;
	}
	return 0;
}

void ntp_stop(void)
{
	u64 ret;

	if (running) {
		running = 0;
		sysThreadJoin(thread, &ret);
		sysMutexDestroy(lock);
	}
}

void ntp_begin(void)
{
	if (!running)
		return;
	sysMutexLock(lock, 0);
	status.state = NTP_ASKING;
	status.valid = status.samples = 0;
	status.span_s = status.dev_pct = status.delay_ms = 0.0;
	sysMutexUnlock(lock);
	busy = 1;
	want_finish = 0;
	want_begin = 1;
}

void ntp_finish(void)
{
	if (!running)
		return;
	busy = 1;
	want_finish = 1;
}

int ntp_busy(void)
{
	return running && busy;
}

void ntp_get(ntp_status *out)
{
	if (!running) {
		memset(out, 0, sizeof(*out));
		return;
	}
	sysMutexLock(lock, 0);
	*out = status;
	sysMutexUnlock(lock);
}
