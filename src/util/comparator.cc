#include "../../include/comparator.h"

#include <algorithm>
#include <string>

#include "../../include/slice.h"

namespace rocketdb {

Comparator::~Comparator() = default;

namespace  {

class BytewiseComparatorImpl : public Comparator {

};

}

const Comparator* BytewiseComparator() {
    static NoDestructor<BytewiseComparatorImpl> singleton;
    return singleton.get();
}

}