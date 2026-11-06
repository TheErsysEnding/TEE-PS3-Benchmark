#include "info.h"

#include <stddef.h>

/* V0.6: Roboto has small letters, so the texts are in sentence case; a paragraph is one string, the
   screen wraps it to its width. */

static const char *const NAMES[M_ENTRIES] = {
	"Float", "Integer", "Memory", "Full test", "Series", "Endurance", "Stress test", "Results"
};

static const char *const TITLES[M_ENTRIES] = {
	"Float (Mandelbrot)", "Integer (ChaCha20)", "Memory (DMA)", "Full test (all three)", "Series PPU to 6 SPEs",
	"Endurance run", "Full-load stress test", "Results"
};

static const char *const DESCS[M_ENTRIES] = {
	"The Mandelbrot set in 32-bit floating point",
	"The ChaCha20 stream cipher, 20 rounds",
	"Main memory (XDR) read by DMA",
	"Float, integer and memory in a row",
	"PPU, then 1 to 6 SPEs - about a minute",
	"The full test round after round",
	"CPU, RAM and RSX at full load",
	"Every score since the app started"
};

static const char *const INFO_FLOAT[] = {
	"#What is computed",
	"The Mandelbrot set: for every point c, z = z * z + c is repeated until z leaves the circle of radius 2 or "
	"1024 iterations are reached. A grid of 2048 x 1024 points in 32-bit floating point, split into 64 jobs of "
	"16 rows. An SPE works on 16 points at once (4 vectors of 4 numbers), the PPU on one after the other.",
	"#What is measured",
	"How many iterations the PPU or the SPEs manage in 3 seconds.",
	"#Unit and points",
	"Million iterations per second (M iter/s). 1 point per million iterations a second.",
	"#Errors",
	"Every job reports the sum of its iterations. The SPE rounds towards zero, the PPU to the nearest number, so "
	"the first SPE result of a job is its reference and every later one must match it to the iteration. A "
	"difference is an error and must never happen. The PPU run has nothing to compare with (so it says in the log).",
	"#Typical values (a reference PS3)",
	"PPU 39, 1 SPE 945, 6 SPEs 5674 - it grows linearly with the number of SPEs.",
	NULL
};

static const char *const INFO_INT[] = {
	"#What is computed",
	"The ChaCha20 stream cipher: 20 rounds of add, xor and rotate on 32-bit integers. 64 jobs of 1 MB of "
	"keystream each (16384 blocks of 64 bytes). An SPE works on 4 blocks at once, one per vector lane; the PPU "
	"works on one at a time (scalar).",
	"#What is measured",
	"How much keystream comes out in 3 seconds.",
	"#Unit and points",
	"MB per second (1 MB = 1048576 bytes). 1 point per MB/s.",
	"#Errors",
	"At the start the PPU works out every job's checksum in advance (xor and sum of all output words). Every "
	"result must match it bit for bit, otherwise it is an error.",
	"#Typical values (a reference PS3)",
	"PPU 112 MB/s, 1 SPE 700 MB/s, 6 SPEs 4144 MB/s - it grows linearly.",
	NULL
};

static const char *const INFO_MEM[] = {
	"#What is computed",
	"A 64 MB buffer in main memory (XDR) is read and added up, in 8 jobs of 8 MB. The SPEs fetch it by DMA in "
	"16 KB pieces, 4 pieces on the way at once. The PPU reads it directly through its caches.",
	"#What is measured",
	"How many bytes come out of main memory in 3 seconds.",
	"#Unit and points",
	"MB/s, shown in GB/s. 1 point per 10 MB/s.",
	"#Errors",
	"The sum of every 8 MB piece is worked out at the start. Any other result means that wrong data arrived, and "
	"counts as an error.",
	"#Typical values (a reference PS3)",
	"PPU 0.6 GB/s, 1 SPE 11.5 GB/s, 6 SPEs 21.1 GB/s. From 3 SPEs on the memory bus is full at about 21 GB/s; "
	"more SPEs add nothing.",
	NULL
};

static const char *const INFO_ALL[] = {
	"#What is computed",
	"Float, integer and memory one after the other, 3 seconds each, with the number of SPEs chosen in the menu "
	"(or the PPU alone).",
	"#What is measured",
	"The same as in the three single tests.",
	"#Unit and points",
	"Per test as in the single test. The total is the sum of the three scores.",
	"#Errors",
	"Every result is checked as in the single test. A red number in the table means that at least one result of "
	"that test was wrong.",
	"#Typical values (a reference PS3)",
	"Total: PPU 209, 1 SPE 2825, 6 SPEs 11976.",
	NULL
};

static const char *const INFO_SERIES[] = {
	"#What is computed",
	"The full test with the PPU and then with 1, 2, 3, 4, 5 and 6 SPEs: 7 x 3 tests of 3 seconds each, about "
	"1 minute in all.",
	"#What is measured",
	"How the performance grows with the number of SPEs. The table fills row by row.",
	"#Unit and points",
	"As in the single tests; every row also gets its total.",
	"#Errors",
	"Every result is checked; the status bar shows the sum of all errors.",
	"#Typical values (a reference PS3)",
	"Float and integer grow linearly (6 SPEs = 6 x 1 SPE); memory is full at about 21 GB/s from 3 SPEs on. "
	"Total: PPU 209, 1 SPE 2825, 6 SPEs 11976.",
	NULL
};

static const char *const INFO_ENDURANCE[] = {
	"#What is computed",
	"The full test with the chosen number of SPEs again and again, round after round, until circle is pressed. "
	"Only one test runs at a time.",
	"#What is measured",
	"Whether the values stay the same over a longer time. The table shows the latest round.",
	"#Unit and points",
	"As in the single tests.",
	"#Errors",
	"Every result is checked. The error count covers all rounds; at the end the log has the rounds, the time "
	"and the errors.",
	"#Typical values",
	"As in the full test, changing only a little from round to round. Full load on all cores at once is what "
	"the stress test does.",
	NULL
};

static const char *const INFO_FULLLOAD[] = {
	"#The loads",
	"Mixed: 2 SPEs each on Mandelbrot, ChaCha20 and memory, moving on every 10 s; PPU thread 1 ChaCha20, thread 2 "
	"Mandelbrot on VMX. CPU max: every SPE issues a fused multiply-add and a shuffle in every cycle, both PPU "
	"threads run a VMX heat kernel. CPU + RAM: the same while the SPEs' DMA units write the RAM and read it back "
	"at full speed. RSX: the graphics chip alone - a shader of multiply-adds (the fastest of four register "
	"layouts), full-screen layers of texture reads and blending that compute as well (like heavy game effects), "
	"vertices for its vertex units, and the VRAM read back. "
	"CPU + RSX and Everything (CPU, RAM and RSX): the whole console at once.",
	"#What is measured",
	"Throughput per part and its history. SPU busy: the share of the time the SPEs compute. Clock: the PPU timer "
	"against a time server (NTP). Cell clock: each SPE times an FMA chain of known cycles. RAM latency: one "
	"128-byte read. Temperatures and fan: webMAN. RSX: each unit alone at the start, then its share of that "
	"(ALU, blend, vertex); RSX busy by its own time stamps - a picture stays two TV frames, so it waits for the TV "
	"only briefly. The app changes no clock.",
	"#Errors and verdict",
	"Every result is checked (heat: the first result of each job is its reference; RAM: every byte against its "
	"pattern; RSX: a check tile each of the shader, the fill and the vertices every 4th picture, bit for bit; VRAM: every 64 KB of "
	"the 16 MB texture against what the PPU wrote). Passed: no error, nothing hangs, CPU and RSX below 85`C (there it "
	"stops), the timer within 0.1 % of NTP.",
	"#Values (a reference PS3)",
	"CPU max: SPEs 152.5 GFLOP/s (99.5 % of the peak), PPU 23.3 (91 %), CPU 64`C after 15 min at fan 32 %. "
	"CPU + RAM: 97.7 GFLOP/s + 17.5 GB/s. RSX: shader 185.7 GFLOP/s alone. Timer -0.0031 %.",
	NULL
};

static const char *const INFO_RESULTS[] = {
	"#What it shows",
	"The table of all results since the app started: one row for the PPU and for 1 to 6 SPEs, one column each for "
	"the points and throughput of float, integer and memory, on the right the total as a number and as a bar.",
	"#Colours and errors",
	"Red: at least one result of that test was wrong. Green: a total without errors. The status bar: all errors "
	"so far.",
	"#Log and photo",
	"Every result is also in the log: /dev_hdd0/tmp/teebench/bench-date-time.log. SELECT saves the screen as a "
	"picture (1280 x 720, PPM) in the same folder.",
	"#Typical values (a reference PS3)",
	"PPU: float 39, integer 112 MB/s, memory 0.6 GB/s, total 209. 1 SPE: 945, 700 MB/s, 11.5 GB/s, total 2825. "
	"6 SPEs: 5674, 4144 MB/s, 21.1 GB/s, total 11976.",
	"#About",
	"The text is set in Roboto by Google, under the Apache License 2.0.",
	NULL
};

static const char *const *const INFOS[M_ENTRIES] = {
	INFO_FLOAT, INFO_INT, INFO_MEM, INFO_ALL, INFO_SERIES, INFO_ENDURANCE, INFO_FULLLOAD, INFO_RESULTS
};

static int ok(int entry)
{
	return entry >= 0 && entry < M_ENTRIES;
}

const char *menu_name(int entry)
{
	return ok(entry) ? NAMES[entry] : "?";
}

const char *menu_title(int entry)
{
	return ok(entry) ? TITLES[entry] : "?";
}

const char *menu_desc(int entry)
{
	return ok(entry) ? DESCS[entry] : "";
}

const char *const *info_lines(int entry)
{
	return INFOS[ok(entry) ? entry : 0];
}
