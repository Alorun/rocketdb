#include "log_reader.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../util/coding.h"
#include "log_format.h"

namespace rocketdb {
namespace log {

Reader::Reporter::~Reporter() = default;

Reader::Reader(SequentialFile* file, Reporter* reperter, bool checksum, uint64_t initial_offset)
        : file_(file),
          reporter_(reperter),
          checksum_(checksum),
          backing_store_(new char[kBlockSize]),
          buffer_(),
          eof_(false),
          last_record_offset_(0),
          end_of_buffer_offset_(0),
          initial_offset_(initial_offset),
          resyncing_(initial_offset > 0) {}

Reader::~Reader() { delete[] backing_store_; }

bool Reader::SkipToInitialBlock() {
    const size_t offset_in_block = initial_offset_ % kBlockSize;
    uint64_t block_start_location = initial_offset_ - offset_in_block;

    if (offset_in_block > kBlockSize - 6) {
        block_start_location += kBlockSize;
    }

    end_of_buffer_offset_ = block_start_location;
    if (block_start_location > 0) {
        Status skip_status = file_->Skip(block_start_location);
        if (!skip_status.ok()) {
            ReportDrop(block_start_location, skip_status);
            return false;
        }
    }

    return true;
}



}
}