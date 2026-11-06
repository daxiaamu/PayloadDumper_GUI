#define PD_UPDATE_TESTING
#include "../src/update.c"
static char *pointer_data,*manifest_data;
static LONG requests;
static int failures;
BOOL updater_test_http(const wchar_t *url,char **memory,DWORD cap,const wchar_t *file,DWORD expected) {
    (void)expected;
    InterlockedIncrement(&requests); Sleep(30);
    if (file || !memory || stopped()) return FALSE;
    const char *source=wcsstr(url,L"latest.json")?pointer_data:manifest_data;
    size_t n=strlen(source);
    if (n>cap) return FALSE;
    *memory=malloc(n+1); if (!*memory) return FALSE;
    memcpy(*memory,source,n+1); return TRUE;
}
static void check(BOOL ok,const char *name) { printf("%s %s\n",ok?"PASS":"FAIL",name); if (!ok) ++failures; }
static char *read_json(const wchar_t *root,const wchar_t *name) {
    wchar_t path[UP_PATH]; up_join(path,root,name); FILE *f=_wfopen(path,L"rb"); if (!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); rewind(f);
    char *data=n>=0?calloc((size_t)n+1,1):NULL;
    if (data && fread(data,1,(size_t)n,f)!=(size_t)n) { free(data); data=NULL; }
    fclose(f); return data;
}
static LRESULT CALLBACK owner_proc(HWND owner,UINT msg,WPARAM wp,LPARAM lp) {
    if (msg==WM_UPDATE_EVENT) { updater_event(wp,lp); return 0; }
    return DefWindowProcW(owner,msg,wp,lp);
}
static BOOL wait_session(void) {
    ULONGLONG end=GetTickCount64()+5000;
    while (updater_busy() && GetTickCount64()<end) {
        MSG msg; while (PeekMessageW(&msg,NULL,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(1);
    }
    return !updater_busy();
}
int wmain(int argc,wchar_t **argv) {
    if (argc!=2) return 2;
    pointer_data=read_json(argv[1],L"session-pointer.json"); manifest_data=read_json(argv[1],L"session-manifest.json");
    if (!pointer_data || !manifest_data) return 2;
    HINSTANCE instance=GetModuleHandleW(NULL);
    WNDCLASSW wc={0}; wc.hInstance=instance; wc.lpfnWndProc=owner_proc; wc.lpszClassName=L"UpdateSessionTest";
    RegisterClassW(&wc);
    HWND owner=CreateWindowW(wc.lpszClassName,L"Test",WS_OVERLAPPEDWINDOW,0,0,500,300,NULL,NULL,instance,NULL);
    HWND button=CreateWindowW(L"BUTTON",L"Check",WS_CHILD,0,0,100,30,owner,NULL,instance,NULL);
    updater_init(owner,instance,button,argv[1],96); updater_task_running(TRUE);
    updater_check(FALSE); HANDLE flight=u.thread; updater_check(TRUE);
    check(u.thread==flight && u.manual,"manual request joins active automatic session");
    check(wait_session() && requests==8 && u.known && u.deferred,"one metadata batch and deferred visible manual result");
    u.deferred=FALSE;
    updater_check(TRUE);
    check(wait_session() && requests==16 && u.deferred,"manual request after completion starts a fresh network session");
    u.deferred=FALSE; u.prefs.skipped=u.manifest.version;
    updater_check(FALSE);
    check(wait_session() && requests==24 && !u.deferred,"automatic checks respect skipped version");
    updater_check(TRUE);
    check(wait_session() && requests==32 && u.deferred,"manual check bypasses skipped-version suppression");
    updater_stop(); updater_destroy(); DestroyWindow(button); DestroyWindow(owner);
    free(pointer_data); free(manifest_data);
    printf("RESULT: %d update session failures\n",failures); return failures?1:0;
}
