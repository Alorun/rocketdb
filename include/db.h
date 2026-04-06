#pragma once

#include <cstdint>
#include <cstdio>

#include "export.h"
#include "iterator.h"
#include "options.h"
#include "slice.h"

namespace rocketdb {

static const int kMajorVersion = 1;
static const int kMinorVersion = 2;

struct Options;
struct ReadOptions;
struct WriteOptions;
class WriteBatch;

class Snapshot {
    protected:
        virtual ~Snapshot();
};

struct Range {
    Range() = default;
    Range(const Slice& s, const Slice& l) : start(s), limit(l) {}

    Slice start;    // Include in the range
    Slice limit;    // Not included in the range
};

}