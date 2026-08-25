#pragma once

#include <string>

namespace rocketdb {

class Histogram {
    public:
        Histogram() { Clear(); }
        ~Histogram() {}

        void Clear();
        void Add(double value);
        void Merge(const Histogram& other);

        std::string ToString() const;

    private:
        enum { kNumBuckets = 158 };

        double Average() const;
        double StandardDeviation() const;

        static const double kBucketLimit[kNumBuckets];

        double min_;
        double max_;
        double num_;
        double sum_;
        double sum_squares_;

        double buckets_[kNumBuckets];
};

}
