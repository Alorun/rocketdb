#pragma once

#include <cstdint>

#include "../db/dbformat.h"
#include "../../include/slice.h"
#include "../../include/status.h"
#include "log_format.h"
#include "../../include/env.h"

namespace rocketdb {

class WritableFile;

namespace log {

class Writer {
    public:
        // Create a writer that dest must be initially empty
        explicit Writer(WritableFile* dest);

        // Create a writer that dest must have initial length dest_length
        Writer(WritableFile* dest, uint64_t dest_length);

        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;

        ~Writer();

        Status AddRecord(const Slice& slice);

    private:
        Status EmitPhysicalRecord(RecordType type, const char* ptr, size_t length);

        WritableFile* dest_;
        int block_offset_;  // Current offset in block

        // Crc32c values for all supported record types
        // These are pre-computed to reduce the overhead of computing the crc of the record type
        uint32_t type_crc_[kMaxRecordType + 1];
};

}
}