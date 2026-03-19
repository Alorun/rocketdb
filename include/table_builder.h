#pragma once

#include <cstdint>

#include "env.h"
#include "options.h"
#include "slice.h"
#include "status.h"

namespace rocketdb {

class BlockBuilder;
class BlockHandle;
class WritableFile;

class TableBuilder {
    public:
        TableBuilder(const Options& options, WritableFile* file);

        TableBuilder(const TableBuilder&) = delete;
        TableBuilder& operator=(const TableBuilder&) = delete;

        ~TableBuilder();

        // Change the options used by this builder
        Status ChangeOptions(const Options& options);

        // Add key, vlaue to the table being constructed
        // REQUIRES: key is after any previously added key according to comparator
        // REQUIRES: Finish(), Abandon() have not been called
        void Add(const Slice& key, const Slice& value);

        // Advance operation: flush any buffered key/value pairs to file
        // Can be used to ensure that two adjacent entries never live in the same data block
        // REQUIRES: Finish(), Abandon() have not been called
        void Flush();

        // Return non-ok iff some error has been detected
        Status status() const;

        // Finish building the table
        // Stop using the file passed to the constructor after the function returns
        // REQUIRES: Finish(), Abandon() have not been called
        Status Finish();

        // Indicate that the contents of this builder should be abandoned
        // Stop using the file passed to the constructor after the function returns
        // REQUIRES: Finish(), Abandon() have not been called
        void Abandon();

        // Number of calls to Add() so far
        uint64_t NumEntries() const;

        // Size of the file generated so far
        uint64_t FileSize() const;

    private:
        bool ok() const { return status().ok(); }
        void WriteBlock(BlockBuilder* block, BlockHandle* handle);
        void WriteRawBlock(const Slice& data, CompressionType, BlockHandle* handle);

        struct Rep;
        Rep* rep_;
};

}