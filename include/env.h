#pragma once

#include <cstddef>
#include <string>
#include <cstdarg>
#include <vector>
#include <cstdint>

#include "slice.h"
#include "status.h"

namespace rocketdb {

class SequentialFile;
class Slice;
class WritableFile;

class SequentialFile {
    public:
        SequentialFile() = default;

        SequentialFile(const SequentialFile&) = delete;
        SequentialFile& operator=(const SequentialFile&) = delete;

        virtual ~SequentialFile();

        virtual Status Read(size_t n, Slice* result, char* scratch) = 0;

        virtual Status Skip(uint64_t n) = 0;
};

class WritableFile {
    public:
        WritableFile() = default;

        WritableFile(const WritableFile&) = delete;
        WritableFile& operator=(const WritableFile&) = delete;

        virtual ~WritableFile();

        virtual Status Append(const Slice& data) = 0;
        virtual Status Close() = 0;
        virtual Status Flush() = 0;
        virtual Status Sync() = 0;
};

}