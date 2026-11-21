/* screen.c - window and pad for the console build.
 *
 * The SDL2 port that ships with the toolchain presents through a window surface
 * (samples/SDL2 uses SDL_CreateSoftwareRenderer). Blitting our frame onto that
 * surface is the shortest path between the renderer that runs on a PC and the
 * screen in the living room.
 */
#include "screen.h"
#include "ui.h"

#include <orbis/Pad.h>
#include <orbis/UserService.h>

#include <SDL2/SDL.h>

#include <stdlib.h>
#include <string.h>

struct ui_screen {
	SDL_Window *win;
	SDL_Surface *win_surface;
	SDL_Surface *frame;
	uint8_t *pixels;
	int pad;
	int32_t user;
};

int ui_screen_open(ui_screen **out)
{
	ui_screen *s = (ui_screen *)calloc(1, sizeof(*s));

	if (!s)
		return 0;

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		free(s);
		return 0;
	}

	s->win = SDL_CreateWindow("PS4 XFCE Linux", SDL_WINDOWPOS_UNDEFINED,
	                          SDL_WINDOWPOS_UNDEFINED, UI_W, UI_H, 0);
	if (!s->win) {
		SDL_Quit();
		free(s);
		return 0;
	}

	s->win_surface = SDL_GetWindowSurface(s->win);
	s->pixels = (uint8_t *)calloc((size_t)UI_W * UI_H, 4);
	if (!s->win_surface || !s->pixels) {
		SDL_DestroyWindow(s->win);
		SDL_Quit();
		free(s->pixels);
		free(s);
		return 0;
	}

	s->frame = SDL_CreateRGBSurfaceFrom(s->pixels, UI_W, UI_H, 32, UI_W * 4,
	                                    0x000000FF, 0x0000FF00, 0x00FF0000,
	                                    0xFF000000);
	if (!s->frame) {
		SDL_DestroyWindow(s->win);
		SDL_Quit();
		free(s->pixels);
		free(s);
		return 0;
	}

	/* The pad is optional: without one the screens still draw, and the caller
	 * falls back to the system dialogs for input. */
	s->pad = -1;
	if (sceUserServiceInitialize(NULL) >= 0 &&
	    sceUserServiceGetInitialUser(&s->user) >= 0 && scePadInit() >= 0)
		s->pad = scePadOpen(s->user, 0, 0, NULL);

	*out = s;
	return 1;
}

void ui_screen_close(ui_screen *s)
{
	if (!s)
		return;
	if (s->pad >= 0)
		scePadClose(s->pad);
	if (s->frame)
		SDL_FreeSurface(s->frame);
	if (s->win)
		SDL_DestroyWindow(s->win);
	SDL_Quit();
	free(s->pixels);
	free(s);
}

void ui_screen_present(ui_screen *s, const uint8_t *rgba)
{
	memcpy(s->pixels, rgba, (size_t)UI_W * UI_H * 4);
	SDL_BlitSurface(s->frame, NULL, s->win_surface, NULL);
	SDL_UpdateWindowSurface(s->win);
	SDL_PumpEvents();
}

uint32_t ui_screen_buttons(ui_screen *s)
{
	OrbisPadData data;

	if (!s || s->pad < 0)
		return 0;
	memset(&data, 0, sizeof(data));
	if (scePadReadState(s->pad, &data) != 0)
		return 0;
	return data.buttons;
}

int ui_screen_has_pad(ui_screen *s)
{
	return s && s->pad >= 0;
}

void ui_screen_wait(int ms)
{
	SDL_Delay((Uint32)ms);
}
