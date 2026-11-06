/* The interface's drawing (V0.6): text in Roboto and shapes out of one texture atlas (ui_atlas.h), every
   piece a quad with a colour, all of a frame's quads in one vertex buffer and a few draw calls. The
   layout is in pixels of a 1920x1080 screen; colours are 0xAARRGGBB. Before the first frame ui_init
   checks on the RSX that texture, colour and blending come out as they should: if not, the same calls
   draw with the old pixel font (no small letters, no round corners). Main thread only. */
#ifndef UI_H
#define UI_H

#include <stdint.h>

#include "ui_atlas.h"

/* after gfx_init: 0 when the textured interface works; -1 when its self-test hung the RSX (the app must
   end); 1 when it draws with the pixel font */
int  ui_init(void);
int  ui_textured(void);

/* a frame: ui_begin clears the picture, the shapes and texts collect, ui_end queues them (before the flip) */
void ui_begin(uint32_t background);
void ui_end(void);
uint32_t ui_last_quads(void);

void  ui_rect(float x, float y, float w, float h, uint32_t argb);
void  ui_round(float x, float y, float w, float h, float r, uint32_t argb);
/* a rounded card: border colour, then the fill inset by line */
void  ui_card(float x, float y, float w, float h, float r, uint32_t fill, uint32_t border, float line);
void  ui_circle(float cx, float cy, float r, uint32_t argb);
void  ui_glow(float cx, float cy, float r, uint32_t argb);
/* a round-ended arc of the given width on radius r, from turn t0 to t1 (0 at the top, clockwise) */
void  ui_arc(float cx, float cy, float r, float width, float t0, float t1, uint32_t argb);
void  ui_icon(int icon, float x, float y, float size, uint32_t argb);
/* text with the top of its capitals at y; returns its width. '`' prints the degree sign */
float ui_text(float x, float y, int style, uint32_t argb, const char *s);
float ui_text_w(int style, const char *s);
float ui_text_right(float right, float y, int style, uint32_t argb, const char *s);
float ui_text_center(float cx, float y, int style, uint32_t argb, const char *s);
float ui_cap(int style);

#endif
