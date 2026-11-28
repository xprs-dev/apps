#include "md5.h"

typedef unsigned int u32;

static u32 rol(u32 x, int c) { return (x << c) | (x >> (32 - c)); }

static const u32 K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};
static const int R[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

/* One buffer, sized for what a digest actually hashes: "user:realm:pass",
 * "METHOD:uri", and "ha1:nonce:ha2". None of them is long. */
#define MD5_MAX 512

void th_md5_hex(const char *in, unsigned len, char *out)
{
    unsigned char msg[MD5_MAX + 72];
    unsigned i;
    if (len > MD5_MAX) len = MD5_MAX;
    for (i = 0; i < len; i++) msg[i] = (unsigned char)in[i];
    msg[len] = 0x80;
    unsigned pad = len + 1;
    while (pad % 64 != 56) msg[pad++] = 0;
    unsigned long long bits = (unsigned long long)len * 8;
    for (i = 0; i < 8; i++) msg[pad + i] = (unsigned char)((bits >> (8 * i)) & 0xFF);
    unsigned total = pad + 8;

    u32 h0 = 0x67452301u, h1 = 0xefcdab89u, h2 = 0x98badcfeu, h3 = 0x10325476u;
    for (unsigned off = 0; off < total; off += 64) {
        u32 w[16];
        for (i = 0; i < 16; i++)
            w[i] = (u32)msg[off + i * 4] | ((u32)msg[off + i * 4 + 1] << 8) |
                   ((u32)msg[off + i * 4 + 2] << 16) | ((u32)msg[off + i * 4 + 3] << 24);
        u32 a = h0, b = h1, c = h2, d = h3;
        for (i = 0; i < 64; i++) {
            u32 f;
            unsigned g;
            if (i < 16)      { f = (b & c) | (~b & d);        g = i; }
            else if (i < 32) { f = (d & b) | (~d & c);        g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d;                 g = (3 * i + 5) % 16; }
            else             { f = c ^ (b | ~d);              g = (7 * i) % 16; }
            u32 tmp = d;
            d = c; c = b;
            b = b + rol(a + f + K[i] + w[g], R[i]);
            a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d;
    }
    const char *hex = "0123456789abcdef";
    u32 hs[4] = { h0, h1, h2, h3 };
    unsigned o = 0;
    for (i = 0; i < 4; i++)
        for (unsigned b = 0; b < 4; b++) {
            unsigned char v = (unsigned char)((hs[i] >> (8 * b)) & 0xFF);
            out[o++] = hex[v >> 4];
            out[o++] = hex[v & 0x0F];
        }
    out[o] = 0;
}
