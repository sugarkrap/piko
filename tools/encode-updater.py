#!/usr/bin/env python3
"""Encode a plaintext updater.sh wrapper into Cacko's recovery-menu cipher.

Cacko's recovery-mode "Update" menu decodes updater.sh before executing it
-- a plaintext updater.sh does not work, confirmed on real hardware. See
docs/DEADLETTER-CIPHER.md for the full writeup of how this was discovered
and how the byte mappings below were derived (known-plaintext attack
against a genuine Cacko 1.23 updater.sh, NOT the table hardcoded in
tools/src/encsh.c, which is documented there as wrong for this device/ROM
version).

This only covers the 21 bytes actually needed so far for short ASCII
wrapper scripts (shell boilerplate + a path + a tool name). If the input
uses a byte outside this set, this script refuses to guess and tells you
to extend the mapping via the same known-plaintext technique documented
in DEADLETTER-CIPHER.md instead of silently producing a wrong encoding.

Usage:
    tools/encode-updater.py [infile] [outfile]

Defaults: infile=flash/updater-uncoded.sh, outfile=flash/updater-encoded.sh
Neither file is tracked in git; both are build outputs. Re-run this
whenever updater-uncoded.sh's content changes, and before staging an SD
card for a flash (see AGENTS.md's "last spare board" constraints).
"""
import sys

# Confirmed plaintext byte -> cipher byte mappings (docs/DEADLETTER-CIPHER.md).
CONFIRMED_MAPPING = {
    0x0a: 0x2f, 0x20: 0x41, 0x21: 0x22, 0x23: 0x24, 0x2d: 0x2e,
    0x2f: 0xc7, 0x61: 0xe9, 0x62: 0x03, 0x63: 0xb0, 0x64: 0x60,
    0x65: 0xec, 0x66: 0x39, 0x68: 0x73, 0x69: 0xa8, 0x6b: 0x12,
    0x6c: 0x31, 0x6d: 0xe4, 0x6e: 0x30, 0x6f: 0x25, 0x70: 0xe7,
    0x72: 0x9d, 0x73: 0x6e, 0x74: 0x3e, 0x75: 0x50, 0x76: 0x77,
}


def main():
    infile = sys.argv[1] if len(sys.argv) > 1 else "flash/updater-uncoded.sh"
    outfile = sys.argv[2] if len(sys.argv) > 2 else "flash/updater-encoded.sh"

    with open(infile, "rb") as f:
        plaintext = f.read()

    missing = sorted(b for b in set(plaintext) if b not in CONFIRMED_MAPPING)
    if missing:
        sys.stderr.write(
            "encode-updater: input uses byte(s) not in the confirmed mapping: "
            + ", ".join(f"0x{b:02x} ({chr(b)!r})" for b in missing) + "\n"
            "  Extend CONFIRMED_MAPPING via the known-plaintext technique in\n"
            "  docs/DEADLETTER-CIPHER.md rather than guessing -- an unconfirmed\n"
            "  mapping produces a corrupt updater.sh that silently fails on\n"
            "  Cacko's recovery menu (no verification path exists there).\n"
        )
        sys.exit(1)

    encoded = bytes(CONFIRMED_MAPPING[b] for b in plaintext)

    with open(outfile, "wb") as f:
        f.write(encoded)

    print(f"{infile} -> {outfile} ({len(plaintext)} bytes)")
    print("first 10 bytes:", encoded[:10].hex(" "))


if __name__ == "__main__":
    main()
