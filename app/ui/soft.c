/* soft.c - the CPU rasteriser for ui_draw, and the only place stb_truetype is
 * compiled. See soft.h for why the console draws this way. */
#include "soft.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

struct ui_soft {
	uint8_t *px;
	stbtt_fontinfo font[3];
	unsigned char *data[3];
};

/* ---------------------------------------------------------------- canvas */
static void blend(uint8_t *fb, int x, int y, ui_colour c, float a)
{
	uint8_t *p;
	float sa;

	if (x < 0 || y < 0 || x >= UI_W || y >= UI_H || a <= 0.0f)
		return;
	if (a > 1.0f)
		a = 1.0f;

	p = fb + ((size_t)y * UI_W + (size_t)x) * 4;
	sa = a * (c.a / 255.0f);
	p[0] = (uint8_t)lroundf(p[0] + ((float)c.r - p[0]) * sa);
	p[1] = (uint8_t)lroundf(p[1] + ((float)c.g - p[1]) * sa);
	p[2] = (uint8_t)lroundf(p[2] + ((float)c.b - p[2]) * sa);
	p[3] = 255;
}

static float clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float sdf_rrect(float px, float py, float x, float y, float w, float h,
                       float r)
{
	float hw = w * 0.5f, hh = h * 0.5f;
	float cx = x + hw, cy = y + hh;
	float qx, qy, ax, ay;

	if (r > hw)
		r = hw;
	if (r > hh)
		r = hh;

	qx = fabsf(px - cx) - (hw - r);
	qy = fabsf(py - cy) - (hh - r);
	ax = qx > 0.0f ? qx : 0.0f;
	ay = qy > 0.0f ? qy : 0.0f;

	return sqrtf(ax * ax + ay * ay) + (qx > qy ? qx : qy) - r;
}

/* ---------------------------------------------------------------- shapes */
static void b_fill_rrect(void *ctx, ui_rect r, int radius, ui_colour c)
{
	uint8_t *fb = ((ui_soft *)ctx)->px;
	float rad = (float)radius;
	int x, y;

	for (y = r.y - 1; y <= r.y + r.h; y++)
		for (x = r.x - 1; x <= r.x + r.w; x++)
			blend(fb, x, y, c,
			      clamp01(0.5f - sdf_rrect(x + 0.5f, y + 0.5f, (float)r.x,
			                               (float)r.y, (float)r.w, (float)r.h,
			                               rad)));
}

static void b_stroke_rrect(void *ctx, ui_rect r, int radius, ui_colour c,
                           int width)
{
	uint8_t *fb = ((ui_soft *)ctx)->px;
	float rad = (float)radius;
	float half = width * 0.5f;
	int x, y;

	for (y = r.y - width - 2; y <= r.y + r.h + width + 2; y++)
		for (x = r.x - width - 2; x <= r.x + r.w + width + 2; x++) {
			float d = sdf_rrect(x + 0.5f, y + 0.5f, (float)r.x, (float)r.y,
			                    (float)r.w, (float)r.h, rad);
			blend(fb, x, y, c, clamp01(0.5f + half - fabsf(d)));
		}
}

static void b_disc(void *ctx, int cx, int cy, int radius, ui_colour c)
{
	uint8_t *fb = ((ui_soft *)ctx)->px;
	int x, y;

	for (y = cy - radius - 1; y <= cy + radius + 1; y++)
		for (x = cx - radius - 1; x <= cx + radius + 1; x++) {
			float dx = (float)(x + 0.5f) - (float)cx;
			float dy = (float)(y + 0.5f) - (float)cy;
			blend(fb, x, y, c,
			      clamp01(0.5f - (sqrtf(dx * dx + dy * dy) - (float)radius)));
		}
}

static void b_ring(void *ctx, int cx, int cy, int radius, ui_colour c, int width)
{
	uint8_t *fb = ((ui_soft *)ctx)->px;
	float half = width * 0.5f;
	int x, y;

	for (y = cy - radius - width - 1; y <= cy + radius + width + 1; y++)
		for (x = cx - radius - width - 1; x <= cx + radius + width + 1; x++) {
			float dx = (float)(x + 0.5f) - (float)cx;
			float dy = (float)(y + 0.5f) - (float)cy;
			float d = fabsf(sqrtf(dx * dx + dy * dy) - (float)radius) - half;
			blend(fb, x, y, c, clamp01(0.5f - d));
		}
}

static void b_line(void *ctx, int x0, int y0, int x1, int y1, ui_colour c,
                   int width)
{
	uint8_t *fb = ((ui_soft *)ctx)->px;
	float vx = (float)(x1 - x0), vy = (float)(y1 - y0);
	float len2 = vx * vx + vy * vy;
	float half = width * 0.5f;
	int minx = (x0 < x1 ? x0 : x1) - width - 2;
	int maxx = (x0 > x1 ? x0 : x1) + width + 2;
	int miny = (y0 < y1 ? y0 : y1) - width - 2;
	int maxy = (y0 > y1 ? y0 : y1) + width + 2;
	int x, y;

	for (y = miny; y <= maxy; y++)
		for (x = minx; x <= maxx; x++) {
			float px = (float)x + 0.5f - (float)x0;
			float py = (float)y + 0.5f - (float)y0;
			float t = len2 > 0.0f ? (px * vx + py * vy) / len2 : 0.0f;
			float d;

			if (t < 0.0f)
				t = 0.0f;
			if (t > 1.0f)
				t = 1.0f;
			px -= vx * t;
			py -= vy * t;
			d = sqrtf(px * px + py * py) - half;
			blend(fb, x, y, c, clamp01(0.5f - d));
		}
}

static int inside(const int *xy, int n, float px, float py)
{
	int i, hit = 0;

	for (i = 0; i < n; i++) {
		int j = (i + 1) % n;
		float ax = (float)xy[i * 2], ay = (float)xy[i * 2 + 1];
		float bx = (float)xy[j * 2], by = (float)xy[j * 2 + 1];

		if (((ay > py) != (by > py)) &&
		    (px < (bx - ax) * (py - ay) / (by - ay) + ax))
			hit = !hit;
	}
	return hit;
}

static void b_poly(void *ctx, const int *xy, int n, ui_colour c)
{
	uint8_t *fb = ((ui_soft *)ctx)->px;
	int i, x, y;
	int minx = UI_W, maxx = -1, miny = UI_H, maxy = -1;

	for (i = 0; i < n; i++) {
		if (xy[i * 2] < minx) minx = xy[i * 2];
		if (xy[i * 2] > maxx) maxx = xy[i * 2];
		if (xy[i * 2 + 1] < miny) miny = xy[i * 2 + 1];
		if (xy[i * 2 + 1] > maxy) maxy = xy[i * 2 + 1];
	}

	for (y = miny; y <= maxy; y++)
		for (x = minx; x <= maxx; x++) {
			int hits = 0, sy, sx;

			for (sy = 0; sy < 2; sy++)
				for (sx = 0; sx < 2; sx++)
					hits += inside(xy, n, (float)x + 0.25f + 0.5f * (float)sx,
					               (float)y + 0.25f + 0.5f * (float)sy);
			if (hits)
				blend(fb, x, y, c, 0.25f * (float)hits);
		}
}

/* ---------------------------------------------------------------- text */
static int utf8_next(const unsigned char **pp)
{
	const unsigned char *p = *pp;
	int cp;

	if (p[0] < 0x80) {
		*pp = p + 1;
		return p[0];
	}
	if ((p[0] & 0xE0) == 0xC0) {
		cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
		*pp = p + 2;
		return cp;
	}
	if ((p[0] & 0xF0) == 0xE0) {
		cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
		*pp = p + 3;
		return cp;
	}
	cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
	     ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
	*pp = p + 4;
	return cp;
}

static void b_text(void *ctx, int x, int y, const char *s, int px, ui_font f,
                   ui_colour c)
{
	ui_soft *b = (ui_soft *)ctx;
	const stbtt_fontinfo *info = &b->font[f];
	const unsigned char *p = (const unsigned char *)s;
	float scale = stbtt_ScaleForMappingEmToPixels(info, (float)px);
	int asc = 0, desc = 0, gap = 0;
	float pen = (float)x;
	float baseline;

	stbtt_GetFontVMetrics(info, &asc, &desc, &gap);
	baseline = (float)y + (float)asc * scale;

	while (*p) {
		int cp = utf8_next(&p);
		int gi = stbtt_FindGlyphIndex(info, cp);
		int adv = 0, lsb = 0, gw = 0, gh = 0, gx = 0, gy = 0;
		unsigned char *bm;

		stbtt_GetGlyphHMetrics(info, gi, &adv, &lsb);
		bm = stbtt_GetGlyphBitmap(info, scale, scale, gi, &gw, &gh, &gx, &gy);
		if (bm && gw > 0 && gh > 0) {
			int bx = (int)lroundf(pen) + gx;
			int by = (int)lroundf(baseline) + gy;
			int iy, ix;

			for (iy = 0; iy < gh; iy++)
				for (ix = 0; ix < gw; ix++) {
					unsigned char a = bm[iy * gw + ix];
					if (a)
						blend(b->px, bx + ix, by + iy, c, a / 255.0f);
				}
			stbtt_FreeBitmap(bm, NULL);
		}
		pen += (float)adv * scale;
	}
}

static int b_text_w(void *ctx, const char *s, int px, ui_font f)
{
	ui_soft *b = (ui_soft *)ctx;
	const stbtt_fontinfo *info = &b->font[f];
	const unsigned char *p = (const unsigned char *)s;
	float scale = stbtt_ScaleForMappingEmToPixels(info, (float)px);
	float w = 0.0f;

	while (*p) {
		int cp = utf8_next(&p);
		int gi = stbtt_FindGlyphIndex(info, cp);
		int adv = 0, lsb = 0;

		stbtt_GetGlyphHMetrics(info, gi, &adv, &lsb);
		w += (float)adv * scale;
	}
	return (int)lroundf(w);
}

/* ---------------------------------------------------------------- api */
ui_soft *ui_soft_new(void)
{
	ui_soft *s = (ui_soft *)calloc(1, sizeof(*s));

	if (!s)
		return NULL;
	s->px = (uint8_t *)calloc((size_t)UI_W * UI_H, 4);
	if (!s->px) {
		free(s);
		return NULL;
	}
	return s;
}

void ui_soft_free(ui_soft *s)
{
	int i;

	if (!s)
		return;
	for (i = 0; i < 3; i++)
		free(s->data[i]);
	free(s->px);
	free(s);
}

uint8_t *ui_soft_pixels(ui_soft *s)
{
	return s->px;
}

static int load_font(ui_soft *s, const char *dir, const char *name, int slot)
{
	char path[512];
	FILE *f;
	long size;
	unsigned char *data;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0) {
		fclose(f);
		return 0;
	}
	data = (unsigned char *)malloc((size_t)size);
	if (!data) {
		fclose(f);
		return 0;
	}
	if (fread(data, 1, (size_t)size, f) != (size_t)size) {
		free(data);
		fclose(f);
		return 0;
	}
	fclose(f);
	if (!stbtt_InitFont(&s->font[slot], data,
	                    stbtt_GetFontOffsetForIndex(data, 0))) {
		free(data);
		return 0;
	}
	s->data[slot] = data;
	return 1;
}

int ui_soft_fonts(ui_soft *s, const char *dir)
{
	if (!load_font(s, dir, "DejaVuSans.ttf", UI_FONT_REG))
		return 0;
	if (!load_font(s, dir, "DejaVuSans-Bold.ttf", UI_FONT_BOLD))
		return 0;
	if (!load_font(s, dir, "DejaVuSansMono.ttf", UI_FONT_MONO))
		return 0;
	return 1;
}

void ui_soft_iface(ui_soft *s, ui_draw *d)
{
	d->ctx = s;
	d->fill_rrect = b_fill_rrect;
	d->stroke_rrect = b_stroke_rrect;
	d->line = b_line;
	d->disc = b_disc;
	d->ring = b_ring;
	d->poly = b_poly;
	d->text = b_text;
	d->text_w = b_text_w;
}

void ui_soft_background(ui_soft *s)
{
	static const float lx[2] = { 1625.0f, 380.0f };
	static const float ly[2] = { 1050.0f, 60.0f };
	static const float lr[2] = { 520.0f, 700.0f };
	static const float la[2] = { 0.30f, 0.10f };
	static const float lc[2][3] = { { 26, 74, 96 }, { 120, 160, 200 } };
	int x, y, i;

	for (y = 0; y < UI_H; y++) {
		float t = (float)y / (float)(UI_H - 1);
		float r = 10.0f + (17.0f - 10.0f) * t;
		float g = 16.0f + (28.0f - 16.0f) * t;
		float b = 24.0f + (43.0f - 24.0f) * t;

		for (x = 0; x < UI_W; x++) {
			float rr = r, gg = g, bb = b;
			uint8_t *p = s->px + ((size_t)y * UI_W + (size_t)x) * 4;

			for (i = 0; i < 2; i++) {
				float dx = (float)x - lx[i];
				float dy = (float)y - ly[i];
				float a = la[i] * expf(-((dx * dx + dy * dy) /
				                       (lr[i] * lr[i])) * 1.6f);
				rr += lc[i][0] * a;
				gg += lc[i][1] * a;
				bb += lc[i][2] * a;
			}
			p[0] = (uint8_t)(rr > 255.0f ? 255.0f : rr);
			p[1] = (uint8_t)(gg > 255.0f ? 255.0f : gg);
			p[2] = (uint8_t)(bb > 255.0f ? 255.0f : bb);
			p[3] = 255;
		}
	}
}
