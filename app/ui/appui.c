/* appui.c - controller handling and the decision loops for each screen.
 *
 * Every loop draws only when something changes: the software rasteriser is
 * fine for a menu that redraws on a button press, not for 60 frames a second.
 */
#include "appui.h"

#include "screen.h"
#include "soft.h"
#include "ui.h"

#include <orbis/Pad.h>

#include <stdio.h>
#include <string.h>

#define PAD_CROSS  ORBIS_PAD_BUTTON_CROSS
#define PAD_CIRCLE ORBIS_PAD_BUTTON_CIRCLE
#define PAD_LEFT   ORBIS_PAD_BUTTON_LEFT
#define PAD_RIGHT  ORBIS_PAD_BUTTON_RIGHT
#define PAD_UP     ORBIS_PAD_BUTTON_UP
#define PAD_DOWN   ORBIS_PAD_BUTTON_DOWN

static ui_screen *screen;
static ui_soft *soft;
static int ready;
static uint32_t previous;

#ifndef APPUI_FONT_DIR
#define APPUI_FONT_DIR "/app0/assets/fonts"
#endif

int appui_init(void)
{
	if (ready)
		return 1;

	if (!ui_screen_open(&screen))
		return 0;
	if (!ui_screen_has_pad(screen)) {
		/* Draw-able but not drivable: the dialogs are still the better answer. */
		ui_screen_close(screen);
		screen = NULL;
		return 0;
	}

	soft = ui_soft_new();
	if (!soft || !ui_soft_fonts(soft, APPUI_FONT_DIR)) {
		ui_soft_free(soft);
		soft = NULL;
		ui_screen_close(screen);
		screen = NULL;
		return 0;
	}

	ready = 1;
	previous = ui_screen_buttons(screen);
	return 1;
}

void appui_shutdown(void)
{
	if (!ready)
		return;
	ui_soft_free(soft);
	ui_screen_close(screen);
	soft = NULL;
	screen = NULL;
	ready = 0;
}

/* ---------------------------------------------------------------- helpers */
static void show(const ui_state *st,
                 void (*fn)(const ui_draw *, const ui_state *))
{
	ui_draw d;

	ui_soft_background(soft);
	ui_soft_iface(soft, &d);
	fn(&d, st);
	ui_screen_present(screen, ui_soft_pixels(soft));
}

static uint32_t pressed(void)
{
	uint32_t now = ui_screen_buttons(screen);
	uint32_t edge = now & ~previous;

	previous = now;
	return edge;
}

static void wait_input(void)
{
	ui_screen_wait(16);
}

/* Which action a home card stands for. ui_home's focus indices are stable:
 * with Linux installed the first card is Start, otherwise it is Install. */
static int home_action(int installed, int focus)
{
	int entry = installed ? focus + 1 : (focus == 0 ? 0 : focus + 1);

	switch (entry) {
	case 0:
		return APPUI_INSTALL;
	case 1:
		return APPUI_START;
	case 2:
		return APPUI_VRAM;
	case 3:
		return APPUI_AUTOLOAD;
	case 4:
		return APPUI_CHROME;
	default:
		return APPUI_RECOVERY;
	}
}

/* ---------------------------------------------------------------- screens */
int appui_home(ui_state *st)
{
	int focus = st->focus;

	for (;;) {
		uint32_t e;

		st->focus = focus;
		show(st, ui_home);
		e = pressed();

		if (e & PAD_LEFT)
			focus = (focus + 4) % 5;
		else if (e & PAD_RIGHT)
			focus = (focus + 1) % 5;
		else if (e & PAD_CROSS)
			return home_action(st->installed, focus);
		else if (e & PAD_CIRCLE)
			return APPUI_CANCEL;

		wait_input();
	}
}

int appui_drive_choice(ui_state *st)
{
	int focus = 0;

	for (;;) {
		uint32_t e;

		st->focus = focus;
		show(st, ui_drive_choice);
		e = pressed();

		if (e & (PAD_LEFT | PAD_RIGHT))
			focus = !focus;
		else if (e & PAD_CROSS)
			return focus;
		else if (e & PAD_CIRCLE)
			return -1;

		wait_input();
	}
}

int appui_install_confirm(ui_state *st)
{
	for (;;) {
		uint32_t e;

		show(st, ui_install_confirm);
		e = pressed();

		if (e & PAD_CROSS)
			return 1;
		if (e & PAD_CIRCLE)
			return 0;

		wait_input();
	}
}

int appui_remove_confirm(ui_state *st)
{
	for (;;) {
		uint32_t e;

		show(st, ui_remove_confirm);
		e = pressed();

		if (e & PAD_CROSS)
			return 1;
		if (e & PAD_CIRCLE)
			return 0;

		wait_input();
	}
}

int appui_graphics_memory(ui_state *st)
{
	int focus = st->focus;

	if (focus < 0 || focus >= ui_vram_count)
		focus = 0;
	for (;;) {
		uint32_t e;

		st->focus = focus;
		st->vram_mb = (int)ui_vram_sizes[focus];
		show(st, ui_graphics_memory);
		e = pressed();

		if (e & PAD_UP)
			focus = (focus + ui_vram_count - 1) % ui_vram_count;
		else if (e & PAD_DOWN)
			focus = (focus + 1) % ui_vram_count;
		else if (e & PAD_CROSS)
			return (int)ui_vram_sizes[focus];
		else if (e & PAD_CIRCLE)
			return -1;

		wait_input();
	}
}

void appui_progress(const ui_state *st)
{
	show(st, ui_installing);
}
