/* Portable C updater. It never runs a shell, elevates, touches ROM outputs or
 * accepts paths from inside the ZIP. Only the exact package file list is used. */
#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include "update_core.h"
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static BOOL atomic_copy(const wchar_t *source,const wchar_t *dest) {
    wchar_t temp[UP_PATH]; size_t len=wcslen(dest);
    if (len+32>=UP_PATH) return FALSE;
    wcscpy(temp,dest); _snwprintf(temp+len,32,L".pdu-%lu.new",GetCurrentProcessId());
    if (!CopyFileW(source,temp,TRUE)) return FALSE;
    BOOL ok=MoveFileExW(temp,dest,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(temp);
    return ok;
}
static BOOL apply_package(const wchar_t *stage,const wchar_t *target,const wchar_t *backup,BOOL *rollback_ok) {
    BOOL existed[16]={0}; size_t applied=0; wchar_t source[UP_PATH],dest[UP_PATH],saved[UP_PATH];
    *rollback_ok=TRUE;
    if (!CreateDirectoryW(backup,NULL)) return FALSE;
    if (!up_join(saved,backup,L"bin") || !CreateDirectoryW(saved,NULL) ||
        !up_join(saved,backup,L"licenses") || !CreateDirectoryW(saved,NULL)) return FALSE;
    /* Prepare every backup before modifying any installed file. */
    for (size_t i=0;i<up_package_file_count;++i) {
        if (!up_join(dest,target,up_package_files[i]) || !up_join(saved,backup,up_package_files[i])) return FALSE;
        DWORD attr=GetFileAttributesW(dest);
        if (attr!=INVALID_FILE_ATTRIBUTES) {
            if ((attr&FILE_ATTRIBUTE_DIRECTORY) || !CopyFileW(dest,saved,TRUE)) return FALSE;
            existed[i]=TRUE;
        }
    }
    if (!up_join(dest,target,L"bin") || (!CreateDirectoryW(dest,NULL) && GetLastError()!=ERROR_ALREADY_EXISTS)) return FALSE;
    if (!up_join(dest,target,L"licenses") || (!CreateDirectoryW(dest,NULL) && GetLastError()!=ERROR_ALREADY_EXISTS)) return FALSE;
    for (size_t i=0;i<up_package_file_count;++i) {
        if (!up_join(source,stage,up_package_files[i]) || !up_join(dest,target,up_package_files[i]) || !atomic_copy(source,dest)) goto rollback;
        ++applied;
    }
    return TRUE;
rollback:
    while (applied) {
        --applied;
        up_join(dest,target,up_package_files[applied]);
        if (existed[applied]) {
            up_join(saved,backup,up_package_files[applied]);
            if (!atomic_copy(saved,dest)) *rollback_ok=FALSE;
        } else if (!DeleteFileW(dest)) *rollback_ok=FALSE;
    }
    return FALSE;
}
static void cleanup_files(const wchar_t *dir) {
    wchar_t file[UP_PATH];
    for (size_t i=0;i<up_package_file_count;++i) if (up_join(file,dir,up_package_files[i])) DeleteFileW(file);
    if (up_join(file,dir,L"bin")) RemoveDirectoryW(file);
    if (up_join(file,dir,L"licenses")) RemoveDirectoryW(file);
    RemoveDirectoryW(dir);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR command,int show) {
    (void)instance; (void)previous; (void)command; (void)show;
    int argc; wchar_t **args=CommandLineToArgvW(GetCommandLineW(),&argc);
    if (!args || argc!=6) {
        MessageBoxW(NULL,L"请通过 Payload Dumper 的“检查更新”入口运行更新器。",L"软件更新",MB_ICONINFORMATION);
        if (args) LocalFree(args);
        return 2;
    }
    wchar_t *end; unsigned long pid=wcstoul(args[1],&end,10);
    if (!pid || *end || pid==GetCurrentProcessId() || wcslen(args[4])!=64) { LocalFree(args); return 2; }
    uint64_t version=wcstoull(args[5],&end,10);
    if (!version || *end || version>MAXDWORD) { LocalFree(args); return 2; }
    wchar_t zip[UP_PATH],stage[UP_PATH],backup[UP_PATH],main_exe[UP_PATH]; char expected[65],actual[65];
    if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,args[4],-1,expected,65,NULL,NULL) ||
        !up_join(zip,args[3],L"update.zip") || !up_join(stage,args[3],L"stage") ||
        !up_join(backup,args[3],L"backup") || !up_join(main_exe,args[2],L"PayloadDumperGUI.exe")) { LocalFree(args); return 2; }
    BOOL ok=up_sha_file(zip,actual) && !strcmp(actual,expected) && CreateDirectoryW(stage,NULL) && up_extract_zip(zip,stage,version);
    if (!ok) {
        MessageBoxW(NULL,L"更新包校验或解压失败，原程序未被修改。",L"软件更新",MB_ICONERROR);
        cleanup_files(stage); LocalFree(args); return 1;
    }
    HANDLE parent=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,(DWORD)pid);
    if (parent) {
        wchar_t image[UP_PATH]; DWORD count=UP_PATH;
        ok=QueryFullProcessImageNameW(parent,0,image,&count) && !_wcsicmp(image,main_exe) && WaitForSingleObject(parent,30000)==WAIT_OBJECT_0;
        CloseHandle(parent);
    } else ok=GetLastError()==ERROR_INVALID_PARAMETER; /* Parent already exited. */
    BOOL can_restart=ok,rollback=TRUE;
    if (ok) ok=apply_package(stage,args[2],backup,&rollback);
    if (!ok) MessageBoxW(NULL,rollback?L"更新未能完成，原程序已保留或恢复。请关闭占用程序文件的应用后重试。":
        L"更新失败，部分文件无法恢复。请从 GitHub 下载完整便携包；备份保留在更新临时目录。",L"软件更新",MB_ICONERROR);
    if (ok || (rollback && can_restart)) {
        STARTUPINFOW si={0}; si.cb=sizeof(si); PROCESS_INFORMATION pi={0};
        wchar_t *cmd=calloc(UP_PATH,sizeof(wchar_t));
        if (cmd && up_quote(cmd,UP_PATH,main_exe) && CreateProcessW(main_exe,cmd,NULL,NULL,FALSE,0,NULL,args[2],&si,&pi)) {
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        }
        free(cmd);
    }
    if (ok) { cleanup_files(stage); cleanup_files(backup); DeleteFileW(zip); }
    LocalFree(args); return ok?0:1;
}
