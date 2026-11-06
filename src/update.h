#ifndef PD_UPDATE_H
#define PD_UPDATE_H
#include <windows.h>
#define WM_UPDATE_EVENT (WM_APP+30)
void updater_init(HWND owner,HINSTANCE instance,HWND button,const wchar_t *directory,UINT dpi);
void updater_check(BOOL manual);
void updater_event(WPARAM kind,LPARAM data);
void updater_task_running(BOOL running);
BOOL updater_busy(void);
void updater_stop(void);
void updater_destroy(void);
BOOL updater_draw_button(const DRAWITEMSTRUCT *item);
#endif
