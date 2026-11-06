/* DWM custom caption: the update action is a real accessible child button.
 * DWM continues to paint and hit-test the native minimize/maximize/close buttons.
 * On classic Windows without composition, retain the standard window frame. */
#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <wchar.h>
#include "titlebar.h"
#include "resource.h"

typedef struct {
    HWND window, update;
    HINSTANCE instance;
    HFONT font;
    UINT dpi;
    BOOL initialized, custom;
} Caption;
static Caption caption;
static int scale(int n) { return MulDiv(n,(int)caption.dpi,96); }
static int metric(int index) {
    typedef int (WINAPI *MetricDpi)(int,UINT);
    MetricDpi fn=(MetricDpi)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetSystemMetricsForDpi");
    return fn?fn(index,caption.dpi):GetSystemMetrics(index);
}
static BOOL composition(void) {
    BOOL enabled=FALSE;
    return SUCCEEDED(DwmIsCompositionEnabled(&enabled)) && enabled;
}
static RECT system_buttons(void) {
    RECT bounds={0},window_rect,client; POINT origin={0,0};
    GetWindowRect(caption.window,&window_rect); GetClientRect(caption.window,&client);
    ClientToScreen(caption.window,&origin);
    if (FAILED(DwmGetWindowAttribute(caption.window,DWMWA_CAPTION_BUTTON_BOUNDS,&bounds,sizeof(bounds))) ||
        bounds.right<=bounds.left || bounds.bottom<=bounds.top) {
        bounds.left=client.right-scale(3*46); bounds.right=client.right;
        bounds.top=0; bounds.bottom=scale(32);
    } else OffsetRect(&bounds,window_rect.left-origin.x,window_rect.top-origin.y);
    return bounds;
}
int titlebar_height(void) {
    if (!caption.initialized || !caption.custom) return 0;
    RECT bounds=system_buttons();
    int height=scale(32);
    if (bounds.bottom>height && bounds.bottom<scale(64)) height=bounds.bottom;
    return height;
}
void titlebar_place_update(void) {
    if (!caption.initialized || !caption.custom || !caption.update) return;
    RECT bounds=system_buttons(); int height=titlebar_height(),button_height=scale(26);
    MoveWindow(caption.update,bounds.left-scale(120),(height-button_height)/2,scale(112),button_height,TRUE);
}
static void extend_frame(void) {
    MARGINS margins={0,0,caption.custom?titlebar_height():0,0};
    DwmExtendFrameIntoClientArea(caption.window,&margins);
}
void titlebar_set_dpi(UINT dpi) {
    if (!caption.initialized) return;
    caption.dpi=dpi;
    if (caption.font) DeleteObject(caption.font);
    caption.font=CreateFontW(-scale(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    titlebar_place_update(); extend_frame();
    InvalidateRect(caption.window,NULL,TRUE);
}
void titlebar_init(HWND window,HWND update_button,HINSTANCE instance,UINT dpi) {
    caption.window=window; caption.update=update_button; caption.instance=instance;
    caption.initialized=TRUE; caption.custom=composition();
    BufferedPaintInit(); titlebar_set_dpi(dpi);
    SetWindowPos(window,NULL,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
}
static void paint_caption(HDC dc) {
    RECT area; GetClientRect(caption.window,&area); area.bottom=titlebar_height();
    HDC target=dc;
    BP_PAINTPARAMS parameters={0}; parameters.cbSize=sizeof(parameters); parameters.dwFlags=BPPF_ERASE;
    HPAINTBUFFER buffer=BeginBufferedPaint(dc,&area,BPBF_TOPDOWNDIB,&parameters,&target);
    if (!buffer) target=dc;
    FillRect(target,&area,(HBRUSH)GetStockObject(WHITE_BRUSH));
    int icon=scale(16),height=area.bottom;
    HICON app_icon=(HICON)LoadImageW(caption.instance,MAKEINTRESOURCEW(IDI_APP),IMAGE_ICON,icon,icon,LR_SHARED);
    if (app_icon) DrawIconEx(target,scale(12),(height-icon)/2,app_icon,icon,icon,0,NULL,DI_NORMAL);
    wchar_t text[256]; GetWindowTextW(caption.window,text,256);
    RECT text_area=area; text_area.left=scale(38);
    RECT button; GetWindowRect(caption.update,&button); MapWindowPoints(NULL,caption.window,(POINT*)&button,2);
    text_area.right=button.left-scale(12);
    HFONT previous=(HFONT)SelectObject(target,caption.font);
    SetBkMode(target,TRANSPARENT);
    SetTextColor(target,GetActiveWindow()==caption.window?RGB(32,38,44):RGB(102,108,114));
    DrawTextW(target,text,-1,&text_area,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    SelectObject(target,previous);
    if (buffer) { BufferedPaintSetAlpha(buffer,&area,255); EndBufferedPaint(buffer,TRUE); }
}
static LRESULT hit_test(LPARAM lp) {
    RECT rect; GetClientRect(caption.window,&rect);
    POINT point={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; ScreenToClient(caption.window,&point);
    int border_x=IsZoomed(caption.window)?0:metric(SM_CXSIZEFRAME)+metric(SM_CXPADDEDBORDER);
    int border_y=IsZoomed(caption.window)?0:metric(SM_CYSIZEFRAME)+metric(SM_CXPADDEDBORDER);
    BOOL left=point.x<border_x,right=point.x>=rect.right-border_x;
    BOOL top=point.y<border_y,bottom=point.y>=rect.bottom-border_y;
    if (top) return left?HTTOPLEFT:right?HTTOPRIGHT:HTTOP;
    if (bottom) return left?HTBOTTOMLEFT:right?HTBOTTOMRIGHT:HTBOTTOM;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    RECT update_rect; GetWindowRect(caption.update,&update_rect); MapWindowPoints(NULL,caption.window,(POINT*)&update_rect,2);
    if (PtInRect(&update_rect,point)) return HTCLIENT;
    if (point.y<titlebar_height()) {
        if (point.x>=scale(8) && point.x<scale(34)) return HTSYSMENU;
        return HTCAPTION;
    }
    return HTCLIENT;
}
BOOL titlebar_message(HWND window,UINT message,WPARAM wp,LPARAM lp,LRESULT *result) {
    if (!caption.initialized || window!=caption.window) return FALSE;
    if (message==WM_DWMCOMPOSITIONCHANGED) {
        caption.custom=composition(); extend_frame();
        SetWindowPos(window,NULL,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
        InvalidateRect(window,NULL,TRUE); return FALSE;
    }
    if (!caption.custom) return FALSE;
    /* DWM must receive leave messages too, so native hover states clear. */
    if (message==WM_NCHITTEST || message==WM_NCMOUSEMOVE || message==WM_NCMOUSELEAVE ||
        message==WM_NCLBUTTONDOWN || message==WM_NCLBUTTONUP || message==WM_NCLBUTTONDBLCLK) {
        if (DwmDefWindowProc(window,message,wp,lp,result)) return TRUE;
    }
    switch (message) {
    case WM_NCCALCSIZE:
        if (wp) {
            NCCALCSIZE_PARAMS *params=(NCCALCSIZE_PARAMS*)lp;
            if (IsZoomed(window)) {
                HMONITOR monitor=MonitorFromRect(&params->rgrc[0],MONITOR_DEFAULTTONEAREST);
                MONITORINFO info={sizeof(info),{0},{0},0};
                if (GetMonitorInfoW(monitor,&info)) params->rgrc[0]=info.rcWork;
            }
            *result=0; return TRUE;
        }
        break;
    case WM_NCHITTEST: *result=hit_test(lp); return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(window,&ps); paint_caption(dc); EndPaint(window,&ps);
        *result=0; return TRUE;
    }
    case WM_PRINTCLIENT: paint_caption((HDC)wp); *result=0; return TRUE;
    case WM_NCACTIVATE:
        *result=DefWindowProcW(window,message,wp,-1);
        InvalidateRect(window,NULL,FALSE); return TRUE;
    case WM_ACTIVATE: extend_frame(); InvalidateRect(window,NULL,FALSE); break;
    case WM_SETTEXT:
        *result=DefWindowProcW(window,message,wp,lp); InvalidateRect(window,NULL,FALSE); return TRUE;
    }
    return FALSE;
}
void titlebar_destroy(void) {
    if (!caption.initialized) return;
    if (caption.font) DeleteObject(caption.font);
    caption.font=NULL; caption.initialized=FALSE; BufferedPaintUnInit();
}
