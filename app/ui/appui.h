/* appui.h - the app's screens, with a controller.
 *
 * The installer calls these instead of sceMsgDialog. Each returns the decision
 * the old dialog returned, and -1 (or 0 for a confirm) when the screen cannot
 * be used, so the caller can fall back to the system dialogs unchanged.
 */
#ifndef PS4UI_APPUI_H
#define PS4UI_APPUI_H

#include "ui.h"

enum {
	APPUI_CANCEL = -1,
	APPUI_INSTALL = 0,
	APPUI_START,
	APPUI_VRAM,
	APPUI_AUTOLOAD,
	APPUI_CHROME,
	APPUI_RECOVERY
};

/* 1 when the screens, the fonts and a pad are all available. */
int appui_init(void);
void appui_shutdown(void);

/* Home menu: returns an APPUI_ action, or APPUI_CANCEL. */
int appui_home(ui_state *st);

/* 0 = PS4 drive, 1 = USB drive, -1 = cancelled. */
int appui_drive_choice(ui_state *st);

/* 1 = install, 0 = cancelled. */
int appui_install_confirm(ui_state *st);

/* 1 = remove, 0 = cancelled. */
int appui_remove_confirm(ui_state *st);

/* The chosen VRAM size in MB, or -1 when cancelled. */
int appui_graphics_memory(ui_state *st);

/* Progress: draw the current state. Caller updates st between calls. */
void appui_progress(const ui_state *st);

#endif
