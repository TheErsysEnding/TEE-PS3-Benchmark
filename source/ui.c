/* See ui.h. The fragment program is tiny3d's "texture times colour" (its nv_shaders.h, in full precision
   with the result in R0); the vertex program, shaders/ui.vasm, only passes position, colour and atlas
   coordinate on - the PPU writes every corner in clip space already. The vertex data lies in main memory
   (two buffers, one for the frame being drawn, one for the frame the RSX may still read). */
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ppu-asm.h>
#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/rsx_program.h>
#include <sys/systime.h>

#include "bench_log.h"
#include "gfx.h"
#include "scene.h"
#include "ui_atlas_bin.h"
#include "ui_vpo.h"

#define MAX_QUADS     12000
#define VB_START      (1u << 20)                       /* host memory 1 MB on: the two vertex buffers */
#define VB_BYTES      (MAX_QUADS * 4 * sizeof(vtx))
#define TEST_START    (VB_START + 2 * 1024 * 1024)     /* the self-test's picture */
#define TEST_SIZE     64
#define BATCH_QUADS   64                               /* rsxDrawVertexArray's simple case: 256 vertices */
#define MAX_BREAKS    2048
#define LABEL_UI_CMD  245
#define LABEL_UI_BACK 246

typedef struct {
	float x, y, u, v;
	uint32_t rgba;   /* bytes R, G, B, A: four normalised unsigned bytes */
} vtx;

/* TEX R0, f[TEX0], TEX0, 2D; MUL R0, R0, f[COL0] (end): tiny3d's program (TEXX into R0, MULX into H0 with
   half-float output) in full precision with the result in R0 and fp_control 0x40, as the RSX load's
   programs, which ran on the console. Bits cleared: precision (22-23) of both, half (7) of the MUL. */
static const u32 FP_TEXCOL[8] = { 0x17009e00, 0x1c9dc801, 0x0, 0x0, 0x02003e01, 0x1c9dc800, 0x0001c801, 0x0 };

static int mode = 1;            /* 0 textured, 1 pixel font */
static float sw, sh, k;         /* the screen, and screen pixels per layout pixel */
static float ax, ay;            /* clip space per screen pixel (set_screen) */
#define IU  (1.0f / UI_ATLAS_W)
#define IV  (1.0f / UI_ATLAS_H)
static float flip_y = 1.0f;     /* clip y +1 at the top row: the self-test finds out */
static vtx *vb[2];
static u32 vb_off[2];
static int cur, quads;
/* RPCS3 blends the soft edges of a quad with the picture as it was before the draw call, not with the
   quads before it in the same call (the console blends in order): its photos showed the card's border
   in a box behind glyphs and dark rims on the ring. So a shape or text that lies over one drawn in the
   same call starts a new call - every item's bounding box (clip space) is checked against those of the
   call so far. The quad numbers where a new call starts: */
#define MAX_RECTS  48
static int breaks[MAX_BREAKS], nbreaks;
static float rects[MAX_RECTS][4], box[4];
static int nrects, item_first;

static void grow(float x, float y);
static uint32_t last_quads;
static u32 *atlas, atlas_off, *fp_mem, fp_off;
static rsxFragmentProgram fp_head;
static const rsxVertexProgram *vp;
static void *vp_ucode;
static gcmTexture tex;

/* ---- quads ------------------------------------------------------------------------------------------- */

/* the screen the quads go to (the self-test's 64x64 picture for a moment) */
static void set_screen(float w, float h)
{
	sw = w;
	sh = h;
	ax = 2.0f / w;
	ay = 2.0f / h;
}

static uint32_t rgba(uint32_t argb)
{
	return argb << 8 | argb >> 24;
}

/* one quad in screen pixels, its atlas corner in atlas pixels */
static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t argb)
{
	vtx *q;
	uint32_t c = rgba(argb);

	if (quads >= MAX_QUADS || x1 <= x0 || y1 <= y0)
		return;
	q = vb[cur] + quads * 4;
	quads++;
	x0 = x0 * ax - 1.0f;
	x1 = x1 * ax - 1.0f;
	y0 = flip_y * (1.0f - y0 * ay);
	y1 = flip_y * (1.0f - y1 * ay);
	grow(x0, y0);
	grow(x1, y1);
	q[0] = (vtx){ x0, y0, u0 * IU, v0 * IV, c };
	q[1] = (vtx){ x1, y0, u1 * IU, v0 * IV, c };
	q[2] = (vtx){ x1, y1, u1 * IU, v1 * IV, c };
	q[3] = (vtx){ x0, y1, u0 * IU, v1 * IV, c };
}

/* an item (a shape, a text) begins and ends: where it lies over one in the call so far, it starts a new one */
static void item_begin(void)
{
	item_first = quads;
	box[0] = box[1] = 1e9f;
	box[2] = box[3] = -1e9f;
}

static void grow(float x, float y)
{
	box[0] = x < box[0] ? x : box[0];
	box[1] = y < box[1] ? y : box[1];
	box[2] = x > box[2] ? x : box[2];
	box[3] = y > box[3] ? y : box[3];
}

static void item_end(void)
{
	int hit = nrects >= MAX_RECTS;

	if (quads <= item_first)
		return;
	for (int r = 0; r < nrects && !hit; r++)
		hit = box[0] < rects[r][2] && rects[r][0] < box[2] && box[1] < rects[r][3] && rects[r][1] < box[3];
	if (hit) {
		if (nbreaks < MAX_BREAKS && item_first > (nbreaks ? breaks[nbreaks - 1] : 0))
			breaks[nbreaks++] = item_first;
		nrects = 0;
	}
	memcpy(rects[nrects++], box, sizeof(box));
}

/* a quad with any four corners (screen pixels, clockwise), each with its own atlas point */
static void quad4(const float *x, const float *y, const float *u, const float *v, uint32_t argb)
{
	vtx *q;
	uint32_t c = rgba(argb);

	if (quads >= MAX_QUADS)
		return;
	q = vb[cur] + quads * 4;
	quads++;
	for (int i = 0; i < 4; i++) {
		q[i] = (vtx){ x[i] * ax - 1.0f, flip_y * (1.0f - y[i] * ay), u[i] * IU, v[i] * IV, c };
		grow(q[i].x, q[i].y);
	}
}

/* a shape of the atlas over a layout rectangle */
static void sprite(const unsigned short *s, float inset, float x, float y, float w, float h, uint32_t argb)
{
	quad(x * k, y * k, (x + w) * k, (y + h) * k, s[0] + inset, s[1] + inset, s[0] + s[2] - inset, s[1] + s[3] - inset,
	     argb);
}

static void white(float x0, float y0, float x1, float y1, uint32_t argb)
{
	float u = UI_WHITE[0] + 4.0f, v = UI_WHITE[1] + 4.0f;   /* the block's middle: white whatever the filter does */

	quad(x0, y0, x1, y1, u, v, u, v, argb);
}

/* ---- the pixel font's stand-ins ------------------------------------------------------------------------ */

static void old_rect(float x, float y, float w, float h, uint32_t argb)
{
	if ((argb >> 24) < 0x80)
		return;
	gfx_rect(NULL, (int)(x * k + 0.5f), (int)(y * k + 0.5f), (int)(w * k + 0.5f), (int)(h * k + 0.5f), argb & 0xFFFFFF);
}

static int old_px(int style)
{
	int p = (int)(UI_STYLE[style].cap / 7.0f * k + 0.5f);
	return p < 1 ? 1 : p;
}

static void upper(char *d, size_t n, const char *s)
{
	size_t i = 0;

	for (; s[i] && i + 1 < n; i++)
		d[i] = s[i] >= 'a' && s[i] <= 'z' ? (char)(s[i] - 32) : s[i];
	d[i] = 0;
}

/* ---- the drawing calls ----------------------------------------------------------------------------------- */

void ui_rect(float x, float y, float w, float h, uint32_t argb)
{
	if (mode) {
		old_rect(x, y, w, h, argb);
		return;
	}
	item_begin();
	white(floorf(x * k + 0.5f), floorf(y * k + 0.5f), floorf((x + w) * k + 0.5f), floorf((y + h) * k + 0.5f), argb);
	item_end();
}

void ui_round(float x, float y, float w, float h, float r, uint32_t argb)
{
	float x0, y0, x1, y1, rr, cx0, cy0, cm, cx1, cy1;

	if (mode) {
		old_rect(x, y, w, h, argb);
		return;
	}
	if (r > w / 2)
		r = w / 2;
	if (r > h / 2)
		r = h / 2;
	x0 = floorf(x * k + 0.5f);
	y0 = floorf(y * k + 0.5f);
	x1 = floorf((x + w) * k + 0.5f);
	y1 = floorf((y + h) * k + 0.5f);
	rr = floorf(r * k + 0.5f);
	item_begin();
	if (rr < 1.0f) {
		white(x0, y0, x1, y1, argb);
		item_end();
		return;
	}
	/* the circle fills 2..98 of its 100x100 cell: the corners are its quarters */
	cx0 = UI_CIRCLE[0] + 2.0f;
	cy0 = UI_CIRCLE[1] + 2.0f;
	cm = 48.0f;
	cx1 = cx0 + 2 * cm;
	cy1 = cy0 + 2 * cm;
	quad(x0, y0, x0 + rr, y0 + rr, cx0, cy0, cx0 + cm, cy0 + cm, argb);
	quad(x1 - rr, y0, x1, y0 + rr, cx0 + cm, cy0, cx1, cy0 + cm, argb);
	quad(x0, y1 - rr, x0 + rr, y1, cx0, cy0 + cm, cx0 + cm, cy1, argb);
	quad(x1 - rr, y1 - rr, x1, y1, cx0 + cm, cy0 + cm, cx1, cy1, argb);
	white(x0 + rr, y0, x1 - rr, y1, argb);
	white(x0, y0 + rr, x0 + rr, y1 - rr, argb);
	white(x1 - rr, y0 + rr, x1, y1 - rr, argb);
	item_end();
}

void ui_card(float x, float y, float w, float h, float r, uint32_t fill, uint32_t border, float line)
{
	if (line > 0.0f) {
		ui_round(x, y, w, h, r, border);
		ui_round(x + line, y + line, w - 2 * line, h - 2 * line, r - line, fill);
	} else {
		ui_round(x, y, w, h, r, fill);
	}
}

void ui_circle(float cx, float cy, float r, uint32_t argb)
{
	if (mode) {
		old_rect(cx - r, cy - r, 2 * r, 2 * r, argb);
		return;
	}
	item_begin();
	sprite(UI_CIRCLE, 2.0f, cx - r, cy - r, 2 * r, 2 * r, argb);
	item_end();
}

void ui_glow(float cx, float cy, float r, uint32_t argb)
{
	if (!mode) {
		item_begin();
		sprite(UI_GLOW, 0.0f, cx - r, cy - r, 2 * r, 2 * r, argb);
		item_end();
	}
}

/* A band from turn t0 to t1, in pieces that only touch: each piece is two quads, edge to middle, that take
   their alpha across from the circle's middle row - its edge, soft over one pixel, then solid - so the
   band's sides are smooth whatever its width. Round ends: a circle on each, in a call of their own. */
void ui_arc(float cx, float cy, float r, float width, float t0, float t1, uint32_t argb)
{
	float sx = cx * k, sy = cy * k, rs = r * k, hw = width * k / 2.0f, len, edge, mid, row;
	int n;

	if (t1 <= t0)
		return;
	if (mode) {
		/* the pixel font's stand-in: squares along the arc */
		len = 2.0f * 3.14159265f * r * (t1 - t0);
		n = (int)(len / (width > 4.0f ? width : 4.0f)) + 1;
		for (int i = 0; i <= n && i < 400; i++) {
			float a = (t0 + (t1 - t0) * (float)i / (float)n) * 2.0f * 3.14159265f;
			old_rect(cx + r * sinf(a) - width / 2, cy - r * cosf(a) - width / 2, width, width, argb);
		}
		return;
	}
	len = 2.0f * 3.14159265f * rs * (t1 - t0);
	n = (int)(len / 6.0f) + 2;
	if (n > 600)
		n = 600;
	/* a pixel more on each side, where the circle's cell is still clear: the edge fades out fully */
	hw += 1.0f;
	edge = UI_CIRCLE[0] + 1.0f;
	mid = edge + (hw < 47.0f ? hw : 47.0f);
	row = UI_CIRCLE[1] + 50.0f;
	item_begin();
	{
		/* the pieces' corners by turning on from the start: sine and cosine twice, not four times a piece */
		float a0 = t0 * 2.0f * 3.14159265f, da = (t1 - t0) * 2.0f * 3.14159265f / (float)n;
		float s0 = sinf(a0), c0 = cosf(a0), sd = sinf(da), cd = cosf(da);

		for (int i = 0; i < n; i++) {
			float s1 = s0 * cd + c0 * sd, c1 = c0 * cd - s0 * sd;
			float ri = rs - hw, ro = rs + hw;
			float xi[4] = { sx + ri * s0, sx + rs * s0, sx + rs * s1, sx + ri * s1 };
			float yi[4] = { sy - ri * c0, sy - rs * c0, sy - rs * c1, sy - ri * c1 };
			float xo[4] = { sx + rs * s0, sx + ro * s0, sx + ro * s1, sx + rs * s1 };
			float yo[4] = { sy - rs * c0, sy - ro * c0, sy - ro * c1, sy - rs * c1 };
			float ui[4] = { edge, mid, mid, edge }, uo[4] = { mid, edge, edge, mid }, v[4] = { row, row, row, row };

			quad4(xi, yi, ui, v, argb);
			quad4(xo, yo, uo, v, argb);
			s0 = s1;
			c0 = c1;
		}
	}
	item_end();
	{
		float a0 = t0 * 2.0f * 3.14159265f, a1 = t1 * 2.0f * 3.14159265f;

		ui_circle(cx + r * sinf(a0), cy - r * cosf(a0), width / 2.0f, argb);
		ui_circle(cx + r * sinf(a1), cy - r * cosf(a1), width / 2.0f, argb);
	}
}

void ui_icon(int icon, float x, float y, float size, uint32_t argb)
{
	if (icon < 0 || icon >= UI_ICONS)
		return;
	if (mode) {
		old_rect(x + size * 0.3f, y + size * 0.3f, size * 0.4f, size * 0.4f, argb);
		return;
	}
	/* the 64x64 picture without its margin of 4 */
	item_begin();
	sprite(UI_ICON[icon], 4.0f, x, y, size, size, argb);
	item_end();
}

float ui_cap(int style)
{
	return UI_STYLE[style].cap;
}

static int glyph_index(unsigned char c)
{
	if (c == '`')
		return 127 - 32;   /* the degree sign */
	if (c < 32 || c > 126)
		return '?' - 32;
	return c - 32;
}

float ui_text_w(int style, const char *s)
{
	float w = 0.0f;

	if (mode) {
		char u[256];

		upper(u, sizeof(u), s);
		return (float)font_width(u, old_px(style)) / k;
	}
	for (; *s; s++)
		w += UI_STYLE[style].glyph[glyph_index((unsigned char)*s)].adv;
	return w;
}

float ui_text(float x, float y, int style, uint32_t argb, const char *s)
{
	const ui_style *st = &UI_STYLE[style];
	float pen = x * k, base = floorf((y + st->cap) * k + 0.5f), start = pen;

	if (mode) {
		char u[256];

		upper(u, sizeof(u), s);
		if ((argb >> 24) >= 0x80)
			font_draw(gfx_rect, NULL, (int)(x * k + 0.5f), (int)(y * k + 0.5f), old_px(style), argb & 0xFFFFFF, u);
		return ui_text_w(style, s);
	}
	item_begin();
	for (; *s; s++) {
		const ui_glyph *g = &st->glyph[glyph_index((unsigned char)*s)];

		if (g->w) {
			/* whole screen pixels at full size: the glyphs stay sharp */
			float gx = k >= 0.99f ? floorf(pen + g->ox + 0.5f) : pen + g->ox * k;
			float gy = k >= 0.99f ? base - g->oy : base - g->oy * k;

			quad(gx, gy, gx + g->w * k, gy + g->h * k, g->x, g->y, g->x + g->w, g->y + g->h, argb);
		}
		pen += g->adv * k;
	}
	item_end();
	return (pen - start) / k;
}

float ui_text_right(float right, float y, int style, uint32_t argb, const char *s)
{
	float w = ui_text_w(style, s);

	ui_text(right - w, y, style, argb, s);
	return w;
}

float ui_text_center(float cx, float y, int style, uint32_t argb, const char *s)
{
	float w = ui_text_w(style, s);

	ui_text(cx - w / 2, y, style, argb, s);
	return w;
}

/* ---- the RSX side ---------------------------------------------------------------------------------------------- */

static void state(gcmContextData *ctx, int buffer)
{
	rsxSetColorMask(ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(ctx, 0);
	rsxSetDepthTestEnable(ctx, GCM_FALSE);
	rsxSetDepthWriteEnable(ctx, GCM_FALSE);
	rsxSetStencilTestEnable(ctx, GCM_FALSE);
	rsxSetAlphaTestEnable(ctx, GCM_FALSE);
	rsxSetCullFaceEnable(ctx, GCM_FALSE);
	rsxSetDitherEnable(ctx, GCM_FALSE);
	rsxSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
	rsxSetUserClipPlaneControl(ctx, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
	                           GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
	                           GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
	rsxSetZMinMaxControl(ctx, GCM_FALSE, GCM_TRUE, GCM_FALSE);
	rsxSetBlendEnable(ctx, GCM_TRUE);
	rsxSetBlendFunc(ctx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_ONE, GCM_ONE_MINUS_SRC_ALPHA);
	rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
	gfx_load_vp(ctx, vp, vp_ucode);
	rsxLoadFragmentProgramLocation(ctx, &fp_head, fp_off, GCM_LOCATION_RSX);
	rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
	rsxLoadTexture(ctx, 0, &tex);
	rsxTextureControl(ctx, 0, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(ctx, 0, 0, GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(ctx, 0, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, 0,
	                   GCM_TEXTURE_ZFUNC_LESS, 0);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0, vb_off[buffer], sizeof(vtx), 2, GCM_VERTEX_DATA_TYPE_F32,
	                         GCM_LOCATION_CELL);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX0, 0, vb_off[buffer] + 8, sizeof(vtx), 2,
	                         GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_CELL);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0, vb_off[buffer] + 16, sizeof(vtx), 4,
	                         GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_CELL);
	/* V0.9: the RSX load's vertices use two more attributes - off here */
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX1, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX2, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxInvalidateVertexCache(ctx);
}

/* the quads in draw calls: one from each break to the next, in pieces of BATCH_QUADS */
static void draw_quads(gcmContextData *ctx, int n)
{
	int q = 0;

	for (int b = 0; b <= nbreaks; b++) {
		int end = b < nbreaks ? breaks[b] : n;

		end = end > n ? n : end;
		while (q < end) {
			int m = end - q < BATCH_QUADS ? end - q : BATCH_QUADS;

			rsxDrawVertexArray(ctx, GCM_TYPE_QUADS, (u32)q * 4, (u32)m * 4);
			q += m;
		}
	}
}

void ui_begin(uint32_t background)
{
	quads = 0;
	nbreaks = nrects = 0;
	gfx_rect(NULL, 0, 0, (int)sw, (int)sh, background & 0xFFFFFF);
}

void ui_end(void)
{
	gcmContextData *ctx = gfx_context();

	last_quads = (uint32_t)quads;
	if (mode || !quads)
		return;
	__asm__ volatile("sync" ::: "memory");   /* the corners are in memory before the RSX reads them */
	state(ctx, cur);
	draw_quads(ctx, quads);
	cur ^= 1;
	quads = 0;
	nbreaks = nrects = 0;
}

uint32_t ui_last_quads(void)
{
	return last_quads;
}

int ui_textured(void)
{
	return !mode;
}

/* ---- the self-test --------------------------------------------------------------------------------------------- */

static u32 label(int i)
{
	return *(vu32 *)gcmGetLabelAddress((u8)i);
}

/* both labels within half a second: 0; the command reader did not get there or the drawing did not end: -1 */
static int wait_step(gcmContextData *ctx, u32 n, const char *what)
{
	uint64_t t0, limit = sysGetTimebaseFrequency() / 2;
	int read = 0;

	rsxSetWriteBackendLabel(ctx, LABEL_UI_BACK, n);
	rsxSetWriteCommandLabel(ctx, LABEL_UI_CMD, n);
	rsxFlushBuffer(ctx);
	t0 = __gettime();
	while (__gettime() - t0 < limit) {
		read = (int32_t)(label(LABEL_UI_CMD) - n) >= 0;
		if (read && (int32_t)(label(LABEL_UI_BACK) - n) >= 0)
			return 0;
		usleep(100);
	}
	log_line("Interface self-test (%s): FAILED - the commands %s, the drawing did not finish (RSX reads at %08x of %08x)",
	         what, read ? "were read" : "were NOT read", (unsigned)gcmGetControlRegister()->get,
	         (unsigned)gcmGetControlRegister()->put);
	return -1;
}

static int near(int a, int b, int tol)
{
	return a - b <= tol && b - a <= tol;
}

static int pixel_is(u32 p, int r, int g, int b, int tol)
{
	return near((int)(p >> 16 & 0xFF), r, tol) && near((int)(p >> 8 & 0xFF), g, tol) && near((int)(p & 0xFF), b, tol);
}

/* A 64x64 A8R8G8B8 picture, cleared to 203040: green over the left half (texture times colour), half
   transparent red over one right quarter (blending), blue from the circle's middle over the other (the
   circle shape). Read back and compared; where the red quarter lands says which way clip y runs. */
static int selftest(void)
{
	gcmContextData *ctx = gfx_context();
	gcmSurface sf;
	u32 color_off, depth_off, test_off, *test_host;
	void *color = rsxMemalign(64, TEST_SIZE * TEST_SIZE * 4), *depth = rsxMemalign(64, TEST_SIZE * TEST_SIZE * 2);
	f32 scale[4] = { TEST_SIZE * 0.5f, TEST_SIZE * 0.5f, 0.5f, 0.0f }, shift[4] = { TEST_SIZE * 0.5f, TEST_SIZE * 0.5f, 0.5f, 0.0f };
	float save_w = sw, save_h = sh, save_k = k;
	int top, bottom;
	u32 p_left, p_top, p_bottom;

	test_host = (u32 *)gfx_host_mem(TEST_START, TEST_SIZE * TEST_SIZE * 4, &test_off);
	if (!color || !depth || !test_host) {
		log_line("Interface self-test: no memory for it");
		return 1;
	}
	rsxAddressToOffset(color, &color_off);
	rsxAddressToOffset(depth, &depth_off);
	memset(test_host, 0, TEST_SIZE * TEST_SIZE * 4);

	memset(&sf, 0, sizeof(sf));
	sf.colorFormat = GCM_SURFACE_A8R8G8B8;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0] = color_off;
	sf.colorPitch[0] = TEST_SIZE * 4;
	for (int i = 1; i < 4; i++) {
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorOffset[i] = 0;
		sf.colorPitch[i] = 64;
	}
	sf.depthFormat = GCM_SURFACE_ZETA_Z16;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = depth_off;
	sf.depthPitch = TEST_SIZE * 2;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = TEST_SIZE;
	sf.height = TEST_SIZE;
	rsxSetSurface(ctx, &sf);
	rsxSetViewport(ctx, 0, 0, TEST_SIZE, TEST_SIZE, 0.0f, 1.0f, scale, shift);
	for (int i = 0; i < 8; i++)
		rsxSetViewportClip(ctx, (u8)i, TEST_SIZE, TEST_SIZE);
	rsxSetScissor(ctx, 0, 0, TEST_SIZE, TEST_SIZE);
	rsxSetClearColor(ctx, 0xFF203040);
	rsxSetColorMask(ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
	gfx_forget_clear_color();

	/* the quads on a 64x64 "screen"; clip y +1 drawn as the top for now */
	set_screen(TEST_SIZE, TEST_SIZE);
	k = 1.0f;
	quads = 0;
	nbreaks = nrects = 0;
	white(0, 0, 32, 64, 0xFF00FF00);
	white(32, 0, 64, 32, 0x80FF0000);
	quad(32, 32, 64, 64, UI_CIRCLE[0] + 34.0f, UI_CIRCLE[1] + 34.0f, UI_CIRCLE[0] + 66.0f, UI_CIRCLE[1] + 66.0f,
	     0xFF0000FF);
	__asm__ volatile("sync" ::: "memory");
	state(ctx, cur);
	draw_quads(ctx, quads);
	quads = 0;
	set_screen(save_w, save_h);
	k = save_k;
	if (wait_step(ctx, 1, "texture, colour, blending") != 0)
		return -1;

	/* back into main memory with the screenshots' copy, one pixel to one pixel: RPCS3 hands a plain memory
	   copy the surface's old memory, this one the picture (on the console both are the same) */
	{
		gcmTransferScale scale;
		gcmTransferSurface surface;

		memset(&scale, 0, sizeof(scale));
		scale.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
		scale.format = GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8;
		scale.operation = GCM_TRANSFER_OPERATION_SRCCOPY;
		scale.clipW = TEST_SIZE;
		scale.clipH = TEST_SIZE;
		scale.outW = TEST_SIZE;
		scale.outH = TEST_SIZE;
		scale.ratioX = rsxGetFixedSint32(1.0f);
		scale.ratioY = rsxGetFixedSint32(1.0f);
		scale.inW = TEST_SIZE;
		scale.inH = TEST_SIZE;
		scale.pitch = TEST_SIZE * 4;
		scale.origin = GCM_TRANSFER_ORIGIN_CORNER;
		scale.interp = GCM_TRANSFER_INTERPOLATOR_NEAREST;
		scale.offset = color_off;
		scale.inX = rsxGetFixedUint16(0.0f);
		scale.inY = rsxGetFixedUint16(0.0f);
		memset(&surface, 0, sizeof(surface));
		surface.format = GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
		surface.pitch = TEST_SIZE * 4;
		surface.offset = test_off;
		rsxSetTransferScaleMode(ctx, GCM_TRANSFER_LOCAL_TO_MAIN, GCM_TRANSFER_SURFACE);
		rsxSetTransferScaleSurface(ctx, &scale, &surface);
	}
	if (wait_step(ctx, 2, "copy to main memory") != 0)
		return -1;

	/* RPCS3 writes its copy's result into main memory a little after the label (its screenshots are read a
	   frame later: 11 ms to 1.7 s seen); the console has it within 1 ms. So it may take up to 5 s. */
	{
		uint64_t t0 = __gettime(), limit = 5 * sysGetTimebaseFrequency(), waited;

		do {
			__asm__ volatile("sync" ::: "memory");
			p_left = test_host[40 * TEST_SIZE + 10];
			p_top = test_host[10 * TEST_SIZE + 50];
			p_bottom = test_host[54 * TEST_SIZE + 50];
			if (p_left | p_top | p_bottom)
				break;
			usleep(1000);
		} while (__gettime() - t0 < limit);
		waited = (__gettime() - t0) * 1000000ull / sysGetTimebaseFrequency();
		if (waited > 1000)
			log_line("Interface self-test: the copy's pixels came %llu us after its label", (unsigned long long)waited);
	}
	/* half red over 203040: about 8F 18 20 */
	top = pixel_is(p_top, 0x8F, 0x18, 0x20, 4) && pixel_is(p_bottom, 0, 0, 0xFF, 2);
	bottom = pixel_is(p_bottom, 0x8F, 0x18, 0x20, 4) && pixel_is(p_top, 0, 0, 0xFF, 2);
	if (!pixel_is(p_left, 0, 0xFF, 0, 2) || (!top && !bottom)) {
		log_line("Interface self-test: the picture is not as it should be (left %08x, right top %08x, right bottom %08x; "
		         "expected 00FF00, 8F1820 and 0000FF) - the old pixel font draws the screens", (unsigned)p_left,
		         (unsigned)p_top, (unsigned)p_bottom);
		return 1;
	}
	flip_y = top ? 1.0f : -1.0f;
	log_line("Interface self-test: texture, colour and blending as they should be (left %08x, right %08x and %08x); "
	         "clip y +1 is the %s row", (unsigned)p_left, (unsigned)p_top, (unsigned)p_bottom, top ? "top" : "bottom");
	return 0;
}

int ui_init(void)
{
	u32 size;
	int r;

	set_screen((float)gfx_width(), (float)gfx_height());
	k = sw / 1920.0f;
	mode = 1;

	vb[0] = (vtx *)gfx_host_mem(VB_START, 2 * VB_BYTES, &vb_off[0]);
	atlas = (u32 *)rsxMemalign(128, UI_ATLAS_W * UI_ATLAS_H * 4);
	fp_mem = (u32 *)rsxMemalign(64, sizeof(FP_TEXCOL));
	if (!vb[0] || !atlas || !fp_mem || ui_atlas_bin_size < UI_ATLAS_W * UI_ATLAS_H) {
		log_line("Interface: no memory for the atlas or the vertices - the old pixel font draws the screens");
		return 1;
	}
	vb[1] = (vtx *)((u8 *)vb[0] + VB_BYTES);
	vb_off[1] = vb_off[0] + VB_BYTES;

	/* the atlas: white, its coverage as alpha */
	for (u32 i = 0; i < UI_ATLAS_W * UI_ATLAS_H; i++)
		atlas[i] = (u32)ui_atlas_bin[i] << 24 | 0x00FFFFFF;
	rsxAddressToOffset(atlas, &atlas_off);
	memset(&tex, 0, sizeof(tex));
	tex.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
	tex.mipmap = 1;
	tex.dimension = GCM_TEXTURE_DIMS_2D;
	tex.cubemap = GCM_FALSE;
	tex.remap = (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
	            (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
	            (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
	            (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
	            (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
	            (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
	            (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
	            (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT);
	tex.width = UI_ATLAS_W;
	tex.height = UI_ATLAS_H;
	tex.depth = 1;
	tex.location = GCM_LOCATION_RSX;
	tex.pitch = UI_ATLAS_W * 4;
	tex.offset = atlas_off;

	/* the fragment program, its words' halves swapped as the RSX reads them */
	for (int i = 0; i < 8; i++)
		fp_mem[i] = FP_TEXCOL[i] << 16 | FP_TEXCOL[i] >> 16;
	rsxAddressToOffset(fp_mem, &fp_off);
	memset(&fp_head, 0, sizeof(fp_head));
	fp_head.magic = 0x4650;
	fp_head.num_regs = 2;
	fp_head.num_insn = 2;
	fp_head.fp_control = 0x40;   /* the result is R0 (32-bit floats), as cgcomp sets it */
	fp_head.texcoords = 1;
	fp_head.texcoord2D = 1;

	vp = (const rsxVertexProgram *)ui_vpo;
	rsxVertexProgramGetUCode(vp, &vp_ucode, &size);

	r = selftest();
	if (r == 0)
		mode = 0;
	return r;
}
