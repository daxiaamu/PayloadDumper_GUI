#!/bin/bash
set -e
HOST=x86_64-w64-mingw32
CC=$HOST-gcc
AR=$HOST-ar
DST="$(cd "$(dirname "$0")" && pwd)"

mkdir -p /tmp/bbb /tmp/bbb/lib /tmp/bbb/include

# === download ALL sources first ===
cd /tmp/bbb
echo "Downloading..."
curl -sL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz | tar xz 2>/dev/null
curl -sL https://github.com/google/brotli/archive/refs/tags/v1.1.0.tar.gz | tar xz 2>/dev/null
curl -sL https://gitlab.com/bzip2/bzip2/-/archive/master/bzip2-master.tar.gz | tar xz 2>/dev/null
mv bzip2-master bzip2-1.0.8 2>/dev/null || true
curl -sL https://github.com/tukaani-project/xz/releases/download/v5.6.4/xz-5.6.4.tar.gz | tar xz 2>/dev/null

echo "Sources ready"

# === zstd ===
echo "=== zstd ==="
cd /tmp/bbb/zstd-1.5.7/lib
for s in decompress/zstd_decompress.c decompress/zstd_ddict.c decompress/huf_decompress.c decompress/zstd_decompress_block.c common/debug.c common/entropy_common.c common/error_private.c common/fse_decompress.c common/xxhash.c common/zstd_common.c; do
    b=$(basename $s .c)
    $CC -O2 -c -o /tmp/bbb/z_$b.o $s -I. -Icommon -Idecompress -Ilegacy -DZSTD_DISABLE_ASM -DZSTD_LEGACY_SUPPORT=0
done
$AR rcs /tmp/bbb/lib/libzstd.a /tmp/bbb/z_*.o
cp zstd.h /tmp/bbb/include/
cp zstd_errors.h /tmp/bbb/include/

# === brotli ===
echo "=== brotli ==="
cd /tmp/bbb/brotli-1.1.0/c
$CC -O2 -c -o /tmp/bbb/br_dic.o common/dictionary.c -I./include
$CC -O2 -c -o /tmp/bbb/br_tr.o common/transform.c -I./include
$CC -O2 -c -o /tmp/bbb/br_sh.o common/shared_dictionary.c -I./include
$CC -O2 -c -o /tmp/bbb/br_br.o dec/bit_reader.c -I./include
$CC -O2 -c -o /tmp/bbb/br_de.o dec/decode.c -I./include
$CC -O2 -c -o /tmp/bbb/br_hu.o dec/huffman.c -I./include
$CC -O2 -c -o /tmp/bbb/br_st.o dec/state.c -I./include
$CC -O2 -c -o /tmp/bbb/br_const.o common/constants.c -I./include
$CC -O2 -c -o /tmp/bbb/br_ctx.o common/context.c -I./include
$CC -O2 -c -o /tmp/bbb/br_plat.o common/platform.c -I./include
$AR rcs /tmp/bbb/lib/libbrotlidec.a /tmp/bbb/br_dic.o /tmp/bbb/br_tr.o /tmp/bbb/br_sh.o /tmp/bbb/br_br.o /tmp/bbb/br_de.o /tmp/bbb/br_hu.o /tmp/bbb/br_st.o
$AR rcs /tmp/bbb/lib/libbrotlicommon.a /tmp/bbb/br_const.o /tmp/bbb/br_ctx.o /tmp/bbb/br_plat.o
cp -r include/brotli /tmp/bbb/include/

# === bzip2 ===
echo "=== bzip2 ==="
cd /tmp/bbb/bzip2-1.0.8
echo '#define BZ_VERSION "1.0.8"' > bz_version.h
$CC -O2 -c -o /tmp/bbb/bz_blk.o blocksort.c
$CC -O2 -c -o /tmp/bbb/bz_huf.o huffman.c
$CC -O2 -c -o /tmp/bbb/bz_crc.o crctable.c
$CC -O2 -c -o /tmp/bbb/bz_rnd.o randtable.c
$CC -O2 -c -o /tmp/bbb/bz_cmp.o compress.c
$CC -O2 -c -o /tmp/bbb/bz_dec.o decompress.c
$CC -O2 -c -o /tmp/bbb/bz_lib.o bzlib.c
$AR rcs /tmp/bbb/lib/libbz2.a /tmp/bbb/bz_*.o
cp bzlib.h /tmp/bbb/include/

# === lzma ===
echo "=== lzma ==="
command -v cmake >/dev/null 2>&1 || {
    echo "Error: CMake is required to build the official liblzma configuration" >&2
    exit 1
}
XZ_SRC=/tmp/bbb/xz-5.6.4
XZ_BUILD=/tmp/bbb/xz-build
rm -rf "$XZ_BUILD"
cmake -S "$XZ_SRC" -B "$XZ_BUILD" \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_AR="$AR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DBUILD_SHARED_LIBS=OFF \
    -DENABLE_NLS=OFF \
    -DENABLE_THREADS=OFF \
    -DENCODERS= \
    '-DDECODERS=lzma1;lzma2;delta;x86;powerpc;ia64;arm;armthumb;arm64;sparc' \
    -DMICROLZMA_ENCODER=OFF \
    -DMICROLZMA_DECODER=OFF \
    -DLZIP_DECODER=OFF
cmake --build "$XZ_BUILD" --target liblzma --parallel
cp "$XZ_BUILD/liblzma.a" /tmp/bbb/lib/liblzma.a
cp -r "$XZ_SRC/src/liblzma/api/"lzma* /tmp/bbb/include/

# === COPY TO PROJECT ===
echo "=== Copying ==="
rm -rf $DST/lib
mkdir -p $DST/lib/include $DST/lib/lib
cp /tmp/bbb/lib/*.a $DST/lib/lib/
cp /tmp/bbb/include/*.h $DST/lib/include/ 2>/dev/null || true
cp -r /tmp/bbb/include/brotli $DST/lib/include/ 2>/dev/null || true
cp -r /tmp/bbb/include/lzma* $DST/lib/include/ 2>/dev/null || true
echo "=== ALL DEP BUILD DONE ==="
ls -la $DST/lib/lib/
