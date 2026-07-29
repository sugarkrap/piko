#!/usr/bin/env python3
"""Recursively unpack a JFFS2 image into a directory tree using jffs2reader.

jffs2reader only supports single-directory listing (-d) and single-file
dump (-f), no bulk/recursive extraction -- this walks the tree by hand.
Ownership is NOT preserved (jffs2reader doesn't need root to read, but we
have no privilege to chown to arbitrary uid/gid on extraction) -- pack the
result with mkfs.jffs2 -U (--squash-uids) to force root ownership back.
"""
import os
import re
import subprocess
import sys

LINE_RE = re.compile(
    r'^(?P<type>[a-z-])(?P<perms>[r-][w-][xsS-][r-][w-][xsS-][r-][w-][xtT-])\s+'
    r'\d+\s+\d+\s+\d+\s+\d+\s+(?P<path>\S+)(?:\s+-> (?P<target>.+?))?\s*$'
)

PERM_BITS = [
    (0, 0o400), (1, 0o200), (2, 0o100),
    (3, 0o040), (4, 0o020), (5, 0o010),
    (6, 0o004), (7, 0o002), (8, 0o001),
]


def perm_mode(perms):
    mode = 0
    for i, bit in PERM_BITS:
        if perms[i] != '-':
            mode |= bit
    return mode


def list_dir(image, path):
    r = subprocess.run(['jffs2reader', image, '-d', path],
                        capture_output=True, text=True)
    if r.returncode != 0:
        print(f"jffs2-unpack: failed to list {path}: {r.stderr.strip()}", file=sys.stderr)
        sys.exit(1)
    entries = []
    for line in r.stdout.splitlines():
        m = LINE_RE.match(line)
        if not m:
            if line.strip():
                print(f"jffs2-unpack: warning: unparsed line: {line!r}", file=sys.stderr)
            continue
        d = m.groupdict()
        d['path'] = re.sub(r'/+', '/', d['path'])
        entries.append(d)
    return entries


def dump_file(image, path, dest):
    r = subprocess.run(['jffs2reader', image, '-f', path], capture_output=True)
    if r.returncode != 0:
        # jffs2reader has a small fixed internal read buffer and refuses
        # to dump anything bigger than it ("File does not fit into
        # buffer!") -- this only ever hits large files (kernel images,
        # big .ko modules), which in practice are exactly the files this
        # extraction exists to REPLACE with a fresh overlay anyway. Skip
        # rather than abort; report a count so a genuinely unexpected
        # large file that ISN'T covered by the overlay doesn't go unnoticed.
        print(f"jffs2-unpack: skipping {path} ({r.stderr.decode(errors='replace').strip()})",
              file=sys.stderr)
        return False
    with open(dest, 'wb') as f:
        f.write(r.stdout)
    return True


def walk(image, src_dir, dest_dir, count, skipped):
    os.makedirs(dest_dir, exist_ok=True)
    for e in list_dir(image, src_dir):
        name = os.path.basename(e['path'].rstrip('/'))
        if not name:
            continue
        src = e['path']
        dest = os.path.join(dest_dir, name)
        t = e['type']
        if t == 'd':
            walk(image, src, dest, count, skipped)
        elif t == 'l':
            if os.path.lexists(dest):
                os.remove(dest)
            os.symlink(e['target'], dest)
            count[0] += 1
        elif t == '-':
            if dump_file(image, src, dest):
                os.chmod(dest, perm_mode(e['perms']))
                count[0] += 1
            else:
                skipped.append(src)
        else:
            print(f"jffs2-unpack: skipping unsupported entry type {t!r}: {src}", file=sys.stderr)
    return count


def main():
    if len(sys.argv) != 3:
        print("usage: jffs2-unpack.py <image.jffs2> <dest-dir>", file=sys.stderr)
        sys.exit(2)
    image, dest = sys.argv[1], sys.argv[2]
    skipped = []
    count = walk(image, '/', dest, [0], skipped)
    print(f"jffs2-unpack: extracted {count[0]} entries from {image} -> {dest}")
    if skipped:
        print(f"jffs2-unpack: skipped {len(skipped)} file(s) too large for jffs2reader's buffer:", file=sys.stderr)
        for p in skipped:
            print(f"  {p}", file=sys.stderr)


if __name__ == '__main__':
    main()
