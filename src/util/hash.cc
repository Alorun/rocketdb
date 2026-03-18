#include "hash.h"

#include <cstdint>
#include <cstring>

#include "coding.h"

#ifndef  FALLTHROUGH_INTEDED
#define  FALLTHROUGH_INTEDED \
    do {                     \
    } while (0)
#endif

namespace rocketdb {

uint32_t Hash(const char* data, size_t n, uint32_t seed) {
    const uint32_t m =0xc6a4a793;
    const uint32_t r = 24;
    const char* limit = data + n;
    uint32_t h = seed ^ (n * m);

    while (limit - data >= 4) {
        uint32_t w = DecodeFixed32(data);
        data += 4;
        h += w;
        h *= m;
        h ^= (h >> 16);
    }

    switch (limit - data) {
        case 3:
            h += static_cast<uint8_t>(data[2]) << 16;
            FALLTHROUGH_INTEDED;
        case 2:
            h += static_cast<uint8_t>(data[1]) << 8;
            FALLTHROUGH_INTEDED;
        case 1:
            h += static_cast<uint8_t>(data[0]);
            h *= m;
            h ^= (h >> r);
            break;
    }
    return h;
}

}