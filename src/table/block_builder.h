#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../include/slice.h"

namespace rocketdb {

struct Options;

// An entry for a particular key-value pair has the form:
//     shared_bytes: varint32
//     unshared_bytes: varint32
//     value_length: varint32
//     key_delta: char[unshared_bytes]
//     value: char[value_length]
// shared_bytes == 0 for restart points.
//
// The trailer of the block has the form:
//     restarts: uint32[num_restarts]
//     num_restarts: uint32
// restarts[i] contains the offset within the block of the ith restart point.
class BlockBuilder {
    public:
        explicit BlockBuilder(const Options* options);

        BlockBuilder(const BlockBuilder&) = delete;
        BlockBuilder& operator=(const BlockBuilder&) = delete;

        void Reset();

        void Add(const Slice& key, const Slice& value);

        Slice Finish();

        size_t CurrentSizeEstimate() const;

        bool empty() const { return buffer_.empty(); }

    private:
        const Options* options_;
        std::string buffer_;                // Destination buffer
        std::vector<uint32_t> restarts_;    // Restart points
        int counter_;                       // Number of entrise emitted since restart ~ Options::block_restart_interval
        bool finished_;                     // Has Finish() been called
        std::string last_key_;              // Used for prefix compression
};

}