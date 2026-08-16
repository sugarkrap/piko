#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SRC="$REPO/userspace/src/phoneme/phoneme_feature"
BUILD="${BUILD:-$REPO/userspace/build-phoneme}"
STAGE_DIR="${STAGE_DIR:-$REPO/userspace/stage-phoneme}"
SDL_STAGE="${SDL_STAGE:-$REPO/userspace/stage-sdl}"
DEPS_STAGE="${DEPS_STAGE:-$REPO/userspace/stage-target}"
CACHE_DIR="${CACHE_DIR:-$REPO/userspace/.thirdparty-cache}"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
HOST="${HOST:-arm-unknown-linux-uclibcgnueabi}"

XALAN_VERSION="${XALAN_VERSION:-2.7.3}"
XALAN_JAR="$CACHE_DIR/xalan-$XALAN_VERSION.jar"
SERIALIZER_JAR="$CACHE_DIR/serializer-$XALAN_VERSION.jar"
XALAN_URL="https://repo1.maven.org/maven2/xalan/xalan/$XALAN_VERSION/xalan-$XALAN_VERSION.jar"
SERIALIZER_URL="https://repo1.maven.org/maven2/xalan/serializer/$XALAN_VERSION/serializer-$XALAN_VERSION.jar"
XALAN_SHA256="febd48bb133a96c447282213951a6b74ea7fb45c0d896121296c014316bda6b0"
SERIALIZER_SHA256="5f6804bacdfdb3ccc52d2538536fab8986696d61559b081054a420c653806667"

if [ -n "${PIKO_VM_TRACE:-}" ]; then
    ENABLE_TTY_TRACE=true
    export ENABLE_TTY_TRACE
fi

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if [ ! -f "$SRC/midp/build/linux_sdl_gcc/GNUmakefile" ]; then
    echo "tools/userspace/build-phoneme.sh: $SRC is empty" >&2
    echo "  it is a git submodule -- run:" >&2
    echo "    git submodule update --init userspace/src/phoneme" >&2
    exit 1
fi

if ! command -v "$TOOLCHAIN_BIN_DIR/$HOST-gcc" >/dev/null 2>&1; then
    echo "tools/userspace/build-phoneme.sh: no cross compiler at $TOOLCHAIN_BIN_DIR/$HOST-gcc" >&2
    exit 1
fi

for lib in libSDL.so libSDL_image.so libSDL_mixer.so; do
    if [ ! -e "$SDL_STAGE/usr/lib/$lib" ]; then
        echo "tools/userspace/build-phoneme.sh: $SDL_STAGE/usr/lib/$lib missing" >&2
        echo "  run tools/userspace/build-sdl.sh, build-sdl-image.sh and build-sdl-mixer.sh first" >&2
        exit 1
    fi
done

if [ -z "${JDK_DIR:-}" ]; then
    for candidate in /usr/lib/jvm/java-8-openjdk /usr/lib/jvm/java-8-openjdk-amd64 \
                     /usr/lib/jvm/java-7-openjdk /usr/lib/jvm/java-6-openjdk; do
        [ -x "$candidate/bin/javac" ] && JDK_DIR="$candidate" && break
    done
fi
if [ -z "${JDK_DIR:-}" ] || [ ! -x "$JDK_DIR/bin/javac" ]; then
    echo "tools/userspace/build-phoneme.sh: no suitable JDK found -- set JDK_DIR" >&2
    echo "  needs a javac that still accepts -source 1.3 and a jre/lib/rt.jar (JDK 8 or older)" >&2
    exit 1
fi
if [ ! -f "$JDK_DIR/jre/lib/rt.jar" ]; then
    echo "tools/userspace/build-phoneme.sh: $JDK_DIR has no jre/lib/rt.jar" >&2
    echo "  cldc's build checks for it; JDK 9+ will not work here" >&2
    exit 1
fi

probe="$(mktemp -d)"
cat > "$probe/Probe.java" <<'EOF'
public class Probe { public static void main(String[] a) { } }
EOF
if ! "$JDK_DIR/bin/javac" -source 1.3 -target 1.3 -d "$probe" "$probe/Probe.java" >/dev/null 2>&1; then
    rm -rf "$probe"
    echo "tools/userspace/build-phoneme.sh: $JDK_DIR/bin/javac cannot emit 1.3 bytecode" >&2
    echo "  cldc's preverifier only accepts class file major 45-48" >&2
    exit 1
fi
rm -rf "$probe"

if ! echo 'int main(void){return 0;}' | gcc -m32 -x c - -o /dev/null 2>/dev/null; then
    echo "tools/userspace/build-phoneme.sh: this host cannot build 32-bit binaries (gcc -m32 failed)" >&2
    echo "  the cldc/midp host tools assume 32-bit pointers; install the multilib runtime" >&2
    exit 1
fi

fetch_jar() {
    path="$1"; url="$2"; want="$3"
    mkdir -p "$CACHE_DIR"
    if [ ! -f "$path" ]; then
        echo "==> downloading $url"
        curl -fL -o "$path.partial" "$url"
        mv "$path.partial" "$path"
    fi
    got="$(sha256sum "$path" | cut -d' ' -f1)"
    if [ "$got" != "$want" ]; then
        echo "tools/userspace/build-phoneme.sh: SHA-256 mismatch for $path" >&2
        echo "  expected: $want" >&2
        echo "  actual:   $got" >&2
        exit 1
    fi
}
fetch_jar "$XALAN_JAR" "$XALAN_URL" "$XALAN_SHA256"
fetch_jar "$SERIALIZER_JAR" "$SERIALIZER_URL" "$SERIALIZER_SHA256"

OUT="$STAGE_DIR/usr/local/lib/phoneme/bin/runMidlet"
if [ "$FORCE" -eq 0 ] && [ -f "$OUT" ]; then
    NEWER="$(find "$SRC" -name .git -prune -o -newer "$OUT" -print 2>/dev/null | head -1)"
    if [ -z "$NEWER" ] && [ ! "$0" -nt "$OUT" ]; then
        echo "==> $OUT already up to date (pass --force to rebuild)"
        exit 0
    fi
fi

if [ "$FORCE" -eq 1 ]; then
    echo "==> --force: removing $BUILD"
    rm -rf "$BUILD"
fi
mkdir -p "$BUILD"

echo "==> preparing host toolchain wrappers (32-bit)"
mkdir -p "$BUILD/hosttools/bin"
for t in gcc g++ cc c++; do
    real="$(command -v "$t" 2>/dev/null || true)"
    [ -n "$real" ] || continue
    printf '#!/bin/sh\nexec %s -m32 "$@"\n' "$real" > "$BUILD/hosttools/bin/$t"
    chmod 0755 "$BUILD/hosttools/bin/$t"
done
printf '#!/bin/sh\nexec %s --32 "$@"\n' "$(command -v as)" > "$BUILD/hosttools/bin/as"
printf '#!/bin/sh\nexec %s -m elf_i386 "$@"\n' "$(command -v ld)" > "$BUILD/hosttools/bin/ld"
chmod 0755 "$BUILD/hosttools/bin/as" "$BUILD/hosttools/bin/ld"

echo "==> preparing cross toolchain shim (unprefixed names)"
mkdir -p "$BUILD/toolchain/bin"
for t in gcc g++ cpp as ar ld ranlib strip nm objcopy objdump readelf size; do
    [ -x "$TOOLCHAIN_BIN_DIR/$HOST-$t" ] || continue
    ln -sf "$TOOLCHAIN_BIN_DIR/$HOST-$t" "$BUILD/toolchain/bin/$t"
done

PATH="$BUILD/hosttools/bin:$JDK_DIR/bin:$PATH"
export PATH JDK_DIR

PCSL_OUT="$BUILD/pcsl"
CLDC_HOST_OUT="$BUILD/cldc-host"
CLDC_ARM_OUT="$BUILD/cldc-arm"
MIDP_OUT="$BUILD/midp"
mkdir -p "$PCSL_OUT" "$CLDC_HOST_OUT" "$CLDC_ARM_OUT" "$MIDP_OUT"

XSLT_CP=":$XALAN_JAR:$SERIALIZER_JAR"
XSLT_FLAGS="-Djavax.xml.transform.TransformerFactory=org.apache.xalan.processor.TransformerFactoryImpl"

echo ""
echo "==> [1/5] pcsl (host i386)"
make -C "$SRC/pcsl" PCSL_PLATFORM=linux_i386_gcc PCSL_OUTPUT_DIR="$PCSL_OUT" \
    NETWORK_MODULE=bsd/generic

echo ""
echo "==> [2/5] pcsl (target arm)"
make -C "$SRC/pcsl" PCSL_PLATFORM=linux_arm_gcc PCSL_OUTPUT_DIR="$PCSL_OUT" \
    NETWORK_MODULE=bsd/generic \
    CC="$TOOLCHAIN_BIN_DIR/$HOST-gcc" \
    CPP="$TOOLCHAIN_BIN_DIR/$HOST-g++" \
    LD="$TOOLCHAIN_BIN_DIR/$HOST-g++" \
    AR="$TOOLCHAIN_BIN_DIR/$HOST-ar -rc"

echo ""
echo "==> [3/5] cldc (host i386: vm, romgen, preverify)"
JVMWorkSpace="$SRC/cldc" JVMBuildSpace="$CLDC_HOST_OUT" \
    make -C "$SRC/cldc/build/linux_i386" host_arch=i386 \
        ENABLE_PCSL=true PCSL_OUTPUT_DIR="$PCSL_OUT" ENABLE_ISOLATES=true \
        ENABLE_COMPILATION_WARNINGS=true

echo ""
echo "==> [4/5] cldc (target arm)"
JVMWorkSpace="$SRC/cldc" JVMBuildSpace="$CLDC_ARM_OUT" \
    make -C "$SRC/cldc/build/linux_arm" host_arch=i386 \
        ENABLE_PCSL=true PCSL_OUTPUT_DIR="$PCSL_OUT" ENABLE_ISOLATES=true \
        GNU_TOOLS_DIR="$BUILD/toolchain" ENABLE_COMPILATION_WARNINGS=true

echo ""
echo "==> [5/5] midp (target arm, sdl)"
MIDP_OUTPUT_DIR="$MIDP_OUT" \
    make -C "$SRC/midp/build/linux_sdl_gcc" \
        USE_SDL_ABB=true TARGET_CPU=arm USE_MULTIPLE_ISOLATES=true \
        JAVAC_SOURCE_TARGET="-source 1.3 -target 1.3" \
        PIKO_SDL_STAGE="$SDL_STAGE" PIKO_DEPS_STAGE="$DEPS_STAGE" \
        PIKO_XSLT_CP="$XSLT_CP" PIKO_XSLT_FLAGS="$XSLT_FLAGS" \
        PCSL_OUTPUT_DIR="$PCSL_OUT" \
        CLDC_DIST_DIR="$CLDC_ARM_OUT/linux_arm/dist" \
        TOOLS_DIR="$SRC/tools" \
        GNU_TOOLS_DIR="$BUILD/toolchain"

RUNMIDLET="$MIDP_OUT/bin/arm/runMidlet"
if [ ! -f "$RUNMIDLET" ]; then
    echo "tools/userspace/build-phoneme.sh: make finished but there is no $RUNMIDLET" >&2
    exit 1
fi

echo ""
echo "==> staging to $STAGE_DIR"
PM="$STAGE_DIR/usr/local/lib/phoneme"
rm -rf "$PM"
mkdir -p "$PM/bin" "$PM/lib" "$PM/appdb"
cp "$RUNMIDLET" "$PM/bin/runMidlet"
chmod 0755 "$PM/bin/runMidlet"
"$TOOLCHAIN_BIN_DIR/$HOST-strip" "$PM/bin/runMidlet" 2>/dev/null || true

for helper in installMidlet listMidlets.sh removeMidlet.sh; do
    if [ -f "$MIDP_OUT/bin/arm/$helper" ]; then
        cp "$MIDP_OUT/bin/arm/$helper" "$PM/bin/$helper"
        chmod 0755 "$PM/bin/$helper"
    else
        echo "tools/userspace/build-phoneme.sh: expected $helper next to runMidlet" >&2
        exit 1
    fi
done
cp "$MIDP_OUT/lib/"* "$PM/lib/" 2>/dev/null || true
[ -d "$MIDP_OUT/appdb" ] && cp -a "$MIDP_OUT/appdb/." "$PM/appdb/" 2>/dev/null || true

needed="$("$TOOLCHAIN_BIN_DIR/$HOST-readelf" -d "$PM/bin/runMidlet" \
    | grep -oE '\[lib[^]]+\]' | tr -d '[]' | tr '\n' ' ')"
echo "    NEEDED: $needed"
for want in libSDL-1.2.so.0 libSDL_image-1.2.so.0 libSDL_mixer-1.2.so.0; do
    case " $needed " in
        *" $want "*) : ;;
        *)
            echo "tools/userspace/build-phoneme.sh: runMidlet does not NEED $want" >&2
            exit 1
            ;;
    esac
done

elf_flags="$("$TOOLCHAIN_BIN_DIR/$HOST-readelf" -h "$PM/bin/runMidlet" | sed -n 's/^ *Flags: *//p')"
case "$elf_flags" in
    0x5000200*) : ;;
    *)
        echo "tools/userspace/build-phoneme.sh: unexpected ELF Flags: $elf_flags" >&2
        echo "  want 0x5000200 (Version5 EABI, soft-float ABI)" >&2
        exit 1
        ;;
esac
echo "    Flags: $elf_flags"

echo ""
echo "==> done: $PM/bin/runMidlet ($(du -h "$PM/bin/runMidlet" | cut -f1))"
echo "    skins:  $PM/lib ($(ls "$PM/lib" | wc -l) files)"
echo "    appdb:  $PM/appdb"
echo ""
