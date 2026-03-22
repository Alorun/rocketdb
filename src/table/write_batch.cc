#include "../../include/write_batch.h"

#include "format.h"
#include "../db/memtable.h"
#include "../db/write_batch_internal.h"
#include "../util/coding.h"
#include <cstddef>

namespace rocketdb {

// WriteBatch::rep_ :=
//    sequence: fixed64
//    count: fixed32
//    data: record[count]

// record := kTypeValue varstring varstring | kTypeDeletion varstring
// varstring := len: varint32 + data: uint8[len]

static const size_t  kHeader = 12;

WriteBatch::WriteBatch() { Clear(); }

WriteBatch::~WriteBatch() = default;

WriteBatch::Handler::~Handler() = default;

void WriteBatch::Clear() {
    rep_.clear();
    rep_.resize(kHeader);
}

size_t WriteBatch::ApproximateSize() const { return rep_.size(); }

Status WriteBatch::Iterate(Handler* handler) const {
    Slice input(rep_);
    if (input.size() < kHeader) {
        return Status::Corruption("malformed WriteBatch (too small)");
    }

    input.remove_prefix(kHeader);
    Slice key, value;
    int found = 0;
    while (!input.empty()) {
        found++;
            
        
    }
}

}