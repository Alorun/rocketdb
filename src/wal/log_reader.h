#pragma once

#include <cstddef>
#include <cstdint>

#include "log_format.h"
#include "../db/dbformat.h"
#include "../../include/slice.h"
#include "../../include/status.h"
#include "../../include/env.h"
#include "../util/crc32c.h"

namespace rocketdb {

class SequentialFile;

namespace log {

class Reader {
    public:
        // For reporting errors
        class Reporter {
            public: 
                virtual ~Reporter();

                virtual void Corruption(size_t bytes, const Status& status) = 0;
        };

        Reader(SequentialFile* file, Reporter* reporter, bool checksum, uint64_t initial_offset);

        Reader(const Reader&) = delete;
        Reader* operator=(const Reader&) = delete;

        ~Reader();

        // Read the next record into record
        // Return true if read successfully, false if we hit end of the input
        // May use scratch when the contents sliced
        bool ReaderRecord(Slice* record, std::string* scratch);

        // Return the physical offset of the last record returned by ReadRecord
        uint64_t LastRecordOffset();

    private:
        // Record type replenish
        enum {
            kEof = kMaxRecordType + 1,

            kBadRecord = kMaxRecordType + 2
        };

        // Skip all blocks that are completely before "initial_offset_"
        bool SkipToInitialBlock();

        // Read real data and examine for ReaderRecord
        unsigned int ReadPhysicalRecord(Slice* result);

        // Reports dropped bytes to the reporter
        void ReportCorruption(uint64_t bytes, const char* reason);
        void ReportDrop(uint64_t bytes, const Status& reason);

        SequentialFile* const file_;   // Read data from file_ to backing_store
        Reporter* const reporter_;
        bool const checksum_;
        char* const backing_store_;  // Raw storage
        Slice buffer_;  // Store unparsed data in backing_store
        bool eof_;  // Flag the file has been read

        // Offset of the last record returned by ReadRecord
        uint64_t last_record_offset_;
        // Offset of the first location past the end of buffer_
        uint64_t end_of_buffer_offset_;

        uint64_t const initial_offset_;

        // Used to skip incomplete records read
        bool resyncing_;
};

}

}