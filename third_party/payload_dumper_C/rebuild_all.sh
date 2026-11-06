#!/bin/bash
set -e
cd /tmp
rm -rf payload_src payload_libs
mkdir -p payload_libs/include payload_libs/lib payload_src
HOST=x86_64-w64-mingw32
CC=$HOST-gcc
AR=$HOST-ar
DST="$(cd "$(dirname "$0")" && pwd)"

echo "=== zstd ==="
curl -sL https://codeload.github.com/facebook/zstd/tar.gz/refs/tags/v1.5.7 > /tmp/payload_src/zstd.tar.gz
tar xf /tmp/payload_src/zstd.tar.gz -C /tmp/payload_src
cd /tmp/payload_src/zstd-1.5.7/lib
$CC -O2 -c -o /tmp/payload_libs/zstd_d.o decompress/zstd_decompress.c -I. -Icommon -Idecompress -Ilegacy -DZSTD_DISABLE_ASM
$CC -O2 -c -o /tmp/payload_libs/zstd_dd.o decompress/zstd_ddict.c -I. -Icommon -Idecompress -Ilegacy
$CC -O2 -c -o /tmp/payload_libs/zstd_huf.o decompress/huf_decompress.c -I. -Icommon -Idecompress -Ilegacy
$CC -O2 -c -o /tmp/payload_libs/zstd_db.o decompress/zstd_decompress_block.c -I. -Icommon -Idecompress -Ilegacy
$CC -O2 -c -o /tmp/payload_libs/zstd_dbg.o common/debug.c -Icommon
$CC -O2 -c -o /tmp/payload_libs/zstd_ent.o common/entropy_common.c -Icommon
$CC -O2 -c -o /tmp/payload_libs/zstd_err.o common/error_private.c -Icommon
$CC -O2 -c -o /tmp/payload_libs/zstd_fse.o common/fse_decompress.c -Icommon
$CC -O2 -c -o /tmp/payload_libs/zstd_xxh.o common/xxhash.c -Icommon
$CC -O2 -c -o /tmp/payload_libs/zstd_com.o common/zstd_common.c -Icommon
cd /tmp/payload_libs
$AR rcs lib/libzstd.a zstd_*.o
cp /tmp/payload_src/zstd-1.5.7/lib/zstd.h /tmp/payload_src/zstd-1.5.7/lib/zstd_errors.h include/
rm -f *.o

echo "=== brotli ==="
curl -sL https://codeload.github.com/google/brotli/tar.gz/refs/tags/v1.1.0 > /tmp/payload_src/brotli.tar.gz
tar xf /tmp/payload_src/brotli.tar.gz -C /tmp/payload_src
cd /tmp/payload_src/brotli-1.1.0/c
$CC -O2 -c -o /tmp/payload_libs/br_dic.o common/dictionary.c -I./include
$CC -O2 -c -o /tmp/payload_libs/br_tr.o common/transform.c -I./include
$CC -O2 -c -o /tmp/payload_libs/br_sh.o common/shared_dictionary.c -I./include
$CC -O2 -c -o /tmp/payload_libs/br_br.o dec/bit_reader.c -I./include
$CC -O2 -c -o /tmp/payload_libs/br_de.o dec/decode.c -I./include
$CC -O2 -c -o /tmp/payload_libs/br_hu.o dec/huffman.c -I./include
$CC -O2 -c -o /tmp/payload_libs/br_st.o dec/state.c -I./include
$CC -O2 -c -o /tmp/payload_libs/br_const.o common/constants.c -I./include
$CC -O2 -c -o /tmp/payload_libs/br_plat.o common/platform.c -I./include
cd /tmp/payload_libs
$AR rcs lib/libbrotlidec.a br_dic.o br_tr.o br_sh.o br_br.o br_de.o br_hu.o br_st.o
$AR rcs lib/libbrotlicommon.a br_const.o br_plat.o
cp -r /tmp/payload_src/brotli-1.1.0/c/include/brotli include/
rm -f *.o

echo "=== bzip2 ==="
curl -sL https://codeload.github.com/enthought/bzip2-1.0.8/tar.gz/refs/heads/master > /tmp/payload_src/bzip2.tar.gz
tar xf /tmp/payload_src/bzip2.tar.gz -C /tmp/payload_src
cd /tmp/payload_src/bzip2-1.0.8
echo '#define BZ_VERSION "1.0.8"' > bz_version.h
$CC -O2 -c -o /tmp/payload_libs/bz2_blk.o blocksort.c
$CC -O2 -c -o /tmp/payload_libs/bz2_huf.o huffman.c
$CC -O2 -c -o /tmp/payload_libs/bz2_crc.o crctable.c
$CC -O2 -c -o /tmp/payload_libs/bz2_rnd.o randtable.c
$CC -O2 -c -o /tmp/payload_libs/bz2_cmp.o compress.c
$CC -O2 -c -o /tmp/payload_libs/bz2_dec.o decompress.c
$CC -O2 -c -o /tmp/payload_libs/bz2_lib.o bzlib.c
cd /tmp/payload_libs
$AR rcs lib/libbz2.a bz2_*.o
cp /tmp/payload_src/bzip2-1.0.8/bzlib.h include/
rm -f *.o

echo "=== lzma ==="
curl -sL https://codeload.github.com/tukaani-project/xz/tar.gz/refs/tags/v5.6.4 > /tmp/payload_src/xz.tar.gz
tar xf /tmp/payload_src/xz.tar.gz -C /tmp/payload_src
command -v cmake >/dev/null 2>&1 || {
    echo "Error: CMake is required to build the official liblzma configuration" >&2
    exit 1
}
XZ_SRC=/tmp/payload_src/xz-5.6.4
XZ_BUILD=/tmp/payload_src/xz-build
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
cp "$XZ_BUILD/liblzma.a" /tmp/payload_libs/lib/liblzma.a
cp -r "$XZ_SRC/src/liblzma/api/"lzma* /tmp/payload_libs/include/

echo "=== Copy to project ==="
rm -rf "$DST/lib"
cp -r /tmp/payload_libs "$DST/lib"
ls -la "$DST/lib/lib/"
echo "=== DEP BUILD DONE ==="
