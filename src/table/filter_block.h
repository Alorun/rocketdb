#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../../include/slice.h"
#include "../util/hash.h"

namespace rocketdb {

class FilterPolicy;

class FilterBlockBuilder {
    public:
        explicit FilterBlockBuilder(const FilterPolicy*);

        FilterBlockBuilder(const FilterBlockBuilder&) = delete;
        FilterBlockBuilder& operator=(const FilterBlockBuilder&) = delete;

        // If block_offset >= 2kb, generate new filter
        void StartBlock(uint64_t block_offset);
        void AddKey(const Slice& key);
        Slice Finish();

    private:
        void GenerateFilter();

        const FilterPolicy* policy_;
        std::string keys_;                      // Flattened key contents
        std::vector<size_t> start_;             // Starting index in keys_ of each key
        std::string result_;                    // Filter data computed so far
        std::vector<Slice> tmp_keys_;           // Policy_->CreateFilter() argument
        std::vector<uint32_t> filter_offsets_;  // The location of each filter
};

class FilterBlockReader {
    public:
        FilterBlockReader(const FilterBlockReader* policy, const Slice& contents);
        bool KeyMayMatch(uint64_t block_offset, const Slice& key);

    private:
        const FilterPolicy* policy_;
        const char* data_;
        const char* offset_;
        size_t num_;
        size_t base_lg_;
};

}