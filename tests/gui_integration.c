/* In-process integration tests for our own UI and worker code. No interaction
 * with other desktop applications. Test windows remain hidden. */
#define wWinMain production_entry
#include "../src/main.c"
#undef wWinMain
static int failures;
static void check(BOOL condition, const char *label) {
    printf("%s %s\n",condition?"PASS":"FAIL",label);
    if (!condition) ++failures;
}
static void pump(void) {
    MSG msg;
    while (PeekMessageW(&msg,NULL,0,0,PM_REMOVE)) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
}
static BOOL wait_done(DWORD timeout) {
    ULONGLONG end=GetTickCount64()+timeout;
    while (active && GetTickCount64()<end) {
        pump(); MsgWaitForMultipleObjects(0,NULL,FALSE,5,QS_ALLINPUT);
    }
    pump();
    if (active) { cancel_task(); return FALSE; }
    return TRUE;
}
static BOOL status_is(const wchar_t *expected) {
    wchar_t text[256]; GetWindowTextW(status,text,256); return wcscmp(text,expected)==0;
}
static BOOL log_contains(const wchar_t *expected) {
    int n=GetWindowTextLengthW(logbox); wchar_t *text=calloc((size_t)n+1,sizeof(wchar_t));
    if (!text) return FALSE;
    GetWindowTextW(logbox,text,n+1); BOOL found=wcsstr(text,expected)!=NULL; free(text); return found;
}
static int log_occurrences(const wchar_t *needle) {
    int n=GetWindowTextLengthW(logbox),count=0;
    wchar_t *text=calloc((size_t)n+1,sizeof(wchar_t));
    if (!text) return -1;
    GetWindowTextW(logbox,text,n+1);
    for (const wchar_t *p=text;(p=wcsstr(p,needle))!=NULL;p+=wcslen(needle)) ++count;
    free(text); return count;
}
int wmain(int argc, wchar_t **argv) {
    if (argc!=4) { puts("gui_integration <package-dir> <fixture-dir> <probe-exe>"); return 2; }
    instance=GetModuleHandleW(NULL);
    wcscpy(base_dir,argv[1]);
    join_path(dumper_path,PATH_CAP,base_dir,L"bin\\payload_dumper.exe");
    join_path(arb_path,PATH_CAP,base_dir,L"bin\\arbextract.exe");
    CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    HMODULE rich=LoadLibraryW(L"Msftedit.dll");
    INITCOMMONCONTROLSEX ic={sizeof(ic),ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES}; InitCommonControlsEx(&ic);
    white=CreateSolidBrush(RGB(255,255,255)); fonts();
    WNDCLASSEXW wc={0}; wc.cbSize=sizeof(wc); wc.hInstance=instance; wc.lpfnWndProc=wndproc;
    wc.hbrBackground=white; wc.lpszClassName=L"PayloadDumperIntegration";
    RegisterClassExW(&wc);
    window=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,APP_WINDOW_TITLE,WS_OVERLAPPEDWINDOW,
        0,0,860,560,NULL,NULL,instance,NULL);
    check(window && input && output && logbox && progress,"create native controls");
    {
        wchar_t caption_text[128]; GetWindowTextW(window,caption_text,128);
        check(wcsstr(caption_text,L"daxiaamu")!=NULL,"window caption contains daxiaamu");
        RECT outer,client,action,indicator,label;
        POINT client_origin={0,0}; GetWindowRect(window,&outer); ClientToScreen(window,&client_origin);
        LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE);
        check(client_origin.y>outer.top && (style&WS_CAPTION)==WS_CAPTION &&
              (style&(WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX))==(WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX),
              "native title bar and system window buttons restored");
        GetClientRect(window,&client); GetWindowRect(updatebtn,&action); GetWindowRect(progress,&indicator); GetWindowRect(status,&label);
        MapWindowPoints(NULL,window,(POINT*)&action,2); MapWindowPoints(NULL,window,(POINT*)&indicator,2); MapWindowPoints(NULL,window,(POINT*)&label,2);
        check(action.left>client.right/2 && action.top>client.bottom/2 && action.right<=client.right && action.bottom<=client.bottom,
              "update action is inside the bottom-right content area");
        check(label.right<indicator.left && indicator.right<action.left,"footer status progress and update action do not overlap");
    }
    check(!(GetWindowLongPtrW(progress,GWL_STYLE)&WS_VISIBLE),"idle task progress is hidden");
    wchar_t path[PATH_CAP], outdir[PATH_CAP];
    join_path(path,PATH_CAP,argv[2],L"OTA 中文 & 空格.zip");
    set_input(path);
    join_path(outdir,PATH_CAP,argv[2],sizeof(void*)==8?L"GUI x64 输出 & 空格":L"GUI x86 输出 & 空格");
    SetWindowTextW(output,outdir);
    start_task(TASK_LIST);
    check(active && !IsWindowEnabled(input) && IsWindowEnabled(cancelbtn),"busy controls and cancellable task");
    check((GetWindowLongPtrW(progress,GWL_STYLE)&WS_VISIBLE)!=0,"running task progress is shown");
    check(wait_done(15000) && status_is(L"任务完成") && log_contains(L"xbl_config"),"partition list reaches UI log");
    check(!(GetWindowLongPtrW(progress,GWL_STYLE)&WS_VISIBLE),"successful task hides its progress indicator");
    SetWindowTextW(partitions,L"boot, xbl_config");
    SetWindowTextW(workers,L"2");
    start_task(TASK_UNPACK);
    check(wait_done(15000) && status_is(L"任务完成"),"selective unpack through GUI worker");
    join_path(path,PATH_CAP,outdir,L"boot.img"); check(exists(path,FALSE),"Unicode output file created");
    join_path(path,PATH_CAP,outdir,L"dtbo.img"); check(!exists(path,FALSE),"partition filter applied");
    start_task(TASK_METADATA);
    check(wait_done(15000) && status_is(L"任务完成") && log_contains(L"6.6.118-android15-test"),"metadata and kernel pipeline");
    join_path(path,PATH_CAP,outdir,L"metadata"); check(exists(path,FALSE),"metadata uses chosen output directory");
    start_task(TASK_ARB);
    check(wait_done(15000) && status_is(L"任务完成") && log_contains(L"ARB (Anti-Rollback)       : 7"),"ARB extraction and external arbextract pipeline");
    join_path(path,PATH_CAP,argv[2],L"bad.bin"); SetWindowTextW(input,path);
    start_task(TASK_LIST);
    check(wait_done(15000) && status_is(L"任务失败，查看日志") && IsWindowEnabled(input),"failure status and UI restored");
    check(!(GetWindowLongPtrW(progress,GWL_STYLE)&WS_VISIBLE),"failed task hides its progress indicator");
    /* Exercise our child runner with an argument containing embedded quotes,
     * ampersands, spaces and a trailing slash. No shell gets involved. */
    wcscpy(dumper_path,argv[3]);
    SetWindowTextW(logbox,L"");
    SetWindowTextW(input,L"https://example.invalid/a b&c/\"quoted\"/path\\");
    start_task(TASK_LIST);
    check(wait_done(15000) && status_is(L"任务完成"),"probe child completed");
    check(!log_contains(L"\r\n> "),"execution command is not echoed into the user log");
    check(log_contains(L"ARG[4]=<https://example.invalid/a b&c/\"quoted\"/path\\>"),"Windows argument quoting round trip");
    check(log_contains(L"stdout-149") && log_contains(L"stderr-149"),"drain merged stdout and stderr beyond pipe capacity");
    check(log_contains(L"\r\n中文\r\n"),"split UTF-8 characters preserved");
    check(log_contains(L"250/250 ops (100%)") && log_occurrences(L" ops (")==1,
          "250 carriage-return progress updates retain only the final line");
    check(!log_contains(L"WRAPPED_OLD_PROGRESS_") && !log_contains(L"XXXXX") && !log_contains(L"LONG_STATUS_TEXT_TO_BE_REPLACED"),
          "carriage return clears an entire wrapped line and stale trailing text");
    check(log_contains(L"PROGRESS_BEGIN\r\n") && log_contains(L"PROGRESS_END\r\nOK\r\nAFTER_CRLF\r\n"),
          "regular log lines and split CRLF boundaries remain intact");
    log_text(L"discard-on-clear\r");
    SendMessageW(window,WM_COMMAND,ID_CLEAR,0);
    log_text(L"new-after-clear\r\n");
    check(log_contains(L"new-after-clear") && !log_contains(L"discard-on-clear"),"clear log resets carriage-return state");
    SetWindowTextW(logbox,L"");
    SetWindowTextW(input,L"https://example.invalid/slow");
    start_task(TASK_LIST);
    ULONGLONG deadline=GetTickCount64()+10000;
    while (active && !log_contains(L"PROBE_RUNNING") && GetTickCount64()<deadline) { pump(); Sleep(5); }
    check(log_contains(L"PROBE_RUNNING"),"slow child is running");
    HANDLE job=active?active->job:NULL; HANDLE job_copy=NULL;
    if (job) DuplicateHandle(GetCurrentProcess(),job,GetCurrentProcess(),&job_copy,0,FALSE,DUPLICATE_SAME_ACCESS);
    cancel_task();
    check(wait_done(5000) && status_is(L"任务已取消"),"cancel terminates running child without blocking UI");
    check(!(GetWindowLongPtrW(progress,GWL_STYLE)&WS_VISIBLE),"cancelled task hides its progress indicator");
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info={0};
    check(job_copy && QueryInformationJobObject(job_copy,JobObjectBasicAccountingInformation,&info,sizeof(info),NULL) && !info.ActiveProcesses,"no surviving child processes after cancellation");
    if (job_copy) CloseHandle(job_copy);
    SetWindowTextW(logbox,L"");
    start_task(TASK_LIST);
    deadline=GetTickCount64()+10000;
    while (active && !log_contains(L"PROBE_RUNNING") && GetTickCount64()<deadline) { pump(); Sleep(5); }
    /* The production close handler cancels work and defers destruction. */
    SendMessageW(window,WM_CLOSE,0,0);
    check(wait_done(5000) && !IsWindow(window),"close during task cancels and destroys window safely");
    if (rich) FreeLibrary(rich);
    DeleteObject(ui_font); DeleteObject(mono_font); DeleteObject(white);
    CoUninitialize();
    printf("RESULT: %d failures\n",failures);
    return failures?1:0;
}
