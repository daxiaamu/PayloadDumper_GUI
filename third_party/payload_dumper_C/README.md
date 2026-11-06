# payload_dumper_c

【机场推荐：[白月光，稳定高速](https://www.sibker.com/register?invite_code=2XQR1UUz)】

**体积最小的，速度最快的多线程 payload.bin 解包工具。**

Multi-threaded Android OTA `payload.bin` dumper in C. Single static-linked .exe, no runtime dependencies.

Extracts OTA payloads at **~220 MB/s** — a full 6 GB OTA in about **28 seconds** (tested on i5-12490F, 32GB RAM, SSD, Windows 10 22H2). Supports both **local files** and **HTTP/HTTPS URLs** — direct range-based extraction from OTA servers, no need to download the full ZIP first.

## Features

| Feature | Description |
|---------|-------------|
| ⚡ **Fast extraction** | ~220 MB/s, fully utilizes all CPU cores for parallel decompression |
| 📦 **Single binary** | ~500 KB `.exe`, no .NET / Python / Go / DLL dependencies |
| 🌐 **URL direct extraction** | Pass an HTTP/HTTPS URL — extracts via range requests |
| 💡 **Low bandwidth** | URL + selective extraction (`-p boot,system`) = download only what you need |
| ⚙️ **Multi-threaded** | Fully parallel across all cores — scales with modern CPUs |
| 🔒 **Integrity** | SHA256 verification on every decompressed block |
| 📋 **List mode** | List partition names without downloading any data |
| 📄 **Metadata** | Extract `META-INF/com/android/metadata` from OTA ZIP |
| 🎯 **Selective** | Extract only specific partitions by name |
| 🔑 **Auto-detect** | `payloadfile` can be local path or HTTP URL — auto-detected |

## Speed

Tested on **Windows 10 22H2**, i5-12490F (6C12T), 32GB RAM, NVMe SSD:

| OTA Package | Size | Time | Throughput |
|-------------|------|------|------------|
| OnePlus 12 ColorOS PLK110 A.67 (oplusrom.com) | 6 GB | **28 seconds** | **~220 MB/s** |

The thread pool fully utilizes all CPU cores simultaneously — every worker independently decompresses BZ2/XZ/Zstd blocks, verifies SHA256, and writes to disk in parallel. There is no single-threaded bottleneck. More cores = faster extraction.

## Multi-Threaded Extraction

All operations (decompression, SHA256 verification, disk I/O) are distributed across worker threads. The thread pool processes partition operations in parallel:

- **Local files**: memory-mapped, zero-copy reads → all threads decompress + write simultaneously
- **URL extraction**: sequential HTTP reads (Range requests) → parallel decompression + write

Default worker count equals CPU cores. Tune with `-w`:

```bash
payload_dumper payload.bin -w 16
```

## URL Extraction — Low Bandwidth

When using a URL as `payloadfile`, the tool reads only what it needs via **HTTP Range requests** (requires server `Accept-Ranges: bytes`). Combined with selective partition extraction (`-p`), you only download the exact bytes for the partitions you want:

```bash
# Extract boot + vendor from a remote OTA — just a few MB of traffic
payload_dumper "https://ota.example.com/PLF110_OTA.zip" -p boot,vendor
```

Without selective extraction, you'd need to download the full 6-8 GB OTA ZIP to get one partition.

## Usage

```
payload_dumper [-h] [-o OUT] [-p PARTITIONS] [-w WORKERS]
               [--list] [--metadata] [--kernel-version] [--arb] payloadfile
```

### Positional Arguments

| Argument | Description |
|----------|-------------|
| `payloadfile` | Local path to `payload.bin` / `.zip`, or an HTTP/HTTPS URL |

Auto-detected: if the argument starts with `http://` or `https://`, it's extracted via HTTP range requests. Otherwise treated as a local file path (supports Unicode paths on Windows).

> **Note**: URL extraction requires the server to support HTTP Range (`Accept-Ranges: bytes`). OPlus / OnePlus OTA servers support this.

### Options

| Option | Description |
|--------|-------------|
| `-h`, `--help` | Show help message |
| `-o`, `--out OUT` | Output directory (default: `output`) |
| `-p`, `--partitions PARTITIONS` | Comma-separated partition names to extract (default: all) |
| `-w`, `--workers WORKERS` | Number of worker threads (default: CPU count) |
| `-l`, `--list` | List partition names in the payload |
| `-m`, `--metadata` | Extract and display the metadata file |
| `--kernel-version` | Read the kernel version from the ROM |
| `--arb` | Read the anti-rollback version from the ROM |

## Examples

### Local OTA ZIP (full extraction)

```bash
payload_dumper ota.zip
```

### Local payload.bin to custom directory

```bash
payload_dumper payload.bin -o extracted
```

### URL — direct extraction (requires Range support)

```bash
payload_dumper "https://ota.example.com/PLF110_OTA.zip"
```

### URL — selective, low bandwidth

```bash
payload_dumper "https://ota.example.com/PLF110_OTA.zip" -p boot,vendor,dtbo
```

### List partitions only (no download for URL)

```bash
payload_dumper payload.bin -l
# Output: boot, system, vendor, dtbo, vbmeta, ...
```

### Extract metadata

```bash
payload_dumper payload.bin -m
# Output: post-timestamp=...
#         product=PLF110
#         version=...
```

### Print kernel version

```bash
payload_dumper ota.zip --kernel-version
# Output: 6.6.118-android15-8-ge58033dc8ea6-abogki498046332-4k
```

`--kernel-version` reads the kernel version from the ROM, the same version shown
in phone Settings -> About phone.

### Print ARB version

```bash
payload_dumper ota.zip --arb
# Output:
# OEM Metadata Major Version : 3
# OEM Metadata Minor Version : 0
# ARB (Anti-Rollback) : 1
```

`--arb` reads the anti-rollback version from the ROM when the package contains
the required bootloader configuration partition.

### URL with all options

```bash
payload_dumper "https://ota.example.com/payload.zip" -o output -p boot,system -w 8
```

## Build

Requires MinGW-w64 (cross-compiler on Linux/WSL, or MSYS2 on Windows).
Rebuilding the bundled decoder libraries with `build_all.sh` or
`rebuild_all.sh` also requires CMake; liblzma uses the official XZ CMake
configuration instead of a hand-maintained source-file list.

The repository includes a verified, decoder-only `lib/lib/liblzma.a` built
from XZ Utils 5.6.4 for Windows x86-64 with MinGW-w64, so the normal Windows
build can link offline. `build_all.sh` and `rebuild_all.sh` remain the source
of truth for auditing, upgrading, or rebuilding it for another toolchain.
The bundled archive is 147,888 bytes and has SHA-256
`20DE3FADA07C66596377A5A434A6F2ADDE76F234A15A7CC475D66894EB569D9B`.

The repository also includes `lib32/` for 32-bit Windows release builds. Its
`liblzma.a` comes from the official MSYS2 `mingw-w64-i686-xz` package. Avoid
hand-maintained liblzma source-file lists: they may link successfully but fail
on some XZ-compressed payload operations.

```bash
git clone https://github.com/daxiaamu/payload_dumper_c.git
cd payload_dumper_c

x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -mconsole \\
  -o payload_dumper.exe main.c dumper.c payload.c decompress.c \\
  threadpool.c ziputil.c sha256.c http_reader.c \\
  -Ilib/include -Llib/lib -lzstd -lbrotlidec -lbrotlicommon -lbz2 -llzma \\
  -lbcrypt -lwinhttp -lshlwapi
strip payload_dumper.exe
```

## License

GPL-3.0
