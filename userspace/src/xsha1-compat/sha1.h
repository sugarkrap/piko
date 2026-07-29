/*
 * sha1.h
 *
 * Public-domain SHA1 implementation (originally by Steve Reid, 1993,
 * released into the public domain; this is the same widely-vendored
 * SHA1_CTX/SHA1Init/SHA1Update/SHA1Final API shape used by BSD's libmd,
 * git, and countless other projects).
 *
 * This file exists because xorg-server's os/xsha1.c has a --with-sha1=libmd
 * path that expects exactly this API from a library named "md" (-lmd), and
 * no such library (or any of xserver's other SHA1 backend options) is
 * available in this ARM cross toolchain's sysroot. See
 * docs/archive/HANDOFF-2026-07-28-X11-XFBDEV.md for the wider cross-compile
 * effort this supports.
 */
#ifndef SHA1_H
#define SHA1_H

#include <stdint.h>

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    unsigned char buffer[64];
} SHA1_CTX;

void SHA1Init(SHA1_CTX *context);
void SHA1Update(SHA1_CTX *context, const unsigned char *data, uint32_t len);
void SHA1Final(unsigned char digest[20], SHA1_CTX *context);

#endif
