/* V1.0 (6th build): what an error line says about its moment - for the RSX its temperature and clocks (lv1, as
   the main loop last read them), for the CPU its temperature - so a log shows how hot, and at which clock, an
   error came. */
#ifndef ERR_INFO_H
#define ERR_INFO_H

#include <stddef.h>

/* " - at RSX 73 C, core 700 MHz, VRAM 750 MHz" (rsx 1) or " - at CPU 51 C" (rsx 0); "" when nothing is known */
void err_context(char *b, size_t n, int rsx);

#endif
