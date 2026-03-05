#include <string>

namespace rocketdb {

class Slice;

class Comparator {
    public:
        virtual ~Comparator();

        virtual int Compare(const Slice& a, const Slice& b) const = 0;

        virtual const char* Name() const = 0;

        // Two compression algorithm

        // By comparing the different letters in the previous block and the next block,
        // the size of the index term can be reduced without affecting the comparison order
        virtual void FindShortestSeparator(std::string* start, const Slice& limit) const = 0;

        // By defining a maximum index for the last block,
        // lookups can be sped up, while the length of the last index can also be reduced
        virtual void FindShortSuccessor(std::string* key) const = 0;
};

const Comparator* BytewiseComparator();

}