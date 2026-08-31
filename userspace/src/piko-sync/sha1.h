#ifndef PIKO_SYNC_SHA1_H
#define PIKO_SYNC_SHA1_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

namespace piko_sync {

class Sha1 {
public:
    Sha1() { reset(); }

    void reset()
    {
        h_[0] = 0x67452301u;
        h_[1] = 0xefcdab89u;
        h_[2] = 0x98badcfeu;
        h_[3] = 0x10325476u;
        h_[4] = 0xc3d2e1f0u;
        len_ = 0;
        fill_ = 0;
    }

    void update(const char *data, size_t len)
    {
        const unsigned char *p = reinterpret_cast<const unsigned char *>(data);
        len_ += static_cast<uint64_t>(len);
        while (len > 0) {
            size_t take = 64 - fill_;
            if (take > len)
                take = len;
            memcpy(block_ + fill_, p, take);
            fill_ += take;
            p += take;
            len -= take;
            if (fill_ == 64) {
                transform(block_);
                fill_ = 0;
            }
        }
    }

    void update(const std::string &s) { update(s.data(), s.size()); }

    std::string final_value()
    {
        uint64_t bits = len_ * 8;

        unsigned char pad = 0x80;
        update(reinterpret_cast<const char *>(&pad), 1);
        unsigned char zero = 0;
        while (fill_ != 56)
            update(reinterpret_cast<const char *>(&zero), 1);

        for (int i = 0; i < 8; i++)
            block_[56 + i] = static_cast<unsigned char>((bits >> (56 - 8 * i)) & 0xff);
        transform(block_);
        fill_ = 0;

        std::string out;
        out.resize(20);
        for (int i = 0; i < 5; i++) {
            out[i * 4 + 0] = static_cast<char>((h_[i] >> 24) & 0xff);
            out[i * 4 + 1] = static_cast<char>((h_[i] >> 16) & 0xff);
            out[i * 4 + 2] = static_cast<char>((h_[i] >> 8) & 0xff);
            out[i * 4 + 3] = static_cast<char>(h_[i] & 0xff);
        }
        return out;
    }

private:
    static uint32_t rol(uint32_t v, int n)
    {
        return (v << n) | (v >> (32 - n));
    }

    void transform(const unsigned char *p)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (static_cast<uint32_t>(p[i * 4]) << 24)
                 | (static_cast<uint32_t>(p[i * 4 + 1]) << 16)
                 | (static_cast<uint32_t>(p[i * 4 + 2]) << 8)
                 | static_cast<uint32_t>(p[i * 4 + 3]);
        for (int i = 16; i < 80; i++)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5a827999u; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ed9eba1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
            else             { f = b ^ c ^ d;                   k = 0xca62c1d6u; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }

        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }

    uint32_t h_[5];
    uint64_t len_;
    unsigned char block_[64];
    size_t fill_;
};

}

#endif
