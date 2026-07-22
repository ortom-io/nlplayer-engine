#!/usr/bin/env bash
# ==============================================================================
# CROSS-COMPILATION TOOLCHAIN SCRIPT
# TARGET: TinyALSA + FFmpeg Decoder (Static Binaries for Android NDK)
# HARDWARE TARGET: Qualcomm Snapdragon 7s Gen 2 (Cortex-A78)

#
# TECHNICAL SPECIFICATIONS:
#   1. Target A78: Tailored instruction scheduling for the specific CPU.
#   2. +LSE: Hardware atomics (CAS) for minimal-latency TinyALSA synchronization.
#   3. +FP16/DotProd: Comprehensive utilization of math/vector units.
#   4. Global Optimizations: IPRA, Pipeliner, LTO, Branchless processing.
# ==============================================================================

set -e

# Enforce POSIX standard locale for strictly English CLI output
export LC_ALL=C
export LANG=C

# ------------------------------------------------------------------------------
# Logging utilities
# ------------------------------------------------------------------------------
[ -t 1 ] && { R="\033[0m"; C="\033[1;36m"; Y="\033[1;33m"; E="\033[1;31m"; G="\033[1;32m"; }

info() { printf "[ ${C}INFO${R} ] %s\n" "$1"; }
warn() { printf "[ ${Y}WARN${R} ] %s\n" "$1" >&2; }
err()  { printf "[${E}FAILED${R}] %s\n" "$1" >&2; exit 1; }
ok()   { printf "[  ${G}OK${R}  ] %s\n" "$1"; }

# ------------------------------------------------------------------------------
# Build Parameters & Global Constants
# ------------------------------------------------------------------------------
export TARGET_ABI="arm64-v8a"
export ANDROID_API="31" # API 31+ required for modern secure cryptography APIs

# ALSA Installation Path for App Private Data
export ALSA_PREFIX="/data/data/com.art.nlplayer/files"
ALSA_INSTALL_ROOT_PATH="${ALSA_PREFIX#/}"

# Source Repositories
ALSA_LIB_REPO="https://github.com/alsa-project/alsa-lib.git"
ALSA_LIB_BRANCH="master"

TINYALSA_REPO="https://github.com/tinyalsa/tinyalsa.git"
TINYALSA_BRANCH="master"

FFMPEG_REPO="https://github.com/FFmpeg/FFmpeg.git"
FFMPEG_BRANCH="master"

info "Source origin: GitHub"

# ------------------------------------------------------------------------------
# Android NDK Resolution (Heuristic Discovery)
# ------------------------------------------------------------------------------
if [ -z "$ANDROID_NDK_HOME" ]; then
    info "ANDROID_NDK_HOME is undefined. Initiating heuristic discovery..."
    
    # Restrict search depth to 6 to balance deep SDK hierarchies against I/O overhead.
    # We reliably identify an NDK root by locating the 'ndk-build' executable.
    NDK_BIN_CANDIDATE=$(find "$HOME" /opt /usr/local -maxdepth 6 -type f -name "ndk-build" -executable 2>/dev/null | sort -V | tail -n 1)
    
    if [ -n "$NDK_BIN_CANDIDATE" ]; then
        export ANDROID_NDK_HOME="$(dirname "$NDK_BIN_CANDIDATE")"
    else
        err "Failed to automatically resolve Android NDK path. Please export ANDROID_NDK_HOME manually."
    fi
fi
ok "Resolved Android NDK: $ANDROID_NDK_HOME"

# ------------------------------------------------------------------------------
# Target Architecture Configuration
# ------------------------------------------------------------------------------
case "$TARGET_ABI" in
    "arm64-v8a")
        export TARGET_TRIPLE="aarch64-linux-android"
        export FFMPEG_ARCH="aarch64"
        export FFMPEG_CPU="cortex-a78"
        # Hardware-specific flags tailored for Snapdragon 7s Gen 2
        export TARGET_FEATURES="-mcpu=cortex-a78+crypto+crc+dotprod+fp16+lse+rcpc+ssbs -mbranch-protection=none -mno-outline-atomics -mno-fix-cortex-a53-835769"
        ;;
    "armeabi-v7a")
        export TARGET_TRIPLE="armv7a-linux-androideabi"
        export FFMPEG_ARCH="arm"
        export FFMPEG_CPU="armv7-a"
        export TARGET_FEATURES="-march=armv7-a -mfpu=neon -mfloat-abi=softfp"
        ;;
    *)
        err "Unsupported TARGET_ABI specified: $TARGET_ABI"
        ;;
esac

# ------------------------------------------------------------------------------
# Environment Directory Setup
# ------------------------------------------------------------------------------
BUILD_DIR="$(pwd)/android_build"
INSTALL_DIR="$(pwd)/ffmpeg"
FINAL_PACKAGE_DIR="${BUILD_DIR}/android_static_binaries"

info "Host installation directory set to: ${INSTALL_DIR}"

# ------------------------------------------------------------------------------
# Toolchain Environment Setup
# ------------------------------------------------------------------------------
export TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64"
export PATH="${TOOLCHAIN}/bin:$PATH"

export CC="${TARGET_TRIPLE}${ANDROID_API}-clang"
export CXX="${TARGET_TRIPLE}${ANDROID_API}-clang++"
export AR="llvm-ar"
export RANLIB="llvm-ranlib"
export STRIP="llvm-strip"
export LD="${CC}"
export SYSROOT="${TOOLCHAIN}/sysroot"
export COMMON_LTO_FLAGS="-flto"

# ==============================================================================
# GLOBAL OPTIMIZATIONS
# ==============================================================================
GLOBAL_DEEP_OPTS="-fno-common -fmerge-all-constants -fno-threadsafe-statics \
-fno-use-cxa-atexit -fno-stack-check -fno-stack-clash-protection -fno-ident \
-mllvm --hot-cold-split \
-mllvm --enable-loopinterchange \
-mllvm --enable-nontrivial-unswitch \
-mllvm --enable-interleaved-mem-accesses \
-mllvm --enable-tbaa \
-mllvm --enable-global-merge \
-Xclang -fmerge-functions \
-mllvm -aarch64-enable-collect-loh \
-mllvm -aarch64-enable-atomic-cfg-tidy \
-mllvm -aarch64-enable-branch-targets \
-mllvm -enable-arm-maskedldst \
-mllvm -enable-pipeliner \
-mllvm --join-globalcopies \
-mllvm -enable-ipra \
-mllvm -enable-memcpyopt-without-libcalls \
-mllvm -aarch64-load-store-renaming \
-mllvm -enable-shrink-wrap \
-mllvm -enable-gvn-sink \
-mllvm -enable-gvn-hoist \
-mllvm -misched-fusion \
-mllvm -enable-aa-sched-mi \
-mllvm --aarch64-enable-mcr \
-mllvm --misched-postra \
-mllvm --aggressive-machine-cse \
-mllvm --aarch64-enable-ldst-opt \
-mllvm --enable-if-conversion \
-mllvm --aarch64-enable-cond-br-tune \
-mllvm --aarch64-enable-sink-fold \
-mllvm --loop-predication-skip-profitability-checks \
-mllvm --enable-ext-tsp-block-placement \
-mllvm --enable-chr \
-mllvm --enable-constraint-elimination \
-mllvm --aarch64-enable-gep-opt \
-mllvm --aarch64-enable-copyelim \
-mllvm --enable-dfa-jump-thread \
-mllvm --aarch64-enable-simd-scalar \
-mllvm --aarch64-enable-ccmp \
-mllvm --aarch64-enable-condopt \
-mllvm --enable-subreg-liveness \
-mllvm --enable-local-reassign \
-mllvm --aarch64-enable-logical-imm \
-mllvm --combiner-store-merging \
-mllvm --combiner-global-alias-analysis \
-mllvm --enable-partial-inlining \
-mllvm --enable-gvn-memdep \
-mllvm --twoaddr-reschedule"

export COMMON_CFLAGS_BASE="-fomit-frame-pointer -fno-semantic-interposition -ffunction-sections -fdata-sections --sysroot=${SYSROOT} -DNDEBUG -fno-unwind-tables -fno-asynchronous-unwind-tables -fPIC -fPIE -fno-stack-protector -fstrict-aliasing -fno-exceptions -fno-rtti -fno-plt -fno-sanitize=all ${TARGET_FEATURES} ${GLOBAL_DEEP_OPTS}"

export COMMON_LDFLAGS_BASE="-Wl,-O3 -Wl,--gc-sections --sysroot=${SYSROOT} -Wl,--sort-section=alignment -Wl,-z,noseparate-code -Wl,--optimize-bb-jumps"

# ------------------------------------------------------------------------------
# Workspace Initialization
# ------------------------------------------------------------------------------
info "Purging previous build artifacts..."
rm -rf "$BUILD_DIR" "$INSTALL_DIR"
mkdir -p "$BUILD_DIR"
mkdir -p "$INSTALL_DIR"

# ------------------------------------------------------------------------------
# FFmpeg Flags
# ------------------------------------------------------------------------------
PERF_FLAGS="-mllvm -cost-kind=throughput \
-mllvm -polly -mllvm -polly-vectorizer=stripmine \
-mllvm --enable-loopinterchange \
-mllvm -enable-loop-distribute \
-mllvm -enable-unroll-and-jam \
-mllvm -enable-loop-flatten \
-mllvm -polly-invariant-load-hoisting \
-mllvm -disable-complex-addr-modes \
-fvectorize -fslp-vectorize -falign-functions=64 \
-fvisibility=hidden -fno-semantic-interposition -funroll-loops \
-fno-math-errno -fno-trapping-math \
-fno-reciprocal-math -fno-associative-math \
-mllvm --vectorizer-maximize-bandwidth \
-mllvm --slp-vectorize-hor \
-mllvm --aggressive-ext-opt \
-mllvm --enable-load-pre \
-mllvm --enable-cond-stores-vec \
-mllvm --unroll-allow-partial \
-mllvm --enable-loop-versioning-licm \
-mllvm --force-precise-rotation-cost \
-mllvm --lsr-complexity-limit=32768 \
-mllvm --slp-vectorize-hor-store \
-mllvm --enable-masked-interleaved-mem-accesses \
-mllvm --aarch64-enable-ext-to-tbl \
-mllvm --enable-complex-deinterleaving \
-mllvm --aarch64-enable-mgather-combine \
-mllvm --enable-split-backedge-in-load-pre"

export CFLAGS_FFMPEG="-O3 ${COMMON_CFLAGS_BASE} ${COMMON_LTO_FLAGS} ${PERF_FLAGS}"

export LDFLAGS_OPTS_FFMPEG="-fuse-ld=lld -Wl,--icf=all -Wl,-z,noexecstack -Wl,--build-id=none -Wl,--lto-O3 \
-Wl,-mllvm,-import-instr-limit=2000 -Wl,-mllvm,-unroll-threshold=2000 \
-Wl,-mllvm,-polly \
-Wl,-z,max-page-size=16384"

export LDFLAGS_FFMPEG="${COMMON_LDFLAGS_BASE} ${COMMON_LTO_FLAGS} ${LDFLAGS_OPTS_FFMPEG}"
export FFMPEG_LDEXEFLAGS="-static ${LDFLAGS_FFMPEG}"

# ------------------------------------------------------------------------------
# TinyALSA Flags
# ------------------------------------------------------------------------------
LINEARITY_OPTS="-mllvm -cost-kind=latency \
-mllvm -enable-machine-outliner=never \
-mllvm -enable-tail-merge \
-mllvm -enable-memcpyopt-without-libcalls \
-mllvm --aarch64-enable-loop-data-prefetch \
-mllvm --enable-nonnull-arg-prop \
-mllvm --dse-optimize-memoryssa \
-mllvm --disable-spill-fusing \
-mllvm --aarch64-enable-early-ifcvt"

export CFLAGS_ALSA="-O2 ${COMMON_CFLAGS_BASE} \
-mllvm -inline-threshold=1500 \
-fvectorize -fslp-vectorize \
-fno-unroll-loops \
-falign-functions=64 \
-falign-loops=32 \
${LINEARITY_OPTS}"

export LDFLAGS_OPTS_ALSA="-fuse-ld=lld -Wl,--icf=all -Wl,-z,noexecstack -Wl,--build-id=none -Wl,-z,max-page-size=16384"
export LDFLAGS_ALSA="${COMMON_LDFLAGS_BASE} ${LDFLAGS_OPTS_ALSA}"

info "Active TARGET_TRIPLE: $TARGET_TRIPLE"

# ------------------------------------------------------------------------------
# Fetch Source Code
# ------------------------------------------------------------------------------
info "Initializing source tree synchronization via git..."
cd "$BUILD_DIR"

info "Fetching alsa-lib (Branch: $ALSA_LIB_BRANCH)..."
git clone --depth 1 --branch "$ALSA_LIB_BRANCH" "$ALSA_LIB_REPO" alsa-lib

info "Fetching tinyalsa (Branch: $TINYALSA_BRANCH)..."
git clone --depth 1 --branch "$TINYALSA_BRANCH" "$TINYALSA_REPO" tinyalsa

info "Fetching FFmpeg (Branch: $FFMPEG_BRANCH)..."
git clone --depth 1 --branch "$FFMPEG_BRANCH" "$FFMPEG_REPO" ffmpeg

# ==============================================================================
# SOURCE PATCHING: TINYPLAY INJECTION
# ==============================================================================
# Preserving the invoking script's directory for local asset resolution
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cd "${BUILD_DIR}/tinyalsa" || err "Failed to enter tinyalsa directory"
TINYPLAY_SRC="utils/tinyplay.c"

# Resolving tinyplay.c source (Local dev override vs Remote production fetch)
if [ -f "$SCRIPT_DIR/tinyplay.c" ]; then
    info "[LOCAL DEV] Deploying local source override: $SCRIPT_DIR/tinyplay.c"
    cp "$SCRIPT_DIR/tinyplay.c" "$TINYPLAY_SRC"
else
    RAW_GITHUB_URL="https://raw.githubusercontent.com/ortom-io/nlplayer-engine/main/tinyplay.c?nocache=$(date +%s)"
    info "[REMOTE] Fetching upstream tinyplay.c..."
    curl -sSL "$RAW_GITHUB_URL" -o "$TINYPLAY_SRC"
fi

[ -s "$TINYPLAY_SRC" ] || err "tinyplay.c payload is missing or empty."
ok "tinyplay.c source tree integration successful."

cd "$BUILD_DIR"
# ==============================================================================

ALSA_LIB_VERSION=$(cd alsa-lib && git rev-parse --short HEAD)
TINYALSA_VERSION=$(cd tinyalsa && git rev-parse --short HEAD)
FFMPEG_VERSION=$(cd ffmpeg && git rev-parse --short HEAD)

info "Resolved Commit Hashes:"
info " - alsa-lib : $ALSA_LIB_VERSION"
info " - tinyalsa : $TINYALSA_VERSION"
info " - FFmpeg   : $FFMPEG_VERSION"

# ------------------------------------------------------------------------------
# Build Phase: alsa-lib (Static)
# ------------------------------------------------------------------------------
info "Initiating alsa-lib build sequence ($ALSA_LIB_VERSION) with -O3 optimization level..."
cd "${BUILD_DIR}/alsa-lib"

info "Executing autoreconf for configure script generation..."
autoreconf -vfi

# Configuration: STRICTLY PLAYBACK ENGINE ONLY (Minimal Footprint)
./configure \
    --host=${TARGET_TRIPLE} \
    --prefix=${ALSA_PREFIX} \
    --enable-static \
    --disable-shared \
    --disable-python \
    --disable-aload \
    --disable-topology \
    --disable-alisp \
    --disable-server \
    --disable-mixer \
    --disable-rawmidi \
    --disable-seq \
    --disable-hwdep \
    --disable-ucm \
    --disable-old-symbols \
    --without-debug \
    --with-pcm-plugins=hw,plug \
    CFLAGS="${CFLAGS_ALSA}" \
    LDFLAGS="${LDFLAGS_ALSA}" \
    LIBS="-pthread -lm -ldl"

make -j$(nproc)
make install DESTDIR="$INSTALL_DIR"

info "Purging obsolete libtool (.la) archives and aclocal metadata..."
find "$INSTALL_DIR/$ALSA_INSTALL_ROOT_PATH/lib" -name "*.la" -delete
rm -rf "$INSTALL_DIR/$ALSA_INSTALL_ROOT_PATH/share/aclocal"

# CRITICAL HOTFIX: Patching alsa.pc for rigorous cross-compilation validation
PC_FILE="$INSTALL_DIR/$ALSA_INSTALL_ROOT_PATH/lib/pkgconfig/alsa.pc"
if [ -f "$PC_FILE" ]; then
    info "Patching $PC_FILE: Enforcing absolute device-prefix, overriding threading links..."
    sed -i -E "s|^prefix=.*|prefix=${ALSA_PREFIX}|" "$PC_FILE"
    sed -i 's/\-lrt//g' "$PC_FILE"
    sed -i 's/\-lpthread/\-pthread/g' "$PC_FILE"

    if grep -q -- "\-lpthread" "$PC_FILE" || grep -q -- "\-lrt" "$PC_FILE"; then
        err "Incompatible link flags (-lpthread or -lrt) persisted in $PC_FILE post-patch."
    fi
else
    err "$PC_FILE missing post-installation phase."
fi

# ------------------------------------------------------------------------------
# Build Phase: tinyplay (Modified, ALSA-lib backend)
# ------------------------------------------------------------------------------
info "Initiating compilation sequence for tinyplay (ALSA-lib backend)..."
cd "${BUILD_DIR}/tinyalsa/utils"

mkdir -p ../bin
rm -f ../bin/tinyplay

# Direct Clang invocation
# Linking aggressively against static libasound.a
"$CC" tinyplay.c -o "../bin/tinyplay" \
    -I"${INSTALL_DIR}/${ALSA_INSTALL_ROOT_PATH}/include" \
    ${CFLAGS_ALSA} \
    ${LDFLAGS_ALSA} \
    -static \
    "${INSTALL_DIR}/${ALSA_INSTALL_ROOT_PATH}/lib/libasound.a" \
    -ldl -lm -pthread

[ -f "../bin/tinyplay" ] || err "Compilation sequence for tinyplay failed."
ok "tinyplay compilation successful."

cd "${BUILD_DIR}/tinyalsa"

# ------------------------------------------------------------------------------
# Build Phase: FFmpeg (Static Decoder, sans ALSA-lib)
# ------------------------------------------------------------------------------
info "Initiating FFmpeg build sequence ($FFMPEG_VERSION) with -O3 optimization level..."
cd "${BUILD_DIR}/ffmpeg"

FFMPEG_BASE_CFLAGS="${CFLAGS_FFMPEG}"

info "Executing configure script for FFmpeg..."
./configure \
--prefix="$INSTALL_DIR" \
--cross-prefix=${TARGET_TRIPLE}- \
--target-os=android \
--arch=${FFMPEG_ARCH} \
--cpu=${FFMPEG_CPU} \
--enable-static \
--disable-shared \
--enable-lto \
--enable-ffmpeg \
--disable-ffprobe \
--disable-ffplay \
--disable-doc \
--disable-avdevice \
--disable-swscale \
--disable-network \
--disable-autodetect \
--disable-debug \
--disable-stripping \
--disable-filters \
--enable-swresample \
--enable-filter=aresample \
--enable-filter=aformat \
--enable-filter=pan \
--enable-filter=volume \
--enable-filter=volumedetect \
--disable-libsoxr \
--enable-gpl \
--enable-version3 \
--disable-nonfree \
--disable-alsa \
--disable-sndio \
--enable-zlib \
--disable-indevs \
--disable-outdevs \
--disable-bsfs \
--enable-bsf=aac_adtstoasc \
--enable-protocol=file \
--enable-protocol=pipe \
--disable-demuxers \
--enable-demuxer=aac \
--enable-demuxer=ac3 \
--enable-demuxer=aiff \
--enable-demuxer=ape \
--enable-demuxer=asf \
--enable-demuxer=au \
--enable-demuxer=caf \
--enable-demuxer=dsf \
--enable-demuxer=dts \
--enable-demuxer=eac3 \
--enable-demuxer=flac \
--enable-demuxer=matroska \
--enable-demuxer=mov \
--enable-demuxer=mp3 \
--enable-demuxer=ogg \
--enable-demuxer=tta \
--enable-demuxer=wav \
--enable-demuxer=wv \
--disable-decoders \
--enable-decoder=aac \
--enable-decoder=ac3 \
--enable-decoder=alac \
--enable-decoder=ape \
--enable-decoder=dca \
--enable-decoder=dsd_lsbf \
--enable-decoder=dsd_msbf \
--enable-decoder=dsd_lsbf_planar \
--enable-decoder=dsd_msbf_planar \
--enable-decoder=eac3 \
--enable-decoder=flac \
--enable-decoder=mp3 \
--enable-decoder=opus \
--enable-decoder=pcm_s16le \
--enable-decoder=pcm_s24le \
--enable-decoder=pcm_s32le \
--enable-decoder=truehd \
--enable-decoder=vorbis \
--enable-decoder=wavpack \
--enable-decoder=mjpeg \
--enable-decoder=png \
--enable-decoder=bmp \
--disable-parsers \
--enable-parser=aac \
--enable-parser=ac3 \
--enable-parser=dca \
--enable-parser=flac \
--enable-parser=opus \
--enable-parser=vorbis \
--enable-parser=png \
--enable-parser=mjpeg \
--disable-encoders \
--enable-encoder=pcm_s16le \
--enable-encoder=pcm_s24le \
--enable-encoder=pcm_s32le \
--enable-encoder=mjpeg \
--disable-muxers \
--enable-muxer=null \
--enable-muxer=image2 \
--enable-muxer=mjpeg \
--enable-muxer=wav \
--enable-muxer=pcm_s16le \
--enable-muxer=pcm_s24le \
--enable-muxer=pcm_s32le \
--enable-hardcoded-tables \
--enable-runtime-cpudetect \
--enable-pic \
--enable-asm \
--enable-neon \
--enable-inline-asm \
--extra-cflags="${FFMPEG_BASE_CFLAGS}" \
--extra-ldflags="${LDFLAGS_FFMPEG}" \
--extra-ldexeflags="${FFMPEG_LDEXEFLAGS}" \
--extra-libs="-pthread -lm -ldl -lz" \
--nm="llvm-nm" \
--ar="llvm-ar" \
--ranlib="llvm-ranlib" \
--cc="${CC}" \
--cxx="${CXX}" \
--ld="${CC}"

ok "FFmpeg configure phase complete."
make -j$(nproc)
ok "FFmpeg build complete."
make install

# ------------------------------------------------------------------------------
# Final Assembly and Packaging
# ------------------------------------------------------------------------------
info "Assembling final artifacts..."

rm -rf "$FINAL_PACKAGE_DIR"
mkdir -p "$FINAL_PACKAGE_DIR/bin"

# Export specifically tinyplay (omitting tinymix, tinycap, etc.)
info "Copying tinyplay executable..."
if [ -f "${BUILD_DIR}/tinyalsa/bin/tinyplay" ]; then
    cp "${BUILD_DIR}/tinyalsa/bin/tinyplay" "$FINAL_PACKAGE_DIR/bin/"
else
    err "tinyplay artifact missing from build directory."
fi

info "Copying FFmpeg executable..."
cp "$INSTALL_DIR/bin/ffmpeg" "$FINAL_PACKAGE_DIR/bin/ffmpeg"

info "Migrating ALSA configurations (share tree)..."
if [ -d "$INSTALL_DIR/$ALSA_INSTALL_ROOT_PATH/share" ]; then
    cp -r "$INSTALL_DIR/$ALSA_INSTALL_ROOT_PATH/share" "$FINAL_PACKAGE_DIR/bin/"
else
    warn "ALSA configuration directory (share) missing from output."
fi

info "Stripping debug symbols from final binaries..."
find "$FINAL_PACKAGE_DIR/bin" -maxdepth 1 -type f -executable -exec "$STRIP" --strip-all {} \;

# ------------------------------------------------------------------------------
# Dependency Verification Routine
# ------------------------------------------------------------------------------
check_dependencies() {
    local binary_path="$1"
    local binary_name="$(basename "$binary_path")"
    local readelf_output
    
    readelf_output="$(llvm-readelf -d "$binary_path" 2>&1)" || true
    if echo "$readelf_output" | grep -qE "(NEEDED|RUNPATH)"; then
        err "$binary_name: Dynamic linkage detected (NOT static)!\n$(echo "$readelf_output" | grep -E "NEEDED|RUNPATH|RPATH|INTERP")"
    else
        if echo "$readelf_output" | grep -q INTERP; then
            warn "$binary_name: .interp section detected."
        else
            ok "$binary_name: Validation OK (Strictly Static)."
        fi
    fi
    file "$binary_path" || true
}

echo ""
info "=============================================================================="
info "BINARY VALIDATION PROTOCOL INITIATED"
info "=============================================================================="
for bin_file in "$FINAL_PACKAGE_DIR/bin/"*; do
    if [ -f "$bin_file" ] && [ -x "$bin_file" ]; then
        check_dependencies "$bin_file"
    fi
done

# ------------------------------------------------------------------------------
# Archive Generation & Version Bumping
# ------------------------------------------------------------------------------
echo ""
info "=============================================================================="
info "COMPRESSION AND RELEASE VERSION BUMP"
info "=============================================================================="

# Centralized target locations to uphold DRY principle
PROJECT_SOURCE_DIR="$HOME/NLPlayer/app/src/main"
ASSETS_ARCHIVE="${PROJECT_SOURCE_DIR}/assets/tools.7z"
KOTLIN_INSTALLER="${PROJECT_SOURCE_DIR}/java/com/art/nlplayer/ToolsInstaller.kt"
FALLBACK_ARCHIVE="${FINAL_PACKAGE_DIR}/bin/tools.7z"

# Path logic and Kotlin source modification
if [ -f "$ASSETS_ARCHIVE" ]; then
    TARGET_ARCHIVE="$ASSETS_ARCHIVE"
    info "Target mapped to application assets. Overwriting existing payload."
    
    if [ -f "$KOTLIN_INSTALLER" ]; then
        CURRENT_VERSION=$(grep -oP 'private const val BINARY_VERSION = \K[0-9]+' "$KOTLIN_INSTALLER")
        
        if [ -n "$CURRENT_VERSION" ]; then
            NEW_VERSION=$((CURRENT_VERSION + 1))
            sed -i "s/private const val BINARY_VERSION = $CURRENT_VERSION/private const val BINARY_VERSION = $NEW_VERSION/" "$KOTLIN_INSTALLER"
            ok "Kotlin source tree patched: BINARY_VERSION updated ($CURRENT_VERSION -> $NEW_VERSION)"
        else
            warn "ToolsInstaller.kt exists, but BINARY_VERSION declaration is missing."
        fi
    else
        warn "ToolsInstaller.kt missing from source tree. Version bump aborted."
    fi
else
    TARGET_ARCHIVE="$FALLBACK_ARCHIVE"
    mkdir -p "$(dirname "$TARGET_ARCHIVE")"
    info "Asset path unresolved. Utilizing local fallback archive path."
fi

info "Writing output to: $TARGET_ARCHIVE"
rm -f "$TARGET_ARCHIVE"

# Context switch to bin dir to enforce flat structure within the archive
cd "$FINAL_PACKAGE_DIR/bin" || err "Failed to enter final package directory"

7z a -t7z -m0=lzma2:d=128m:fb=273 -mx=9 -mf=off -ms=on "$TARGET_ARCHIVE" tinyplay ffmpeg share >/dev/null

cd - >/dev/null

echo ""
ok "TOOLCHAIN EXECUTION COMPLETE."
ok "Final deployment archive established: ${TARGET_ARCHIVE}"