/* Child-process fixture: interleaved streams, split UTF-8 and cancellable work. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
int main(void) {
    int argc; wchar_t **argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if (!argv) return 1;
    setvbuf(stdout,NULL,_IONBF,0); setvbuf(stderr,NULL,_IONBF,0);
    for (int i=1;i<argc;++i) {
        char buf[4096];
        WideCharToMultiByte(CP_UTF8,0,argv[i],-1,buf,sizeof(buf),NULL,NULL);
        printf("ARG[%d]=<%s>\n",i,buf);
    }
    for (int i=0;i<150;++i) {
        fprintf(stdout,"stdout-%d-abcdefghijklmnopqrstuvwxyz\n",i);
        fprintf(stderr,"stderr-%d-abcdefghijklmnopqrstuvwxyz\n",i);
    }
    /* Chinese text deliberately split between pipe writes. */
    fwrite("\xe4",1,1,stdout); Sleep(25);
    fwrite("\xb8\xad\xe6",1,3,stdout); Sleep(25);
    fwrite("\x96\x87\n",1,3,stdout);
    puts("PROGRESS_BEGIN");
    fputs("WRAPPED_OLD_PROGRESS_",stderr);
    for (int i=0;i<300;++i) fputc('X',stderr);
    fputc('\r',stderr); Sleep(25);
    for (int i=1;i<=250;++i) fprintf(stderr,"\r  %d/250 ops (%d%%)    ",i,i*100/250);
    fputs("\nPROGRESS_END\n",stderr);
    fputs("LONG_STATUS_TEXT_TO_BE_REPLACED\r",stderr); Sleep(25);
    fputs("OK\r",stderr); Sleep(25); fputs("\nAFTER_CRLF\n",stderr);
    BOOL slow=FALSE;
    for (int i=1;i<argc;++i) if (wcsstr(argv[i],L"/slow")) slow=TRUE;
    if (slow) { puts("PROBE_RUNNING"); Sleep(60000); }
    LocalFree(argv); return 0;
}
