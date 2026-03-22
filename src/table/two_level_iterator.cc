#include "two_level_iterator.h"

#include "../../include/table.h"
#include "format.h"
#include "block.h"
#include "iterator_wrapper.h"
#include <cassert>

namespace rocketdb {

typedef Iterator* (*BlockFunction)(void*, const ReadOptions&, const Slice&);

class TwoLevelIterator : public Iterator {
    public:
        TwoLevelIterator(Iterator* index_iter, BlockFunction block_function, void* arg, const ReadOptions& options);

        ~TwoLevelIterator() override;

        void Seek(const Slice& target) override;
        void SeekToFirst() override;
        void SeekToLast() override;
        void Next() override;
        void Prev() override;

        bool Valid() const override { return data_iter_.Valid(); }
        Slice key() const override {
            assert(Valid());
            return data_iter_.key();
        }
        Slice value() const override {
            assert(Valid());
            return data_iter_.value();
        }
        Status status() const override {
            if (!index_iter_.status().ok()) {
                return index_iter_.status();
            } else if (data_iter_.iter() != nullptr && !data_iter_.status().ok()) {
                return data_iter_.status();
            } else {
                return status_;
            }
        }

    private:
        void SaveError(const Status& s) {
            if (status_.ok() && !s.ok()) status_ = s;
        }
        void SkipEmptyDataBlockForward();
        void SkipEmptyDataBlockBackward();
        void SetDataIterator(Iterator* data_iter);
        void InitDataBlock();

        BlockFunction block_function_;
        void* arg_;
        const ReadOptions options_;
        Status status_;
        IteratorWrapper index_iter_;
        IteratorWrapper data_iter_;
        std::string dta_block_handle_;
};

}