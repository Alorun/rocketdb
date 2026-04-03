#pragma once

namespace rocketdb {

class Comparator;
class Iterator;

// Wrap mutiple input iterators into a single iterator
Iterator* NewMergingIterator(const Comparator* comparator, Iterator** children, int n);

}