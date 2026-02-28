#pragma once

#include <atomic>
#include <assert.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rocketdb {

class Arena {

public:
    Arena();

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    ~Arena();

    char* Allocate(size_t bytes);

    char* AllocateAligned(size_t bytes);
    
}

}