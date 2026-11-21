/* ui.c - the app's screens, laid out and drawn through ui_draw.
 *
 * Nothing here knows about the PS4. Everything is geometry in 1080p pixels and
 * strings, so tools/ui-preview can render exactly these screens to PNG.
 *
 * The proportions follow the approved mockups in artwork/ (artwork/mockui.py
 * holds the type research). Where the mockups printed a path or a sentence that
 * the code does not actually do, the code here says what the code does.
 */
#include "ui.h"
#include "status.h"

#include <stdio.h>
#include <string.h>

const ui_colour UI_INK        = { 243, 246, 250, 255 };
const ui_colour UI_MUTED      = { 150, 165, 182, 255 };
const ui_colour UI_DIM        = { 108, 124, 142, 255 };
const ui_colour UI_FAINT      = {  92, 108, 128, 255 };
const ui_colour UI_ACCENT     = {  72, 163, 240, 255 };
const ui_colour UI_ACCENT_DIM = {  46, 110, 172, 255 };
const ui_colour UI_CARD       = {  16,  27,  41, 255 };
const ui_colour UI_CARD_FOCUS = {  23,  39,  58, 255 };
const ui_colour UI_EDGE       = {  30,  44,  62, 255 };
const ui_colour UI_AMBER      = { 232, 178,  96, 255 };
const ui_colour UI_RED        = { 224,  62,  47, 255 };

/* What install_run() really does, in order: copy the boot files, adopt a real
 * loader payload if one is on the stick, reserve the image, write the manifest,
 * wait for the drive to settle. */
const char *const ui_install_steps[] = {
	"Copy boot files", "Adopt loader", "Reserve image", "Write manifest",
	"Settle drive"
};
const int ui_install_step_count = 5;

const unsigned ui_vram_sizes[] = { 1024, 2048, 3072, 4096, 5120, 6144 };
const int ui_vram_count = 6;

void ui_state_from_status(ui_state *st, const struct ps4_status *s)
{
	memset(st, 0, sizeof(*st));
	st->installed = s->linux_installed;
	st->linux_on_usb = s->linux_on_usb;
	st->size_gb = s->linux_size_gb;
	st->free_gb = s->free_bytes >> 30;
	st->keep_gb = s->keep_bytes >> 30;
	st->chromeos_found = s->chromeos_found;
	st->chromeos_on_usb = s->chromeos_on_usb;
	st->chromeos_bytes = s->chromeos_bytes;
	st->chromeos_image_bytes = s->chromeos_image_bytes;
}

/* ---------------------------------------------------------------- wrappers */
static int tw(const ui_draw *d, const char *s, int px, ui_font f)
{
	return d->text_w(d->ctx, s, px, f);
}

static void tx(const ui_draw *d, int x, int y, const char *s, int px, ui_font f,
               ui_colour c)
{
	d->text(d->ctx, x, y, s, px, f, c);
}

static void upper(char *dst, const char *src, int cap)
{
	int i = 0;
	for (; src[i] && i < cap - 1; i++)
		dst[i] = (src[i] >= 'a' && src[i] <= 'z')
		         ? (char)(src[i] - 32) : src[i];
	dst[i] = 0;
}

/* Sizes read from disk: GB with one decimal, or MB when that is what it is. */
static void size_str(char *buf, size_t cap, unsigned long long bytes)
{
	if (bytes >= (1ULL << 30))
		snprintf(buf, cap, "%.1f GB", (double)bytes / 1073741824.0);
	else
		snprintf(buf, cap, "%llu MB", bytes >> 20);
}

/* ---------------------------------------------------------------- text flow */
#define UI_LINES 12
#define UI_WORD  256

typedef struct {
	const char *p;
	int n;
} ui_span;

/* Greedy wrap, measured through the backend so console and preview agree. */
static int split_lines(const ui_draw *d, const char *text, int px, ui_font f,
                       int maxw, ui_span *out, int max)
{
	char buf[UI_WORD];
	int count = 0;
	const char *p = text;

	while (*p && count < max) {
		const char *line;
		const char *q;
		int best = 0;

		while (*p == ' ')
			p++;
		if (!*p)
			break;

		line = p;
		q = p;
		while (*q) {
			int n;

			while (*q && *q != ' ')
				q++;
			n = (int)(q - line);
			if (n > UI_WORD - 1)
				n = UI_WORD - 1;
			memcpy(buf, line, (size_t)n);
			buf[n] = 0;
			if (tw(d, buf, px, f) > maxw)
				break;
			best = (int)(q - line);
			while (*q == ' ')
				q++;
		}
		if (best == 0) {           /* one word wider than the line */
			while (line[best] && line[best] != ' ' && best < UI_WORD - 1)
				best++;
		}
		out[count].p = line;
		out[count].n = best;
		count++;
		p = line + best;
	}
	return count;
}

static void draw_lines(const ui_draw *d, int x, int y, int step, int px,
                       ui_font f, ui_colour c, const ui_span *lines, int n)
{
	char buf[UI_WORD];
	int i, k;

	for (i = 0; i < n; i++) {
		k = lines[i].n;
		if (k > UI_WORD - 1)
			k = UI_WORD - 1;
		memcpy(buf, lines[i].p, (size_t)k);
		buf[k] = 0;
		tx(d, x, y + i * step, buf, px, f, c);
	}
}

/* Draws a paragraph and returns the y after its last line. */
static int paragraph(const ui_draw *d, int x, int y, int maxw, int step, int px,
                     ui_font f, ui_colour c, const char *text)
{
	ui_span lines[UI_LINES];
	int n = split_lines(d, text, px, f, maxw, lines, UI_LINES);

	draw_lines(d, x, y, step, px, f, c, lines, n);
	return y + n * step;
}

/* ---------------------------------------------------------------- pieces */
typedef struct {
	const char *text;
	ui_colour colour;
} ui_pill_spec;

static int pill(const ui_draw *d, int right_x, int y, const char *text,
                ui_colour colour, int h)
{
	/* UI_META, not a size up: with two pills the row has to clear the app name
	 * even when both say where something lives. */
	int w = tw(d, text, UI_META, UI_FONT_BOLD) + 70;
	int x = right_x - w;
	ui_rect r = { x, y, w, h };
	ui_colour bg = { 20, 32, 47, 255 };

	d->fill_rrect(d->ctx, r, h / 2, bg);
	d->stroke_rrect(d->ctx, r, h / 2, UI_EDGE, 2);
	d->disc(d->ctx, x + 26, y + h / 2, 6, colour);
	tx(d, x + 42, y + (h - UI_META - 2) / 2 - 1, text, UI_META, UI_FONT_BOLD,
	   (ui_colour){ 206, 218, 231, 255 });
	return x;
}

static int chip(const ui_draw *d, int right_x, int y, const char *text,
                ui_colour colour, int px)
{
	int w = tw(d, text, px, UI_FONT_BOLD) + 30;
	int h = px + 18;
	int x = right_x - w;
	ui_rect r = { x, y, w, h };
	ui_colour bg = { (uint8_t)(colour.r / 5), (uint8_t)(colour.g / 5),
	                 (uint8_t)(colour.b / 5), 255 };

	d->fill_rrect(d->ctx, r, h / 2, bg);
	d->stroke_rrect(d->ctx, r, h / 2, colour, 1);
	tx(d, x + 15, y + 8, text, px, UI_FONT_BOLD, colour);
	return w;
}

static void card(const ui_draw *d, ui_rect box, ui_colour fill,
                 ui_colour edge, int width, int radius)
{
	d->fill_rrect(d->ctx, box, radius, fill);
	d->stroke_rrect(d->ctx, box, radius, edge, width);
}

/* A focus halo. Three cheap passes instead of a blur: the console can afford
 * them and the preview matches. */
static void glow(const ui_draw *d, ui_rect box, ui_colour colour, int radius,
                 int width)
{
	ui_rect far_box = { box.x - 9, box.y - 9, box.w + 18, box.h + 18 };
	ui_rect near_box = { box.x - 4, box.y - 4, box.w + 8, box.h + 8 };
	ui_colour far = { colour.r, colour.g, colour.b, 26 };
	ui_colour near = { colour.r, colour.g, colour.b, 52 };

	d->stroke_rrect(d->ctx, far_box, radius + 9, far, width + 8);
	d->stroke_rrect(d->ctx, near_box, radius + 4, near, width + 2);
}

/* Controller glyphs. CIRCLE_O is the PS4 back button: a circle in a circle. */
enum { G_CIRCLE_X, G_CIRCLE_O, G_LR, G_CHECK };

static void glyph(const ui_draw *d, int kind, int cx, int cy, int r,
                  ui_colour colour, int weight)
{
	ui_colour ring_col = { (uint8_t)(colour.r * 66 / 100),
	                       (uint8_t)(colour.g * 66 / 100),
	                       (uint8_t)(colour.b * 66 / 100), colour.a };

	if (kind == G_CIRCLE_X || kind == G_CIRCLE_O)
		d->ring(d->ctx, cx, cy, r, ring_col, weight);

	if (kind == G_CIRCLE_X) {
		int k = r * 40 / 100;
		d->line(d->ctx, cx - k, cy - k, cx + k, cy + k, colour, weight);
		d->line(d->ctx, cx - k, cy + k, cx + k, cy - k, colour, weight);
	} else if (kind == G_CIRCLE_O) {
		int k = r * 47 / 100;
		d->ring(d->ctx, cx, cy, k, colour, weight);
	} else if (kind == G_LR) {
		int left[6] = { cx - r, cy, cx - r + 8, cy - 7, cx - r + 8, cy + 7 };
		int right[6] = { cx + r, cy, cx + r - 8, cy - 7, cx + r - 8, cy + 7 };

		d->line(d->ctx, cx - r, cy, cx + r, cy, colour, weight);
		d->poly(d->ctx, left, 3, colour);
		d->poly(d->ctx, right, 3, colour);
	} else if (kind == G_CHECK) {
		d->line(d->ctx, cx - 7, cy, cx - 1, cy + 7, colour, weight);
		d->line(d->ctx, cx - 1, cy + 7, cx + 8, cy - 7, colour, weight);
	}
}

/* kind: 0 primary, 1 danger, 2 ghost */
static void button(const ui_draw *d, int x, int y, int w, int h, const char *text,
                   int kind, int glyph_kind)
{
	ui_rect r = { x, y, w, h };
	ui_colour tcol, gcol;
	int gx = x + 32;

	if (kind == 0) {
		d->fill_rrect(d->ctx, r, 16, UI_ACCENT);
		tcol = (ui_colour){ 10, 20, 32, 255 };
		gcol = tcol;
	} else if (kind == 1) {
		d->fill_rrect(d->ctx, r, 16, (ui_colour){ 180, 44, 34, 255 });
		tcol = (ui_colour){ 255, 236, 233, 255 };
		gcol = tcol;
	} else {
		d->fill_rrect(d->ctx, r, 16, (ui_colour){ 20, 31, 46, 255 });
		d->stroke_rrect(d->ctx, r, 16, (ui_colour){ 46, 62, 82, 255 }, 2);
		tcol = (ui_colour){ 208, 219, 231, 255 };
		gcol = (ui_colour){ 150, 165, 182, 255 };
	}

	if (glyph_kind >= 0) {
		glyph(d, glyph_kind, gx + 16, y + h / 2, 16, gcol, 3);
		gx += 56;
	}
	tx(d, gx, y + (h - UI_VALUE) / 2 - 3, text, UI_VALUE,
	   kind == 2 ? UI_FONT_REG : UI_FONT_BOLD, tcol);
}

typedef struct {
	int glyph;
	const char *text;
} ui_hint;

static void footer(const ui_draw *d, const ui_hint *items, int n,
                   const char *right, const char *note, ui_colour note_colour)
{
	int gx = UI_M;
	int gy = 996;
	int i;

	d->line(d->ctx, UI_M, 944, UI_W - UI_M, 944, (ui_colour){ 26, 39, 55, 255 }, 2);

	if (note)
		tx(d, UI_M, gy - 15, note, UI_SECONDARY, UI_FONT_REG, note_colour);

	for (i = 0; i < n; i++) {
		int w;

		glyph(d, items[i].glyph, gx + 17, gy, 17,
		      (ui_colour){ 206, 218, 231, 255 }, 3);
		tx(d, gx + 50, gy - 15, items[i].text, UI_META + 2, UI_FONT_REG,
		   (ui_colour){ 168, 182, 197, 255 });
		w = tw(d, items[i].text, UI_META + 2, UI_FONT_REG);
		gx += 50 + w + 52;
	}

	if (right) {
		int w = tw(d, right, UI_META, UI_FONT_REG);
		tx(d, UI_W - UI_M - w, gy - 13, right, UI_META, UI_FONT_REG,
		   (ui_colour){ 96, 110, 126, 255 });
	}
}

static void header(const ui_draw *d, const ui_pill_spec *pills, int np,
                   const char *subtitle)
{
	const int icon = 88;
	ui_rect slot = { UI_M, 58, icon, icon };
	int tx0 = UI_M + icon + 28;
	int rx = UI_W - UI_M;
	int i;

	/* The package tile is a texture; the interface has no texture op yet, so
	 * the slot shows the app mark's diagonal. */
	d->fill_rrect(d->ctx, slot, 20, (ui_colour){ 20, 33, 48, 255 });
	d->stroke_rrect(d->ctx, slot, 20, (ui_colour){ 34, 49, 68, 255 }, 2);
	d->line(d->ctx, UI_M + 26, 58 + 62, UI_M + 62, 58 + 26, UI_ACCENT, 6);
	d->disc(d->ctx, UI_M + 30, 58 + 26, 4, UI_ACCENT);

	tx(d, tx0, 50, "PS4 XFCE Linux", UI_TITLE, UI_FONT_BOLD, UI_INK);
	tx(d, tx0 + 3, 128, subtitle ? subtitle
	                             : "Debian 13  \xC2\xB7  XFCE  \xC2\xB7  kernel 7.3.4-baikal",
	   UI_SECONDARY, UI_FONT_REG, UI_DIM);

	for (i = np - 1; i >= 0; i--)
		rx = pill(d, rx, 74, pills[i].text, pills[i].colour, 52) - 16;
}

static void breadcrumb(const ui_draw *d, const char *const *trail, int n)
{
	char buf[64];
	int x = UI_M;
	int i;

	for (i = 0; i < n; i++) {
		int last = i == n - 1;
		int w;

		upper(buf, trail[i], (int)sizeof(buf));
		tx(d, x, 296, buf, UI_META, UI_FONT_BOLD,
		   last ? (ui_colour){ 150, 168, 190, 255 } : UI_FAINT);
		w = tw(d, buf, UI_META, UI_FONT_BOLD);
		x += w + 12;
		if (!last) {
			tx(d, x, 294, "\xE2\x80\xBA", UI_META, UI_FONT_BOLD,
			   (ui_colour){ 70, 84, 100, 255 });
			x += 26;
		}
	}
}

/* ---------------------------------------------------------------- icons */
enum {
	IC_PLAY, IC_CHIP, IC_LOOP, IC_CHROME, IC_FLAG, IC_DOWN, IC_DISK, IC_USB
};

static void icon(const ui_draw *d, int kind, int x, int y, int s, ui_colour c)
{
	int w = s * 75 / 1000;
	int i;

	if (w < 3)
		w = 3;

	switch (kind) {
	case IC_PLAY: {
		int pts[6] = { x + s * 30 / 100, y + s * 16 / 100,
		               x + s * 30 / 100, y + s * 84 / 100,
		               x + s * 84 / 100, y + s * 50 / 100 };
		d->poly(d->ctx, pts, 3, c);
		break;
	}
	case IC_CHIP: {
		ui_rect outer = { x + s * 22 / 100, y + s * 22 / 100,
		                  s * 56 / 100, s * 56 / 100 };
		ui_rect inner = { x + s * 40 / 100, y + s * 40 / 100,
		                  s * 20 / 100, s * 20 / 100 };
		d->stroke_rrect(d->ctx, outer, s * 10 / 100, c, w);
		d->fill_rrect(d->ctx, inner, s * 4 / 100, c);
		for (i = 34; i <= 66; i += 16) {
			d->line(d->ctx, x + s * i / 100, y + s * 6 / 100,
			        x + s * i / 100, y + s * 20 / 100, c, w);
			d->line(d->ctx, x + s * i / 100, y + s * 80 / 100,
			        x + s * i / 100, y + s * 94 / 100, c, w);
		}
		break;
	}
	case IC_LOOP: {
		int pts[6] = { x + s * 88 / 100, y + s * 22 / 100,
		               x + s * 88 / 100, y + s * 48 / 100,
		               x + s * 66 / 100, y + s * 35 / 100 };
		d->ring(d->ctx, x + s / 2, y + s / 2, s * 34 / 100, c, w);
		d->poly(d->ctx, pts, 3, c);
		break;
	}
	case IC_CHROME: {
		int cx = x + s / 2, cy = y + s / 2;
		/* integer trigonometry, so this stays libm-free */
		static const int dx[2] = { 90, -82 };
		static const int dy[2] = { -42, 82 };

		d->ring(d->ctx, cx, cy, s * 37 / 100, c, w);
		d->ring(d->ctx, cx, cy, s * 16 / 100, c, w);
		for (i = 0; i < 2; i++)
			d->line(d->ctx, cx + s * 17 / 100 * dx[i] / 100,
			        cy + s * 17 / 100 * dy[i] / 100,
			        cx + s * 37 / 100 * dx[i] / 100,
			        cy + s * 37 / 100 * dy[i] / 100, c, w);
		break;
	}
	case IC_FLAG: {
		int pts[6] = { x + s * 32 / 100, y + s * 14 / 100,
		               x + s * 84 / 100, y + s * 30 / 100,
		               x + s * 32 / 100, y + s * 48 / 100 };
		d->line(d->ctx, x + s * 30 / 100, y + s * 10 / 100,
		        x + s * 30 / 100, y + s * 90 / 100, c, w);
		d->poly(d->ctx, pts, 3, c);
		break;
	}
	case IC_DOWN: {
		int pts[6] = { x + s * 26 / 100, y + s * 52 / 100,
		               x + s * 74 / 100, y + s * 52 / 100,
		               x + s * 50 / 100, y + s * 86 / 100 };
		d->line(d->ctx, x + s * 50 / 100, y + s * 14 / 100,
		        x + s * 50 / 100, y + s * 60 / 100, c, w);
		d->poly(d->ctx, pts, 3, c);
		d->line(d->ctx, x + s * 18 / 100, y + s * 94 / 100,
		        x + s * 82 / 100, y + s * 94 / 100, c, w);
		break;
	}
	case IC_DISK: {
		ui_rect slab = { x + s * 10 / 100, y + s * 24 / 100,
		                 s * 80 / 100, s * 52 / 100 };
		d->stroke_rrect(d->ctx, slab, s * 10 / 100, c, w);
		d->line(d->ctx, x + s * 22 / 100, y + s * 58 / 100,
		        x + s * 78 / 100, y + s * 58 / 100, c, w);
		d->disc(d->ctx, x + s * 71 / 100, y + s * 675 / 1000, s * 5 / 100, c);
		break;
	}
	case IC_USB: {
		ui_rect plug = { x + s * 10 / 100, y + s * 37 / 100,
		                 s * 24 / 100, s * 26 / 100 };
		ui_rect body = { x + s * 32 / 100, y + s * 20 / 100,
		                 s * 58 / 100, s * 60 / 100 };
		d->stroke_rrect(d->ctx, plug, 0, c, w);
		d->stroke_rrect(d->ctx, body, s * 10 / 100, c, w);
		d->line(d->ctx, x + s * 52 / 100, y + s * 40 / 100,
		        x + s * 52 / 100, y + s * 60 / 100, c, w);
		break;
	}
	default:
		break;
	}
}

/* ---------------------------------------------------------------- home */
static const char *const ENTRY_NAME[] = {
	"Install Linux", "Start Linux", "Graphics memory", "Autoload",
	"Remove ChromeOS", "Recovery"
};
static const int ENTRY_ICON[] = {
	IC_DOWN, IC_PLAY, IC_CHIP, IC_LOOP, IC_CHROME, IC_FLAG
};
static const char *const ENTRY_DESC[] = {
	"Writes the kernel, its boot files and the image. Games and saves stay put.",
	"Boots the installed system through the loader payload.",
	"Give the GPU 1024, 2048, 3072 or 4096 MB of the 8 GB pool.",
	"See what is armed, remove the entry, choose boot behaviour.",
	"Opens the separate app, which asks twice before deleting.",
	"Writes the rescue flag to a USB stick, recovers a boot loop."
};
static const int ORDER_NOT_INSTALLED[5] = { 0, 2, 3, 4, 5 };
static const int ORDER_INSTALLED[5] = { 1, 2, 3, 4, 5 };

void ui_home(const ui_draw *d, const ui_state *st)
{
	enum { CW = 326, GAP = 24, TOP = 344, CH = 396, GROW = 16 };
	char chrome[64];
	char installed[64];
	ui_pill_spec pills[2];
	const int *order = st->installed ? ORDER_INSTALLED : ORDER_NOT_INSTALLED;
	int np = 0;
	int i;

	if (st->installed) {
		if (st->linux_on_usb)
			snprintf(installed, sizeof(installed), "LINUX ON USB DRIVE");
		else if (st->size_gb > 0)
			snprintf(installed, sizeof(installed),
			         "LINUX ON PS4 DRIVE  \xC2\xB7  %d GB", st->size_gb);
		else
			snprintf(installed, sizeof(installed), "LINUX ON PS4 DRIVE");
		pills[np].text = installed;
		pills[np].colour = UI_ACCENT;
		np++;
	} else {
		pills[np].text = "LINUX NOT INSTALLED";
		pills[np].colour = UI_RED;
		np++;
	}
	if (st->chromeos_found) {
		char csize[32];

		size_str(csize, sizeof(csize), st->chromeos_bytes);
		snprintf(chrome, sizeof(chrome), "CHROMEOS ON %s  \xC2\xB7  %s",
		         st->chromeos_on_usb ? "USB DRIVE" : "PS4 DRIVE", csize);
		pills[np].text = chrome;
		pills[np].colour = UI_AMBER;
		np++;
	}

	header(d, pills, np, NULL);
	tx(d, UI_M + 2, 292, "MAIN MENU", UI_META, UI_FONT_BOLD, UI_FAINT);

	for (i = 0; i < 5; i++) {
		int key = order[i];
		int focus = i == st->focus;
		int x = UI_M + i * (CW + GAP);
		int y = focus ? TOP - GROW : TOP;
		int h = focus ? CH + 2 * GROW : CH;
		ui_rect box = { x, y, CW, h };
		ui_span lines[UI_LINES];
		int n, j, ly, iw;

		if (focus)
			glow(d, box, UI_ACCENT, 22, 7);
		card(d, box, focus ? UI_CARD_FOCUS : UI_CARD,
		     focus ? UI_ACCENT_DIM : UI_EDGE, focus ? 3 : 2, 22);

		if (key == 4 && st->chromeos_found)
			chip(d, x + CW - 22, y + 22, "FOUND", UI_AMBER, UI_META - 4);

		iw = focus ? 148 : 128;
		icon(d, ENTRY_ICON[key], x + (CW - iw) / 2, y + 84, iw,
		     focus ? UI_ACCENT : (ui_colour){ 108, 124, 144, 255 });

		n = split_lines(d, ENTRY_NAME[key], UI_CARD_TITLE, UI_FONT_BOLD, CW - 52,
		                lines, UI_LINES);
		ly = y + h - 64 - (n - 1) * 42;
		for (j = 0; j < n; j++) {
			char buf[UI_WORD];
			int k = lines[j].n;
			int w;

			memcpy(buf, lines[j].p, (size_t)k);
			buf[k] = 0;
			w = tw(d, buf, UI_CARD_TITLE, UI_FONT_BOLD);
			tx(d, x + (CW - w) / 2, ly, buf, UI_CARD_TITLE, UI_FONT_BOLD,
			   focus ? UI_INK : (ui_colour){ 176, 190, 205, 255 });
			ly += 42;
		}
	}

	{
		char buf[64];
		int key = order[st->focus];
		const char *desc = ENTRY_DESC[key];

		/* Say where the thing this entry is about actually is, using the scan. */
		if (key == 1 && st->installed)
			desc = st->linux_on_usb
			     ? "Boots the installed system from the USB drive."
			     : "Boots the installed system from the PS4 drive.";
		else if (key == 4 && st->chromeos_found)
			desc = st->chromeos_on_usb
			     ? "ChromeOS was found on the USB drive. The separate app "
			       "deletes it after two questions."
			     : "ChromeOS was found on the PS4 drive. The separate app "
			       "deletes it after two questions.";

		upper(buf, ENTRY_NAME[key], (int)sizeof(buf));
		tx(d, UI_M + 2, 784, buf, UI_META + 2, UI_FONT_BOLD, UI_ACCENT);
		paragraph(d, UI_M, 820, 1180, 40, UI_BODY, UI_FONT_REG,
		          (ui_colour){ 196, 208, 221, 255 }, desc);
	}

	{
		ui_hint hints[3] = { { G_CIRCLE_X, "Select" }, { G_LR, "Change" },
		                     { G_CIRCLE_O, "Back" } };
		footer(d, hints, 3, "v1.0.0", NULL, UI_AMBER);
	}
}

/* ---------------------------------------------------------------- drive choice */
void ui_drive_choice(const ui_draw *d, const ui_state *st)
{
	const char *trail[2] = { "install linux", "where linux lives" };
	ui_pill_spec pill_spec;
	enum { Y0 = 452, CH = 420, CW = 848 };
	int x, pad = 36, ind = 76;
	ui_rect left = { UI_M, Y0, CW, CH };
	ui_rect right = { UI_M + CW + 32, Y0, CW, CH };
	char buf[64];

	pill_spec.text = "NOT INSTALLED";
	pill_spec.colour = UI_ACCENT;
	header(d, &pill_spec, 1, NULL);
	breadcrumb(d, trail, 2);

	tx(d, UI_M, 340, "Where should Linux be installed?", UI_HEADING, UI_FONT_BOLD,
	   UI_INK);

	/* ---- internal */
	glow(d, left, UI_ACCENT, 22, 7);
	card(d, left, UI_CARD_FOCUS, UI_ACCENT_DIM, 3, 20);

	x = left.x;
	icon(d, IC_DISK, x + pad, Y0 + 30, 56, UI_ACCENT);
	tx(d, x + pad + ind, Y0 + 32, "INTERNAL STORAGE", UI_META, UI_FONT_BOLD, UI_FAINT);
	tx(d, x + pad + ind, Y0 + 62, "PS4 drive", UI_CARD_TITLE, UI_FONT_BOLD, UI_INK);
	chip(d, left.x + CW - 24, Y0 + 30, "RECOMMENDED", UI_ACCENT, UI_META);

	paragraph(d, x + pad + ind, Y0 + 126, CW - 2 * pad - ind, 40, UI_BODY,
	          UI_FONT_REG, UI_MUTED,
	          "Linux lives in one large file on the PS4 drive. Games, saves "
	          "and the PS4 system stay exactly as they are.");

	d->line(d->ctx, x + pad, Y0 + 262, x + CW - pad, Y0 + 262,
	        (ui_colour){ 38, 54, 74, 255 }, 2);

	tx(d, x + pad, Y0 + 286, "FREE SPACE NOW", UI_META, UI_FONT_BOLD, UI_FAINT);
	if (st->free_gb)
		snprintf(buf, sizeof(buf), "%llu GB", st->free_gb);
	else
		snprintf(buf, sizeof(buf), "\xE2\x80\x94"); /* could not be read */
	tx(d, x + pad, Y0 + 314, buf, UI_HEADING - 8, UI_FONT_BOLD, UI_INK);

	tx(d, x + pad + 400, Y0 + 286, "ALWAYS KEPT FOR THE PS4", UI_META, UI_FONT_BOLD,
	   UI_FAINT);
	snprintf(buf, sizeof(buf), "%llu GB", st->keep_gb);
	tx(d, x + pad + 400, Y0 + 314, buf, UI_HEADING - 8, UI_FONT_BOLD, UI_INK);

	/* ---- external */
	card(d, right, UI_CARD, UI_EDGE, 2, 20);
	x = right.x;
	icon(d, IC_USB, x + pad, Y0 + 30, 56, UI_MUTED);
	tx(d, x + pad + ind, Y0 + 32, "EXTERNAL STORAGE", UI_META, UI_FONT_BOLD, UI_FAINT);
	tx(d, x + pad + ind, Y0 + 62, "USB drive", UI_CARD_TITLE, UI_FONT_BOLD, UI_INK);
	chip(d, right.x + CW - 24, Y0 + 30, "ERASED", UI_RED, UI_META);

	paragraph(d, x + pad + ind, Y0 + 126, CW - 2 * pad - ind, 40, UI_BODY,
	          UI_FONT_REG, UI_MUTED,
	          "Linux lives on the drive itself. Faster on an SSD, and nothing "
	          "on the PS4 drive is touched.");

	{
		ui_rect warn = { x + 24, Y0 + 236, CW - 48, 136 };
		d->fill_rrect(d->ctx, warn, 16, (ui_colour){ 33, 24, 16, 255 });
		d->stroke_rrect(d->ctx, warn, 16, (ui_colour){ 74, 58, 32, 255 }, 2);
		d->fill_rrect(d->ctx, (ui_rect){ warn.x, warn.y + 18, 8, 100 }, 4,
		              UI_AMBER);
		paragraph(d, warn.x + 30, warn.y + 22, warn.w - 40, 36, UI_SECONDARY,
		          UI_FONT_REG, (ui_colour){ 232, 214, 186, 255 },
		          "The drive is ERASED completely. You pick which one on the "
		          "TV after the restart, with a USB keyboard.");
	}

	{
		ui_hint hints[2] = { { G_CIRCLE_X, "Choose" }, { G_CIRCLE_O, "Back" } };
		footer(d, hints, 2, "v1.0.0", NULL, UI_AMBER);
	}
}

/* ---------------------------------------------------------------- graphics memory */
void ui_graphics_memory(const ui_draw *d, const ui_state *st)
{
	const char *trail[2] = { "main menu", "graphics memory" };
	static const char *const NOTES[6] = {
		"recommended until Linux is installed",
		"the usual desktop choice",
		"3 GB to the GPU, 5 GB left for programs",
		"4 GB to the GPU, 4 GB left for programs",
		"5 GB to the GPU, 3 GB left for programs",
		"6 GB to the GPU, 2 GB left for programs"
	};
	ui_pill_spec pill_spec;
	char cur[64], buf[64];
	int i, gpu = st->vram_mb, sys_mb = 8192 - st->vram_mb;

	snprintf(cur, sizeof(cur), "CURRENT  %d MB", st->vram_mb);
	pill_spec.text = cur;
	pill_spec.colour = UI_ACCENT;
	header(d, &pill_spec, 1, NULL);
	breadcrumb(d, trail, 2);

	for (i = 0; i < ui_vram_count; i++) {
		int y = 344 + i * (84 + 12);
		int focus = i == st->focus;
		ui_rect box = { UI_M, y, 820, 84 };
		int nw;

		if (focus)
			glow(d, box, UI_ACCENT, 20, 7);
		card(d, box, focus ? UI_CARD_FOCUS : UI_CARD,
		     focus ? UI_ACCENT_DIM : UI_EDGE, focus ? 3 : 2, 20);

		snprintf(buf, sizeof(buf), "%u", ui_vram_sizes[i]);
		tx(d, UI_M + 32, y + 10, buf, UI_NUMBER, UI_FONT_BOLD,
		   focus ? UI_INK : (ui_colour){ 206, 217, 229, 255 });
		nw = tw(d, buf, UI_NUMBER, UI_FONT_BOLD);
		tx(d, UI_M + 38 + nw, y + 34, "MB", UI_META + 2, UI_FONT_REG, UI_DIM);
		tx(d, UI_M + 276, y + 26, NOTES[i], UI_SECONDARY, UI_FONT_REG,
		   focus ? UI_MUTED : UI_DIM);

		if ((unsigned)st->vram_mb == ui_vram_sizes[i])
			chip(d, UI_M + 820 - 24, y + 20, "CURRENT", UI_ACCENT, UI_META);
	}

	{
		ui_rect panel = { 948, 344, 876, 550 };
		ui_rect bar = { 980, 508, 812, 34 };
		int gpu_w = 812 * gpu / 8192;
		int lx = 980;
		int w;
		char l1[64], l2[64];

		card(d, panel, UI_CARD, UI_EDGE, 2, 20);
		tx(d, 980, 372, "HOW THE 8 GB POOL IS SPLIT", UI_META, UI_FONT_BOLD, UI_FAINT);
		snprintf(buf, sizeof(buf), "%d MB", gpu);
		tx(d, 980, 404, buf, UI_NUMBER, UI_FONT_BOLD, UI_INK);
		w = tw(d, buf, UI_NUMBER, UI_FONT_BOLD);
		tx(d, 980 + w + 16, 428, "for the graphics chip", UI_SECONDARY,
		   UI_FONT_REG, UI_MUTED);

		d->fill_rrect(d->ctx, bar, 17, (ui_colour){ 26, 40, 56, 255 });
		d->fill_rrect(d->ctx, (ui_rect){ bar.x, bar.y, gpu_w, bar.h }, 17,
		              UI_ACCENT);

		snprintf(l1, sizeof(l1), "GPU  %d MB", gpu);
		snprintf(l2, sizeof(l2), "SYSTEM  %d MB", sys_mb);
		tx(d, lx, 556, l1, UI_META + 2, UI_FONT_BOLD, UI_ACCENT);
		w = tw(d, l2, UI_META + 2, UI_FONT_BOLD);
		tx(d, lx + bar.w - w, 556, l2, UI_META + 2, UI_FONT_BOLD, UI_MUTED);

		paragraph(d, 980, 614, 800, 38, UI_SECONDARY, UI_FONT_REG, UI_MUTED,
		          "One 8 GB pool is shared by both. Whatever the GPU takes, "
		          "Linux cannot use. Changing this writes vram.txt next to the "
		          "kernel; the next boot reads it, so no different payload is "
		          "needed.");

		d->line(d->ctx, 980, 790, 1792, 790, (ui_colour){ 26, 39, 55, 255 }, 2);
		tx(d, 980, 812, "WRITES", UI_META, UI_FONT_BOLD, UI_FAINT);
		tx(d, 1090, 808, "/data/linux/boot/vram.txt", UI_MONO, UI_FONT_MONO,
		   (ui_colour){ 186, 200, 216, 255 });
	}

	{
		ui_hint hints[2] = { { G_CIRCLE_X, "Apply" }, { G_CIRCLE_O, "Back" } };
		footer(d, hints, 2, "v1.0.0", NULL, UI_AMBER);
	}
}

/* ---------------------------------------------------------------- install confirm */
void ui_install_confirm(const ui_draw *d, const ui_state *st)
{
	const char *trail[3] = { "main menu", "install linux", "confirm" };
	ui_pill_spec pill_spec;
	char size[32], reserve[96], target[96];
	const char *labels[5] = { "TARGET", "SIZE", "RESERVE", "KERNEL", "IMAGE" };
	const char *values[5];
	const char *bullets[4] = {
		"Writes only its own files: the image, /data/linux/boot and the payload.",
		"ChromeOS stays where it is. The remover app is what deletes it.",
		"Do not power off the console while it runs.",
		"1024 MB of VRAM is recommended for the first boot."
	};
	int i;

	snprintf(target, sizeof(target), "Internal storage  \xC2\xB7  /user/app/ps4linux.img");
	snprintf(size, sizeof(size), "%d GB", st->size_gb > 0 ? st->size_gb : 32);
	snprintf(reserve, sizeof(reserve),
	         "%llu GB  \xE2\x80\x94  always kept free for the PS4 system",
	         st->keep_gb);
	values[0] = target;
	values[1] = size;
	values[2] = reserve;
	values[3] = "7.3.4-baikal";
	values[4] = "Debian 13  \xC2\xB7  XFCE";

	pill_spec.text = "READY";
	pill_spec.colour = UI_ACCENT;
	header(d, &pill_spec, 1, NULL);
	breadcrumb(d, trail, 3);

	{
		ui_rect box = { UI_M, 344, 1000, 416 };
		int y = 424;

		card(d, box, UI_CARD, UI_EDGE, 2, 20);
		tx(d, UI_M + 34, 372, "CONFIGURATION", UI_META, UI_FONT_BOLD, UI_FAINT);
		for (i = 0; i < 5; i++) {
			tx(d, UI_M + 34, y + 7, labels[i], UI_META, UI_FONT_BOLD, UI_FAINT);
			tx(d, UI_M + 236, y, values[i], UI_VALUE, UI_FONT_REG,
			   (ui_colour){ 206, 217, 229, 255 });
			y += 66;
		}
	}

	{
		ui_rect box = { 1130, 344, 694, 416 };
		int y = 430;

		card(d, box, UI_CARD, (ui_colour){ 58, 47, 28, 255 }, 2, 20);
		d->fill_rrect(d->ctx, (ui_rect){ 1130, 366, 8, 372 }, 4, UI_AMBER);
		tx(d, 1168, 370, "Before you continue", UI_BODY, UI_FONT_BOLD, UI_INK);
		for (i = 0; i < 4; i++) {
			ui_span lines[UI_LINES];
			int n = split_lines(d, bullets[i], UI_SECONDARY, UI_FONT_REG, 600,
			                    lines, UI_LINES);

			tx(d, 1168, y, "\xE2\x80\x94", UI_SECONDARY, UI_FONT_REG, UI_AMBER);
			draw_lines(d, 1204, y, 36, UI_SECONDARY, UI_FONT_REG, UI_MUTED,
			           lines, n);
			y += 36 * n + 20;
		}
	}

	button(d, UI_M, 800, 360, 76, "Install Linux", 0, G_CIRCLE_X);
	button(d, UI_M + 384, 800, 220, 76, "Cancel", 2, G_CIRCLE_O);

	{
		ui_hint hints[2] = { { G_CIRCLE_X, "Install" }, { G_CIRCLE_O, "Cancel" } };
		footer(d, hints, 2, "v1.0.0", NULL, UI_AMBER);
	}
}

/* ---------------------------------------------------------------- installing */
void ui_installing(const ui_draw *d, const ui_state *st)
{
	const char *trail[3] = { "main menu", "install linux", "installing" };
	ui_pill_spec pill_spec;
	char pct[16], msg[160], right[64];
	int i, pw;

	pill_spec.text = "INSTALLING";
	pill_spec.colour = UI_ACCENT;
	header(d, &pill_spec, 1, NULL);
	breadcrumb(d, trail, 3);

	snprintf(pct, sizeof(pct), "%d", st->percent);
	tx(d, UI_M - 8, 344, pct, UI_HUGE, UI_FONT_BOLD, UI_ACCENT);
	pw = tw(d, pct, UI_HUGE, UI_FONT_BOLD);
	tx(d, UI_M + pw - 4, 396, "%", UI_HEADING + 8, UI_FONT_BOLD, UI_ACCENT);
	tx(d, UI_M + pw + 62, 404, "complete", UI_BODY, UI_FONT_REG, UI_MUTED);

	{
		int barw = UI_W - 2 * UI_M;
		ui_rect back = { UI_M, 552, barw, 18 };
		ui_rect fill_ = { UI_M, 552, barw * st->percent / 100, 18 };

		d->fill_rrect(d->ctx, back, 9, (ui_colour){ 26, 40, 56, 255 });
		d->fill_rrect(d->ctx, fill_, 9, UI_ACCENT);
	}

	/* The same screen serves the copy, the reserve and the install itself, so the
	 * line under the bar names the step that is running. */
	snprintf(msg, sizeof(msg), "%s  \xE2\x80\x94  %d%% of the %d GB image",
	         ui_install_steps[st->step], st->percent,
	         st->size_gb > 0 ? st->size_gb : 32);
	tx(d, UI_M, 592, msg, UI_SECONDARY, UI_FONT_REG, (ui_colour){ 176, 190, 205, 255 });

	if (st->minutes_left > 0) {
		int w;

		snprintf(right, sizeof(right), "\xE2\x89\x88 %d min left", st->minutes_left);
		w = tw(d, right, UI_SECONDARY, UI_FONT_REG);
		tx(d, UI_W - UI_M - w, 592, right, UI_SECONDARY, UI_FONT_REG, UI_DIM);
	}

	if (st->step >= 2)
		tx(d, UI_M, 648,
		   "This step cannot be cancelled. It is writing to the console's drive.",
		   UI_SECONDARY, UI_FONT_REG, UI_AMBER);

	{
		int step_w = (UI_W - 2 * UI_M) / ui_install_step_count;

		for (i = 0; i < ui_install_step_count; i++) {
			int x = UI_M + i * step_w;
			int cx = x + 16, cy = 722;

			if (i < st->step) {
				d->disc(d->ctx, cx, cy, 16, UI_ACCENT);
				glyph(d, G_CHECK, cx, cy, 16, (ui_colour){ 10, 20, 32, 255 }, 4);
			} else if (i == st->step) {
				d->ring(d->ctx, cx, cy, 16, UI_ACCENT, 4);
				d->disc(d->ctx, cx, cy, 6, UI_ACCENT);
			} else {
				d->ring(d->ctx, cx, cy, 15, (ui_colour){ 56, 72, 92, 255 }, 2);
			}
			tx(d, x + 44, cy - 16, ui_install_steps[i], UI_META + 2, UI_FONT_REG,
			   i == st->step ? UI_INK
			                 : (i < st->step ? (ui_colour){ 206, 217, 229, 255 }
			                                 : UI_DIM));
		}
	}

	footer(d, NULL, 0, "Elapsed 02:14", "Do not power off the console.",
	       UI_AMBER);
}

/* ---------------------------------------------------------------- remove confirm */
void ui_remove_confirm(const ui_draw *d, const ui_state *st)
{
	const char *trail[2] = { "remove chromeos", "confirm" };
	ui_pill_spec pill_spec;
	char chrome[64], total[32];
	const char *paths[4] = {
		"/user/app/chromeos.img", "\xE2\x80\xA6/boot/chromeos.img.xz",
		"\xE2\x80\xA6/boot/ps4-layer.sqfs", "\xE2\x80\xA6/boot + 2 more files"
	};
	const char *sizes[4] = { NULL, "812 MB", "1.6 GB", "9 MB" };
	int i;

	{
		char csize[32];

		size_str(csize, sizeof(csize), st->chromeos_bytes);
		snprintf(chrome, sizeof(chrome), "CHROMEOS ON %s  \xC2\xB7  %s",
		         st->chromeos_on_usb ? "USB DRIVE" : "PS4 DRIVE", csize);
	}
	pill_spec.text = chrome;
	pill_spec.colour = UI_RED;
	header(d, &pill_spec, 1, NULL);
	breadcrumb(d, trail, 2);

	tx(d, UI_M, 340, "Delete the ChromeOS installation?", UI_HEADING, UI_FONT_BOLD,
	   UI_INK);
	paragraph(d, UI_M, 404, 1000, 36, UI_SECONDARY, UI_FONT_REG, UI_MUTED,
	          "This frees the space it uses. Your Linux install, your saves and "
	          "your settings are not touched.");

	{
		ui_rect box = { UI_M, 512, 1000, 364 };
		int y = 586;

		card(d, box, UI_CARD, (ui_colour){ 58, 32, 32, 255 }, 2, 20);
		tx(d, UI_M + 34, 538, "WHAT WILL BE REMOVED", UI_META, UI_FONT_BOLD, UI_FAINT);

		size_str(total, sizeof(total), st->chromeos_image_bytes);
		for (i = 0; i < 4; i++) {
			const char *size = i == 0 ? total : sizes[i];
			int w;

			tx(d, UI_M + 34, y, paths[i], UI_MONO, UI_FONT_MONO,
			   (ui_colour){ 190, 204, 219, 255 });
			w = tw(d, size, UI_MONO, UI_FONT_REG);
			tx(d, UI_M + 1000 - 34 - w, y, size, UI_MONO, UI_FONT_REG, UI_DIM);
			y += 68;
		}
	}

	{
		ui_rect box = { 1130, 512, 694, 364 };

		card(d, box, UI_CARD, (ui_colour){ 74, 34, 30, 255 }, 2, 20);
		d->fill_rrect(d->ctx, (ui_rect){ 1130, 534, 8, 320 }, 4, UI_RED);
		tx(d, 1168, 538, "SECOND CONFIRM", UI_META, UI_FONT_BOLD, UI_FAINT);
		paragraph(d, 1168, 578, 580, 36, UI_SECONDARY, UI_FONT_REG,
		          (ui_colour){ 226, 214, 210, 255 },
		          "You will be asked once more after this. Removal cannot be "
		          "undone.");
		button(d, 1168, 764, 240, 72, "Remove", 1, G_CIRCLE_X);
		button(d, 1432, 764, 210, 72, "Cancel", 2, G_CIRCLE_O);
	}

	{
		ui_hint hints[2] = { { G_CIRCLE_X, "Remove" }, { G_CIRCLE_O, "Cancel" } };
		footer(d, hints, 2, "v1.0.0", NULL, UI_AMBER);
	}
}
