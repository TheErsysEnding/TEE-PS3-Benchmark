#include "lv1_clock.h"

#include <string.h>

#include <ppu-lv2.h>

/* Where lv1 keeps the RSX's clocks, as webMAN MOD reads them for its clock display (include/feat/clock.h,
   the overclocking research by Chattrapat Sangmanee, github.com/aomsin2526): byte 6 of the core register
   times 50 MHz, byte 6 of the VRAM register times 25 MHz, byte 2 of the second VRAM register times 25 MHz.
   A stock PS3: core 500 MHz (10), VRAM 650 MHz (26). The app only reads them. */
#define CLOCK_CORE   0x28000004028ULL
#define CLOCK_VRAM   0x28000004010ULL
#define CLOCK_VRAM2  0x28000004020ULL

#define SC_PEEK_LV1_COBRA 11   /* Cobra / Evilnat: lv1 peek */

static volatile int last_core, last_vram;   /* the last plausible reading, for the stress test's thread */

#ifdef LV1_FAKE
/* the test build for RPCS3 (make LV1TEST=1), which has no lv1: the registers as a PS3 would show them - stock for
   120 s, then core 600 / VRAM 700 (the second VRAM clock 675), from 150 s core 650 / VRAM 750, as when webMAN
   overclocks during a test. The other bytes hold junk, as they may on a PS3 */
#include <sys/systime.h>

static u64 peek_lv1(u64 addr)
{
	static u64 t0;
	u64 now = (u64)sysGetSystemTime(), s;
	u64 core = 10, vram = 26, vram2 = 26;

	if (!t0)
		t0 = now;
	s = (now - t0) / 1000000;
	if (s >= 150) {
		core = 13;
		vram = vram2 = 30;
	} else if (s >= 120) {
		core = 12;
		vram = 28;
		vram2 = 27;
	}
	if (addr == CLOCK_CORE)
		return 0x1122334455660077ULL | core << 8;
	if (addr == CLOCK_VRAM)
		return 0x1122334455660077ULL | vram << 8;
	if (addr == CLOCK_VRAM2)
		return 0x1122004455667788ULL | vram2 << 40;
	return 0;
}
#else
static u64 peek_lv1(u64 addr)
{
	lv2syscall1(SC_PEEK_LV1_COBRA, addr);
	return_to_user_prog(u64);
}
#endif

/* ENOSYS 0x80010003: no such syscall, or the CFW's are switched off - lv2 hands it back sign-extended (as webMAN
   checks it), RPCS3 zero-extended */
static int no_syscall(u64 r)
{
	return (u32)r == 0x80010003u && (r >> 32 == 0 || r >> 32 == 0xFFFFFFFFu);
}

static int plausible(int mhz)
{
	return mhz >= 300 && mhz <= 1200;   /* webMAN's own bounds for these clocks */
}

int lv1_clock_read(lv1_clocks *c)
{
	memset(c, 0, sizeof(*c));
	c->raw_core = peek_lv1(CLOCK_CORE);
	if (no_syscall(c->raw_core)) {
		c->no_peek = 1;
		return -1;
	}
	c->raw_vram = peek_lv1(CLOCK_VRAM);
	c->raw_vram2 = peek_lv1(CLOCK_VRAM2);
	c->core_mhz = 50 * (int)(c->raw_core >> 8 & 0xFF);
	c->vram_mhz = 25 * (int)(c->raw_vram >> 8 & 0xFF);
	c->vram2_mhz = 25 * (int)(c->raw_vram2 >> 40 & 0xFF);
	if (!plausible(c->core_mhz))
		c->core_mhz = 0;
	if (!plausible(c->vram_mhz))
		c->vram_mhz = 0;
	if (!plausible(c->vram2_mhz))
		c->vram2_mhz = 0;
	c->read = c->core_mhz > 0 && c->vram_mhz > 0;
	if (c->read) {
		last_core = c->core_mhz;
		last_vram = c->vram_mhz;
	}
	return c->read ? 0 : -1;
}

int lv1_clock_poll(lv1_clocks *c, lv1_clocks *was)
{
	lv1_clocks n;

	if (!c->read || lv1_clock_read(&n) != 0)
		return 0;   /* an implausible reading keeps the last good one */
	if (n.vram2_mhz == 0)
		n.vram2_mhz = c->vram2_mhz;
	if (n.core_mhz == c->core_mhz && n.vram_mhz == c->vram_mhz && n.vram2_mhz == c->vram2_mhz)
		return 0;
	*was = *c;
	*c = n;
	return 1;
}

void lv1_clock_last(int *core_mhz, int *vram_mhz)
{
	*core_mhz = last_core;
	*vram_mhz = last_vram;
}

void lv1_clock_forget(void)
{
	last_core = last_vram = 0;
}
