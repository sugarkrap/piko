# Dead Letter — the kexec "SIGILL" was a bad SYSCALL NUMBER, not a bad instruction

*Written 2026-07-22. Read this before ever chasing a "kexec Illegal
instruction / exit 132" by disassembling for illegal opcodes again.*

---

## The symptom and the long wrong turn

`kexec -l` on real hardware died with **"Illegal instruction", exit code
132** (128 + SIGILL), producing **zero stdout/stderr** before the crash.
`exit 132` = killed by SIGILL, and "Illegal instruction" is what the shell
prints for a SIGILL child. Natural first assumption: the binary contains a
CPU instruction the PXA255 (XScale, ARMv5TE, no VFP, no iWMMXt) can't
execute.

That assumption cost hours. The hunt found and "fixed" three real-but-
irrelevant things:

1. **uClibc's prebuilt `memcpy`/`memmove` use VFP** (`vldmia`/`vstmia`) as a
   bulk-copy trick — overridden with byte-loop versions
   (`kexec/memcpy_override.c`). *(This one is genuinely worth keeping: the
   large ELF-segment copy in `kexec -l`/`-e` would have hit it as a real
   second SIGILL after the true bug was fixed. Keep it.)*
2. **libgcc's EHABI VFP unwind helpers** (`__gnu_Unwind_*_VFP*`) — no-op
   overridden (`kexec/vfp_unwind_override.c`).
3. **libgcc's iWMMXt unwind helpers** (`__gnu_Unwind_*_WMMX*`) — same file.

Fixes 2 and 3 were treating non-problems: those helpers only execute during
an actual C++ stack unwind, which plain-C kexec never triggers. They were
harmless dead code all along. An exhaustive whole-binary instruction-
mnemonic audit eventually confirmed that **every instruction on the real
execution path is legal ARMv5TE** — which should have been the clue that
SIGILL wasn't coming from an illegal *instruction* at all.

## What actually finds these: CONFIG_DEBUG_USER + user_debug

A userspace SIGILL is **silent by default** — ARM Linux only prints the
faulting details if `CONFIG_DEBUG_USER=y` is built in AND the boot cmdline
carries a `user_debug=` bit. That's why `dmesg` showed nothing and we were
debugging blind for so long.

Enabling it in the *bootstrap* kernel (the one that runs kexec) was the
turning point:

```
# kernel .config
CONFIG_DEBUG_USER=y
CONFIG_CMDLINE="console=tty0 user_debug=31"   # 31 = all fault classes
```

(`CONFIG_CMDLINE_FORCE=y` here, so the bit has to go in CONFIG_CMDLINE, not
`-append`.) It added only ~48 bytes to the image. Next boot, the crash
printed:

```
kexec: obsolete system call 0090015b
Code: e59d4028 e59d502c e58dc000 ef000000 (...)
```

That single line is the whole answer.

## The real bug

- `ef000000` = `svc #0` — the **EABI** syscall instruction. So kexec IS an
  EABI binary making an EABI-style syscall. It just put the **wrong number**
  in r7.
- `0x0090015b` = `0x900000 + 0x15b` = **OABI base + 347**. Syscall **347 is
  `kexec_load`** (`arch/arm/tools/syscall.tbl`), and `0x900000` is the old
  **OABI** syscall base (`__NR_OABI_SYSCALL_BASE`).
- Our kernel is pure EABI. Syscall number `0x90015b` is wildly out of range,
  so the kernel takes the bad-syscall path: `bad_syscall()` prints "obsolete
  system call" (because `user_debug` bit for syscalls was set) and calls
  `arm_notify_die(..., SIGILL, ...)`. **That SIGILL is the exit-132 "Illegal
  instruction" we chased.** Not an instruction fault — a syscall-number
  fault that produces the same signal.

### Why the number was wrong

`kexec/kexec-syscall.h` computes, for ARM:
```c
#define __NR_kexec_load   (__NR_SYSCALL_BASE + 347)
```
`__NR_SYSCALL_BASE` comes from the kernel headers and is supposed to be `0`
on EABI, `0x900000` on OABI — gated on `__ARM_EABI__`. **But this
buildroot/uClibc toolchain does not define `__ARM_EABI__`** (confirmed:
`gcc -dM -E` shows `__ARMEL__` but not `__ARM_EABI__`), even though it emits
genuine EABI binaries (`svc 0`, ELF flags `0x600`). So `__NR_SYSCALL_BASE`
falls back to the OABI `0x900000`, and `__NR_kexec_load` becomes `0x90015b`.

Every *other* syscall in kexec worked because uClibc's own wrappers
(`open`/`read`/`write`/…) use the correct raw EABI numbers internally. Only
kexec's hand-rolled `kexec_load` went through the poisoned `__NR_SYSCALL_BASE`.

### OABI_COMPAT does NOT fix this

`CONFIG_OABI_COMPAT` was already `=y` in our kernel and the call still
failed — proof it's irrelevant here. OABI_COMPAT adds handling for the old
*instruction form* (`swi #(0x900000+n)`); it does not rewrite a bogus r7
value on an `svc 0`. (Keep OABI_COMPAT anyway — it's wanted for running
genuine legacy OABI Zaurus apps under the full kernel. That's a separate
concern from this bug.)

## The fix

In `kexec/kexec-syscall.h`, a **hard override placed after all headers and
after the `#ifndef __NR_kexec_load` guard block** (the guard is skipped
entirely because `<sys/syscall.h>` already defines the symbol with the
poisoned value, so an edit *inside* the guard is dead code — this bit us
once):

```c
#if defined(__arm__)
#undef  __NR_kexec_load
#define __NR_kexec_load  347
#endif
```

Verify after building: the literal-pool word that was `0x0090015b` becomes
`0x0000015b` at the same address, and `objdump -d | grep 90015b` is empty.
A partial rebuild is NOT enough — `kexec_load` is `static inline`, folded
into `kexec.o`, and header-dependency tracking missed it; do `make clean`
then `make`.

## Standing lessons

1. **exit 132 / "Illegal instruction" ≠ illegal instruction.** SIGILL is
   also raised for bad/obsolete syscall numbers (`ILL_ILLTRP`). Before
   disassembling for opcodes, get the kernel to tell you the actual fault:
   `CONFIG_DEBUG_USER=y` + `user_debug=31`. One cheap diagnostic build would
   have saved the entire VFP/WMMX detour.
2. **This toolchain does not define `__ARM_EABI__`** despite producing EABI
   binaries. Any third-party code that keys syscall numbers or ABI decisions
   off `__ARM_EABI__` / `__NR_SYSCALL_BASE` may silently get the OABI path.
   Suspect this pattern for any "works for most syscalls, one specific
   syscall SIGILLs" symptom.
3. **Header edits inside `#ifndef FOO` are dead code if the system already
   defines `FOO`.** Override *after* the guard, after all includes.
