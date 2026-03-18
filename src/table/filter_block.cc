#include "filter_block.h"

#include "../../include/filter_policy.h"
#include "../util/coding.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace rocketdb {

static const size_t kFilterBaseLg = 11;
static const size_t kFilterBase = 1 << kFilterBaseLg;

FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* policy) : policy_(policy) {}\

void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
    uint64_t filter_index = (block_offset / kFilterBase);
    assert(filter_index >= filter_offsets_.size());
    while(filter_index > filter_offsets_.size()) {
        GenerateFilter();
    }
}

void FilterBlockBuilder::AddKey(const Slice& key) {
    Slice k = key;
    start_.push_back(keys_.size());
    keys_.append(k.data(), k.size());
}

Slice FilterBlockBuilder::Finish() {
    if (!start_.empty()) {
        GenerateFilter();
    }

    // Append array of per-filter offsets
    const uint32_t array_offset = result_.size();
    for (size_t i = 0; i < filter_offsets_.size(); i++) {
        PutFixed32(&result_, filter_offsets_[i]);
    }

    PutFixed32(&result_, array_offset);
    result_.push_back(kFilterBaseLg);
    return Slice(result_);
}

void FilterBlockBuilder::GenerateFilter() {
    const size_t num_keys = start_.size();
    if (num_keys == 0) {
        filter_offsets_.push_back(result_.size()); // ?
        return;
    }

    start_.push_back(keys_.size());
    tmp_keys_.resize(num_keys);

}


}