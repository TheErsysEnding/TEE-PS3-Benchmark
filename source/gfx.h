/* RSX side of the skeleton: three display buffers at the TV's resolution (V0.8), filled rectangles,
   and scaling the finished frame into main memory for the stream. */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>

#include <ppu-types.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx_program.h>

int  gfx_init(void);
void gfx_shutdown(void);
int  gfx_width(void);
int  gfx_height(void);

/* a scene_rect_fn: fills the rectangle on the RSX (scissor + clear, no shaders needed) */
void gfx_rect(void *ctx, int x, int y, int w, int h, uint32_t rgb);

/* shows the frame drawn so far at the next vsync and starts the next one */
void gfx_flip(void);
/* the last picture once more, nothing drawn since gfx_flip (see gfx.c) */
void gfx_reflip(void);

/* for the RSX load (rsx_load.c): the command buffer's context; a function gfx_flip and gfx_reflip call
   right after the flip (whatever it queues runs while the picture waits for its vertical blank, and it
   leaves the render target to them); 1 MB of main memory the RSX can write, with its offset */
gcmContextData *gfx_context(void);
void gfx_set_frame_hook(void (*hook)(void));
void *gfx_host_area(u32 *offset);
/* main memory the RSX can read and write, from byte start of the mapped area (1-8 MB; the RSX load has
   4-5 MB, the interface 1-3.1 MB); NULL outside it */
void *gfx_host_mem(u32 start, u32 size, u32 *offset);
/* someone else set the clear colour: gfx_rect sets its own again */
void gfx_forget_clear_color(void);
/* the picture as the render target again (after self-tests drew elsewhere) */
void gfx_reset_target(void);

/* V0.9: room for `words` in the command buffer's current segment; a vertex program loaded with that room
   asked for first (PSL1GHT's own breaks at a segment change); the command buffer back to its start between
   long runs of commands without a flip */
void gfx_reserve(gcmContextData *ctx, u32 words);
void gfx_load_vp(gcmContextData *ctx, const rsxVertexProgram *program, void *ucode);
void gfx_rewind(void);

/* TEE_CAPTURE_BUFFERS capture buffers (4 MB each, up to 1280x720) in main memory the RSX can
   write to. Copies finish in the order they were queued. */
uint32_t *gfx_capture_buffer(int index);
/* queues a scaled copy of the frame drawn so far into a capture buffer; the copy is there once
   gfx_capture_done(ticket) says so */
uint32_t gfx_capture(int index, int width, int height);
/* the same from the picture on screen (the last one flipped), not the one being drawn */
uint32_t gfx_capture_shown(int index, int width, int height);
int      gfx_capture_done(uint32_t ticket);

/* what the start-up check found, for the on-screen text */
const char *gfx_selftest(void);

/* how often a wait on the RSX ran into its timeout; 0 while all is well */
int gfx_stalls(void);

/* how often the waits on the RSX have slept so far (each sleep wakes the PPU again), a running count */
uint32_t gfx_sleeps(void);
/* flips that came later than gfx_flip_frames() TV frames after the one before (by the system's flip
   times): the RSX had not got through its commands in time (a late main thread is not counted) */
uint32_t gfx_late_flips(void);

/* V0.8: the RSX's own time stamps. Each flip has GFX_REP_SLOTS: gfx writes BEFORE (all drawn, the flip
   command next), AFTER (the RSX went on after it: on the console at the vertical blank) and WAITED
   (after the wait for the flip, behind the hook's work); the frame hook may write the others. */
#define GFX_REP_BEFORE  0
#define GFX_REP_AFTER   1
#define GFX_REP_LOAD    2     /* 2, 3, 4: the RSX load's */
#define GFX_REP_WAITED  5
#define GFX_REP_SLOTS   6
#define GFX_REPORT_FREE 1200  /* report indices from here on are free for others (the probe) */
/* the number of the flip being queued (in the frame hook: the one just queued); flips are numbered 0, 1, ... */
u32 gfx_cycle(void);
void gfx_report(gcmContextData *ctx, int slot);
/* the time stamps above are written from now on (the RSX load's probe saw the RSX write reports) */
void gfx_reports_on(void);
void gfx_report_at(gcmContextData *ctx, u32 index);
/* the RSX's time (ns) in that slot of flip `cycle`, 0 while not written (the last 16 flips are kept) */
uint64_t gfx_report_ns(u32 cycle, int slot);
uint64_t gfx_report_index_ns(u32 index);
void gfx_report_index_clear(u32 index);
/* the main thread came for flip `cycle` over 2 ms after it had happened (its next picture went late) */
int gfx_ppu_late(u32 cycle);
/* the TV's frame (us), learnt from the flips; whether the last flip's time was the system's */
double gfx_period_us(void);
int gfx_flip_time_sys(void);
/* TV frames a picture is meant to stay: the waits sleep for that long, later flips count as late */
void gfx_set_flip_frames(int n);
int gfx_flip_frames(void);

#endif
