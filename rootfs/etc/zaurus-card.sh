
. /etc/piko-media
ZAURUS_CARD_ROOT="$PIKO_CARD_ROOT"
export ZAURUS_CARD_ROOT

case ":$PATH:" in
    *":$ZAURUS_CARD_ROOT/usr/bin:"*) ;;
    *) PATH="$PATH:$ZAURUS_CARD_ROOT/usr/bin" ;;
esac
export PATH
