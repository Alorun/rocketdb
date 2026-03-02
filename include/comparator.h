#include <string>

namespace rocketdb {

class Slice;

class Comparator {

public:
    virtual ~Comparator();

    virtual int Compare(const Slice& a, const Slice& b) const = 0;

    virtual const char* Name() const = 0;

    virtual void FindShortestSperator(std::string* start, const Slice& limit) const = 0;

    virtual void FindShortSuccessor(std::string* key) const = 0;
};

const Comparator* BytewiseComparator();

}