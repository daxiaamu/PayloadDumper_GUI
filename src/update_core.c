#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include "update_core.h"
#include <bcrypt.h>
#include <winver.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#define JSMN_STATIC
#define JSMN_STRICT
#define JSMN_PARENT_LINKS
#include "../third_party/jsmn/jsmn.h"
#include "../third_party/miniz/miniz_tinfl.h"

const wchar_t *const up_package_files[] = {
    L"PayloadDumperGUI.exe", L"bin\\payload_dumper.exe", L"bin\\arbextract.exe", L"bin\\updater.exe",
    L"licenses\\arbextract-MIT.txt", L"licenses\\payload_dumper-GPL-3.0.txt",
    L"licenses\\miniz-MIT.txt", L"licenses\\jsmn-MIT.txt", L"README.md"
};
const size_t up_package_file_count = sizeof(up_package_files)/sizeof(up_package_files[0]);

BOOL up_join(wchar_t *out, const wchar_t *dir, const wchar_t *name) {
    size_t d=wcslen(dir),n=wcslen(name);
    BOOL sep=d && dir[d-1]!=L'\\' && dir[d-1]!=L'/';
    if (d+n+sep+1>UP_PATH) return FALSE;
    memmove(out,dir,d*sizeof(wchar_t));
    if (sep) out[d++]=L'\\';
    memcpy(out+d,name,(n+1)*sizeof(wchar_t)); return TRUE;
}
BOOL up_new_temp(wchar_t *dir) {
    wchar_t temp[UP_PATH]; DWORD n=GetTempPathW(UP_PATH,temp);
    if (!n || n>=UP_PATH || !GetTempFileNameW(temp,L"pdu",0,dir)) return FALSE;
    if (!DeleteFileW(dir)) return FALSE;
    return CreateDirectoryW(dir,NULL);
}
BOOL up_quote(wchar_t *cmd, size_t cap, const wchar_t *arg) {
    size_t p=wcslen(cmd);
    if (p+3>=cap) return FALSE;
    if (p) cmd[p++]=L' ';
    cmd[p++]=L'"';
    while (*arg) {
        size_t n=0; while (*arg==L'\\') { ++n; ++arg; }
        size_t count=n*((!*arg || *arg==L'"')?2:1);
        if (p+count+4>=cap) return FALSE;
        while (count--) cmd[p++]=L'\\';
        if (*arg==L'"') cmd[p++]=L'\\';
        if (*arg) cmd[p++]=*arg++;
    }
    cmd[p++]=L'"'; cmd[p]=0; return TRUE;
}
typedef struct { BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE hash; UCHAR *object; } Hash;
static void hash_free(Hash *h) {
    if (h->hash) BCryptDestroyHash(h->hash);
    if (h->alg) BCryptCloseAlgorithmProvider(h->alg,0);
    free(h->object);
}
static BOOL hash_begin(Hash *h) {
    DWORD n=0,got=0;
    memset(h,0,sizeof(*h));
    if (BCryptOpenAlgorithmProvider(&h->alg,BCRYPT_SHA256_ALGORITHM,NULL,0)<0 ||
        BCryptGetProperty(h->alg,BCRYPT_OBJECT_LENGTH,(PUCHAR)&n,sizeof(n),&got,0)<0 || !n) return FALSE;
    h->object=malloc(n);
    return h->object && BCryptCreateHash(h->alg,&h->hash,h->object,n,NULL,0,0)>=0;
}
static BOOL hash_end(Hash *h,char out[65]) {
    UCHAR digest[32]; static const char hex[]="0123456789abcdef";
    if (BCryptFinishHash(h->hash,digest,sizeof(digest),0)<0) return FALSE;
    for (int i=0;i<32;++i) { out[i*2]=hex[digest[i]>>4]; out[i*2+1]=hex[digest[i]&15]; }
    out[64]=0; return TRUE;
}
BOOL up_sha_memory(const void *data,size_t size,char out[65]) {
    Hash h; BOOL ok=hash_begin(&h);
    if (size>MAXDWORD) ok=FALSE;
    if (ok) ok=BCryptHashData(h.hash,(PUCHAR)data,(ULONG)size,0)>=0 && hash_end(&h,out);
    hash_free(&h); return ok;
}
BOOL up_sha_file(const wchar_t *path,char out[65]) {
    HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
    if (f==INVALID_HANDLE_VALUE) return FALSE;
    Hash h; BOOL ok=hash_begin(&h); UCHAR buf[32768]; DWORD got;
    while (ok) {
        if (!ReadFile(f,buf,sizeof(buf),&got,NULL)) { ok=FALSE; break; }
        if (!got) break;
        ok=BCryptHashData(h.hash,buf,got,0)>=0;
    }
    if (ok) ok=hash_end(&h,out);
    CloseHandle(f); hash_free(&h); return ok;
}
typedef struct { const char *s; jsmntok_t *t; int count; } Json;
static BOOL json_open(Json *j,const char *s,size_t len) {
    memset(j,0,sizeof(*j));
    if (!s || !len || len>UP_MAX_META || memchr(s,0,len)) return FALSE;
    j->t=calloc(8192,sizeof(jsmntok_t)); if (!j->t) return FALSE;
    jsmn_parser p; jsmn_init(&p); j->s=s; j->count=jsmn_parse(&p,s,len,j->t,8192);
    if (j->count<1 || j->t[0].type!=JSMN_OBJECT) goto bad;
    for (size_t i=(size_t)j->t[0].end;i<len;++i)
        if (s[i]!=' ' && s[i]!='\r' && s[i]!='\n' && s[i]!='\t') goto bad;
    return TRUE;
bad: free(j->t); j->t=NULL; return FALSE;
}
static int hex4(const char *s) {
    int n=0;
    for (int i=0;i<4;++i) {
        int v=s[i]>='0'&&s[i]<='9'?s[i]-'0':s[i]>='a'&&s[i]<='f'?s[i]-'a'+10:s[i]>='A'&&s[i]<='F'?s[i]-'A'+10:-1;
        if (v<0) return -1;
        n=n*16+v;
    }
    return n;
}
static BOOL json_string(Json *j,int index,wchar_t *out,size_t cap) {
    if (index<0 || index>=j->count || j->t[index].type!=JSMN_STRING || !cap) return FALSE;
    const char *s=j->s+j->t[index].start,*end=j->s+j->t[index].end;
    size_t max=(size_t)(end-s)+1,n=0;
    char *utf8=malloc(max); if (!utf8) return FALSE;
    BOOL ok=FALSE;
    while (s<end) {
        unsigned c=(unsigned char)*s++;
        if (c<32) goto done;
        if (c!='\\') { utf8[n++]=(char)c; continue; }
        if (s==end) goto done;
        char esc=*s++;
        if (esc=='u') {
            if (end-s<4) goto done;
            int u=hex4(s); s+=4; if (u<0 || !u) goto done;
            if (u>=0xd800 && u<=0xdbff) {
                if (end-s<6 || s[0]!='\\' || s[1]!='u') goto done;
                int low=hex4(s+2); if (low<0xdc00 || low>0xdfff) goto done;
                u=0x10000+(u-0xd800)*1024+low-0xdc00; s+=6;
            } else if (u>=0xdc00 && u<=0xdfff) goto done;
            if (u<0x80) utf8[n++]=(char)u;
            else if (u<0x800) { utf8[n++]=(char)(0xc0|(u>>6)); utf8[n++]=(char)(0x80|(u&63)); }
            else if (u<0x10000) { utf8[n++]=(char)(0xe0|(u>>12)); utf8[n++]=(char)(0x80|((u>>6)&63)); utf8[n++]=(char)(0x80|(u&63)); }
            else { utf8[n++]=(char)(0xf0|(u>>18)); utf8[n++]=(char)(0x80|((u>>12)&63)); utf8[n++]=(char)(0x80|((u>>6)&63)); utf8[n++]=(char)(0x80|(u&63)); }
        } else {
            switch (esc) {
            case '"': case '\\': case '/': utf8[n++]=esc; break;
            case 'b': utf8[n++]='\b'; break; case 'f': utf8[n++]='\f'; break;
            case 'n': utf8[n++]='\n'; break; case 'r': utf8[n++]='\r'; break; case 't': utf8[n++]='\t'; break;
            default: goto done;
            }
        }
    }
    utf8[n]=0;
    ok=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,utf8,-1,out,(int)cap)>0;
done: free(utf8); return ok;
}
static int next_token(Json *j,int i) {
    int end=j->t[i].end; ++i;
    while (i<j->count && j->t[i].start<end) ++i;
    return i;
}
static int get(Json *j,int object,const wchar_t *key) {
    if (object<0 || object>=j->count || j->t[object].type!=JSMN_OBJECT) return -1;
    int found=-1;
    for (int i=object+1;i<j->count && j->t[i].start<j->t[object].end;) {
        wchar_t name[128];
        if (!json_string(j,i,name,128) || i+1>=j->count) return -1;
        if (!wcscmp(name,key)) { if (found!=-1) return -1; found=i+1; }
        i=next_token(j,i+1);
    }
    return found;
}
static BOOL number(Json *j,int i,uint64_t *out) {
    if (i<0 || i>=j->count || j->t[i].type!=JSMN_PRIMITIVE) return FALSE;
    const char *s=j->s+j->t[i].start,*end=j->s+j->t[i].end; uint64_t n=0;
    if (s==end || (end-s>1 && *s=='0')) return FALSE;
    while (s<end) {
        if (*s<'0'||*s>'9'||n>(UINT64_MAX-(unsigned)(*s-'0'))/10) return FALSE;
        n=n*10+(unsigned)(*s++-'0');
    }
    *out=n; return TRUE;
}
static BOOL digest(Json *j,int i,char out[65]) {
    wchar_t s[65]; if (!json_string(j,i,s,65) || wcslen(s)!=64) return FALSE;
    for (int n=0;n<64;++n) { if (!((s[n]>=L'0'&&s[n]<=L'9')||(s[n]>=L'a'&&s[n]<=L'f'))) return FALSE; out[n]=(char)s[n]; }
    out[64]=0; return TRUE;
}
static BOOL schema(Json *j) {
    uint64_t n; wchar_t channel[32];
    return number(j,get(j,0,L"schemaVersion"),&n) && n==1 &&
        json_string(j,get(j,0,L"channel"),channel,32) && !wcscmp(channel,APP_CHANNEL_W);
}
static BOOL https_host(const wchar_t *url,wchar_t *host,size_t cap) {
    if (_wcsnicmp(url,L"https://",8)) return FALSE;
    const wchar_t *start=url+8,*p=start;
    while (*p && *p!=L'/') {
        if (!(iswalnum(*p) || *p==L'.' || *p==L'-') || *p>127) return FALSE;
        ++p;
    }
    size_t n=(size_t)(p-start);
    if (!n || n>=cap || *p!=L'/') return FALSE;
    memcpy(host,start,n*sizeof(wchar_t)); host[n]=0; return TRUE;
}
BOOL up_parse_pointer(const char *json,size_t len,UpPointer *out) {
    Json j; if (!json_open(&j,json,len)) return FALSE;
    memset(out,0,sizeof(*out));
    BOOL ok=schema(&j) && number(&j,get(&j,0,L"policyRevision"),&out->revision) && out->revision>0 &&
        digest(&j,get(&j,0,L"manifestSha256"),out->sha256) &&
        json_string(&j,get(&j,0,L"manifestUrl"),out->manifest_url,UP_URL);
    const wchar_t *prefix=L"https://raw.githubusercontent.com/daxiaamu/PayloadDumper_GUI/main/updates/manifests/";
    if (ok) {
        char tail[128]; wchar_t wide[128];
        snprintf(tail,sizeof(tail),"%llu-%s.json",(unsigned long long)out->revision,out->sha256);
        MultiByteToWideChar(CP_UTF8,0,tail,-1,wide,128);
        ok=!wcsncmp(out->manifest_url,prefix,wcslen(prefix)) && !wcscmp(out->manifest_url+wcslen(prefix),wide);
    }
    free(j.t); return ok;
}
BOOL up_parse_manifest(const char *json,size_t len,UpManifest *out) {
    Json j; if (!json_open(&j,json,len)) return FALSE;
    memset(out,0,sizeof(*out)); wchar_t id[128];
    uint64_t size=0;
    int packages=get(&j,0,L"packages"), package=get(&j,packages,UP_ARCH_W);
    BOOL ok=schema(&j) && json_string(&j,get(&j,0,L"appId"),id,128) && !wcscmp(id,L"daxiaamu.PayloadDumperGUI") &&
        number(&j,get(&j,0,L"policyRevision"),&out->revision) && out->revision>0 &&
        number(&j,get(&j,0,L"versionCode"),&out->version) && out->version>0 && out->version<=MAXDWORD &&
        number(&j,get(&j,0,L"maxForcedVersionCode"),&out->forced) && out->forced<out->version &&
        json_string(&j,get(&j,0,L"versionName"),out->version_name,64) && *out->version_name &&
        json_string(&j,get(&j,0,L"releaseUrl"),out->release_url,UP_URL) &&
        digest(&j,get(&j,package,L"sha256"),out->sha256) &&
        number(&j,get(&j,package,L"size"),&size) && size>0 && size<=UP_MAX_ZIP;
    const wchar_t *release_prefix=L"https://github.com/daxiaamu/PayloadDumper_GUI/releases/tag/";
    if (ok && wcsncmp(out->release_url,release_prefix,wcslen(release_prefix))) ok=FALSE;
    out->size=(DWORD)size;
    int i=get(&j,0,L"publishedAt");
    if (i>=0 && !json_string(&j,i,out->published,64)) out->published[0]=0;
    i=get(&j,0,L"changelog");
    if (i>=0 && !json_string(&j,i,out->notes,UP_MAX_NOTES)) ok=FALSE;
    int list=get(&j,package,L"urls");
    if (list<0 || j.t[list].type!=JSMN_ARRAY) ok=FALSE;
    wchar_t hosts[UP_MAX_URLS][256];
    if (ok) for (i=list+1;i<j.count && j.t[i].start<j.t[list].end;i=next_token(&j,i)) {
        int n=out->url_count;
        if (n>=UP_MAX_URLS || !json_string(&j,i,out->urls[n],UP_URL) || !https_host(out->urls[n],hosts[n],256)) { ok=FALSE; break; }
        for (int k=0;k<n;++k) if (!_wcsicmp(hosts[k],hosts[n])) ok=FALSE;
        ++out->url_count;
    }
    if (out->url_count<6) ok=FALSE; /* Five mirrors plus official GitHub. */
    free(j.t); return ok;
}
BOOL up_select_pointer(const UpPointer *p,const BOOL *valid,size_t count,uint64_t floor,
                       const char *accepted_hash,UpPointer *out) {
    int chosen=-1;
    if (count && valid[0]) chosen=0;
    else for (size_t i=1;i<count;++i) if (valid[i]) {
        unsigned matches=0;
        for (size_t k=1;k<count;++k) if (valid[k] && p[k].revision==p[i].revision &&
            !strcmp(p[k].sha256,p[i].sha256) && !wcscmp(p[k].manifest_url,p[i].manifest_url)) ++matches;
        if (matches>=2 && (chosen<0 || p[i].revision>p[chosen].revision)) chosen=(int)i;
    }
    if (chosen<0 || p[chosen].revision<floor ||
        (p[chosen].revision==floor && accepted_hash && *accepted_hash && strcmp(p[chosen].sha256,accepted_hash))) return FALSE;
    for (size_t i=0;i<count;++i) if (valid[i] && p[i].revision==p[chosen].revision && strcmp(p[i].sha256,p[chosen].sha256)) return FALSE;
    *out=p[chosen]; return TRUE;
}
BOOL up_local_time(const wchar_t *iso,wchar_t *out,size_t cap) {
    unsigned y,mo,d,h,mi,s; wchar_t extra;
    if (swscanf(iso,L"%u-%u-%uT%u:%u:%uZ%c",&y,&mo,&d,&h,&mi,&s,&extra)!=6 || wcslen(iso)!=20 || iso[19]!=L'Z') return FALSE;
    SYSTEMTIME utc={0},local={0}; FILETIME ft;
    utc.wYear=(WORD)y; utc.wMonth=(WORD)mo; utc.wDay=(WORD)d; utc.wHour=(WORD)h; utc.wMinute=(WORD)mi; utc.wSecond=(WORD)s;
    if (!SystemTimeToFileTime(&utc,&ft) || !SystemTimeToTzSpecificLocalTime(NULL,&utc,&local)) return FALSE;
    wchar_t date[128],time[128];
    if (!GetDateFormatW(LOCALE_USER_DEFAULT,DATE_SHORTDATE,&local,NULL,date,128) ||
        !GetTimeFormatW(LOCALE_USER_DEFAULT,TIME_NOSECONDS,&local,NULL,time,128)) return FALSE;
    if (wcslen(date)+wcslen(time)+2>cap) return FALSE;
    wcscpy(out,date); wcscat(out,L" "); wcscat(out,time); return TRUE;
}
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0]|(p[1]<<8)); }
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static unsigned char *read_file(const wchar_t *path,DWORD cap,DWORD *size) {
    HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (f==INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER n; unsigned char *data=NULL; DWORD got;
    if (GetFileSizeEx(f,&n) && n.QuadPart>0 && n.QuadPart<=cap) {
        *size=(DWORD)n.QuadPart; data=malloc(*size);
        if (data && (!ReadFile(f,data,*size,&got,NULL) || got!=*size)) { free(data); data=NULL; }
    }
    CloseHandle(f); return data;
}
BOOL up_verify_exe(const wchar_t *file,uint64_t version,BOOL main_exe) {
    DWORD n=0; unsigned char *data=read_file(file,8*1024*1024,&n); if (!data) return FALSE;
    BOOL ok=n>=64 && data[0]=='M' && data[1]=='Z';
    uint32_t pe=ok?le32(data+0x3c):0;
    ok=ok && pe<=n && n-pe>=24 && !memcmp(data+pe,"PE\0\0",4) && le16(data+pe+4)==UP_MACHINE;
    free(data);
    if (!ok || !main_exe) return ok;
    DWORD ignored=0,size=GetFileVersionInfoSizeW(file,&ignored);
    if (!size || size>UP_MAX_META) return FALSE;
    void *info=malloc(size); if (!info) return FALSE;
    VS_FIXEDFILEINFO *fixed=NULL; UINT bytes=0;
    ok=GetFileVersionInfoW(file,0,size,info) && VerQueryValueW(info,L"\\",(LPVOID*)&fixed,&bytes) && bytes>=sizeof(*fixed);
    if (ok) {
        uint64_t code=(uint64_t)HIWORD(fixed->dwFileVersionMS)*1000000+LOWORD(fixed->dwFileVersionMS)*1000+HIWORD(fixed->dwFileVersionLS);
        wchar_t *product=NULL; UINT chars=0;
        ok=code==version && VerQueryValueW(info,L"\\StringFileInfo\\040904b0\\ProductName",(LPVOID*)&product,&chars) &&
            product && chars && !wcscmp(product,L"Payload Dumper");
    }
    free(info); return ok;
}
static uint32_t crc32_bytes(const unsigned char *p,size_t n) {
    uint32_t c=0xffffffffU;
    while (n--) { c^=*p++; for (int k=0;k<8;++k) c=(c>>1)^(0xedb88320U&(~(c&1)+1)); }
    return ~c;
}
BOOL up_extract_zip(const wchar_t *zip,const wchar_t *stage,uint64_t expected_version) {
    DWORD size=0; unsigned char *data=read_file(zip,UP_MAX_ZIP,&size); if (!data) return FALSE;
    BOOL ok=FALSE; uint64_t seen=0; uint32_t total=0;
    size_t eocd=SIZE_MAX,low=size>65557?size-65557:0;
    if (size<22) goto done;
    for (size_t p=size-22;;--p) {
        if (le32(data+p)==0x06054b50U && p+22+le16(data+p+20)==size) { eocd=p; break; }
        if (p==low) break;
    }
    if (eocd==SIZE_MAX || le16(data+eocd+4) || le16(data+eocd+6)) goto done;
    unsigned count=le16(data+eocd+10);
    if (!count || count>32 || count!=le16(data+eocd+8)) goto done;
    uint32_t cdsize=le32(data+eocd+12),cd=le32(data+eocd+16);
    if ((uint64_t)cd+cdsize>eocd) goto done;
    wchar_t dir[UP_PATH];
    if (!up_join(dir,stage,L"bin") || !CreateDirectoryW(dir,NULL)) goto done;
    if (!up_join(dir,stage,L"licenses") || !CreateDirectoryW(dir,NULL)) goto done;
    size_t p=cd;
    for (unsigned entry=0;entry<count;++entry) {
        if (p+46>(uint64_t)cd+cdsize || le32(data+p)!=0x02014b50U) goto done;
        uint16_t flags=le16(data+p+8),method=le16(data+p+10),name_len=le16(data+p+28);
        uint32_t compressed=le32(data+p+20),raw=le32(data+p+24),local=le32(data+p+42),crc=le32(data+p+16);
        size_t next=p+46+name_len+le16(data+p+30)+le16(data+p+32);
        if (next>(uint64_t)cd+cdsize || !name_len || name_len>256 || (flags&1) || (method!=0 && method!=8) || le16(data+p+34)) goto done;
        char name[257]; memcpy(name,data+p+46,name_len); name[name_len]=0;
        if (strlen(name)!=name_len) goto done;
        for (unsigned k=0;k<name_len;++k) if (name[k]=='\\') name[k]='/';
        if (!strcmp(name,"bin/") || !strcmp(name,"licenses/")) { if (raw || compressed) goto done; p=next; continue; }
        wchar_t wide[257]; if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name,-1,wide,257)) goto done;
        for (wchar_t *c=wide;*c;++c) if (*c==L'/') *c=L'\\';
        size_t file=0; while (file<up_package_file_count && wcscmp(wide,up_package_files[file])) ++file;
        if (file==up_package_file_count || (seen&((uint64_t)1<<file)) || raw>8*1024*1024 || total>UP_MAX_ZIP-raw) goto done;
        if ((uint64_t)local+30>cd || le32(data+local)!=0x04034b50U || le16(data+local+6)!=flags || le16(data+local+8)!=method) goto done;
        uint16_t local_name=le16(data+local+26),extra=le16(data+local+28);
        uint64_t start=(uint64_t)local+30+local_name+extra;
        if (local_name!=name_len || start>cd || start+compressed>cd || memcmp(data+local+30,data+p+46,name_len)) goto done;
        unsigned char *out=malloc(raw?raw:1); if (!out) goto done;
        BOOL decoded=FALSE;
        if (method==0) { decoded=raw==compressed; if (decoded) memcpy(out,data+(size_t)start,raw); }
        else decoded=tinfl_decompress_mem_to_mem(out,raw,data+(size_t)start,compressed,0)==raw;
        decoded=decoded && crc32_bytes(out,raw)==crc;
        if (decoded && up_join(dir,stage,wide)) {
            HANDLE f=CreateFileW(dir,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL); DWORD written=0;
            decoded=f!=INVALID_HANDLE_VALUE && WriteFile(f,out,raw,&written,NULL) && written==raw && FlushFileBuffers(f);
            if (f!=INVALID_HANDLE_VALUE) CloseHandle(f);
        } else decoded=FALSE;
        free(out); if (!decoded) goto done;
        seen|=(uint64_t)1<<file; total+=raw; p=next;
    }
    if (p!=(uint64_t)cd+cdsize || seen!=(((uint64_t)1<<up_package_file_count)-1)) goto done;
    for (size_t k=0;k<4;++k) if (!up_join(dir,stage,up_package_files[k]) || !up_verify_exe(dir,expected_version,k==0)) goto done;
    ok=TRUE;
done: free(data); return ok;
}
