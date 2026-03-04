#pragma once

#include <string>

namespace rocketdb {

class Slice;

class FilterPolicy {
    public:
        virtual ~FilterPolicy();

        virtual const char* Name() const = 0;

        virtual void CreateFilter(const Slice* key, int n, std::string* dst) const = 0;

        virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const = 0;
};

}