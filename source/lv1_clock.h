/* V1.0 (5th build): the RSX's clocks as the hypervisor (lv1) has them - read, never written. The app only
   measures: there is no function here, or anywhere in it, that sets a clock. */
#ifndef LV1_CLOCK_H
#define LV1_CLOCK_H

#include <ppu-types.h>

typedef struct {
	int read;                          /* 1: lv1 answered and the core and VRAM clocks are plausible */
	int no_peek;                       /* 1: no lv1 peek (syscall 11 not there, or the CFW's switched off) */
	int core_mhz, vram_mhz, vram2_mhz; /* 0 where not plausible; vram2: the second VRAM clock register */
	u64 raw_core, raw_vram, raw_vram2; /* the registers as read, for the log */
} lv1_clocks;

/* reads the three clock registers through the CFW's lv1 peek (syscall 11); 0 when read */
int lv1_clock_read(lv1_clocks *c);
/* reads them again (the main loop, once a second, when the start's read worked - webMAN can change them while the
   app runs): 1 when a plausible reading differs from *c, then *was keeps the old clocks and *c holds the new */
int lv1_clock_poll(lv1_clocks *c, lv1_clocks *was);
/* the last plausible core and VRAM clocks, for another thread (the stress test's ranges); 0 while unknown */
void lv1_clock_last(int *core_mhz, int *vram_mhz);
/* the start found lv1's clocks wrong (against the measured RSX clock): no more clocks from lv1 */
void lv1_clock_forget(void);

#endif
