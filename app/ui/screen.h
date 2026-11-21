/* screen.h - the console's window and controller.
 *
 * SDL on the PS4 draws through the window surface in software (its own sample
 * does exactly this), so the app never needs a GPU drawing path: the frame is
 * rasterised by soft.c, blitted, and presented.
 */
#ifndef PS4UI_SCREEN_H
#define PS4UI_SCREEN_H

#include <stdint.h>

typedef struct ui_screen ui_screen;

/* 1 on success. On failure the caller keeps using the system dialogs. */
int ui_screen_open(ui_screen **out);
void ui_screen_close(ui_screen *s);

/* Copies the RGBA frame into the window and presents it. */
void ui_screen_present(ui_screen *s, const uint8_t *rgba);

/* Buttons held right now, as ORBIS_PAD_BUTTON_* bits; 0 if there is no pad. */
uint32_t ui_screen_buttons(ui_screen *s);

/* 0 when no controller could be opened: the screens cannot be driven then. */
int ui_screen_has_pad(ui_screen *s);

/* Sleeps for the given number of milliseconds (SDL_Delay on this build). */
void ui_screen_wait(int ms);

#endif
