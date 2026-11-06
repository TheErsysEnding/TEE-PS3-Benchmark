/* Display setup and flipping follow PSL1GHT's samples/graphics/blitting/source/rsxutil.cpp. */
#include "gfx.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <lv2/systime.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>

#define TEE_CAPTURE_BUFFERS 3   /* the capture part is TEE PS3 Live's; the bench never calls it */

#define CB_SIZE          0x100000           /* RSX command buffer */
#define CB_REWIND_BYTES  (CB_SIZE / 2)      /* start over once this much of it is used */
#define WAIT_TIMEOUT_US  500000
#define HOST_SIZE        (20 * 1024 * 1024) /* main memory mapped for the RSX */
#define CAPTURE_OFFSET   (8 * 1024 * 1024)  /* capture buffers at 8, 12 and 16 MB, away from the commands */
#define CAPTURE_BYTES    (4 * 1024 * 1024)  /* 1280x720 x 4 bytes fits */
#define LOAD_OFFSET      (4 * 1024 * 1024)  /* 4-5 MB: the RSX load's check tiles (rsx_load.c) */
#define LOAD_BYTES       (1024 * 1024)
#define BUFFERS          3     /* V0.8: three pictures (see gfx_flip) */
/* V0.8: the RSX's time stamps - the reports of the last REPORT_RING flips, GFX_REP_SLOTS each (report
   memory holds 2048; the system uses none of these) */
#define REPORT_BASE      1024
#define REPORT_RING      16

/* labels 0-63 belong to the system */
/* librsx's own: asks libgcm for `count` words in the command buffer (a new segment if need be) */
extern s32 rsxContextCallback(gcmContextData *context, u32 count);

#define LABEL_SYNC       253
#define LABEL_CAPTURE    254
#define LABEL_FINISH     255

static gcmContextData *context;
static void *host_addr;
static u32 width, height;
static u32 color_pitch, color_offset[BUFFERS], *color_buffer[BUFFERS];
static u32 depth_pitch, depth_offset, *depth_buffer;
static u32 curr_fb, shown_fb, first_flip = 1;   /* the picture being drawn, the one last flipped */
static u32 flips;             /* flips queued so far: the number of the one being queued */
/* the clear colour last set, 0xFFFFFFFF: set it again (never a valid one). V0.8, second build: forgotten at
   every flip - the flip resets the RSX's state (RPCS3's flip command does; on the console the background
   came out black, not K_BACK). Before, only every 4th picture of the RSX load (after the fill's check tile,
   which forgets it) had the right background, the others black: the flicker seen on the console since V0.7 */
static u32 last_color = 0xFFFFFFFF;
static u32 sync_value = 1;
static u32 capture_offset[TEE_CAPTURE_BUFFERS];
static u32 capture_ticket;
static int scissor_from_bottom;
static char selftest[64] = "SELF-TEST RUNNING";
static u32 *cb_first_begin, *cb_first_end;   /* the first segment, as gcmInit set it up */
static u32 cb_first_offset;
static int stalls;
static void (*frame_hook)(void);   /* the RSX load, between a flip and the wait for it */
static volatile u32 sleeps;   /* every usleep while waiting on the RSX: each one wakes the PPU again */
static s64 flip_us;           /* the last flip's time: the system's (gcmGetLastFlipTime), else when the PPU
                                 caught it within 200 us; 0 unknown */
static uint32_t late_flips;   /* flips that came later than flip_frames TV frames after the one before */
static int flip_frames = 1, frames_changed;   /* TV frames a picture is meant to stay (2 under the RSX load) */
static double period_us = 16683.0;            /* the TV's frame: 59.94 Hz, or 50; learnt from the flips */
static int flip_time_sys;     /* the last flip's time came from the system */
static unsigned char ppu_late[REPORT_RING];   /* the main thread came over 2 ms after that flip */
static int reports_on;        /* the flips' time stamps: only once the RSX load's probe saw reports work */

/* wait_flip sleeps in one go until FLIP_EARLY_US before the next flip can come, then looks every 200 us */
#define FLIP_EARLY_US  1500

/* every wait on the RSX gives up after WAIT_TIMEOUT_US: if it ever stops, the app still reads the
   pad and the system messages, so START and the power button keep working */
static int wait_label(u8 index, u32 value)
{
	s64 start = sysGetSystemTime();

	while (*(vu32 *)gcmGetLabelAddress(index) != value) {
		if (sysGetSystemTime() - start > WAIT_TIMEOUT_US) {
			stalls++;
			return -1;
		}
		usleep(30);
		sleeps++;
	}
	return 0;
}

static void wait_finish(void)
{
	rsxSetWriteBackendLabel(context, LABEL_FINISH, sync_value);
	rsxFlushBuffer(context);
	wait_label(LABEL_FINISH, sync_value);
	sync_value++;
}

static void wait_idle(void)
{
	rsxSetWriteBackendLabel(context, LABEL_FINISH, sync_value);
	rsxSetWaitLabel(context, LABEL_FINISH, sync_value);
	sync_value++;
	wait_finish();
}

static void set_render_target(u32 index)
{
	gcmSurface sf;

	memset(&sf, 0, sizeof(sf));
	sf.colorFormat = GCM_SURFACE_X8R8G8B8;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0] = color_offset[index];
	sf.colorPitch[0] = color_pitch;
	for (int i = 1; i < 4; i++) {
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorOffset[i] = 0;
		sf.colorPitch[i] = 64;
	}
	sf.depthFormat = GCM_SURFACE_ZETA_Z16;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = depth_offset;
	sf.depthPitch = depth_pitch;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = width;
	sf.height = height;
	sf.x = 0;
	sf.y = 0;
	rsxSetSurface(context, &sf);
	/* the whole picture again: the RSX load (rsx_load.c) leaves the viewport and the window clips at its
	   256x256 tiles, and the real RSX keeps its clears inside the window clips - V9.0 redrew only the
	   top left corner then, the rest stood still (RPCS3 cleared it all) */
	{
		f32 scale[4] = { width * 0.5f, height * 0.5f, 0.5f, 0.0f }, shift[4] = { width * 0.5f, height * 0.5f, 0.5f, 0.0f };

		rsxSetViewport(context, 0, 0, (u16)width, (u16)height, 0.0f, 1.0f, scale, shift);
		for (int i = 0; i < 8; i++)
			rsxSetViewportClip(context, (u8)i, (u16)width, (u16)height);
	}
}

/* The flip waited for comes with the vertical blank one frame after the last flip. V8.3 looked every
   200 us from the start - 82 wakeups a frame, each taking the PPU from a load thread for a moment;
   one long sleep first leaves about 8. The time of a flip is only kept when the 200 us looks caught it:
   one that was over before them (a long frame, a late wakeup) would put the next sleep too late. */
static void flip_seen(int looks);

static void wait_flip(void)
{
	s64 start = sysGetSystemTime(), frames_us = (s64)(flip_frames * period_us);
	int looks = 0;

	if (flip_us && gcmGetFlipStatus() != 0) {
		s64 wake = flip_us + frames_us - FLIP_EARLY_US;

		if (wake > start && wake - start < frames_us) {
			usleep((u64)(wake - start));
			sleeps++;
		}
	}
	while (gcmGetFlipStatus() != 0) {
		if (sysGetSystemTime() - start > WAIT_TIMEOUT_US) {
			stalls++;
			break;
		}
		usleep(200);
		sleeps++;
		looks++;
	}
	gcmResetFlipStatus();
	flip_seen(looks);
}

/* The flip just seen: its time (V0.8: the system's own, so a main thread that comes late does not move
   it; V0.7 only had the moment the PPU saw it), the TV frames since the one before, the frame's length.
   A flip later than flip_frames frames counts as late unless the main thread queued its picture late. */
static void flip_seen(int looks)
{
	s64 now = sysGetSystemTime(), t = gcmGetLastFlipTime(), prev = flip_us;
	u32 waited_for = flips - 1;   /* the flip queued last, the one this waited for */
	int sys = t > 0 && t <= now + 1000 && now - t < 200000 && t > prev;

	flip_time_sys = sys;
	flip_us = sys ? t : looks ? now : 0;
	ppu_late[waited_for % REPORT_RING] = flip_us && !looks && now - flip_us > 2000;
	if (flip_us && prev) {
		double d = (double)(flip_us - prev);
		int n = (int)(d / period_us + 0.5);

		if (sys && flip_frames == 1 && n == 1 && d > 0.8 * period_us && d < 1.25 * period_us)
			period_us += 0.05 * (d - period_us);
		/* its picture was queued by a main thread that came late itself (a photo being saved): not the RSX */
		if (!frames_changed && n > flip_frames && !ppu_late[(waited_for - 1) % REPORT_RING])
			late_flips++;
	}
	if (frames_changed)
		frames_changed--;
}

/* The command buffer must never wrap. When libgcm_sys's callback leaves its last segment, the RSX
   is inside libgcm's own flip commands behind the buffer, and the callback waits for it to come
   back into the buffer - forever: the PPU hangs (on a real PS3 and in RPCS3, PSL1GHT's samples
   included). So once half the buffer is used, the RSX reads up to here, and the next frame starts
   again at the first segment. Called right after the flip wait, when it has little left to do. */
static void cb_init(void)
{
	cb_first_begin = context->begin;
	cb_first_end = context->end;
	rsxAddressToOffset(cb_first_begin, &cb_first_offset);
}

static void cb_rewind(void)
{
	gcmControlRegister volatile *ctrl = gcmGetControlRegister();
	s64 start = sysGetSystemTime();

	if ((u8 *)context->current - (u8 *)cb_first_begin < CB_REWIND_BYTES)
		return;

	rsxSetJumpCommand(context, cb_first_offset);
	__asm__ volatile("sync" ::: "memory");
	ctrl->put = cb_first_offset;
	while (ctrl->get != cb_first_offset) {
		if (sysGetSystemTime() - start > WAIT_TIMEOUT_US) {
			stalls++;
			break;
		}
		usleep(30);
		sleeps++;
	}
	context->begin = cb_first_begin;
	context->current = cb_first_begin;
	context->end = cb_first_end;
}

/* Draws a white block into the top-left corner, scales the frame into capture buffer 0 and looks
   where the block landed. That settles which way the RSX counts scissor rows - PSL1GHT sets the
   window origin to the bottom - and proves the copy into main memory works, all before the first
   frame is shown. */
static void run_selftest(void)
{
	const int cw = 640, ch = 360;
	volatile uint32_t *pixels = gfx_capture_buffer(0);
	uint32_t ticket;
	int waited = 0, top, bottom;

	for (int i = 0; i < cw * ch; i++)
		pixels[i] = 0x55555555;

	scissor_from_bottom = 0;
	gfx_rect(NULL, 0, 0, width, height, 0x000000);
	gfx_rect(NULL, 0, 0, width / 8, height / 8, 0xFFFFFF);
	ticket = gfx_capture(0, cw, ch);
	while (!gfx_capture_done(ticket) && waited < 500000) {
		usleep(100);
		waited += 100;
	}

	if (!gfx_capture_done(ticket)) {
		snprintf(selftest, sizeof(selftest), "SELF-TEST: RSX DOES NOT ANSWER");
		return;
	}
	if (pixels[(ch / 2) * cw + cw / 2] == 0x55555555) {
		snprintf(selftest, sizeof(selftest), "SELF-TEST: CAPTURE DOES NOT WRITE");
		return;
	}
	top = (pixels[10 * cw + 10] & 0xFFFFFF) == 0xFFFFFF;
	bottom = (pixels[(ch - 11) * cw + 10] & 0xFFFFFF) == 0xFFFFFF;
	if (top && !bottom) {
		snprintf(selftest, sizeof(selftest), "SELF-TEST OK");
	} else if (bottom && !top) {
		scissor_from_bottom = 1;
		snprintf(selftest, sizeof(selftest), "SELF-TEST OK (Y FROM THE BOTTOM)");
	} else {
		snprintf(selftest, sizeof(selftest), "SELF-TEST: SCISSOR UNCLEAR %d/%d", top, bottom);
	}
}

int gfx_init(void)
{
	videoState state;
	videoResolution res;
	videoConfiguration vconfig;

	host_addr = memalign(1024 * 1024, HOST_SIZE);
	if (!host_addr || rsxInit(&context, CB_SIZE, HOST_SIZE, host_addr) != 0)
		return -1;
	cb_init();

	videoGetState(0, 0, &state);
	videoGetResolution(state.displayMode.resolution, &res);

	memset(&vconfig, 0, sizeof(vconfig));
	vconfig.resolution = state.displayMode.resolution;
	vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
	vconfig.pitch = res.width * sizeof(u32);

	wait_idle();
	videoConfigure(0, &vconfig, NULL, 0);
	videoGetState(0, 0, &state);
	gcmSetFlipMode(GCM_FLIP_VSYNC);
	if ((state.displayMode.refreshRates & VIDEO_REFRESH_50HZ) && !(state.displayMode.refreshRates & VIDEO_REFRESH_59_94HZ))
		period_us = 20000.0;

	width = res.width;
	height = res.height;
	color_pitch = width * sizeof(u32);
	for (int i = 0; i < BUFFERS; i++) {
		color_buffer[i] = (u32 *)rsxMemalign(64, height * color_pitch);
		if (!color_buffer[i])
			return -1;
		rsxAddressToOffset(color_buffer[i], &color_offset[i]);
		gcmSetDisplayBuffer(i, color_offset[i], color_pitch, width, height);
	}
	depth_pitch = width * sizeof(u32);
	depth_buffer = (u32 *)rsxMemalign(64, height * depth_pitch);
	if (!depth_buffer)
		return -1;
	rsxAddressToOffset(depth_buffer, &depth_offset);

	for (int i = 0; i < TEE_CAPTURE_BUFFERS; i++)
		rsxAddressToOffset((u8 *)host_addr + CAPTURE_OFFSET + i * CAPTURE_BYTES, &capture_offset[i]);
	*(vu32 *)gcmGetLabelAddress(LABEL_CAPTURE) = 0;
	for (int i = 0; i < REPORT_RING * GFX_REP_SLOTS; i++)
		gfx_report_index_clear(REPORT_BASE + (u32)i);

	set_render_target(curr_fb);
	run_selftest();
	return 0;
}

void gfx_shutdown(void)
{
	gcmSetWaitFlip(context);
	wait_finish();
}

int gfx_stalls(void)
{
	return stalls;
}

uint32_t gfx_late_flips(void)
{
	return late_flips;
}

uint32_t gfx_sleeps(void)
{
	return sleeps;
}

int gfx_width(void)
{
	return (int)width;
}

int gfx_height(void)
{
	return (int)height;
}

void gfx_rect(void *ctx, int x, int y, int w, int h, uint32_t rgb)
{
	(void)ctx;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > (int)width) w = (int)width - x;
	if (y + h > (int)height) h = (int)height - y;
	if (w <= 0 || h <= 0)
		return;
	if (scissor_from_bottom)
		y = (int)height - y - h;

	rsxSetScissor(context, (u16)x, (u16)y, (u16)w, (u16)h);
	if (rgb != last_color) {
		rsxSetClearColor(context, rgb);
		last_color = rgb;
	}
	rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
}

/* A flip and what goes with it: the RSX's time stamps before the flip and after it, the RSX load (the
   hook), the wait for the flip and a time stamp after it. The gaps are the RSX's idle time: on the
   console the wait for the flip holds it until the vertical blank (the screen alone: 14 ms there, 1 ms
   at the flip command), and under the paced load it waits about 0.25 ms a picture at the flip command. */
static void queue_flip(u32 fb)
{
	last_color = 0xFFFFFFFF;
	for (int i = 0; i < GFX_REP_SLOTS; i++)   /* REPORT_RING flips ago: read long since */
		gfx_report_index_clear(REPORT_BASE + (flips % REPORT_RING) * GFX_REP_SLOTS + (u32)i);
	gfx_report(context, GFX_REP_BEFORE);
	gcmSetFlip(context, (u8)fb);
	gfx_report(context, GFX_REP_AFTER);
	rsxFlushBuffer(context);
	if (frame_hook)
		frame_hook();
	gcmSetWaitFlip(context);
	gfx_report(context, GFX_REP_WAITED);
	flips++;
}

/* V0.8: three pictures in turn. The one drawn next is neither on the screen nor the one waiting for its
   flip, so even if the wait for the flip let the RSX on before the picture changed, it never draws into
   what the TV shows. */
void gfx_flip(void)
{
	if (!first_flip)
		wait_flip();
	else
		gcmResetFlipStatus();
	cb_rewind();

	queue_flip(curr_fb);
	shown_fb = curr_fb;
	curr_fb = (curr_fb + 1) % BUFFERS;
	set_render_target(curr_fb);
	first_flip = 0;
}

/* Shows the last picture once more without drawing: the picture on screen is flipped again, so the flips
   keep coming as the system expects, and the next picture is still drawn into another one. Like gfx_flip
   it sets the render target again after the flip: without that, RPCS3 drew the next pictures where the
   capture did not find them (its photos showed the picture from before the test). */
void gfx_reflip(void)
{
	if (first_flip)
		return;
	wait_flip();
	cb_rewind();

	queue_flip(shown_fb);
	set_render_target(curr_fb);
}

gcmContextData *gfx_context(void)
{
	return context;
}

void gfx_set_frame_hook(void (*hook)(void))
{
	frame_hook = hook;
}

void *gfx_host_mem(u32 start, u32 size, u32 *offset)
{
	void *p = (u8 *)host_addr + start;

	/* only between the command buffer and the capture buffers */
	if (!host_addr || start < CB_SIZE || start + size > CAPTURE_OFFSET || rsxAddressToOffset(p, offset) != 0)
		return NULL;
	return p;
}

void gfx_forget_clear_color(void)
{
	last_color = 0xFFFFFFFF;
}

void gfx_reset_target(void)
{
	set_render_target(curr_fb);
}

void *gfx_host_area(u32 *offset)
{
	void *p = (u8 *)host_addr + LOAD_OFFSET;

	if (!host_addr || rsxAddressToOffset(p, offset) != 0)
		return NULL;
	return p;
}

uint32_t *gfx_capture_buffer(int index)
{
	return (uint32_t *)((u8 *)host_addr + CAPTURE_OFFSET + index * CAPTURE_BYTES);
}

static uint32_t capture(u32 fb, int index, int cw, int ch)
{
	gcmTransferScale scale;
	gcmTransferSurface surface;

	/* the 2D engine must not read the frame before the 3D side has finished drawing it */
	rsxSetWriteBackendLabel(context, LABEL_SYNC, sync_value);
	rsxSetWaitLabel(context, LABEL_SYNC, sync_value);
	sync_value++;

	memset(&scale, 0, sizeof(scale));
	scale.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
	scale.format = GCM_TRANSFER_SCALE_FORMAT_X8R8G8B8;
	scale.operation = GCM_TRANSFER_OPERATION_SRCCOPY;
	scale.clipX = 0;
	scale.clipY = 0;
	scale.clipW = (u16)cw;
	scale.clipH = (u16)ch;
	scale.outX = 0;
	scale.outY = 0;
	scale.outW = (u16)cw;
	scale.outH = (u16)ch;
	scale.ratioX = rsxGetFixedSint32((float)width / cw);    /* source size / destination size */
	scale.ratioY = rsxGetFixedSint32((float)height / ch);
	scale.inW = (u16)width;
	scale.inH = (u16)height;
	scale.pitch = (u16)color_pitch;
	scale.origin = GCM_TRANSFER_ORIGIN_CORNER;
	scale.interp = GCM_TRANSFER_INTERPOLATOR_LINEAR;
	scale.offset = color_offset[fb];
	scale.inX = rsxGetFixedUint16(0.0f);
	scale.inY = rsxGetFixedUint16(0.0f);

	memset(&surface, 0, sizeof(surface));
	surface.format = GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
	surface.pitch = (u16)(cw * sizeof(u32));
	surface.offset = capture_offset[index];

	rsxSetTransferScaleMode(context, GCM_TRANSFER_LOCAL_TO_MAIN, GCM_TRANSFER_SURFACE);
	rsxSetTransferScaleSurface(context, &scale, &surface);

	capture_ticket++;
	rsxSetWriteBackendLabel(context, LABEL_CAPTURE, capture_ticket);
	rsxFlushBuffer(context);
	return capture_ticket;
}

uint32_t gfx_capture(int index, int cw, int ch)
{
	return capture(curr_fb, index, cw, ch);
}

uint32_t gfx_capture_shown(int index, int cw, int ch)
{
	return capture(first_flip ? curr_fb : shown_fb, index, cw, ch);
}

int gfx_capture_done(uint32_t ticket)
{
	return (int32_t)(*(vu32 *)gcmGetLabelAddress(LABEL_CAPTURE) - ticket) >= 0;
}

const char *gfx_selftest(void)
{
	return selftest;
}

/* ---- V0.8: the RSX's time stamps, the flips' timing -------------------------------------------------------- */

/* A report of the pixel counter, written once everything before it is drawn: its 64-bit time (the RSX's
   clock, ns) is the time stamp. PSL1GHT's rsxSetTimeStamp writes its argument one word too far (the
   RSX gets whatever was there before), so the plain report is used. */
void gfx_report(gcmContextData *ctx, int slot)
{
	if (reports_on)
		gfx_report_at(ctx, REPORT_BASE + (flips % REPORT_RING) * GFX_REP_SLOTS + (u32)slot);
}

void gfx_reports_on(void)
{
	reports_on = 1;
}

void gfx_report_at(gcmContextData *ctx, u32 index)
{
	rsxSetReport(ctx, GCM_ZPASS_PIXEL_CNT, index);
}

/* V0.9: room for `words` in the command buffer's current segment. PSL1GHT's rsxLoadVertexProgramBlock writes
   its first words at the old position after a segment change and the rest at the new one (stale words in
   between: RPCS3 "Vertex program block aborts prematurely ... Dead FIFO" with the 268-instruction vertex
   load, at a 32 KB segment boundary); with the room asked for beforehand it never changes segments. */
void gfx_reserve(gcmContextData *ctx, u32 words)
{
	if (ctx->current + words > ctx->end)
		rsxContextCallback(ctx, words);
}

void gfx_load_vp(gcmContextData *ctx, const rsxVertexProgram *program, void *ucode)
{
	/* the program's words (4 an instruction, a header every 8), its other commands, and some to spare */
	gfx_reserve(ctx, (u32)program->num_insn * 5 + 256);
	rsxLoadVertexProgram(ctx, program, ucode);
}

/* V0.9: the command buffer back to its start when half of it is used and the RSX got through it (as each
   flip does) - for long runs of commands without a flip (the probe) */
void gfx_rewind(void)
{
	cb_rewind();
}

uint64_t gfx_report_index_ns(u32 index)
{
	gcmReportData *r = gcmGetReportDataAddress(index);

	return r ? r->timer : 0;
}

void gfx_report_index_clear(u32 index)
{
	gcmReportData *r = gcmGetReportDataAddress(index);

	if (r)
		r->timer = 0;
}

uint64_t gfx_report_ns(u32 cycle, int slot)
{
	return gfx_report_index_ns(REPORT_BASE + (cycle % REPORT_RING) * GFX_REP_SLOTS + (u32)slot);
}

u32 gfx_cycle(void)
{
	return flips;
}

int gfx_ppu_late(u32 cycle)
{
	return ppu_late[cycle % REPORT_RING];
}

double gfx_period_us(void)
{
	return period_us;
}

int gfx_flip_time_sys(void)
{
	return flip_time_sys;
}

void gfx_set_flip_frames(int n)
{
	n = n < 1 ? 1 : n > 4 ? 4 : n;
	if (n != flip_frames)
		frames_changed = 3;   /* the flips already queued still come at the old pace */
	flip_frames = n;
}

int gfx_flip_frames(void)
{
	return flip_frames;
}
