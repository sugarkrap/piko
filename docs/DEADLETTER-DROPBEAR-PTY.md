# Dead Letter — SSH into the Zaurus: dropbear auth + PTY (three linked bugs)

*Written 2026-07-23. WiFi→SSH is the only remote path (`AGENTS.md`: no USB,
no serial), so a working dropbear login is load-bearing, not a nicety.*

---

## Symptom timeline

1. `ssh piko@zaurus` → `Permission denied (publickey,password)` — **both**
   methods, despite verified-correct `passwd`/`shadow`/`authorized_keys` and a
   qemu test proving `getpwnam`/`getspnam`/`crypt` all succeed. Console
   `login` (busybox) accepted the same credentials fine.
2. After fixing that → `PTY allocation request failed on channel 0` /
   `shell request failed on channel 0`.
3. After fixing that → login **succeeds, command exec works** (non-PTY SSH is
   fully functional), but an **interactive PTY session runs the shell and
   exits 0 with no output ever flowing back**.

## Cause & fix, per link

### 1. "invalid shell" — missing `/etc/shells`
dropbear validates the user's login shell against `/etc/shells` (via
`getusershell`) and rejects the user *before* checking key or password if the
shell isn't listed. We had no `/etc/shells`, so `/bin/zsh` (piko) and
`/bin/ash` (root) were both "invalid". busybox `login` doesn't do this check,
which is why console login worked and only SSH failed.
**Fix:** create `/etc/shells` listing every login shell in `/etc/passwd`:
```
/bin/sh
/bin/ash
/bin/zsh
```
The server log line that identified this: `User 'piko' has invalid shell,
rejected` — get it with a debug instance: `dropbear -F -E -p 2222` (spare
port, logs each attempt's reason to the console).

### 2. `PTY allocation request failed` — dropbear compiled for legacy PTYs
Cross-compiling, dropbear's `./configure` can't *run* its probe binaries, so
it left **all** PTY methods undef in `config.h` (`USE_DEV_PTMX`,
`HAVE_OPENPTY`, `HAVE_DEV_PTS_AND_PTC`). `sshpty.c` then falls through to the
**legacy `/dev/pty??` BSD loop**, which fails because the kernel has
`CONFIG_LEGACY_PTYS` off (UNIX98 only).
**Fix:** `#define USE_DEV_PTMX 1` in dropbear `config.h`, rebuild.
- `HAVE_OPENPTY` is NOT usable: this uClibc has **no `openpty`** (no libutil).
- `USE_DEV_PTMX` uses `open("/dev/ptmx")` + `grantpt`/`unlockpt`/`ptsname`,
  all of which uClibc *does* provide (`__UNIX98PTY_ONLY__=1`), so it links.
- `stropts.h` is absent → the Solaris `I_PUSH` streams block compiles out.
- **Build gotcha:** dropbear must be built as the **single** `dropbear`
  server binary (not `MULTI=1`) and linked with the repo's `syscall_shim.o`
  (uClibc has no `syscall()`, used by `dbutil.c gettime_wrapper`):
  ```
  make dropbear PROGRAMS=dropbear LIBS="$PWD/syscall_shim.o"
  ```
  A `MULTI=1` build drops the shim from the link → `undefined reference to
  syscall`. Verify the result: `strings dropbear | grep /dev/ptmx` present,
  legacy `/dev/pty`/"Failed to open any" strings gone.

### 3. Interactive session exits 0 with no output — devpts instance mismatch
With `USE_DEV_PTMX`, dropbear opens `/dev/ptmx`. devtmpfs does **not** create
`/dev/ptmx`, so rcS made it — but as a **classic `mknod c 5 2` node**. Opening
that allocates the master in the kernel's *default* devpts instance, while
`ptsname()` → `/dev/pts/N` opens the slave in the instance **mounted on
`/dev/pts`**. Different instances ⇒ master and slave are **not paired**: the
shell writes to a slave nothing reads, so it runs and exits cleanly but no
output reaches dropbear's master. (Symptom in `ssh -vvv`: `PTY allocation
request accepted`, `Exit status 0`, but zero data frames.)
**Fix:** mount devpts with `ptmxmode=0666` (creates `/dev/pts/ptmx`, the
instance's own clone device) and point `/dev/ptmx` at it with a **symlink**,
so master and slave share one instance:
```sh
mount -t devpts devpts /dev/pts -o mode=0620,ptmxmode=0666
ln -sf pts/ptmx /dev/ptmx        # NOT mknod c 5 2
```

## Test harness gotcha
Automated `ssh -tt … 'cmd'` with **closed/piped stdin** sends an immediate
channel EOF that tears the interactive session down before output flushes —
it shows a false "no output" even when the fix is correct. Test with a real
client PTY:
```sh
script -qec "ssh -tt -i key piko@zaurus 'echo OK; exit'" /dev/null
# full login-shell (sources .zshrc/.zprofile):
script -qec "ssh -tt -i key piko@zaurus" /dev/null <<< $'echo OK; exit'
```

## Result
Full interactive zsh login over WiFi works: real prompt `[piko@zaurus piko]$`,
ZLE line editing / bracketed paste active, rc files sourced. All three fixes
live in stage-2 `rcS` (`/etc/shells`, ptmx symlink) and the rebuilt dropbear
(`USE_DEV_PTMX`), shipped in mtd3 build **STAGE2-BUILD-F**.

## Leftover (cosmetic)
`tty`, `whoami`, `id`, `clear` are not enabled busybox applets — "command not
found" in the shell. Enable in busybox config + rebuild if wanted; not
required for login. piko's home is `chown`ed to piko at boot in rcS (the
`mkfs.jffs2 -q` squash makes everything root-owned).
