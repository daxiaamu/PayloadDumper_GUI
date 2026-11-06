"""Generate immutable Windows update metadata from a published GitHub Release.

Only packages whose main PE version/architecture match the release are used.
Five distinct mirror hosts must return the exact ZIP bytes before publishing.
Uses the authenticated gh CLI; never reads or prints credentials.
"""
import argparse
import concurrent.futures
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile

REPO = "daxiaamu/PayloadDumper_GUI"
MAX_ZIP = 32 * 1024 * 1024
MIRRORS = [
    "https://ghfast.top/{official}", "https://gh-proxy.com/{official}",
    "https://ghproxy.net/{official}", "https://gh.llkk.cc/{official}",
    "https://ghp.keleyaa.com/{official}", "https://gh.monlor.com/{official}",
    "https://xget.xi-xu.me/gh/{repo}/releases/download/{tag}/{asset}",
    "https://ghproxy.vip/{official}", "https://gh.jasonzeng.dev/{official}",
    "https://gh.3w.pm/{official}", "https://gh-proxy.org/{official}",
    "https://gh-proxy.net/{official}", "https://ghproxy.cc/{official}",
    "https://ghproxy.cn/{official}", "https://github.moeyy.xyz/{official}"
]

def gh(*args):
    return subprocess.check_output(["gh", *args], text=True, encoding="utf-8")

def pe_version(data, arch):
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("Missing PE header")
    pe = struct.unpack_from("<I",data,0x3c)[0]
    if data[pe:pe+4] != b"PE\0\0":
        raise ValueError("Invalid PE signature")
    machine, sections = struct.unpack_from("<HH",data,pe+4)
    if machine != {"x64":0x8664,"x86":0x14c}[arch]:
        raise ValueError("Wrong PE architecture")
    optional_size = struct.unpack_from("<H",data,pe+20)[0]
    resource = None
    for index in range(sections):
        section = pe+24+optional_size+40*index
        if data[section:section+8].rstrip(b"\0") == b".rsrc":
            size, offset = struct.unpack_from("<II",data,section+16)
            resource = data[offset:offset+size]
            break
    if not resource or "Payload Dumper".encode("utf-16le") not in resource:
        raise ValueError("Missing application product identity")
    offset = resource.find(struct.pack("<II",0xFEEF04BD,0x00010000))
    if offset < 0:
        raise ValueError("Missing fixed version resource")
    major_minor, patch_build = struct.unpack_from("<II",resource,offset+8)
    major,minor,patch,build = major_minor>>16,major_minor&65535,patch_build>>16,patch_build&65535
    if minor >= 1000 or patch >= 1000 or build:
        raise ValueError("Unsupported version resource")
    return major*1000000+minor*1000+patch, f"{major}.{minor}.{patch}"

class SecureRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not newurl.startswith("https://"):
            raise ValueError("Refusing non-HTTPS redirect")
        return super().redirect_request(req,fp,code,msg,headers,newurl)

def fetch_verified(url, size, digest):
    try:
        opener = urllib.request.build_opener(SecureRedirect())
        request = urllib.request.Request(url,headers={"User-Agent":"PayloadDumper-release-check/1.0","Cache-Control":"no-cache"})
        with opener.open(request,timeout=18) as response:
            if response.status != 200 or not response.geturl().startswith("https://"):
                return None
            body = response.read(min(MAX_ZIP,size)+1)
        if len(body) != size or hashlib.sha256(body).hexdigest() != digest:
            return None
        return url
    except (OSError,ValueError,urllib.error.URLError):
        return None

def encode(data):
    return (json.dumps(data,ensure_ascii=False,indent=2,sort_keys=True)+"\n").encode("utf-8")

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--tag",required=True)
    parser.add_argument("--root",type=Path,default=Path(__file__).resolve().parents[1])
    args=parser.parse_args()
    release=json.loads(gh("api",f"repos/{REPO}/releases/tags/{args.tag}"))
    if release["draft"] or release["prerelease"]:
        raise SystemExit("Stable OTA metadata requires a published stable Release")
    policy=json.loads((args.root/"updates/policy.json").read_text(encoding="utf-8"))
    if policy.get("channel")!="stable" or policy.get("schemaVersion")!=1:
        raise ValueError("Invalid policy channel/schema")
    packages={}
    versions=set()
    with tempfile.TemporaryDirectory(prefix="pd-release-") as scratch:
        for arch in ["x64","x86"]:
            asset=f"PayloadDumperGUI-win-{arch}.zip"
            matches=[a for a in release["assets"] if a["name"]==asset]
            if len(matches)!=1:
                raise ValueError(f"Expected exactly one {asset}")
            subprocess.run(["gh","release","download",args.tag,"--repo",REPO,"--pattern",asset,"--dir",scratch],check=True)
            data=(Path(scratch)/asset).read_bytes()
            if not 0<len(data)<=MAX_ZIP:
                raise ValueError("ZIP size outside update limits")
            with zipfile.ZipFile(Path(scratch)/asset) as z:
                if z.testzip() is not None:
                    raise ValueError("Release ZIP CRC failure")
                version,name=pe_version(z.read("PayloadDumperGUI.exe"),arch)
            versions.add((version,name))
            if args.tag != f"v{name}":
                raise ValueError("Release tag must match the EXE version resource")
            sha=hashlib.sha256(data).hexdigest()
            official=matches[0]["browser_download_url"]
            if not fetch_verified(official,len(data),sha):
                raise ValueError("Official release asset is not downloadable")
            candidates=[template.format(official=official,repo=REPO,tag=args.tag,asset=asset) for template in MIRRORS]
            good=[]
            with concurrent.futures.ThreadPoolExecutor(max_workers=len(candidates)) as pool:
                for url in pool.map(lambda url:fetch_verified(url,len(data),sha),candidates):
                    if url:
                        good.append(url)
                        print(f"Verified {arch}: {urllib.parse.urlsplit(url).hostname}",flush=True)
            hosts={urllib.parse.urlsplit(url).hostname for url in good}
            if len(hosts)<5:
                raise RuntimeError(f"Only {len(hosts)} verified {arch} mirror hosts; preserving the previous metadata")
            packages[arch]={"fileName":asset,"size":len(data),"sha256":sha,"urls":good[:12]+[official]}
    if len(versions)!=1:
        raise ValueError("Architecture packages have different versions")
    version,name=versions.pop()
    forced=policy.get("maxForcedVersionCode")
    revision=policy.get("policyRevision")
    if type(forced) is not int or type(revision) is not int or not 0<=forced<version or revision<1:
        raise ValueError("Invalid forced boundary or policy revision")
    pointer_path=args.root/"updates/stable/latest.json"
    if pointer_path.exists():
        previous=json.loads(pointer_path.read_text(encoding="utf-8"))
        if revision <= previous["policyRevision"]:
            raise ValueError("Increment policyRevision before publishing a new manifest")
    manifest={"schemaVersion":1,"appId":"daxiaamu.PayloadDumperGUI","channel":"stable",
              "policyRevision":revision,"versionCode":version,"versionName":name,
              "maxForcedVersionCode":forced,"publishedAt":release["published_at"],
              "releaseUrl":release["html_url"],"changelog":release["body"] or "", "packages":packages}
    body=encode(manifest)
    digest=hashlib.sha256(body).hexdigest()
    relative=f"updates/manifests/{revision}-{digest}.json"
    path=args.root/relative
    if path.exists() and path.read_bytes()!=body:
        raise ValueError("Refusing to overwrite immutable metadata")
    pointer={"schemaVersion":1,"channel":"stable","policyRevision":revision,
             "manifestUrl":f"https://raw.githubusercontent.com/{REPO}/main/{relative}","manifestSha256":digest}
    # Nothing is written until both architectures and all mirror requirements pass.
    path.parent.mkdir(parents=True,exist_ok=True)
    pointer_path.parent.mkdir(parents=True,exist_ok=True)
    path.write_bytes(body)
    temporary=pointer_path.with_suffix(".json.tmp")
    temporary.write_bytes(encode(pointer))
    temporary.replace(pointer_path)
    print(f"Generated stable pointer and immutable manifest: {relative}")

if __name__=="__main__":
    main()
