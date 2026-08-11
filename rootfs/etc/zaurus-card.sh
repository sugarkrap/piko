
ZAURUS_CARD_ROOT=/mnt/card/.zaurus
export ZAURUS_CARD_ROOT

case ":$PATH:" in
    *":$ZAURUS_CARD_ROOT/usr/bin:"*) ;;
    *) PATH="$PATH:$ZAURUS_CARD_ROOT/usr/bin" ;;
esac
export PATH
