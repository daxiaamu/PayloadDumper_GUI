/* Native Win32 frontend. All controls belong to the UI thread; workers send
 * synchronous log messages and post completion. Close is deferred until done. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <richedit.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>
#include "resource.h"
#include "update.h"

#define PATH_CAP 32768
#define WM_LOG (WM_APP + 1)
#define WM_DONE (WM_APP + 2)
#define LOG_LIMIT 1048576
enum { ID_INPUT=201, ID_OUTPUT, ID_PARTITIONS, ID_WORKERS,
       ID_FILE, ID_FOLDER, ID_UNPACK, ID_METADATA, ID_ARB, ID_LIST,
       ID_OPEN, ID_CANCEL, ID_CLEAR, ID_UPDATE };
enum { TASK_UNPACK, TASK_METADATA, TASK_ARB, TASK_LIST };
typedef struct {
    int kind;
    wchar_t input[PATH_CAP], output[PATH_CAP], partitions[4096], workers[16];
    HANDLE cancel, job;
} Task;
static HINSTANCE instance;
static HWND window, input, output, partitions, workers, logbox, status, progress;
static HWND labels[4], hint;
static HWND filebtn, folderbtn, unpackbtn, metadatabtn, arbbtn, listbtn;
static HWND openbtn, cancelbtn, clearbtn, updatebtn;
static HFONT ui_font, mono_font;
static HBRUSH white;
static UINT dpi = 96;
static wchar_t base_dir[PATH_CAP], dumper_path[PATH_CAP], arb_path[PATH_CAP];
static Task *active;
static HANDLE worker_handle;
static BOOL close_pending;
static BOOL last_cr;

static int px(int n) { return MulDiv(n, (int)dpi, 96); }
static BOOL exists(const wchar_t *path, BOOL directory) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && ((a & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
}
static BOOL join_path(wchar_t *dest, size_t cap, const wchar_t *dir, const wchar_t *name) {
    size_t d = wcslen(dir), n = wcslen(name);
    BOOL sep = d && dir[d-1] != L'\\' && dir[d-1] != L'/';
    if (d + sep + n + 1 > cap) return FALSE;
    /* dest may alias dir. */
    memmove(dest, dir, d * sizeof(wchar_t));
    if (sep) dest[d++] = L'\\';
    memcpy(dest+d, name, (n+1) * sizeof(wchar_t));
    return TRUE;
}
static BOOL cancelled(Task *t) { return WaitForSingleObject(t->cancel, 0) == WAIT_OBJECT_0; }
static void log_text(const wchar_t *text) {
    SendMessageW(window, WM_LOG, 0, (LPARAM)text);
}
static void log_error(const wchar_t *what, DWORD error) {
    wchar_t message[1024], system[512] = L"";
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, error, 0, system, 512, NULL);
    _snwprintf(message, 1024, L"\r\n%s失败（%lu）：%s\r\n", what, error, system);
    message[1023] = 0;
    log_text(message);
}
static void append_log(const wchar_t *text) {
    LRESULT len = GetWindowTextLengthW(logbox);
    if (len > LOG_LIMIT) {
        SendMessageW(logbox, EM_SETSEL, 0, 131072);
        SendMessageW(logbox, EM_REPLACESEL, FALSE, (LPARAM)L"");
    }
    SendMessageW(logbox, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(logbox, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(logbox, EM_SCROLLCARET, 0, 0);
}
/* Follow the Windows CRT quoting rules, including quotes and trailing slashes.
 * No shell, command interpreter, PATH lookup, or string interpolation is used. */
static BOOL quote_arg(wchar_t *cmd, size_t cap, const wchar_t *arg) {
    size_t p = wcslen(cmd);
    if (p + 3 >= cap) return FALSE;
    if (p) cmd[p++] = L' ';
    cmd[p++] = L'"';
    while (*arg) {
        size_t slashes = 0;
        while (*arg == L'\\') { ++slashes; ++arg; }
        size_t count = slashes * ((*arg == L'"' || !*arg) ? 2 : 1);
        if (p + count + 4 >= cap) return FALSE;
        while (count--) cmd[p++] = L'\\';
        if (*arg == L'"') cmd[p++] = L'\\';
        if (*arg) cmd[p++] = *arg++;
    }
    cmd[p++] = L'"'; cmd[p] = 0;
    return TRUE;
}
/* Decode only complete UTF-8 codepoints; a pipe read can split a character.
 * Normalize both progress carriage returns and newlines for RichEdit. */
static void pipe_log(char *data, DWORD *pending, BOOL final) {
    DWORD n = *pending, complete = n;
    if (!final && n) {
        DWORD lead = n-1;
        while (lead && ((unsigned char)data[lead] & 0xc0) == 0x80) --lead;
        unsigned char ch = (unsigned char)data[lead];
        DWORD need = ch >= 0xf0 ? 4 : ch >= 0xe0 ? 3 : ch >= 0xc0 ? 2 : 1;
        if (n - lead < need) complete = lead;
    }
    if (complete) {
        wchar_t wide[8194], normalized[16390];
        int chars = MultiByteToWideChar(CP_UTF8, 0, data, (int)complete, wide, 8193);
        size_t j = 0;
        for (int i = 0; i < chars; ++i) {
            wchar_t c = wide[i];
            if (c == L'\r') { normalized[j++] = L'\r'; normalized[j++] = L'\n'; last_cr = TRUE; }
            else if (c == L'\n') {
                if (!last_cr) { normalized[j++] = L'\r'; normalized[j++] = L'\n'; }
                last_cr = FALSE;
            } else { normalized[j++] = c ? c : L' '; last_cr = FALSE; }
        }
        normalized[j] = 0;
        log_text(normalized);
    }
    memmove(data, data+complete, n-complete);
    *pending = n-complete;
}
static DWORD run_tool(Task *t, const wchar_t *exe, const wchar_t **args, int argc,
                      const wchar_t *cwd) {
    wchar_t *cmd = calloc(PATH_CAP, sizeof(wchar_t));
    HANDLE readpipe = NULL, writepipe = NULL, nullin = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    PROCESS_INFORMATION pi = {0};
    STARTUPINFOW si = {0};
    DWORD result = ERROR_GEN_FAILURE, error;
    if (!cmd) { log_text(L"内存不足。\r\n"); return ERROR_NOT_ENOUGH_MEMORY; }
    if (!quote_arg(cmd, PATH_CAP, exe)) goto too_long;
    for (int i = 0; i < argc; ++i) if (!quote_arg(cmd, PATH_CAP, args[i])) goto too_long;
    if (cancelled(t)) { result = ERROR_CANCELLED; goto cleanup; }
    log_text(L"\r\n> "); log_text(cmd); log_text(L"\r\n");
    if (!CreatePipe(&readpipe, &writepipe, &sa, 0)) goto failed;
    if (!SetHandleInformation(readpipe, HANDLE_FLAG_INHERIT, 0)) goto failed;
    nullin = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         &sa, OPEN_EXISTING, 0, NULL);
    if (nullin == INVALID_HANDLE_VALUE) goto failed;
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nullin;
    si.hStdOutput = si.hStdError = writepipe;
    if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, cwd, &si, &pi)) goto failed;
    if (!AssignProcessToJobObject(t->job, pi.hProcess)) {
        error = GetLastError(); TerminateProcess(pi.hProcess, error);
        log_error(L"绑定子进程", error); goto cleanup;
    }
    if (cancelled(t)) { TerminateProcess(pi.hProcess, ERROR_CANCELLED); result = ERROR_CANCELLED; goto cleanup; }
    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        error = GetLastError(); TerminateProcess(pi.hProcess, error);
        log_error(L"启动子进程", error); goto cleanup;
    }
    CloseHandle(writepipe); writepipe = NULL;
    CloseHandle(nullin); nullin = INVALID_HANDLE_VALUE;
    CloseHandle(pi.hThread); pi.hThread = NULL;
    {
        char bytes[8192]; DWORD pending = 0, got;
        last_cr = FALSE;
        while (ReadFile(readpipe, bytes+pending, sizeof(bytes)-pending, &got, NULL) && got) {
            pending += got;
            pipe_log(bytes, &pending, FALSE);
        }
        pipe_log(bytes, &pending, TRUE);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (!GetExitCodeProcess(pi.hProcess, &result)) result = ERROR_GEN_FAILURE;
    goto cleanup;
too_long:
    log_text(L"命令行过长，请缩短文件路径或分区列表。\r\n");
    result = ERROR_FILENAME_EXCED_RANGE; goto cleanup;
failed:
    error = GetLastError(); log_error(L"运行工具", error); result = error;
cleanup:
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) { WaitForSingleObject(pi.hProcess, INFINITE); CloseHandle(pi.hProcess); }
    if (readpipe) CloseHandle(readpipe);
    if (writepipe) CloseHandle(writepipe);
    if (nullin != INVALID_HANDLE_VALUE) CloseHandle(nullin);
    free(cmd);
    return result;
}
static BOOL ensure_directory(const wchar_t *path) {
    int r = SHCreateDirectoryExW(NULL, path, NULL);
    if ((r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS) && exists(path, TRUE)) return TRUE;
    log_error(L"创建输出目录", (DWORD)r); return FALSE;
}
static unsigned __stdcall task_worker(void *param) {
    Task *t = param;
    DWORD rc = ERROR_GEN_FAILURE;
    const wchar_t *args[12]; int n = 0;
    if (t->kind == TASK_UNPACK) {
        if (ensure_directory(t->output)) {
            args[n++] = L"--out"; args[n++] = t->output;
            if (*t->partitions) { args[n++] = L"--partitions"; args[n++] = t->partitions; }
            if (*t->workers) { args[n++] = L"--workers"; args[n++] = t->workers; }
            args[n++] = t->input;
            rc = run_tool(t, dumper_path, args, n, base_dir);
        }
    } else if (t->kind == TASK_METADATA) {
        /* Metadata mode writes a file: isolate its working directory as well. */
        if (ensure_directory(t->output)) {
            log_text(L"\r\n──── OTA 元数据 ────\r\n");
            const wchar_t *meta[] = {L"--metadata", L"--out", t->output, t->input};
            rc = run_tool(t, dumper_path, meta, 4, t->output);
            if (!cancelled(t)) {
                log_text(L"\r\n──── 内核版本 ────\r\n");
                const wchar_t *kernel[] = {L"--kernel-version", L"--out", t->output, t->input};
                DWORD kr = run_tool(t, dumper_path, kernel, 4, t->output);
                if (kr) rc = kr;
            }
        }
    } else if (t->kind == TASK_LIST) {
        /* The dumper creates --out even for a list query. Isolate this side
         * effect instead of creating an empty output folder next to the app. */
        wchar_t tempdir[PATH_CAP], scratch[PATH_CAP];
        DWORD len = GetTempPathW(PATH_CAP, tempdir);
        if (!len || len >= PATH_CAP || !GetTempFileNameW(tempdir,L"pdl",0,scratch)) {
            log_error(L"创建查询临时目录",GetLastError()); goto finished;
        }
        if (!DeleteFileW(scratch) || !CreateDirectoryW(scratch,NULL)) {
            log_error(L"创建查询临时目录",GetLastError()); goto finished;
        }
        const wchar_t *list[] = {L"--list", L"--out", scratch, t->input};
        rc = run_tool(t, dumper_path, list, 4, scratch);
        if (!RemoveDirectoryW(scratch)) log_error(L"清理查询临时目录",GetLastError());
    } else {
        wchar_t tempdir[PATH_CAP], tempfile[PATH_CAP], image[PATH_CAP];
        DWORD len = GetTempPathW(PATH_CAP, tempdir);
        tempfile[0] = 0;
        if (!len || len >= PATH_CAP || !GetTempFileNameW(tempdir, L"pda", 0, tempfile)) {
            log_error(L"创建临时路径", GetLastError()); goto finished;
        }
        /* GetTempFileName reserves a unique name belonging to this task. */
        if (!DeleteFileW(tempfile) || !CreateDirectoryW(tempfile, NULL)) {
            log_error(L"创建 ARB 临时目录", GetLastError()); goto finished;
        }
        log_text(L"\r\n──── 提取 xbl_config 并检查 ARB ────\r\n");
        const wchar_t *extract[] = {L"--partitions", L"xbl_config", L"--out", tempfile, t->input};
        rc = run_tool(t, dumper_path, extract, 5, tempfile);
        if (!rc && !cancelled(t)) {
            if (!join_path(image, PATH_CAP, tempfile, L"xbl_config.img") || !exists(image, FALSE)) {
                log_text(L"未找到 xbl_config.img；该固件可能不含此分区。\r\n"); rc = ERROR_FILE_NOT_FOUND;
            } else {
                const wchar_t *arbargs[] = {image};
                rc = run_tool(t, arb_path, arbargs, 1, tempfile);
            }
        }
        /* Delete only the expected task-owned file, never recursively delete. */
        if (join_path(image, PATH_CAP, tempfile, L"xbl_config.img")) DeleteFileW(image);
        if (!RemoveDirectoryW(tempfile)) {
            log_text(L"临时目录未能清空，保留于："); log_text(tempfile); log_text(L"\r\n");
        }
    }
finished:
    if (cancelled(t)) { rc = ERROR_CANCELLED; log_text(L"\r\n任务已取消。解包目录中可能保留未完成的镜像。\r\n"); }
    else if (!rc) log_text(L"\r\n任务完成。\r\n");
    else {
        wchar_t msg[128]; _snwprintf(msg, 128, L"\r\n任务失败，退出码：%lu。请查看上方日志。\r\n", rc); log_text(msg);
    }
    PostMessageW(window, WM_DONE, rc, 0);
    return 0;
}
static void trim(wchar_t *s) {
    wchar_t *p = s; while (iswspace(*p)) ++p;
    if (p != s) memmove(s, p, (wcslen(p)+1)*sizeof(wchar_t));
    size_t len = wcslen(s); while (len && iswspace(s[len-1])) s[--len] = 0;
    if (len >= 2 && s[0] == L'"' && s[len-1] == L'"') {
        memmove(s, s+1, (len-2)*sizeof(wchar_t)); s[len-2] = 0;
    }
}
static BOOL is_url(const wchar_t *s) { return !_wcsnicmp(s, L"https://", 8) || !_wcsnicmp(s, L"http://", 7); }
static void default_output(void) {
    if (GetWindowTextLengthW(output)) return;
    wchar_t source[PATH_CAP], name[300] = L"output", path[PATH_CAP];
    GetWindowTextW(input, source, PATH_CAP); trim(source);
    if (*source && !is_url(source)) {
        const wchar_t *base = PathFindFileNameW(source);
        if (wcslen(base) < 240) {
            wcscpy(name, L"output_"); wcscat(name, base);
            wchar_t *dot = wcsrchr(name, L'.'); if (dot) *dot = 0;
        }
    }
    if (join_path(path, PATH_CAP, base_dir, name)) SetWindowTextW(output, path);
}
static void set_input(const wchar_t *path) { SetWindowTextW(input, path); default_output(); }
static void set_busy(BOOL busy) {
    HWND controls[] = {input, output, partitions, workers, filebtn, folderbtn, unpackbtn, metadatabtn, arbbtn, listbtn};
    for (size_t i=0; i<sizeof(controls)/sizeof(controls[0]); ++i) EnableWindow(controls[i], !busy);
    EnableWindow(cancelbtn, busy);
    SendMessageW(progress, PBM_SETMARQUEE, busy, 30);
    updater_task_running(busy);
}
static void start_task(int kind) {
    if (active) return;
    Task *t = calloc(1, sizeof(*t));
    if (!t) { MessageBoxW(window, L"内存不足。", L"无法开始", MB_ICONERROR); return; }
    t->kind = kind;
    GetWindowTextW(input, t->input, PATH_CAP); trim(t->input);
    if (!*t->input || (!is_url(t->input) && !exists(t->input, FALSE))) {
        MessageBoxW(window, L"请选择有效的 payload.bin / OTA ZIP 文件，或输入 HTTP/HTTPS 链接。", L"检查输入", MB_ICONWARNING); goto reject;
    }
    if (!is_url(t->input)) {
        wchar_t full[PATH_CAP]; DWORD len = GetFullPathNameW(t->input, PATH_CAP, full, NULL);
        if (!len || len >= PATH_CAP) goto path_error;
        wcscpy(t->input, full);
    }
    if (!exists(dumper_path, FALSE) || (kind == TASK_ARB && !exists(arb_path, FALSE))) {
        MessageBoxW(window, L"缺少配套工具。请保持 bin\\payload_dumper.exe、bin\\arbextract.exe 与主程序的目录结构完整。", L"缺少工具", MB_ICONERROR); goto reject;
    }
    default_output();
    GetWindowTextW(output, t->output, PATH_CAP); trim(t->output);
    if (kind == TASK_UNPACK || kind == TASK_METADATA) {
        wchar_t full[PATH_CAP]; DWORD len = GetFullPathNameW(t->output, PATH_CAP, full, NULL);
        if (!len || len >= PATH_CAP) goto path_error;
        wcscpy(t->output, full); SetWindowTextW(output, full);
    }
    GetWindowTextW(partitions, t->partitions, 4096); trim(t->partitions);
    /* Accept spaces around comma-separated names. Reject ambiguous whitespace. */
    wchar_t *p = t->partitions, *q = p;
    while (*p) { if (!iswspace(*p)) *q++ = *p; ++p; } *q = 0;
    GetWindowTextW(workers, t->workers, 16); trim(t->workers);
    if (kind == TASK_UNPACK && *t->workers) {
        wchar_t *end; long count = wcstol(t->workers, &end, 10);
        if (*end || count < 1 || count > 256) {
            MessageBoxW(window, L"线程数应为 1–256，留空则由工具自动选择。", L"检查线程数", MB_ICONWARNING); goto reject;
        }
    }
    t->cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    t->job = CreateJobObjectW(NULL, NULL);
    if (!t->cancel || !t->job) goto handle_error;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji = {0};
    ji.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(t->job, JobObjectExtendedLimitInformation, &ji, sizeof(ji))) goto handle_error;
    active = t; set_busy(TRUE); SetWindowTextW(status, L"正在处理…");
    worker_handle = (HANDLE)_beginthreadex(NULL, 0, task_worker, t, 0, NULL);
    if (!worker_handle) {
        active = NULL; set_busy(FALSE); SetWindowTextW(status, L"无法启动任务"); goto handle_error;
    }
    return;
path_error:
    MessageBoxW(window, L"无法解析路径，或路径过长。", L"检查路径", MB_ICONERROR); goto reject;
handle_error:
    MessageBoxW(window, L"无法创建后台任务。", L"启动失败", MB_ICONERROR);
reject:
    if (t->cancel) CloseHandle(t->cancel);
    if (t->job) CloseHandle(t->job);
    free(t);
}
static void cancel_task(void) {
    if (!active) return;
    SetEvent(active->cancel); TerminateJobObject(active->job, ERROR_CANCELLED);
    EnableWindow(cancelbtn, FALSE); SetWindowTextW(status, L"正在取消…");
}
static void browse_file(void) {
    wchar_t path[PATH_CAP] = L"";
    OPENFILENAMEW ofn = {0}; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = window;
    ofn.lpstrFile = path; ofn.nMaxFile = PATH_CAP;
    ofn.lpstrFilter = L"OTA / Payload (*.zip;*.bin)\0*.zip;*.bin\0所有文件\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) set_input(path);
}
static void browse_folder(void) {
    BROWSEINFOW bi = {0}; bi.hwndOwner = window; bi.lpszTitle = L"选择解包输出目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH]; if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(output, path);
        CoTaskMemFree(pidl);
    }
}
static HWND control(const wchar_t *cls, const wchar_t *text, DWORD style, int id, DWORD ex) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
        0,0,0,0,window,(HMENU)(INT_PTR)id,instance,NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)ui_font, TRUE); return h;
}
static void move(HWND h, int x, int y, int w, int height) { MoveWindow(h, px(x),px(y),px(w),px(height),TRUE); }
static void layout(void) {
    RECT r; GetClientRect(window, &r);
    int w = MulDiv(r.right,96,dpi), h = MulDiv(r.bottom,96,dpi), field = w-230;
    move(labels[0],24,29,100,24); move(input,128,24,field,30); move(filebtn,w-90,24,66,30);
    move(labels[1],24,73,100,24); move(output,128,68,field,30); move(folderbtn,w-90,68,66,30);
    move(labels[2],24,117,100,24); move(partitions,128,112,w-386,30);
    move(labels[3],w-238,117,54,24); move(workers,w-178,112,154,30);
    move(hint,128,148,w-152,24);
    move(unpackbtn,24,188,108,34); move(listbtn,144,188,108,34);
    move(metadatabtn,264,188,154,34); move(arbbtn,430,188,108,34);
    move(cancelbtn,w-108,188,84,34);
    move(logbox,24,240,w-48,h-322);
    move(clearbtn,24,h-65,88,30); move(openbtn,124,h-65,132,30);
    move(updatebtn,268,h-65,112,30);
    move(progress,w-146,h-59,122,16); move(status,398,h-61,w-558,24);
}
static void fonts(void) {
    if (ui_font) DeleteObject(ui_font);
    if (mono_font) DeleteObject(mono_font);
    ui_font = CreateFontW(-px(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    mono_font = CreateFontW(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Consolas");
    if (window) {
        for (HWND child = GetWindow(window,GW_CHILD); child; child=GetWindow(child,GW_HWNDNEXT))
            SendMessageW(child,WM_SETFONT,(WPARAM)ui_font,TRUE);
        SendMessageW(logbox,WM_SETFONT,(WPARAM)mono_font,TRUE);
    }
}
static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        window = hwnd;
        const wchar_t *names[] = {L"文件 / 链接",L"输出目录",L"分区",L"线程数"};
        for (int i=0;i<4;++i) labels[i]=control(L"STATIC",names[i],0,0,0);
        input=control(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,ID_INPUT,WS_EX_CLIENTEDGE);
        output=control(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,ID_OUTPUT,WS_EX_CLIENTEDGE);
        partitions=control(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,ID_PARTITIONS,WS_EX_CLIENTEDGE);
        workers=control(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL|ES_NUMBER,ID_WORKERS,WS_EX_CLIENTEDGE);
        SendMessageW(input,EM_SETLIMITTEXT,PATH_CAP-1,0); SendMessageW(output,EM_SETLIMITTEXT,PATH_CAP-1,0);
        SendMessageW(partitions,EM_SETLIMITTEXT,4095,0); SendMessageW(workers,EM_SETLIMITTEXT,3,0);
        SendMessageW(input,EM_SETCUEBANNER,FALSE,(LPARAM)L"拖入 payload.bin / OTA ZIP，或粘贴 HTTP/HTTPS 链接");
        SendMessageW(partitions,EM_SETCUEBANNER,FALSE,(LPARAM)L"留空提取全部，例如 boot,init_boot,vendor_boot");
        SendMessageW(workers,EM_SETCUEBANNER,FALSE,(LPARAM)L"留空=自动");
        hint=control(L"STATIC",L"分区用英文逗号分隔；输出目录留空时自动创建。",0,0,0);
        filebtn=control(L"BUTTON",L"浏览…",WS_TABSTOP,ID_FILE,0);
        folderbtn=control(L"BUTTON",L"浏览…",WS_TABSTOP,ID_FOLDER,0);
        unpackbtn=control(L"BUTTON",L"开始解包",WS_TABSTOP|BS_DEFPUSHBUTTON,ID_UNPACK,0);
        listbtn=control(L"BUTTON",L"列出分区",WS_TABSTOP,ID_LIST,0);
        metadatabtn=control(L"BUTTON",L"元数据 / 内核版本",WS_TABSTOP,ID_METADATA,0);
        arbbtn=control(L"BUTTON",L"检查 ARB",WS_TABSTOP,ID_ARB,0);
        cancelbtn=control(L"BUTTON",L"取消任务",WS_TABSTOP,ID_CANCEL,0);
        logbox=control(L"RICHEDIT50W",L"",WS_TABSTOP|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL,0,WS_EX_CLIENTEDGE);
        SendMessageW(logbox,EM_EXLIMITTEXT,0,LOG_LIMIT+65536);
        SendMessageW(logbox,EM_SETBKGNDCOLOR,0,RGB(248,250,252));
        clearbtn=control(L"BUTTON",L"清空日志",WS_TABSTOP,ID_CLEAR,0);
        openbtn=control(L"BUTTON",L"打开输出目录",WS_TABSTOP,ID_OPEN,0);
        updatebtn=control(L"BUTTON",L"检查更新",WS_TABSTOP,ID_UPDATE,0);
        status=control(L"STATIC",L"就绪",0,0,0);
        progress=control(PROGRESS_CLASSW,L"",PBS_MARQUEE,0,0);
        fonts(); set_busy(FALSE); layout(); DragAcceptFiles(hwnd,TRUE);
        return 0;
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lp)->ptMinTrackSize.x=px(800);
        ((MINMAXINFO*)lp)->ptMinTrackSize.y=px(485); return 0;
    case WM_DPICHANGED: {
        dpi=HIWORD(wp); fonts(); RECT *r=(RECT*)lp;
        SetWindowPos(hwnd,NULL,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
        layout(); return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp,RGB(40,52,65)); SetBkColor((HDC)wp,RGB(255,255,255)); return (LRESULT)white;
    case WM_DRAWITEM:
        if (updater_draw_button((const DRAWITEMSTRUCT*)lp)) return TRUE;
        break;
    case WM_DROPFILES: {
        HDROP drop=(HDROP)wp;
        if (!active) {
            wchar_t path[PATH_CAP];
            if (DragQueryFileW(drop,0,path,PATH_CAP) && exists(path,FALSE)) set_input(path);
        }
        DragFinish(drop); return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_FILE: if (!active) browse_file(); break;
        case ID_FOLDER: if (!active) browse_folder(); break;
        case ID_UNPACK: start_task(TASK_UNPACK); break;
        case ID_METADATA: start_task(TASK_METADATA); break;
        case ID_ARB: start_task(TASK_ARB); break;
        case ID_LIST: start_task(TASK_LIST); break;
        case ID_CANCEL: cancel_task(); break;
        case ID_CLEAR: SetWindowTextW(logbox,L""); break;
        case ID_UPDATE: updater_check(TRUE); break;
        case ID_OPEN: {
            wchar_t path[PATH_CAP]; GetWindowTextW(output,path,PATH_CAP); trim(path);
            if (exists(path,TRUE)) ShellExecuteW(hwnd,L"open",path,NULL,NULL,SW_SHOWNORMAL);
            else MessageBoxW(hwnd,L"输出目录尚未创建。",L"打开目录",MB_ICONINFORMATION);
            break;
        }
        } return 0;
    case WM_LOG: append_log((const wchar_t*)lp); return 0;
    case WM_DONE:
        if (worker_handle) { WaitForSingleObject(worker_handle,INFINITE); CloseHandle(worker_handle); worker_handle=NULL; }
        CloseHandle(active->job); CloseHandle(active->cancel); free(active); active=NULL;
        set_busy(FALSE);
        SetWindowTextW(status,wp==0 ? L"任务完成" : wp==ERROR_CANCELLED ? L"任务已取消" : L"任务失败，查看日志");
        if (close_pending && !updater_busy()) DestroyWindow(hwnd);
        return 0;
    case WM_TIMER:
        if (wp==1) { KillTimer(hwnd,1); updater_check(FALSE); }
        return 0;
    case WM_UPDATE_EVENT:
        updater_event(wp,lp);
        if (close_pending && !active && !updater_busy()) DestroyWindow(hwnd);
        return 0;
    case WM_CLOSE:
        close_pending=TRUE; KillTimer(hwnd,1); updater_stop();
        if (active) cancel_task();
        if (!active && !updater_busy()) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: updater_destroy(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev; (void)cmdline;
    instance=inst;
    HMODULE user=GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI *DpiFunc)(HANDLE);
    DpiFunc setdpi=(DpiFunc)(void*)GetProcAddress(user,"SetProcessDpiAwarenessContext");
    if (setdpi) setdpi((HANDLE)(INT_PTR)-4); else SetProcessDPIAware();
    HDC screen=GetDC(NULL); dpi=(UINT)GetDeviceCaps(screen,LOGPIXELSX); ReleaseDC(NULL,screen);
    DWORD len=GetModuleFileNameW(NULL,base_dir,PATH_CAP);
    if (!len || len>=PATH_CAP || !PathRemoveFileSpecW(base_dir)) return 1;
    if (!join_path(dumper_path,PATH_CAP,base_dir,L"bin\\payload_dumper.exe") ||
        !join_path(arb_path,PATH_CAP,base_dir,L"bin\\arbextract.exe")) return 1;
    CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    HMODULE rich=LoadLibraryW(L"Msftedit.dll");
    if (!rich) { MessageBoxW(NULL,L"无法加载 Windows RichEdit。",L"启动失败",MB_ICONERROR); CoUninitialize(); return 1; }
    INITCOMMONCONTROLSEX ic={sizeof(ic),ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES}; InitCommonControlsEx(&ic);
    white=CreateSolidBrush(RGB(255,255,255)); fonts();
    WNDCLASSEXW wc={0}; wc.cbSize=sizeof(wc); wc.hInstance=instance; wc.lpfnWndProc=wndproc;
    wc.hCursor=LoadCursorW(NULL,IDC_ARROW); wc.hbrBackground=white; wc.lpszClassName=L"PayloadDumperGuiC";
    wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(IDI_APP)); wc.hIconSm=wc.hIcon;
    if (!RegisterClassExW(&wc)) return 1;
    HWND main_window=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,L"Payload Dumper",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,px(980),px(645),NULL,NULL,instance,NULL);
    if (!main_window || !input || !output || !logbox || !progress) {
        MessageBoxW(NULL,L"无法创建窗口控件。",L"启动失败",MB_ICONERROR); return 1;
    }
    int argc; wchar_t **argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if (argv) { if (argc>1 && (is_url(argv[1]) || exists(argv[1],FALSE))) set_input(argv[1]); LocalFree(argv); }
    updater_init(main_window,instance,updatebtn,base_dir,dpi);
    SetTimer(main_window,1,2200,NULL);
    ShowWindow(main_window,show); UpdateWindow(main_window);
    MSG msg;
    while (GetMessageW(&msg,NULL,0,0)>0) if (!IsDialogMessageW(main_window,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    DeleteObject(ui_font); DeleteObject(mono_font); DeleteObject(white);
    FreeLibrary(rich); CoUninitialize();
    return (int)msg.wParam;
}
