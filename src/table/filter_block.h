#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../../include/slice.h"
#include "../util/hash.h"

namespace rocketdb {

class FilterPolicy;

// Filter Format
//      Filter[0] 
//      Filter[1]
//         ~ 
//      Filter[n]
//      Offset[0] 
//      Offset[1] 
//         ~ 
//      Offset[n] (every offset is 4 bytes)
//      Offset Array Point (point to offset[0] is 4 bytes)
//      Base Lg (default is 11 also is 2KB)

class FilterBlockBuilder {
    public:
        explicit FilterBlockBuilder(const FilterPolicy*);

        FilterBlockBuilder(const FilterBlockBuilder&) = delete;
        FilterBlockBuilder& operator=(const FilterBlockBuilder&) = delete;

        // If block_offset >= 2kb, generate new filter
        void StartBlock(uint64_t block_offset);
        void AddKey(const Slice& key);
        // Call the function when the file was finish
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
        FilterBlockReader(const FilterPolicy* policy, const Slice& contents);
        bool KeyMayMatch(uint64_t block_offset, const Slice& key);

    private:
        const FilterPolicy* policy_;
        const char* data_;              // The Raw pointer
        const char* offset_;            // The begin of offset array
        size_t num_;                    // The number of filters
        size_t base_lg_;                // The size fo handled by each filter
};

}