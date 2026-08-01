#!/bin/sh
set -eu

# Generates the ROM manifest that ships to /etc/zaurus/manifest and is
# displayed by the Software Center's System Update tab (pikostore).
#
# Usage:
#   tools/gen-rom-manifest.sh [output-file]     # default: stdout
#
# Format -- an email-style header block, a blank line, then the changelog
# paragraph as free text:
#
#     PIKO-ROM-MANIFEST 1
#     version: r148
#     built: 2026-07-31T21:04:00Z
#     commit: 224c038
#
#     Software Center arrives, with a system update tab that can
#     install piko.tar packages straight from the SD card.
#
# The blank-line split is the whole parser contract: everything before it
# is "key: value", everything after it is prose to be word-wrapped on a
# 640x480 screen. That means a changelog paragraph can contain colons,
# blank-ish lines and punctuation without needing to be escaped or
# flattened onto one line.
#
# VERSIONING: the version is the git commit count (`git rev-list --count
# HEAD`), rendered as rN. That auto-increments on every commit with no
# tracked counter to bump, no build-time file mutation, and no way for a
# CI build and a local build of the same tree to disagree -- all of which
# a checked-in VERSION file gets wrong. It also maps straight back to an
# exact commit, which a date-based scheme does not.
#
# A dirty or unknown tree is marked, because a ROM built from uncommitted
# work is NOT the rN that its commit count claims:
#   r148      clean tree at that commit
#   r148+     uncommitted changes present -- do not trust this to match
#   r0        no git at all (tarball export); commit shows as "unknown"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-}"

CHANGELOG_FILE="${CHANGELOG_FILE:-$REPO/CHANGELOG}"

if git -C "$REPO" rev-parse --git-dir >/dev/null 2>&1; then
    count="$(git -C "$REPO" rev-list --count HEAD 2>/dev/null || echo 0)"
    commit="$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    # --porcelain covers staged, unstaged and untracked-but-not-ignored.
    if [ -n "$(git -C "$REPO" status --porcelain 2>/dev/null)" ]; then
        version="r${count}+"
    else
        version="r${count}"
    fi
else
    version="r0"
    commit="unknown"
fi

built="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

# The changelog is the first paragraph of CHANGELOG: everything from the
# first non-blank line up to the first blank line. Later paragraphs are
# older entries and the "--" line ends the file's live section.
changelog=""
if [ -f "$CHANGELOG_FILE" ]; then
    changelog="$(awk '
        /^--$/            { exit }
        NF == 0           { if (started) exit; next }
                          { started = 1; print }
    ' "$CHANGELOG_FILE")"
fi

emit() {
    echo "PIKO-ROM-MANIFEST 1"
    echo "version: $version"
    echo "built: $built"
    echo "commit: $commit"
    echo ""
    if [ -n "$changelog" ]; then
        echo "$changelog"
    fi
    # No changelog means no body at all. pikostore shows its own message
    # for that case rather than the manifest carrying placeholder prose.
}

if [ -n "$OUT" ]; then
    emit > "$OUT"
    echo "gen-rom-manifest: $version ($commit) -> $OUT" >&2
else
    emit
fi
