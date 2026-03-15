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

        Status ChangeOptions(const Options& options);

        void Add(const Slice& key, const Slice& value);

        void Flush();

        Status status() const;

        Status Finish();

        void Abandon();

        uint64_t NumEntries() const;
        uint64_t FileSize() const;


    private:
        bool ok() const { return status().ok(); }
        void WriteBlock(BlockBuilder* block, BlockHandle* handle);
        void WirteRawBlock(const Slice& data, CompressionType, BlockHandle* handle);

        struct Rep;
        Rep* rep_;
};

}