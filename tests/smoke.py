"""Offline package integration checks using small, generated OTA fixtures.

Run after build.ps1: python tests/smoke.py
Does not touch existing ROM outputs or use Internet servers.
"""
from pathlib import Path
import bz2
import hashlib
import http.server
import json
import lzma
import struct
import subprocess
import threading
import zipfile

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / ".build" / "smoke"
FIXTURE = WORK / "中文 路径 & fixture"
KERNEL = "6.6.118-android15-test"

class RangeHandler(http.server.BaseHTTPRequestHandler):
    """Real WinHTTP range requests, served entirely on the loopback adapter."""
    def log_message(self, *args):
        pass

    def do_GET(self):
        data = (FIXTURE / "OTA 中文 & 空格.zip").read_bytes()
        range_header = self.headers.get("Range", "")
        if range_header.startswith("bytes="):
            start, end = range_header[6:].split("-",1)
            start = int(start)
            end = min(int(end), len(data)-1) if end else len(data)-1
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
        else:
            start, end = 0, len(data)-1
            self.send_response(200)
        body = data[start:end+1]
        self.send_header("Content-Length",str(len(body)))
        self.send_header("Accept-Ranges","bytes")
        self.end_headers()
        self.wfile.write(body)

def varint(value):
    data = bytearray()
    while value > 127:
        data.append((value & 127) | 128)
        value >>= 7
    return bytes(data + bytes([value]))

def field(number, value):
    if isinstance(value, int):
        return varint(number << 3) + varint(value)
    return varint((number << 3) | 2) + varint(len(value)) + value

def make_fixture():
    FIXTURE.mkdir(parents=True, exist_ok=True)
    elf = bytearray(4096)
    elf[:5] = b"\x7fELF\x02"
    struct.pack_into("<Q", elf, 32, 64)
    struct.pack_into("<HH", elf, 54, 56, 1)
    struct.pack_into("<IIQQQQQQ", elf, 64, 0, 0, 120, 0, 0, 48, 48, 0)
    struct.pack_into("<9I", elf, 120, 1, 0, 0, 12, 0, 0, 0, 0, 0)
    struct.pack_into("<III", elf, 156, 3, 0, 7)
    boot = (b"Linux version " + KERNEL.encode() + b" (test@localhost)\0").ljust(4096,b"\0")
    images = {"boot": boot, "xbl_config": bytes(elf),
              "vendor_boot": bytes(range(256))*16, "dtbo": b"dtbo-test".ljust(4096,b"\0")}
    manifest = field(3,4096) + field(12,0)
    blobs = bytearray()
    for name, raw in images.items():
        op_type = 8 if name == "vendor_boot" else 1 if name == "dtbo" else 0
        blob = lzma.compress(raw) if op_type == 8 else bz2.compress(raw) if op_type == 1 else raw
        extent = field(1,0) + field(2,1)
        op = field(1,op_type) + field(2,len(blobs)) + field(3,len(blob)) + field(6,extent) + field(7,len(raw))
        part = field(1,name.encode()) + field(7,field(1,len(raw))) + field(8,op)
        manifest += field(13,part)
        blobs.extend(blob)
    payload = b"CrAU" + struct.pack(">QQI",2,len(manifest),0) + manifest + blobs
    (FIXTURE / "payload.bin").write_bytes(payload)
    (FIXTURE / "xbl 中文.img").write_bytes(elf)
    metadata = b"ota-type=AB\npost-build=synthetic-integration-test\npost-timestamp=1\n"
    with zipfile.ZipFile(FIXTURE / "OTA 中文 & 空格.zip", "w", compression=zipfile.ZIP_STORED) as z:
        z.writestr("payload.bin",payload)
        z.writestr("META-INF/com/android/metadata",metadata)
    return images, metadata

def run(exe, *args, ok=True):
    result = subprocess.run([str(exe), *map(str,args)], cwd=str(WORK),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=30)
    text = result.stdout.decode("utf-8", errors="replace")
    if ok and result.returncode:
        raise AssertionError(f"{exe.name} failed {result.returncode}: {text}")
    if not ok and result.returncode == 0:
        raise AssertionError("Invalid input unexpectedly succeeded")
    return text

def check_machine(path, machine):
    data = path.read_bytes()
    pe = struct.unpack_from("<I",data,0x3c)[0]
    assert data[pe:pe+4] == b"PE\0\0"
    assert struct.unpack_from("<H",data,pe+4)[0] == machine, path

def main_with_server(server):
    images, metadata = make_fixture()
    zip_path = FIXTURE / "OTA 中文 & 空格.zip"
    report = []
    for arch,machine in [("x64",0x8664),("x86",0x14c)]:
        package = ROOT / "dist" / f"win-{arch}"
        dumper, arb = package / "bin" / "payload_dumper.exe", package / "bin" / "arbextract.exe"
        for exe in [package / "PayloadDumperGUI.exe", dumper, arb]:
            check_machine(exe,machine)
        assert "--kernel-version" in run(dumper,"--help")
        listed = run(dumper,"--list",zip_path)
        assert all(name in listed for name in images)
        # All three decoder paths + byte-for-byte output validation.
        out = WORK / f"{arch} 完整输出"
        run(dumper,"--out",out,"--workers","2",zip_path)
        for name, raw in images.items():
            assert (out / f"{name}.img").read_bytes() == raw, (arch,name)
        selected = WORK / f"{arch} selected"
        run(dumper,"--partitions","boot,xbl_config","--out",selected,FIXTURE / "payload.bin")
        assert sorted(p.name for p in selected.glob("*.img")) == ["boot.img","xbl_config.img"]
        assert KERNEL in run(dumper,"--kernel-version",zip_path)
        run(dumper,"--metadata","--out",out,zip_path)
        assert (out / "metadata").read_bytes() == metadata
        assert ": 7" in run(arb,FIXTURE / "xbl 中文.img")
        assert ": 7" in run(arb,selected / "xbl_config.img")
        invalid = FIXTURE / "bad.bin"
        invalid.write_bytes(b"not-a-payload"*16)
        run(dumper,"--list",invalid,ok=False)
        run(arb,invalid,ok=False)
        url = f"http://127.0.0.1:{server.server_address[1]}/ota.zip"
        assert "xbl_config" in run(dumper,"--list",url)
        remote = WORK / f"{arch} HTTP 输出"
        run(dumper,"--partitions","boot,xbl_config","--out",remote,url)
        assert (remote / "boot.img").read_bytes() == images["boot"]
        assert ": 7" in run(arb,remote / "xbl_config.img")
        assert KERNEL in run(dumper,"--kernel-version",url)
        report.append({"architecture":arch,"checks":"PE architecture, help, ZIP list, raw/XZ/BZ2 extraction, selective BIN extraction, metadata, kernel, ARB, Unicode paths, invalid input, HTTP Range list/extract/kernel",
                       "executables":{str(p.relative_to(package)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [package/"PayloadDumperGUI.exe",dumper,arb]}})
        print(f"PASS {arch}: extraction, metadata, kernel, ARB, Unicode, negative cases, HTTP Range")
    (WORK / "report.json").write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")
    print("Report:",WORK / "report.json")
    print("GUI fixture:",zip_path)

if __name__ == "__main__":
    with http.server.ThreadingHTTPServer(("127.0.0.1",0),RangeHandler) as server:
        worker = threading.Thread(target=server.serve_forever,daemon=True)
        worker.start()
        try:
            main_with_server(server)
        finally:
            server.shutdown()
