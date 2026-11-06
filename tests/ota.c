#define wWinMain updater_production_entry
#include "../src/updater.c"
#undef wWinMain
static int failures;
static void check(BOOL ok,const char *name) {
    printf("%s %s\n",ok?"PASS":"FAIL",name); if (!ok) ++failures;
}
static char *json_file(const wchar_t *base,const wchar_t *name) {
    wchar_t path[UP_PATH]; if (!up_join(path,base,name)) return NULL;
    FILE *f=_wfopen(path,L"rb"); if (!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); rewind(f);
    if (n<0 || (unsigned long)n>UP_MAX_META) { fclose(f); return NULL; }
    char *data=calloc((size_t)n+1,1);
    if (data && fread(data,1,(size_t)n,f)!=(size_t)n) { free(data); data=NULL; }
    fclose(f); return data;
}
static BOOL old_files(const wchar_t *target,BOOL create) {
    wchar_t path[UP_PATH]; BOOL ok=TRUE;
    if (create) {
        CreateDirectoryW(target,NULL); up_join(path,target,L"bin"); CreateDirectoryW(path,NULL);
        up_join(path,target,L"licenses"); CreateDirectoryW(path,NULL);
    }
    for (size_t i=0;i<up_package_file_count;++i) {
        up_join(path,target,up_package_files[i]);
        FILE *f=_wfopen(path,create?L"wb":L"rb");
        if (!f) return FALSE;
        if (create) { if (fwrite("original",1,8,f)!=8) ok=FALSE; }
        else { char text[16]={0}; if (fread(text,1,sizeof(text),f)!=8 || strcmp(text,"original")) ok=FALSE; }
        fclose(f);
    }
    return ok;
}
int wmain(int argc,wchar_t **argv) {
    if (argc!=2) return 2;
    char hash[65];
    check(up_sha_memory("abc",3,hash) && !strcmp(hash,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),"SHA-256 known answer");
    char *text=json_file(argv[1],L"manifest.json");
    UpManifest *manifest=calloc(1,sizeof(*manifest));
    check(text && up_parse_manifest(text,strlen(text),manifest) && manifest->version==APP_VERSION_CODE && wcsstr(manifest->notes,L"中文")!=NULL,"strict architecture manifest and escaped Unicode");
    free(text);
    const wchar_t *bad[]={L"bad-hash.json",L"bad-http.json",L"bad-hosts.json",L"bad-forced.json",L"bad-channel.json",L"bad-duplicate.json",L"bad-trailing.json"};
    for (size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        text=json_file(argv[1],bad[i]); check(text && !up_parse_manifest(text,strlen(text),manifest),"reject malformed or unsafe update manifest"); free(text);
    }
    text=json_file(argv[1],L"pointer.json"); UpPointer p[7],chosen; BOOL valid[7]={0}; memset(p,0,sizeof(p));
    check(text && up_parse_pointer(text,strlen(text),&p[0]),"immutable authority pointer"); free(text);
    valid[0]=TRUE;
    check(up_select_pointer(p,valid,7,0,"",&chosen),"authority pointer accepted");
    check(!up_select_pointer(p,valid,7,p[0].revision+1,"",&chosen),"reject metadata rollback");
    valid[0]=FALSE; p[1]=p[0]; valid[1]=TRUE;
    check(!up_select_pointer(p,valid,7,0,"",&chosen),"single mirror cannot authorize update");
    p[2]=p[0]; valid[2]=TRUE;
    check(up_select_pointer(p,valid,7,0,"",&chosen),"matching mirror quorum accepted");
    p[2].sha256[0]=p[2].sha256[0]=='a'?'b':'a'; valid[0]=TRUE;
    check(!up_select_pointer(p,valid,7,0,"",&chosen),"same-revision conflict rejected even with authority");
    wchar_t local[128];
    check(up_local_time(L"2026-11-06T16:00:00Z",local,128) && !wcschr(local,L'T') && !wcschr(local,L'Z'),"publication time uses local presentation");
    check(!up_local_time(L"2026-99-99T88:00:00Z",local,128),"invalid date rejected");
    wchar_t zip[UP_PATH],stage[UP_PATH],target[UP_PATH],backup[UP_PATH],locked[UP_PATH];
    up_join(zip,argv[1],L"good.zip"); up_join(stage,argv[1],L"stage"); CreateDirectoryW(stage,NULL);
    check(up_extract_zip(zip,stage,APP_VERSION_CODE),"extract exact package and verify PE identity/version/architecture");
    up_join(target,argv[1],L"installed 中文"); check(old_files(target,TRUE),"prepare previous installation");
    up_join(locked,target,L"bin\\arbextract.exe");
    HANDLE lock=CreateFileW(locked,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    up_join(backup,argv[1],L"backup-failed"); BOOL rollback=FALSE;
    check(lock!=INVALID_HANDLE_VALUE && !apply_package(stage,target,backup,&rollback) && rollback,"locked companion file causes rollback");
    if (lock!=INVALID_HANDLE_VALUE) CloseHandle(lock);
    check(old_files(target,FALSE),"every modified file restored after failed update");
    up_join(backup,argv[1],L"backup-success");
    check(apply_package(stage,target,backup,&rollback),"install full package into Unicode directory");
    BOOL matches=TRUE;
    for (size_t i=0;i<up_package_file_count;++i) {
        wchar_t src[UP_PATH],dest[UP_PATH]; char a[65],b[65];
        up_join(src,stage,up_package_files[i]); up_join(dest,target,up_package_files[i]);
        if (!up_sha_file(src,a) || !up_sha_file(dest,b) || strcmp(a,b)) matches=FALSE;
    }
    check(matches,"installed bytes equal verified staging files");
    const wchar_t *bad_zip[]={L"traversal.zip",L"duplicate.zip",L"wrong-arch.zip",L"corrupt.zip"};
    for (size_t i=0;i<4;++i) {
        wchar_t dir[UP_PATH],name[32]; _snwprintf(name,32,L"bad-stage-%u",(unsigned)i);
        up_join(dir,argv[1],name); CreateDirectoryW(dir,NULL); up_join(zip,argv[1],bad_zip[i]);
        check(!up_extract_zip(zip,dir,APP_VERSION_CODE),"reject traversal/duplicate/architecture/corruption ZIP");
    }
    free(manifest); printf("RESULT: %d OTA failures\n",failures); return failures?1:0;
}
