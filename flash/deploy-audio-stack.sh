#!/bin/sh
set -eu

# Deploy Corgi/Husky audio modules + short helper scripts to a live Zaurus.
# Usage:
#   flash/deploy-audio-stack.sh [root@ip]
# Example:
#   flash/deploy-audio-stack.sh root@10.43.112.72

TARGET="${1:-root@10.43.112.72}"
KEY="${HOME}/.ssh/zaurus_ed25519"
KERNEL_DIR="/home/makaron/Code/zaurus-refresh/kernel-src/linux-7.1.4"
STAGE="/tmp/zaurus-audio-stage"

# NOTE: regmap-i2c is CONFIG_REGMAP_I2C=y (built into vmlinux) in this
# kernel's config, not a module -- there is no regmap-i2c.ko to copy or
# insmod. Don't re-add it here without checking .config first.
MODULES="
$KERNEL_DIR/sound/soundcore.ko
$KERNEL_DIR/sound/core/snd.ko
$KERNEL_DIR/sound/core/snd-timer.ko
$KERNEL_DIR/sound/core/snd-pcm.ko
$KERNEL_DIR/sound/core/snd-pcm-dmaengine.ko
$KERNEL_DIR/sound/arm/snd-pxa2xx-lib.ko
$KERNEL_DIR/sound/ac97_bus.ko
$KERNEL_DIR/sound/pci/ac97/snd-ac97-codec.ko
$KERNEL_DIR/sound/soc/snd-soc-core.ko
$KERNEL_DIR/sound/soc/pxa/snd-soc-pxa2xx.ko
$KERNEL_DIR/sound/soc/pxa/snd-soc-pxa2xx-i2s.ko
$KERNEL_DIR/sound/soc/codecs/snd-soc-wm8731.ko
$KERNEL_DIR/sound/soc/codecs/snd-soc-wm8731-i2c.ko
$KERNEL_DIR/sound/soc/pxa/snd-soc-corgi.ko
$KERNEL_DIR/sound/core/oss/snd-mixer-oss.ko
$KERNEL_DIR/sound/core/oss/snd-pcm-oss.ko
"

rm -rf "$STAGE"
mkdir -p "$STAGE/mods"

for m in $MODULES; do
    if [ ! -f "$m" ]; then
        echo "missing module: $m" >&2
        exit 1
    fi
    cp "$m" "$STAGE/mods/"
done

cat > "$STAGE/audioon" << 'EOF_AUDIOON'
#!/bin/sh
set -eu
KVER="$(uname -r)"
MD="/lib/modules/${KVER}/zaurus-audio"

load_ko() {
    ko="$1"
    name="$(echo "${ko%.ko}" | tr '-' '_')"
    if lsmod 2>/dev/null | grep -q "^${name} "; then
        return 0
    fi
    insmod "$MD/$ko"
}

load_ko soundcore.ko
load_ko snd.ko
load_ko snd-timer.ko
load_ko snd-pcm.ko
load_ko snd-pcm-dmaengine.ko
load_ko snd-pxa2xx-lib.ko
load_ko ac97_bus.ko
load_ko snd-ac97-codec.ko
load_ko snd-soc-core.ko
load_ko snd-soc-pxa2xx.ko
load_ko snd-soc-pxa2xx-i2s.ko
load_ko snd-soc-wm8731.ko
load_ko snd-soc-wm8731-i2c.ko
load_ko snd-soc-corgi.ko
load_ko snd-mixer-oss.ko
load_ko snd-pcm-oss.ko

[ -e /dev/mixer ] || mknod /dev/mixer c 14 0
[ -e /dev/dsp ] || mknod /dev/dsp c 14 3

echo "audio stack loaded"
ls -l /dev/dsp /dev/mixer
EOF_AUDIOON

cat > "$STAGE/audinfo" << 'EOF_AUDINFO'
#!/bin/sh
set -eu
uname -a
echo "-- lsmod (audio) --"
lsmod | grep -E 'snd|wm8731|corgi|regmap|i2c' || true
echo "-- /proc/asound --"
ls -la /proc/asound || true
cat /proc/asound/cards 2>/dev/null || true
echo "-- device nodes --"
ls -l /dev/dsp /dev/mixer 2>/dev/null || true
EOF_AUDINFO

chmod 0755 "$STAGE/audioon" "$STAGE/audinfo"

ssh -i "$KEY" -o StrictHostKeyChecking=accept-new "$TARGET" '
set -eu
KVER="$(uname -r)"
mkdir -p "/lib/modules/${KVER}/zaurus-audio" /usr/sbin
'

for m in $MODULES; do
    b="$(basename "$m")"
    cat "$m" | ssh -i "$KEY" -o StrictHostKeyChecking=accept-new "$TARGET" \
        'KVER="$(uname -r)"; cat > "/lib/modules/${KVER}/zaurus-audio/'"$b"'"'
done

cat "$STAGE/audioon" | ssh -i "$KEY" -o StrictHostKeyChecking=accept-new "$TARGET" \
    'cat > /usr/sbin/audioon'
cat "$STAGE/audinfo" | ssh -i "$KEY" -o StrictHostKeyChecking=accept-new "$TARGET" \
    'cat > /usr/sbin/audinfo'

ssh -i "$KEY" -o StrictHostKeyChecking=accept-new "$TARGET" '
set -eu
KVER="$(uname -r)"
MODDST="/lib/modules/${KVER}/zaurus-audio"
chmod 0755 /usr/sbin/audioon /usr/sbin/audinfo
sync
echo "deployed to $MODDST"
ls -1 "$MODDST"
'

echo "done: run on device: audioon ; audinfo"
