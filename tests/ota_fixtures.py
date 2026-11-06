from pathlib import Path
import hashlib
import json
import shutil
import struct
import tempfile
import warnings
import zipfile

ROOT=Path(__file__).resolve().parents[1]

def generate(arch):
    parent=ROOT/".build"/"ota-fixtures"/arch
    parent.mkdir(parents=True,exist_ok=True)
    work=Path(tempfile.mkdtemp(prefix="run-",dir=str(parent)))
    package=ROOT/"release"/f"PayloadDumperGUI-win-{arch}.zip"
    shutil.copyfile(package,work/"good.zip")
    other="x86" if arch=="x64" else "x64"
    shutil.copyfile(ROOT/"release"/f"PayloadDumperGUI-win-{other}.zip",work/"wrong-arch.zip")
    with zipfile.ZipFile(package) as z:
        members={n:z.read(n) for n in z.namelist()}
    for name,extra in [("traversal.zip","../escape.txt"),("duplicate.zip","PayloadDumperGUI.exe")]:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore",UserWarning)
            with zipfile.ZipFile(work/name,"w",compression=zipfile.ZIP_DEFLATED) as z:
                for key,data in members.items():
                    z.writestr(key,data)
                z.writestr(extra,b"unexpected")
    corrupt=bytearray(package.read_bytes())
    central=corrupt.index(b"PK\x01\x02")
    corrupt[central+16]^=1
    (work/"corrupt.zip").write_bytes(corrupt)
    urls=[f"https://cdn{i}.example/asset.zip" for i in range(5)]+["https://github.com/daxiaamu/PayloadDumper_GUI/releases/download/v1.0.0/asset.zip"]
    asset={"sha256":hashlib.sha256(package.read_bytes()).hexdigest(),"size":package.stat().st_size,"urls":urls}
    manifest={"schemaVersion":1,"appId":"daxiaamu.PayloadDumperGUI","channel":"stable","policyRevision":1,
              "versionCode":1000000,"versionName":"1.0.0","maxForcedVersionCode":0,"publishedAt":"2026-11-06T16:00:00Z",
              "releaseUrl":"https://github.com/daxiaamu/PayloadDumper_GUI/releases/tag/v1.0.0","changelog":"## 中文\n\n- 测试 \U0001f4e6",
              "packages":{"x64":asset,"x86":asset}}
    def write(name,data):
        (work/name).write_text(json.dumps(data,ensure_ascii=True),encoding="utf-8")
    write("manifest.json",manifest)
    digest=hashlib.sha256((work/"manifest.json").read_bytes()).hexdigest()
    write("pointer.json",{"schemaVersion":1,"channel":"stable","policyRevision":1,"manifestSha256":digest,
                          "manifestUrl":f"https://raw.githubusercontent.com/daxiaamu/PayloadDumper_GUI/main/updates/manifests/1-{digest}.json"})
    session=json.loads(json.dumps(manifest)); session["versionCode"]=1000001; session["versionName"]="1.0.1"
    write("session-manifest.json",session)
    digest=hashlib.sha256((work/"session-manifest.json").read_bytes()).hexdigest()
    write("session-pointer.json",{"schemaVersion":1,"channel":"stable","policyRevision":1,"manifestSha256":digest,
                                  "manifestUrl":f"https://raw.githubusercontent.com/daxiaamu/PayloadDumper_GUI/main/updates/manifests/1-{digest}.json"})
    for case in ["hash","http","hosts","forced","channel"]:
        data=json.loads(json.dumps(manifest))
        if case=="hash": data["packages"][arch]["sha256"]="invalid"
        if case=="http": data["packages"][arch]["urls"][0]="http://cdn0.example/asset.zip"
        if case=="hosts": data["packages"][arch]["urls"][1]=data["packages"][arch]["urls"][0]+"?duplicate=1"
        if case=="forced": data["maxForcedVersionCode"]=data["versionCode"]
        if case=="channel": data["channel"]="beta"
        write(f"bad-{case}.json",data)
    raw=(work/"manifest.json").read_text(encoding="utf-8")
    (work/"bad-duplicate.json").write_text(raw[:-1]+',"versionCode":2000000}',encoding="utf-8")
    (work/"bad-trailing.json").write_text(raw+'{}',encoding="utf-8")
    return work

if __name__=="__main__":
    import sys
    # JSON escaping keeps the path machine-readable under any console codepage.
    print(json.dumps(str(generate(sys.argv[1]))))
