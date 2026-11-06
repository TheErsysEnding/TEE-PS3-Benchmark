/* The PPU's timer against a time server on the internet (NTP over UDP), from a thread of its own.
   V5 compared the timer with LV2's real-time clock and found 0.0000 % on the real console: LV2 works
   its clock out from the same timebase. Only a clock outside the console can tell whether the timebase
   - and with it every second the console counts - runs at the right speed.
   ntp_begin() asks the server four times and keeps the answer with the shortest round trip, then asks
   once more after 15 s, 30 s and every 60 s; ntp_finish() asks four times again for the last value.
   One request is one 48-byte UDP packet: about one a minute, as public servers like it.
   The servers: pool.ntp.org, then time.google.com, then time.google.com's fixed address. The autotest
   build asks a stand-in on the Mac instead (see the Makefile). */
#ifndef NTP_H
#define NTP_H

enum { NTP_OFF, NTP_ASKING, NTP_NO_SERVER, NTP_RUNNING };

typedef struct {
	int state;
	int valid;              /* dev_pct holds a value: two answers some time apart */
	int samples;            /* answers used so far */
	double span_s;          /* timer seconds between the first and the latest answer used */
	double dev_pct;         /* how much faster the timer runs than the server's clock, in % */
	double delay_ms;        /* the shortest round trip so far */
	char server[48];        /* name or address asked */
} ntp_status;

/* the thread; the network must be up. 0 if it runs. */
int  ntp_start(void);
void ntp_stop(void);

/* a new measurement from now on */
void ntp_begin(void);
/* the last four requests of this measurement; ntp_busy() is 0 once they are in (or gave up) */
void ntp_finish(void);
int  ntp_busy(void);
void ntp_get(ntp_status *out);

#endif
