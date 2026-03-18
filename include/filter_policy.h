#pragma once

#include <string>

namespace rocketdb {

class Slice;

class FilterPolicy {
    public:
        virtual ~FilterPolicy();

        // Return the name of the policy
        virtual const char* Name() const = 0;

        // Keys should are ordered according to the user supplied comparator
        // Append a filter that summarizes keys[0, n-1] to *dst
        virtual void CreateFilter(const Slice* key, int n, std::string* dst) const = 0;

        // This method must return true if the key was in the list of keys passed to CreateFilter()
        virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const = 0;
};

// Return a new filter policy that uses a bloom filter with approximately the specified number of bits per key
const FilterPolicy* NewBloomFilterPolicy(int bits_per_key);

}