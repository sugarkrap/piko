# /etc/zaurus-card.sh -- make software on the SD card usable when a card is
# present, and cost nothing when it is not.
#
# Sourced by /etc/profile (ash) and /etc/zshrc (zsh). Not executable, not a
# program: it only sets environment variables.
#
# LAYOUT. Software and data that are too big for the ~68 MiB root jffs2 live
# in a hidden directory on the card, so a card can still be used normally for
# ordinary files without a pile of system directories cluttering its root:
#
#   /mnt/card/.zaurus/
#       usr/bin/        binaries (mplayer, ...)
#       usr/lib/        libraries, if anything ever needs them (everything we
#                       build today is static -- this rootfs has no dynamic
#                       linker at all)
#       usr/share/      data files
#
# WHY THE PATH ENTRY IS UNCONDITIONAL. It is deliberately added whether or
# not a card is mounted. A PATH element that does not exist is simply skipped
# during command lookup -- it is not an error and costs one failed stat per
# lookup. Making it unconditional means:
#
#   * no detection logic, and nothing to go wrong in it;
#   * shells started BEFORE a card was inserted still find card software
#     afterwards (subject to the shell's command hash -- see `sdapps`,
#     which clears it for you);
#   * inserting or removing a card never leaves a shell with a stale PATH.
#
# The alternative -- rewriting PATH from an mdev hook on insertion -- cannot
# work anyway: a hook runs in its own process and cannot alter the
# environment of shells that are already running.
#
# NOTE: nothing here executes anything from the card. An earlier sketch had
# the insert hook run a script off the card automatically; that is a bad
# idea on removable media (anyone who hands you a card would get code
# execution as root on insertion), and it is not needed -- putting the
# binaries on PATH is enough for the user to run what they want.

ZAURUS_CARD_ROOT=/mnt/card/.zaurus
export ZAURUS_CARD_ROOT

case ":$PATH:" in
    *":$ZAURUS_CARD_ROOT/usr/bin:"*) ;;
    *) PATH="$PATH:$ZAURUS_CARD_ROOT/usr/bin" ;;
esac
export PATH
