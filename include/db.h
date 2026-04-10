#pragma once

#include <cstdint>
#include <cstdio>

#include "export.h"
#include "iterator.h"
#include "options.h"
#include "slice.h"
#include "status.h"

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

class DB {
    public:
        static Status Open(const Options& options, const std::string& name, DB** dbptr);

        DB() = default;

        DB(const DB&) = delete;
        DB& operator=(const DB&) = delete;

        virtual ~DB();

        virtual Status Put(const WriteOptions& options, const Slice& key, const Slice& value) = 0;

        virtual Status Delete(const WriteOptions& options, const Slice& key) = 0;

        virtual Status Write(const WriteOptions& options, WriteBatch* updates) = 0;

        virtual Status Get(const ReadOptions& options, const Slice& key, std::string* value) = 0;

        virtual Iterator* NewIterator(const ReadOptions& options) = 0;

        virtual const Snapshot* GetSnapshot() = 0;

        virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;

        virtual bool GetProperty(const Slice& property, std::string* value) = 0;

        virtual void GetApproximateSizes(const Range* range, int n, uint64_t* sizes) = 0;

        virtual void CompactRange(const Slice* begin, const Slice* end) = 0;
};

Status DestroyDN(const std::string& name, const Options& options);

Status RepairDB(const std::string& dbname, const Options& options);

}