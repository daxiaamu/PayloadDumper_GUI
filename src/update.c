#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include "update.h"
#include "update_core.h"
#include <winhttp.h>
#include <commctrl.h>
#include <richedit.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

enum { CHECK_DONE=1, DOWNLOAD_PROGRESS, DOWNLOAD_DONE };
enum { U_SKIP=3101,U_LATER,U_DOWNLOAD };
typedef struct {
    uint64_t revision,version,forced,skipped;
    char manifest_hash[65];
} Preferences;
typedef struct { BOOL ok; UpPointer pointer; UpManifest manifest; } CheckResult;
typedef struct { UpManifest manifest; wchar_t directory[UP_PATH]; BOOL ok; } DownloadResult;
typedef struct {
    HWND owner,button,dialog,body,skip,later,download,label,bar;
    HINSTANCE instance;
    HFONT font,mono;
    UINT dpi;
    wchar_t directory[UP_PATH];
    HANDLE thread,stop;
    BOOL initialized,manual,checking,downloading,closing,task,known,ignored,deferred,dialog_shown,dismiss;
    Preferences prefs;
    UpManifest manifest;
} UpdateState;
static UpdateState u;
static const wchar_t *const sources[]={
    L"https://api.github.com/repos/daxiaamu/PayloadDumper_GUI/contents/updates/stable/latest.json?ref=main",
    L"https://raw.githubusercontent.com/daxiaamu/PayloadDumper_GUI/main/updates/stable/latest.json",
    L"https://cdn.jsdelivr.net/gh/daxiaamu/PayloadDumper_GUI@main/updates/stable/latest.json",
    L"https://fastly.jsdelivr.net/gh/daxiaamu/PayloadDumper_GUI@main/updates/stable/latest.json",
    L"https://gcore.jsdelivr.net/gh/daxiaamu/PayloadDumper_GUI@main/updates/stable/latest.json",
    L"https://testingcf.jsdelivr.net/gh/daxiaamu/PayloadDumper_GUI@main/updates/stable/latest.json",
    L"https://cdn.statically.io/gh/daxiaamu/PayloadDumper_GUI/main/updates/stable/latest.json"
};
#define SOURCE_COUNT (sizeof(sources)/sizeof(sources[0]))
static const wchar_t *const manifest_prefixes[]={
    L"https://raw.githubusercontent.com/daxiaamu/PayloadDumper_GUI/main/",
    L"https://cdn.jsdelivr.net/gh/daxiaamu/PayloadDumper_GUI@main/",
    L"https://fastly.jsdelivr.net/gh/daxiaamu/PayloadDumper_GUI@main/",
    L"https://gcore.jsdelivr.net/gh/daxiaamu/PayloadDumper_GUI@main/",
    L"https://testingcf.jsdelivr.net/gh/daxiaamu/PayloadDumper_GUI@main/",
    L"https://cdn.statically.io/gh/daxiaamu/PayloadDumper_GUI/main/"
};
static BOOL stopped(void) { return WaitForSingleObject(u.stop,0)==WAIT_OBJECT_0; }
static void save_preferences(void) {
#ifdef PD_UPDATE_TESTING
    return; /* Session tests never modify the real user's update settings. */
#endif
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Daxiaamu\\PayloadDumperGUI",0,NULL,0,KEY_SET_VALUE,NULL,&key,NULL)==ERROR_SUCCESS) {
        RegSetValueExW(key,L"UpdateStateV1",0,REG_BINARY,(const BYTE*)&u.prefs,sizeof(u.prefs)); RegCloseKey(key);
    }
}
/* No automatic redirect following: validate HTTPS again on every hop. */
static BOOL http_get(const wchar_t *initial,char **memory,DWORD cap,const wchar_t *file,DWORD expected) {
#ifdef PD_UPDATE_TESTING
    extern BOOL updater_test_http(const wchar_t*,char**,DWORD,const wchar_t*,DWORD);
    return updater_test_http(initial,memory,cap,file,expected);
#endif
    wchar_t url[UP_URL]; if (wcslen(initial)>=UP_URL) return FALSE;
    wcscpy(url,initial);
    BOOL ok=FALSE; HINTERNET session=NULL,connection=NULL,request=NULL;
    HANDLE output=INVALID_HANDLE_VALUE; char *data=NULL; DWORD total=0;
    session=WinHttpOpen(L"PayloadDumper/" APP_VERSION_WNAME,WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,NULL,NULL,0);
    if (!session) {
        /* Windows 7 fallback: honor the current user's explicitly configured
         * proxy instead of silently forcing direct connections. */
        WINHTTP_CURRENT_USER_IE_PROXY_CONFIG proxy={0};
        if (WinHttpGetIEProxyConfigForCurrentUser(&proxy) && proxy.lpszProxy)
            session=WinHttpOpen(L"PayloadDumper/" APP_VERSION_WNAME,WINHTTP_ACCESS_TYPE_NAMED_PROXY,proxy.lpszProxy,proxy.lpszProxyBypass,0);
        else session=WinHttpOpen(L"PayloadDumper/" APP_VERSION_WNAME,WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,NULL,NULL,0);
        if (proxy.lpszAutoConfigUrl) GlobalFree(proxy.lpszAutoConfigUrl);
        if (proxy.lpszProxy) GlobalFree(proxy.lpszProxy);
        if (proxy.lpszProxyBypass) GlobalFree(proxy.lpszProxyBypass);
    }
    if (!session) goto done;
    DWORD protocols=WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(session,WINHTTP_OPTION_SECURE_PROTOCOLS,&protocols,sizeof(protocols));
    for (int redirect=0;redirect<6 && !stopped();++redirect) {
        URL_COMPONENTS parts={0}; parts.dwStructSize=sizeof(parts);
        parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=(DWORD)-1;
        parts.dwUserNameLength=parts.dwPasswordLength=(DWORD)-1;
        if (!WinHttpCrackUrl(url,0,0,&parts) || parts.nScheme!=INTERNET_SCHEME_HTTPS || parts.nPort!=443 ||
            parts.dwUserNameLength || parts.dwPasswordLength || !parts.dwHostNameLength || parts.dwHostNameLength>=256) goto done;
        wchar_t host[256],path[UP_URL];
        wcsncpy(host,parts.lpszHostName,parts.dwHostNameLength); host[parts.dwHostNameLength]=0;
        if (parts.dwUrlPathLength+parts.dwExtraInfoLength+1>=UP_URL) goto done;
        wcsncpy(path,parts.lpszUrlPath,parts.dwUrlPathLength); path[parts.dwUrlPathLength]=0;
        if (parts.dwExtraInfoLength) wcsncat(path,parts.lpszExtraInfo,parts.dwExtraInfoLength);
        connection=WinHttpConnect(session,host,parts.nPort,0); if (!connection) goto done;
        request=WinHttpOpenRequest(connection,L"GET",path,NULL,NULL,NULL,WINHTTP_FLAG_SECURE); if (!request) goto done;
        DWORD disable=WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(request,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable));
        WinHttpSetTimeouts(request,3000,3000,5000,5000);
        const wchar_t *headers=L"Accept: application/vnd.github.raw+json\r\nCache-Control: no-cache\r\nPragma: no-cache\r\n";
        if (!WinHttpSendRequest(request,headers,(DWORD)-1L,NULL,0,0,0) || !WinHttpReceiveResponse(request,NULL)) goto done;
        DWORD status=0,size=sizeof(status);
        if (!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,NULL,&status,&size,NULL)) goto done;
        if (status>=300 && status<400) {
            size=sizeof(url);
            if (!WinHttpQueryHeaders(request,WINHTTP_QUERY_LOCATION,NULL,url,&size,NULL) || _wcsnicmp(url,L"https://",8)) goto done;
            WinHttpCloseHandle(request); request=NULL; WinHttpCloseHandle(connection); connection=NULL;
            continue;
        }
        if (status!=200) goto done;
        DWORD length=0; size=sizeof(length);
        if (WinHttpQueryHeaders(request,WINHTTP_QUERY_CONTENT_LENGTH|WINHTTP_QUERY_FLAG_NUMBER,NULL,&length,&size,NULL) &&
            (length>cap || (expected && length!=expected))) goto done;
        if (file) { output=CreateFileW(file,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL); if (output==INVALID_HANDLE_VALUE) goto done; }
        else { data=malloc((size_t)cap+1); if (!data) goto done; }
        ULONGLONG report=0;
        while (!stopped()) {
            char buffer[32768]; DWORD got=0;
            if (!WinHttpReadData(request,buffer,sizeof(buffer),&got)) goto done;
            if (!got) { ok=!expected || total==expected; break; }
            if (got>cap-total) goto done;
            if (file) {
                DWORD written=0;
                if (!WriteFile(output,buffer,got,&written,NULL) || written!=got) goto done;
            } else memcpy(data+total,buffer,got);
            total+=got;
            if (file && GetTickCount64()-report>=250) {
                DWORD pct=expected?(DWORD)((uint64_t)total*99/expected):0;
                PostMessageW(u.owner,WM_UPDATE_EVENT,DOWNLOAD_PROGRESS,(LPARAM)pct); report=GetTickCount64();
            }
        }
        break;
    }
    if (stopped()) ok=FALSE;
    if (ok && output!=INVALID_HANDLE_VALUE && !FlushFileBuffers(output)) ok=FALSE;
    if (ok && !file && memory) {
        if (memchr(data,0,total)) ok=FALSE;
        else { data[total]=0; *memory=data; data=NULL; }
    }
done:
    if (output!=INVALID_HANDLE_VALUE) CloseHandle(output);
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    if (!ok && file) DeleteFileW(file);
    free(data); return ok;
}
typedef struct { const wchar_t *url; UpPointer pointer; BOOL valid; } Fetch;
static unsigned __stdcall fetch_pointer(void *param) {
    Fetch *f=param; wchar_t url[UP_URL]; char suffix[64]; wchar_t wide[64];
    snprintf(suffix,sizeof(suffix),"%c_check=%llu",wcschr(f->url,L'?')?'&':'?',(unsigned long long)GetTickCount64());
    MultiByteToWideChar(CP_UTF8,0,suffix,-1,wide,64);
    wcscpy(url,f->url); wcscat(url,wide);
    char *json=NULL;
    if (http_get(url,&json,UP_MAX_META,NULL,0)) f->valid=up_parse_pointer(json,strlen(json),&f->pointer);
    free(json); return 0;
}
static BOOL load_manifest(const UpPointer *pointer,UpManifest *manifest) {
    const wchar_t *relative=pointer->manifest_url+wcslen(manifest_prefixes[0]);
    for (int i=-1;i<(int)(sizeof(manifest_prefixes)/sizeof(manifest_prefixes[0])) && !stopped();++i) {
        wchar_t url[UP_URL];
        if (i<0) {
            wcscpy(url,L"https://api.github.com/repos/daxiaamu/PayloadDumper_GUI/contents/");
            wcscat(url,relative); wcscat(url,L"?ref=main");
        } else { wcscpy(url,manifest_prefixes[i]); wcscat(url,relative); }
        char *json=NULL,hash[65];
        BOOL ok=http_get(url,&json,UP_MAX_META,NULL,0) && up_sha_memory(json,strlen(json),hash) &&
            !strcmp(hash,pointer->sha256) && up_parse_manifest(json,strlen(json),manifest) &&
            manifest->revision==pointer->revision && manifest->version>=u.prefs.version && manifest->forced>=u.prefs.forced;
        free(json); if (ok) return TRUE;
    }
    return FALSE;
}
static unsigned __stdcall check_thread(void *unused) {
    (void)unused;
    CheckResult *result=calloc(1,sizeof(*result));
    if (!result) { PostMessageW(u.owner,WM_UPDATE_EVENT,CHECK_DONE,0); return 0; }
    Fetch fetches[SOURCE_COUNT]; HANDLE threads[SOURCE_COUNT]; DWORD n=0;
    memset(fetches,0,sizeof(fetches));
    for (size_t i=0;i<SOURCE_COUNT;++i) {
        fetches[i].url=sources[i];
        HANDLE h=(HANDLE)_beginthreadex(NULL,0,fetch_pointer,&fetches[i],0,NULL);
        if (h) threads[n++]=h;
    }
    if (n) WaitForMultipleObjects(n,threads,TRUE,INFINITE);
    for (DWORD i=0;i<n;++i) CloseHandle(threads[i]);
    UpPointer pointers[SOURCE_COUNT]; BOOL valid[SOURCE_COUNT];
    for (size_t i=0;i<SOURCE_COUNT;++i) { pointers[i]=fetches[i].pointer; valid[i]=fetches[i].valid; }
    if (!stopped() && up_select_pointer(pointers,valid,SOURCE_COUNT,u.prefs.revision,u.prefs.manifest_hash,&result->pointer)) {
        result->ok=load_manifest(&result->pointer,&result->manifest);
    }
    PostMessageW(u.owner,WM_UPDATE_EVENT,CHECK_DONE,(LPARAM)result); return 0;
}
static unsigned __stdcall download_thread(void *param) {
    DownloadResult *r=param;
    if (up_new_temp(r->directory)) {
        wchar_t zip[UP_PATH]; up_join(zip,r->directory,L"update.zip");
        for (int i=0;i<r->manifest.url_count && !stopped();++i) {
            char hash[65];
            PostMessageW(u.owner,WM_UPDATE_EVENT,DOWNLOAD_PROGRESS,0);
            if (http_get(r->manifest.urls[i],NULL,r->manifest.size,zip,r->manifest.size) &&
                up_sha_file(zip,hash) && !strcmp(hash,r->manifest.sha256)) { r->ok=TRUE; break; }
            DeleteFileW(zip);
        }
        if (!r->ok) RemoveDirectoryW(r->directory);
    }
    PostMessageW(u.owner,WM_UPDATE_EVENT,DOWNLOAD_DONE,(LPARAM)r); return 0;
}
static int dp(int v) { return MulDiv(v,(int)u.dpi,96); }
static BOOL required(void) { return u.known && APP_VERSION_CODE<=u.manifest.forced; }
static void update_caption(void) {
    if (u.checking) SetWindowTextW(u.button,L"正在检查…");
    else SetWindowTextW(u.button,L"检查更新");
    InvalidateRect(u.button,NULL,TRUE);
}
static void rich_append(const wchar_t *s,DWORD effects,BOOL code) {
    SendMessageW(u.body,EM_SETSEL,(WPARAM)-1,(LPARAM)-1);
    CHARFORMAT2W fmt={0}; fmt.cbSize=sizeof(fmt); fmt.dwMask=CFM_BOLD|CFM_ITALIC|CFM_FACE;
    fmt.dwEffects=effects; wcscpy(fmt.szFaceName,code?L"Consolas":L"Microsoft YaHei UI");
    SendMessageW(u.body,EM_SETCHARFORMAT,SCF_SELECTION,(LPARAM)&fmt);
    SendMessageW(u.body,EM_REPLACESEL,FALSE,(LPARAM)s);
}
static void markdown_line(const wchar_t *line,BOOL block) {
    DWORD effects=0; const wchar_t *p=line; BOOL code=block;
    if (!block) {
        if (*p==L'#') { while (*p==L'#') ++p; if (*p==L' ') ++p; effects=CFE_BOLD; }
        else if (!wcsncmp(p,L"- ",2) || !wcsncmp(p,L"* ",2)) { rich_append(L"• ",0,FALSE); p+=2; }
        else if (*p==L'>') { rich_append(L"│ ",0,FALSE); ++p; if (*p==L' ') ++p; }
        else if (!wcscmp(p,L"---") || !wcscmp(p,L"***")) { rich_append(L"────────────────────────",0,FALSE); p+=wcslen(p); }
    }
    wchar_t chunk[512]; size_t n=0;
    while (*p) {
        BOOL bold=!block && p[0]==L'*' && p[1]==L'*';
        BOOL italic=!block && p[0]==L'*' && !bold;
        BOOL tick=!block && p[0]==L'`';
        if (bold || italic || tick || n==510) {
            chunk[n]=0; rich_append(chunk,effects,code); n=0;
            if (bold) { effects^=CFE_BOLD; p+=2; }
            else if (italic) { effects^=CFE_ITALIC; ++p; }
            else if (tick) { code=!code; ++p; }
        } else chunk[n++]=*p++;
    }
    chunk[n]=0; rich_append(chunk,effects,code); rich_append(L"\r\n",0,FALSE);
}
static void render_notes(void) {
    wchar_t version[128],date[256]; SetWindowTextW(u.body,L"");
    wcscpy(version,L"新版本："); wcscat(version,u.manifest.version_name);
    rich_append(version,CFE_BOLD,FALSE); rich_append(L"\r\n",0,FALSE);
    if (up_local_time(u.manifest.published,date,256)) { rich_append(L"发布时间：",0,FALSE); rich_append(date,0,FALSE); rich_append(L"\r\n",0,FALSE); }
    rich_append(L"\r\n",0,FALSE);
    BOOL block=FALSE; const wchar_t *p=u.manifest.notes;
    while (*p) {
        wchar_t line[4096]; size_t n=0;
        while (*p && *p!=L'\n' && n<4095) { if (*p!=L'\r') line[n++]=*p; ++p; }
        line[n]=0; if (*p==L'\n') ++p;
        if (!wcsncmp(line,L"```",3)) block=!block; else markdown_line(line,block);
    }
    SendMessageW(u.body,EM_SETSEL,0,0); SendMessageW(u.body,WM_VSCROLL,SB_TOP,0);
}
static void close_dialog(BOOL ignore) {
    if (ignore) u.ignored=TRUE;
    if (u.dialog) DestroyWindow(u.dialog);
    u.dialog=NULL; EnableWindow(u.owner,TRUE); SetActiveWindow(u.owner); update_caption();
}
static void begin_download(void) {
    if (u.thread || u.task || !u.known) return;
    DownloadResult *r=calloc(1,sizeof(*r)); if (!r) return;
    r->manifest=u.manifest; ResetEvent(u.stop); u.downloading=TRUE;
    u.thread=(HANDLE)_beginthreadex(NULL,0,download_thread,r,0,NULL);
    if (!u.thread) { free(r); u.downloading=FALSE; SetWindowTextW(u.label,L"无法启动下载，请重试。"); return; }
    EnableWindow(u.skip,FALSE); EnableWindow(u.download,FALSE); SetWindowTextW(u.download,L"下载中…");
    SetWindowTextW(u.later,L"取消下载"); SetWindowTextW(u.label,L"正在下载更新包…");
    ShowWindow(u.bar,SW_SHOW);
}
static void dialog_layout(HWND hwnd) {
    RECT r; GetClientRect(hwnd,&r); int w=r.right,h=r.bottom;
    MoveWindow(u.body,dp(20),dp(20),w-dp(40),h-dp(140),TRUE);
    MoveWindow(u.label,dp(20),h-dp(106),w-dp(40),dp(25),TRUE);
    MoveWindow(u.bar,dp(20),h-dp(78),w-dp(40),dp(8),TRUE);
    MoveWindow(u.skip,dp(20),h-dp(54),dp(120),dp(34),TRUE);
    MoveWindow(u.later,dp(152),h-dp(54),dp(100),dp(34),TRUE);
    MoveWindow(u.download,w-dp(170),h-dp(54),dp(150),dp(34),TRUE);
}
static HWND dialog_control(HWND dlg,const wchar_t *cls,const wchar_t *text,DWORD style,int id) {
    HWND h=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,0,0,0,0,dlg,(HMENU)(INT_PTR)id,u.instance,NULL);
    SendMessageW(h,WM_SETFONT,(WPARAM)u.font,TRUE); return h;
}
static LRESULT CALLBACK dialog_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        u.dialog=hwnd;
        u.body=dialog_control(hwnd,L"RICHEDIT50W",L"",ES_READONLY|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP,0);
        SendMessageW(u.body,EM_SETBKGNDCOLOR,0,RGB(255,255,255));
        SendMessageW(u.body,EM_EXLIMITTEXT,0,UP_MAX_NOTES*2);
        SendMessageW(u.body,EM_AUTOURLDETECT,TRUE,0);
        SendMessageW(u.body,EM_SETEVENTMASK,0,ENM_LINK);
        u.label=dialog_control(hwnd,L"STATIC",L"",0,0);
        u.bar=dialog_control(hwnd,PROGRESS_CLASSW,L"",PBS_SMOOTH,0);
        ShowWindow(u.bar,SW_HIDE);
        u.skip=dialog_control(hwnd,L"BUTTON",L"跳过此版本",WS_TABSTOP,U_SKIP);
        u.later=dialog_control(hwnd,L"BUTTON",L"稍后",WS_TABSTOP,U_LATER);
        u.download=dialog_control(hwnd,L"BUTTON",L"下载并更新",WS_TABSTOP|BS_DEFPUSHBUTTON,U_DOWNLOAD);
        if (required()) { ShowWindow(u.skip,SW_HIDE); ShowWindow(u.later,SW_HIDE); SetWindowTextW(u.label,L"此版本需要更新后继续使用。"); }
        render_notes(); dialog_layout(hwnd); return 0;
    case WM_SIZE: dialog_layout(hwnd); return 0;
    case WM_COMMAND:
        if (LOWORD(wp)==U_SKIP && !required() && !u.downloading) {
            u.prefs.skipped=u.manifest.version; save_preferences(); close_dialog(FALSE);
        } else if (LOWORD(wp)==U_LATER && !required()) {
            if (u.downloading) { SetEvent(u.stop); SetWindowTextW(u.label,L"正在取消下载…"); }
            else close_dialog(TRUE);
        } else if (LOWORD(wp)==U_DOWNLOAD) begin_download();
        return 0;
    case WM_NOTIFY:
        if (((NMHDR*)lp)->code==EN_LINK) {
            ENLINK *link=(ENLINK*)lp;
            if (link->msg==WM_LBUTTONUP && link->chrg.cpMax-link->chrg.cpMin<UP_URL) {
                wchar_t url[UP_URL]; TEXTRANGEW range={link->chrg,url};
                SendMessageW(u.body,EM_GETTEXTRANGE,0,(LPARAM)&range);
                if (!_wcsnicmp(url,L"https://",8) || !_wcsnicmp(url,L"http://",7)) ShellExecuteW(hwnd,L"open",url,NULL,NULL,SW_SHOWNORMAL);
            }
        }
        break;
    case WM_CLOSE:
        if (required()) { PostMessageW(u.owner,WM_CLOSE,0,0); return 0; }
        if (u.downloading) { u.dismiss=TRUE; SetEvent(u.stop); SetWindowTextW(u.label,L"正在取消下载…"); }
        else close_dialog(TRUE);
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
static void show_dialog(void) {
    if (!u.known || u.closing) return;
    if (u.task) { u.deferred=TRUE; return; }
    if (u.dialog) { SetForegroundWindow(u.dialog); return; }
    u.dialog_shown=TRUE; u.deferred=FALSE;
    WNDCLASSEXW wc={0}; wc.cbSize=sizeof(wc); wc.hInstance=u.instance; wc.lpfnWndProc=dialog_proc;
    wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.hCursor=LoadCursorW(NULL,IDC_ARROW);
    wc.lpszClassName=L"PayloadDumperUpdate"; wc.hIcon=LoadIconW(u.instance,MAKEINTRESOURCEW(101));
    RegisterClassExW(&wc);
    RECT owner; GetWindowRect(u.owner,&owner);
    int w=dp(640),h=dp(500);
    HWND dialog=CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_CONTROLPARENT,wc.lpszClassName,L"软件更新",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,owner.left+(owner.right-owner.left-w)/2,
        owner.top+(owner.bottom-owner.top-h)/2,w,h,u.owner,NULL,u.instance,NULL);
    if (dialog) { EnableWindow(u.owner,FALSE); ShowWindow(dialog,SW_SHOW); UpdateWindow(dialog); }
}
static BOOL launch_updater(DownloadResult *r) {
    if (u.task || stopped()) return FALSE;
    wchar_t helper[UP_PATH],launcher[UP_PATH],zip[UP_PATH],probe[UP_PATH]; char hash[65];
    if (!up_join(helper,u.directory,L"bin\\updater.exe") || !up_join(launcher,r->directory,L"apply-update.exe") ||
        !up_join(zip,r->directory,L"update.zip") || !up_sha_file(zip,hash) || strcmp(hash,r->manifest.sha256)) return FALSE;
    if (!up_join(probe,u.directory,L".payload-update-write-test")) return FALSE;
    HANDLE f=CreateFileW(probe,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,NULL);
    if (f==INVALID_HANDLE_VALUE) { SetWindowTextW(u.label,L"程序目录不可写，请将程序移动到可写目录后更新。"); return FALSE; }
    CloseHandle(f);
    if (!CopyFileW(helper,launcher,TRUE)) return FALSE;
    wchar_t *cmd=calloc(UP_PATH,sizeof(wchar_t)); if (!cmd) return FALSE;
    wchar_t pid[32],version[32],sha[65];
    _snwprintf(pid,32,L"%lu",GetCurrentProcessId());
    char num[32]; snprintf(num,sizeof(num),"%llu",(unsigned long long)r->manifest.version);
    MultiByteToWideChar(CP_UTF8,0,num,-1,version,32); MultiByteToWideChar(CP_UTF8,0,r->manifest.sha256,-1,sha,65);
    const wchar_t *args[]={launcher,pid,u.directory,r->directory,sha,version};
    BOOL ok=TRUE; for (size_t i=0;i<6;++i) if (!up_quote(cmd,UP_PATH,args[i])) ok=FALSE;
    STARTUPINFOW si={0}; si.cb=sizeof(si); PROCESS_INFORMATION pi={0};
    if (ok) ok=CreateProcessW(launcher,cmd,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,r->directory,&si,&pi);
    free(cmd);
    if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); PostMessageW(u.owner,WM_CLOSE,0,0); }
    return ok;
}
void updater_init(HWND owner,HINSTANCE instance,HWND button,const wchar_t *directory,UINT dpi) {
    memset(&u,0,sizeof(u)); u.owner=owner; u.instance=instance; u.button=button; u.dpi=dpi;
    wcscpy(u.directory,directory); u.stop=CreateEventW(NULL,TRUE,FALSE,NULL); if (!u.stop) return;
    u.font=CreateFontW(-dp(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    DWORD size=sizeof(u.prefs),type=0; HKEY key;
#ifndef PD_UPDATE_TESTING
    if (RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Daxiaamu\\PayloadDumperGUI",0,KEY_QUERY_VALUE,&key)==ERROR_SUCCESS) {
        if (RegQueryValueExW(key,L"UpdateStateV1",NULL,&type,(BYTE*)&u.prefs,&size)!=ERROR_SUCCESS || type!=REG_BINARY || size!=sizeof(u.prefs)) memset(&u.prefs,0,sizeof(u.prefs));
        RegCloseKey(key);
    }
#else
    (void)size; (void)type; (void)key;
#endif
    u.prefs.manifest_hash[64]=0; u.initialized=TRUE;
    SetWindowLongPtrW(u.button,GWL_STYLE,(GetWindowLongPtrW(u.button,GWL_STYLE)&~(LONG_PTR)BS_TYPEMASK)|BS_OWNERDRAW);
    if (APP_VERSION_CODE<=u.prefs.forced) EnableWindow(u.owner,FALSE);
}
void updater_check(BOOL manual) {
    if (!u.initialized || u.closing) return;
    if (u.thread) { if (u.checking && manual) u.manual=TRUE; if (u.dialog) SetForegroundWindow(u.dialog); return; }
    if (u.dialog) { SetForegroundWindow(u.dialog); return; }
    u.manual=manual; u.checking=TRUE; ResetEvent(u.stop); update_caption();
    u.thread=(HANDLE)_beginthreadex(NULL,0,check_thread,NULL,0,NULL);
    if (!u.thread) { u.checking=FALSE; update_caption(); if (manual) MessageBoxW(u.owner,L"无法启动更新检查。",L"软件更新",MB_ICONERROR); }
}
void updater_event(WPARAM kind,LPARAM data) {
    if (!u.initialized) return;
    if (kind==DOWNLOAD_PROGRESS) {
        if (u.downloading && u.dialog) {
            wchar_t text[64]; _snwprintf(text,64,L"正在下载更新… %lu%%",(DWORD)data);
            SetWindowTextW(u.label,text); SendMessageW(u.bar,PBM_SETPOS,(WPARAM)data,0);
        }
        return;
    }
    if (u.thread) { WaitForSingleObject(u.thread,INFINITE); CloseHandle(u.thread); u.thread=NULL; }
    if (kind==CHECK_DONE) {
        CheckResult *r=(CheckResult*)data; u.checking=FALSE;
        if (!u.closing && r && r->ok) {
            u.prefs.revision=r->pointer.revision; strcpy(u.prefs.manifest_hash,r->pointer.sha256);
            u.prefs.version=r->manifest.version; u.prefs.forced=r->manifest.forced; save_preferences();
            u.manifest=r->manifest; u.known=r->manifest.version>APP_VERSION_CODE;
            if (u.known && (u.manual || required() || (!u.dialog_shown && u.prefs.skipped!=u.manifest.version))) show_dialog();
            else if (u.manual && !u.known) MessageBoxW(u.owner,L"当前版本 " APP_VERSION_WNAME L"，已是最新版本。",L"软件更新",MB_ICONINFORMATION);
        } else if (!u.closing && APP_VERSION_CODE<=u.prefs.forced) {
            int response=MessageBoxW(u.owner,L"此版本需要更新后继续使用，但暂时无法核验更新源。\n请连接网络后重试，或退出程序。",L"软件更新",MB_RETRYCANCEL|MB_ICONWARNING);
            if (response==IDRETRY) { free(r); updater_check(FALSE); return; }
            PostMessageW(u.owner,WM_CLOSE,0,0);
        } else if (!u.closing && u.manual) MessageBoxW(u.owner,L"检查更新失败，请稍后重试。\n网络不可用、发布源冲突或元数据校验失败时不会安装更新。",L"软件更新",MB_ICONWARNING);
        free(r); update_caption();
    } else if (kind==DOWNLOAD_DONE) {
        DownloadResult *r=(DownloadResult*)data; u.downloading=FALSE;
        if (r && r->ok && !u.closing && !stopped()) {
            SetWindowTextW(u.label,L"SHA-256 校验通过，正在准备更新…"); SendMessageW(u.bar,PBM_SETPOS,100,0);
            if (!launch_updater(r)) { SetWindowTextW(u.label,L"无法启动更新，请重试或从 GitHub 下载便携包。"); }
        } else if (!u.closing && u.dialog) SetWindowTextW(u.label,stopped()?L"下载已取消。":L"所有下载源均失败或校验不符，请重试。");
        if (r && (!r->ok || u.closing || stopped())) {
            wchar_t zip[UP_PATH]; if (up_join(zip,r->directory,L"update.zip")) DeleteFileW(zip); RemoveDirectoryW(r->directory);
        }
        free(r);
        if (u.dialog && !u.closing) {
            EnableWindow(u.skip,!required()); EnableWindow(u.download,TRUE);
            SetWindowTextW(u.download,L"重试更新"); SetWindowTextW(u.later,L"稍后");
            if (u.dismiss) { u.dismiss=FALSE; close_dialog(TRUE); }
        }
    }
}
void updater_task_running(BOOL running) {
    if (!u.initialized) return;
    u.task=running;
    if (!running && u.deferred && !u.closing) show_dialog();
}
BOOL updater_busy(void) { return u.initialized && u.thread!=NULL; }
void updater_stop(void) { if (u.initialized) { u.closing=TRUE; SetEvent(u.stop); if (u.dialog) EnableWindow(u.dialog,FALSE); } }
void updater_destroy(void) {
    if (!u.initialized) return;
    if (u.dialog) DestroyWindow(u.dialog);
    CloseHandle(u.stop); if (u.font) DeleteObject(u.font); u.initialized=FALSE;
}
BOOL updater_draw_button(const DRAWITEMSTRUCT *item) {
    if (!u.initialized || item->hwndItem!=u.button) return FALSE;
    RECT r=item->rcItem; int state=(item->itemState&ODS_DISABLED)?4:(item->itemState&ODS_SELECTED)?3:(item->itemState&ODS_HOTLIGHT)?2:1;
    HTHEME theme=OpenThemeData(u.button,L"BUTTON");
    if (theme) { DrawThemeBackground(theme,item->hDC,1,state,&r,NULL); CloseThemeData(theme); }
    else DrawFrameControl(item->hDC,&r,DFC_BUTTON,DFCS_BUTTONPUSH|((item->itemState&ODS_SELECTED)?DFCS_PUSHED:0));
    wchar_t text[64]; GetWindowTextW(u.button,text,64);
    SetBkMode(item->hDC,TRANSPARENT); SetTextColor(item->hDC,GetSysColor((item->itemState&ODS_DISABLED)?COLOR_GRAYTEXT:COLOR_BTNTEXT));
    HFONT old=(HFONT)SelectObject(item->hDC,(HFONT)SendMessageW(u.button,WM_GETFONT,0,0));
    DrawTextW(item->hDC,text,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(item->hDC,old);
    if (u.known && u.ignored) {
        HBRUSH dot=CreateSolidBrush(RGB(213,48,48)); HGDIOBJ oldbrush=SelectObject(item->hDC,dot),oldpen=SelectObject(item->hDC,GetStockObject(NULL_PEN));
        Ellipse(item->hDC,r.right-dp(13),dp(5),r.right-dp(6),dp(12));
        SelectObject(item->hDC,oldpen); SelectObject(item->hDC,oldbrush); DeleteObject(dot);
    }
    if (item->itemState&ODS_FOCUS) { InflateRect(&r,-dp(3),-dp(3)); DrawFocusRect(item->hDC,&r); }
    return TRUE;
}
