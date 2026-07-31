/*
 * sha1.c
 *
 * Public-domain SHA1 implementation, originally written by Steve Reid
 * <steve@edmweb.com> in 1993 and released into the public domain. This
 * exact SHA1_CTX/SHA1Init/SHA1Update/SHA1Final shape is the same one used
 * by BSD's libmd, git's compat/sha1, and many other projects -- it is
 * reproduced here (not vendored from a specific project) to satisfy
 * xorg-server's --with-sha1=libmd build path. See sha1.h for why this
 * exists.
 */

#include "sha1.h"
#include <string.h>

#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

static void SHA1_Transform(uint32_t state[5], const unsigned char buffer[64])
{
    uint32_t a, b, c, d, e;
    uint32_t block[80];
    int i;

    for (i = 0; i < 16; i++) {
        block[i] = ((uint32_t)buffer[i * 4] << 24) |
                   ((uint32_t)buffer[i * 4 + 1] << 16) |
                   ((uint32_t)buffer[i * 4 + 2] << 8) |
                   ((uint32_t)buffer[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++) {
        block[i] = rol(block[i - 3] ^ block[i - 8] ^ block[i - 14] ^ block[i - 16], 1);
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    for (i = 0; i < 80; i++) {
        uint32_t f, k, temp;

        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        temp = rol(a, 5) + f + e + k + block[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void SHA1Init(SHA1_CTX *context)
{
    context->state[0] = 0x67452301;
    context->state[1] = 0xEFCDAB89;
    context->state[2] = 0x98BADCFE;
    context->state[3] = 0x10325476;
    context->state[4] = 0xC3D2E1F0;
    context->count[0] = 0;
    context->count[1] = 0;
}

void SHA1Update(SHA1_CTX *context, const unsigned char *data, uint32_t len)
{
    uint32_t i, j;

    j = (context->count[0] >> 3) & 63;
    if ((context->count[0] += len << 3) < (len << 3))
        context->count[1]++;
    context->count[1] += (len >> 29);

    if ((j + len) > 63) {
        i = 64 - j;
        memcpy(&context->buffer[j], data, i);
        SHA1_Transform(context->state, context->buffer);
        for (; i + 63 < len; i += 64)
            SHA1_Transform(context->state, &data[i]);
        j = 0;
    } else {
        i = 0;
    }
    memcpy(&context->buffer[j], &data[i], len - i);
}

void SHA1Final(unsigned char digest[20], SHA1_CTX *context)
{
    unsigned char finalcount[8];
    unsigned char c;
    int i;

    for (i = 0; i < 8; i++) {
        finalcount[i] = (unsigned char)
            ((context->count[(i >= 4 ? 0 : 1)] >>
              ((3 - (i & 3)) * 8)) & 255);
    }

    c = 0200;
    SHA1Update(context, &c, 1);
    while ((context->count[0] & 504) != 448) {
        c = 0000;
        SHA1Update(context, &c, 1);
    }
    SHA1Update(context, finalcount, 8);

    for (i = 0; i < 20; i++) {
        digest[i] = (unsigned char)
            ((context->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
    }

    memset(context, 0, sizeof(*context));
    memset(finalcount, 0, sizeof(finalcount));
}
