/* made by tools/make_font_atlas.swift - do not edit. Roboto (Apache License 2.0) in the interface's
   sizes, in pixels of a 1920x1080 screen; the atlas is data/ui_atlas.bin. */
#ifndef UI_ATLAS_INCLUDED
#define UI_ATLAS_INCLUDED

#define UI_ATLAS_W  2048
#define UI_ATLAS_H  496

enum { UI_SMALL, UI_BODY, UI_LABEL, UI_SUB, UI_NUM, UI_TITLE, UI_HERO, UI_BIG, UI_STYLES };

typedef struct {
	unsigned short x, y, w, h;   /* in the atlas; w 0: nothing to draw */
	short ox, oy;                /* the bitmap's left edge from the pen, its top above the baseline */
	float adv;                   /* how far the pen moves on */
} ui_glyph;

typedef struct {
	float size, ascent, descent, line, cap;
	const ui_glyph *glyph;      /* 96 entries: ' ' (32) to '~' (126) and the degree sign at 127 */
} ui_style;

extern const ui_style UI_STYLE[UI_STYLES];
/* the shapes: x, y, w, h in the atlas */
extern const unsigned short UI_WHITE[4], UI_CIRCLE[4], UI_GLOW[4];
/* the icons, white line drawings in 72x72 (a 64x64 picture and a margin of 4) */
enum { UI_ICON_CROSS, UI_ICON_CIRCLE_BTN, UI_ICON_TRIANGLE, UI_ICON_SQUARE, UI_ICON_WAVE, UI_ICON_HASH, UI_ICON_CHIP, UI_ICON_LIST, UI_ICON_BARS, UI_ICON_REPEAT, UI_ICON_FLAME, UI_ICON_TABLE, UI_ICON_CHECK, UI_ICON_GPU, UI_ICONS };
extern const unsigned short UI_ICON[UI_ICONS][4];

#endif
