/* Test scene, 5x7 text and test tone. Drawing goes through a rectangle callback, so the PS3 fills
   rectangles on the RSX and the Mac test sender fills them in memory - same picture on both. */
#ifndef TEE_SCENE_H
#define TEE_SCENE_H

#include <stdint.h>

typedef void (*scene_rect_fn)(void *ctx, int x, int y, int w, int h, uint32_t rgb);

/* draws text with each font pixel px*px big; lower case is shown as upper case, '`' as a degree sign.
   Returns the width. */
int font_draw(scene_rect_fn rect, void *ctx, int x, int y, int px, uint32_t rgb, const char *text);
int font_width(const char *text, int px);

/* the flash and the beep share one clock: both fire at every full second after epoch. 150 ms is
   long enough to land in a streamed picture even when the stream only manages 20 fps. */
#define SCENE_FLASH_US   150000
#define SCENE_BEEP_US    150000

int scene_flash_on(uint64_t now_us, uint64_t epoch_us);

/* viewers < 0 hides the eye and its count next to the frame counter */
void scene_draw(scene_rect_fn rect, void *ctx, int width, int height, uint32_t frame,
                uint64_t now_us, uint64_t epoch_us, const char *const *lines, int line_count,
                int viewers);

typedef struct {
	uint64_t sample;   /* running sample count, keeps the hum's phase continuous */
} tone_state;

/* interleaved float samples: a quiet 330 Hz hum (gaps are audible) and a 1 kHz beep at every
   full second of pts - the moment the picture flashes */
void tone_fill(tone_state *state, float *out, uint32_t frames, int channels, uint32_t rate,
               uint64_t pts_us, uint64_t epoch_us);

#endif
