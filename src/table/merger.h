#pragma once

namespace leveldb {

class Comparator;
class Iterator;

Iterator* NewMergingIterator(const Comparator* comparator, Iterator** children, int n);

}