#pragma once

#include <cstdint>

namespace rocketdb {

class Random {

public:
    explicit Random(uint32_t s) : seed_(s & 0x7fffffffu) {
        if (seed_ == 0 || seed_ == 2147483647L) {
            seed_ = 1;
        }
    }

    uint32_t Next() {
        static const uint32_t M = 2147483647L; // 2^31 - 1
        static const uint32_t A = 16807;

        uint64_t product = seed_ * A;

        // The product is split into high 31 bits and low 31 bits,
        // according to the formula 2^k ≡ 1 ( mod (2^k) - 1),
        // taking the modulo of both sides of the formula, we get the quuivalent formula
        seed_ = static_cast<uint32_t>((product >> 31) + (product & M));

        if (seed_ > M) {
            seed_ -= M;
        }
        return seed_;
    }

    uint32_t Uniform(int n) { return Next() % n; }

    // The probability og obtaining 1/n
    bool OneIn(int n) { return (Next() % n) == 0; }

    // Random numbers biased towards smaller values
    uint32_t Skewed(int max_log) { return Uniform(1 << Uniform(max_log + 1)); }

private:
    uint32_t seed_;

};


}