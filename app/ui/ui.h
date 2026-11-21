/* ui.h - what the console UI is made of, with no graphics API in sight.
 *
 * Layout and drawing are separate. ui.c turns state into rectangles, lines and
 * strings; a backend only has to fill a rounded rectangle, stroke one, draw a
 * line, a disc, a ring, a polygon and a string. That is what lets the same code
 * paint PNGs on a PC (tools/ui-preview) and, later, the real screen through GNM
 * or OpenGL on the console.
 *
 * Sizes are 1080p pixels measured for a sofa, not a desk: body 30, card titles
 * 34, nothing below 22. The research behind those numbers is recorded in
 * artwork/mockui.py.
 */
#ifndef PS4UI_UI_H
#define PS4UI_UI_H

#include <stdint.h>

#define UI_W 1920
#define UI_H 1080
#define UI_M 96

typedef struct {
	int x, y, w, h;
} ui_rect;

typedef struct {
	uint8_t r, g, b, a;
} ui_colour;

/* The type scale, in pixels at 1080p. */
enum {
	UI_TITLE     = 60,   /* app name in the header */
	UI_HEADING   = 48,   /* the question a screen asks */
	UI_CARD_TITLE = 34,   /* card titles - read from the sofa */
	UI_BODY      = 30,   /* sentences */
	UI_VALUE     = 28,   /* list values, button labels */
	UI_SECONDARY = 26,   /* supporting sentences, warnings */
	UI_META      = 22,   /* uppercase labels, breadcrumbs, footer hints */
	UI_MONO      = 24,   /* paths and sizes */
	UI_NUMBER    = 56,   /* big row values */
	UI_HUGE      = 150   /* progress percentage */
};

typedef enum {
	UI_FONT_REG = 0,
	UI_FONT_BOLD = 1,
	UI_FONT_MONO = 2
} ui_font;

/* A backend. Every function must clip to the 1920x1080 canvas and blend alpha.
 * text() takes the TOP of the line (not the baseline) so layout code can add
 * line heights without knowing anything about font metrics. */
typedef struct ui_draw {
	void *ctx;

	void (*fill_rrect)(void *ctx, ui_rect r, int radius, ui_colour c);
	void (*stroke_rrect)(void *ctx, ui_rect r, int radius, ui_colour c,
	                     int width);
	void (*line)(void *ctx, int x0, int y0, int x1, int y1, ui_colour c,
	             int width);
	void (*disc)(void *ctx, int cx, int cy, int radius, ui_colour c);
	void (*ring)(void *ctx, int cx, int cy, int radius, ui_colour c,
	             int width);
	void (*poly)(void *ctx, const int *xy, int n, ui_colour c);

	void (*text)(void *ctx, int x, int y, const char *s, int px, ui_font font,
	             ui_colour c);
	int  (*text_w)(void *ctx, const char *s, int px, ui_font font);
} ui_draw;

extern const ui_colour UI_INK;
extern const ui_colour UI_MUTED;
extern const ui_colour UI_DIM;
extern const ui_colour UI_FAINT;
extern const ui_colour UI_ACCENT;
extern const ui_colour UI_ACCENT_DIM;
extern const ui_colour UI_CARD;
extern const ui_colour UI_CARD_FOCUS;
extern const ui_colour UI_EDGE;
extern const ui_colour UI_AMBER;
extern const ui_colour UI_RED;

/* The steps a real install runs, in order (installer/main.c install_run()).
 * The progress screen gets its labels from here, not from a drawing. */
extern const char *const ui_install_steps[];
extern const int ui_install_step_count;

/* The graphics memory sizes the app offers, in MB. The kernel may be given more
 * of the 8 GB pool than the desktop needs, so 5 and 6 GB are here too. */
extern const unsigned ui_vram_sizes[];
extern const int ui_vram_count;

typedef struct {
	int installed;               /* Linux is installed */
	int linux_on_usb;            /* ...on a USB drive rather than the PS4 drive */
	int chromeos_found;          /* the ChromeOS scan found an install */
	int chromeos_on_usb;         /* ...on a USB drive */
	unsigned long long chromeos_bytes;       /* ...how large it is in total */
	unsigned long long chromeos_image_bytes; /* ...and the disk image alone */
	unsigned long long free_gb;  /* free space on the PS4 drive */
	unsigned long long keep_gb;  /* always kept free for the PS4 */
	int size_gb;                 /* chosen image size */
	int vram_mb;
	int focus;                   /* focused card / row */
	int percent;
	int step;                    /* ui_install_steps index running now */
	unsigned long long written_gb;
	int minutes_left;
} ui_state;

struct ps4_status;

/* Fills in everything that exists on disk (install state, where it is, free
 * space, the ChromeOS scan). Values that belong to an operation in progress are
 * left at zero for the caller to set. */
void ui_state_from_status(ui_state *st, const struct ps4_status *s);

void ui_home(const ui_draw *d, const ui_state *st);
void ui_drive_choice(const ui_draw *d, const ui_state *st);
void ui_graphics_memory(const ui_draw *d, const ui_state *st);
void ui_install_confirm(const ui_draw *d, const ui_state *st);
void ui_installing(const ui_draw *d, const ui_state *st);
void ui_remove_confirm(const ui_draw *d, const ui_state *st);

#endif
