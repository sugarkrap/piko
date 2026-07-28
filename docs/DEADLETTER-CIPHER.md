# Dead Letter — The updater.sh Cipher

*Written 2026-07-22. Read this before touching updater.sh on the SD card again.*

---

## Summary

Cacko's recovery-mode "Update" flash-menu option (boot-ROM level, reached via
battery-pull + hold-"OK"+power-on, no USB needed) **decodes `updater.sh`
before executing it**. A plain-text `updater.sh` does not work — confirmed
directly on hardware. Every future `updater.sh` swap must be encoded first.

This was not obvious going in: earlier in this session a plain-text
`updater.sh` *appeared* to have worked (we were running the resulting
kernel). That was wrong to conclude from — something else must have put that
kernel there, not a plain `updater.sh` run through the boot-ROM menu. Trust
direct hardware confirmation over inference from indirect evidence.

## The tool: `tools/src/encsh.c`

Real, historical Sharp/Cacko tool, written by "sash@cacko.biz", part of the
Cacko ROM kit. Original source recovered from
`https://www.rot13.org/~dpavlin/zaurus/encsh.c` (fetched via raw `curl`, not
`WebFetch` — `WebFetch` summarizes content through a model and is not safe
for byte-exact data like a 256-entry cipher table; verified this the hard
way, see below). It's a **static single-byte substitution cipher**: a fixed
256-byte lookup table, not XOR, not anything key-dependent. Encoding and
decoding are both simple table lookups (decode is the inverse permutation).

Our `tools/src/encsh.c` is a clean reimplementation with the same algorithm,
built as a **host-side tool** (compiled with the system's native `gcc`, not
the ARM cross-toolchain — it operates on files before they reach the SD
card, never runs on the Zaurus itself). Usage:

```sh
gcc -O2 -o encsh encsh.c
./encsh -e infile outfile   # encode (plaintext -> cipher)
./encsh -d infile outfile   # decode (cipher -> plaintext)
```

(The original tool's own `-d`/other-flag branch was inverted relative to its
own usage message — a real bug in the historical tool. Our version's `-e`/
`-d` mean what they say.)

## The table in `encsh.c` is WRONG for this device/ROM version

The `rot13.org` page explicitly documents it for "SL-C7x0" via an unstated,
probably older Cacko release. Verified this table does **not** decode a
genuine SL-C860 Cacko 1.23 `updater.sh` (downloaded from
`archive.org/details/sharp-zaurus-sl-c-860-cacko-1.23`) into anything
legible. Cacko apparently changed the cipher table at some point between
whatever version rot13.org's example came from and 1.23. **Do not use the
table currently hardcoded in `encsh.c`'s `enctab[]` for this project** — it
was useful only to prove the *mechanism* (single-byte substitution) is real,
not to get a working table. Consider it a placeholder needing replacement;
see below for the actual working data.

## How the real table was recovered: known-plaintext, not brute force

`flash/updater-encoded.sh` (43 bytes, already present in the repo before
this investigation, provenance unclear — likely produced by an earlier
session that already had a working `encsh`) turned out to be genuinely,
correctly encoded — its first 10 bytes are byte-for-byte identical to the
genuine downloaded Cacko 1.23 `updater.sh`'s first 10 bytes
(`24 22 c7 03 a8 30 c7 6e 73 2f`), which is far too improbable to be
coincidence (`1/256^10`) and is exactly what you'd expect from two different
Cacko-era scripts sharing the same `#!/bin/sh\n` shebang.

Confirmed on real hardware that decoding `updater-encoded.sh` and running it
through the recovery "Update" menu launches `piko-install` — meaning its
full plaintext is known with certainty: `"#!/bin/sh\n/mnt/card/piko-install
/mnt/card\n"` (43 bytes, matches `updater-piko.sh`'s content exactly, and
the byte count matches exactly too). That gave a **complete 43-byte
known-plaintext/ciphertext pair**, not just a fragment — yielding 21 unique
byte mappings with zero internal conflicts (every repeated character, e.g.
`/`, mapped consistently every time it appeared — the confirmation that
this is a real, correct derivation and not coincidence).

From there, decoding the genuine archive.org Cacko 1.23 file with just
those 21 mappings (leaving unknown bytes as placeholders) produced
recognizable word-shapes by context — `"acko"` → `Cacko`, `".pdat."` →
`update` (revealing `u`→`0x50`, `e`→`0xec`), `"th.n"` → `then`, `"i."` →
`if` (`f`→`0x39`), `"/d../mtd"` → `/dev/mtd` (`v`→`0x77`, confirming `e`
independently a second and third time). This is the standard
known-plaintext cryptanalysis technique for substitution ciphers: don't
guess blind, extend confirmed mappings by reading partially-decoded
context.

### Confirmed mappings (plaintext byte -> cipher byte) as of this writing

```
0x0a '\n' -> 0x2f     0x6b 'k'  -> 0x12
0x20 ' '  -> 0x41     0x6c 'l'  -> 0x31
0x21 '!'  -> 0x22     0x6d 'm'  -> 0xe4
0x23 '#'  -> 0x24     0x6e 'n'  -> 0x30
0x2d '-'  -> 0x2e     0x6f 'o'  -> 0x25
0x2f '/'  -> 0xc7     0x70 'p'  -> 0xe7
0x61 'a'  -> 0xe9     0x72 'r'  -> 0x9d
0x62 'b'  -> 0x03     0x73 's'  -> 0x6e
0x63 'c'  -> 0xb0     0x74 't'  -> 0x3e
0x64 'd'  -> 0x60     0x75 'u'  -> 0x50
0x65 'e'  -> 0xec (confirmed independently 3x)
0x66 'f'  -> 0x39
0x68 'h'  -> 0x73
0x69 'i'  -> 0xa8
0x76 'v'  -> 0x77
```

25 of 256 bytes confirmed. That's already enough to encode short ASCII
wrapper scripts like `updater-piko.sh`/`updater-backup.sh` (shell
boilerplate + a path + a tool name uses a small alphabet), which is all
we've actually needed so far — used to encode
`"#!/bin/sh\n/mnt/card/piko-backup /mnt/card\n"` directly (see
`flash/updater-backup-real.sh.enc`) without needing the full table.

## Statistical cracking (in progress / supplementary, not required for the above)

In parallel, ran a quadgram-frequency hill-climbing solver
(`/tmp/crack_cipher2.py`, corpus built from ~400 real `.sh` files in the
kernel source tree) seeded with the known mappings, to try to recover the
**full** 256-byte table (useful for encoding arbitrary future content, e.g.
if we ever want to encode a full custom `updater.sh` rather than a short
wrapper). This is a nice-to-have, not a blocker — the known-plaintext
method above is what actually unblocked real progress. If this finishes
with a full table, it should be reconciled against the 25 confirmed
mappings above (which take priority as ground truth) and written into a
corrected `encsh.c`.

## Practical workflow going forward

For any new `updater.sh` swap:
1. If the wrapper script only uses characters from the confirmed set above,
   encode directly with a small Python one-liner (see the piko-backup
   derivation above) — no need to wait for the full table.
2. If it needs an unconfirmed character, either extend the known mappings
   via the same context-reading technique against the genuine archive.org
   file, or wait for the statistical crack to finish and cross-check its
   result against the confirmed set (any statistically-derived mapping that
   contradicts a confirmed one is wrong — the confirmed set is ground
   truth, not the solver's output).
3. Once a fuller table is confirmed, update `tools/src/encsh.c`'s `enctab[]`
   with the corrected values so the general-purpose tool works without
   needing this manual derivation every time.

## A process note, not just a technical one

`WebFetch` passes fetched content through a summarization model before
returning it — fine for prose, unsafe for anything requiring byte-exact
reproduction (a 256-entry hex table, in this case). When re-verified via
direct `curl`, the table matched exactly — so no transcription error
actually occurred here, but the risk was real and worth flagging: any time
a task needs an exact byte/hex payload from a fetched page, fetch it raw,
don't trust a summarized re-statement of it.
