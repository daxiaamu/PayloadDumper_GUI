#ifndef PD_TITLEBAR_H
#define PD_TITLEBAR_H
#include <windows.h>
void titlebar_init(HWND window, HWND update_button, HINSTANCE instance, UINT dpi);
BOOL titlebar_message(HWND window, UINT message, WPARAM wp, LPARAM lp, LRESULT *result);
int titlebar_height(void);
void titlebar_place_update(void);
void titlebar_set_dpi(UINT dpi);
void titlebar_destroy(void);
#endif
