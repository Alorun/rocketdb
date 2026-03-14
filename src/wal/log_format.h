#pragma once

namespace rocketdb {
namespace log {

// For sharding strategy (a block cannot hold all the data)
enum RecordType {
    // Reserved for preallocated files
    kZeroType = 0,

    // OK
    kFullType = 1,

    // For fragments
    kFirstType = 2,
    kMiddleType = 3,
    kLastType = 4
};

static const int kMaxRecordType = kLastType;

// Wal block size
static const int kBlockSize = 32768;

// Data header (checksum : 4 bytes + length : 2 bytes + type : 1 byte)
static const int kHeaderSize = 4 + 2 + 1;

}
}