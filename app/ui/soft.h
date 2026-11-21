/* soft.h - the software renderer behind ui_draw.
 *
 * The console has no GPU drawing path in this project, and the SDL port the
 * toolchain ships draws in software anyway (its own sample uses
 * SDL_CreateSoftwareRenderer). So the app rasterises its 1920x1080 frame on the
 * CPU and blits it. The preview tool uses this exact code, which is why a
 * rendered PNG can be trusted to look like the console.
 *
 * Only whole screens are drawn, and only when something changes: a Jaguar core
 * would not enjoy 60 frames a second of this, and it never has to.
 */
#ifndef PS4UI_SOFT_H
#define PS4UI_SOFT_H

#include "ui.h"

typedef struct ui_soft ui_soft;

ui_soft *ui_soft_new(void);
void ui_soft_free(ui_soft *s);

/* Loads DejaVuSans.ttf, DejaVuSans-Bold.ttf and DejaVuSansMono.ttf from dir.
 * Returns 1 on success; on failure the screens must not be used. */
int ui_soft_fonts(ui_soft *s, const char *dir);

uint8_t *ui_soft_pixels(ui_soft *s);

/* Paints the background gradient. Then ui_soft_iface() gives the ui_draw the
 * screen functions talk to. */
void ui_soft_background(ui_soft *s);
void ui_soft_iface(ui_soft *s, ui_draw *d);

#endif
