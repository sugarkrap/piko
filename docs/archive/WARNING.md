# WARNING: archived, outdated documentation

The files in this folder are historical postmortems and handoff notes.
**They reference paths that no longer exist in the current layout** — they
predate several repository reorganizations and were written against the
tree as it existed at the time.

Notably, these docs refer to:
- `hostap-work/` — moved to `modules/hostap/`
- `nand-root/` — renamed to `rootfs/`
- `initramfs/` — deleted entirely (it held a full checked-in busybox
  rootfs build that duplicated and competed with `rootfs/` as the
  project's actual rootfs; the embedded/initramfs-as-root approach these
  docs describe is not how the project boots today)

Do not treat file paths, directory layouts, or "current state" claims in
this folder as accurate. They are kept for their historical/debugging
narrative value (root-cause writeups, hardware incident reports) — not as
a guide to the present-day repo structure or build process. For current
docs, see the rest of `docs/` and the root `README.md`/`AGENTS.md`.
