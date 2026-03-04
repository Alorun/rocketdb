#pragma once

#include <algorithm>
#include <cstddef>
#include <string>

#include "slice.h"

namespace rocketdb {
    class Status {
    public:
        Status() noexcept : state_(nullptr) {}
        ~Status() { delete[] state_; }

        Status(const Status& rhs);
        Status& operator=(const Status& rhs);

        Status(Status&& rhs) noexcept : state_(rhs.state_) { rhs.state_ = nullptr; }
        Status& operator=(Status&& rhs) noexcept;


        // Static functions reduce repeated constructions
        static Status OK() { return Status(); }
        
        static Status NotFound(const Slice& msg1, const Slice& msg2 = Slice()) {
            return Status(kNotFound, msg1, msg2);
        }

        static Status Corruption(const Slice& msg1, const Slice& msg2 = Slice()) {
            return Status(kCorruption, msg1, msg2);
        }

        static Status NotSupported(const Slice& msg1, const Slice& msg2 = Slice()) {
            return Status(kNotSupported, msg1, msg2);
        }

        static Status InvalidArgument(const Slice& msg1, const Slice& msg2 = Slice()) {
            return Status(kInvalidArgument, msg1, msg2);
        }

        static Status IOError(const Slice& msg1, const Slice& msg2 = Slice()) {
            return Status(kIOError, msg1, msg2);
        }

        bool ok() const { return (state_ == nullptr); }

        bool IsNotFound() const { return code() == kNotFound; }

        bool IsCorruption() const { return code() == kCorruption; }

        bool IsIOError() const { return code() == kIOError; }

        bool IsNotSupportedError() const { return code() == kNotSupported; }

        bool IsInvalidArgument() const { return code() == kInvalidArgument; }

        std::string ToString() const;


    private:
        enum Code {
            kOk = 0,
            kNotFound = 1,
            kCorruption = 2,
            kNotSupported = 3,
            kInvalidArgument = 4,
            kIOError = 5
        };

        Code code() const {
            return (state_ == nullptr) ? kOk : static_cast<Code>(state_[4]);
        }

        Status(Code code, const Slice& msg1, const Slice& msg2);
        static const char* CopyState(const char* s);

        // OK status has a null state_
        // Otherwise state is a array of following 
        // state_[0..3] length of message
        // state_[4] code
        // state_[5..] message
        const char* state_;
};

inline Status::Status(const Status& rhs) {
    state_ = (rhs.state_ == nullptr) ? nullptr : CopyState(rhs.state_);
}

inline Status& Status::operator=(const Status& rhs) {
    if (state_ != rhs.state_) {
        delete [] state_;
        state_ = (rhs.state_ == nullptr) ? nullptr : CopyState(rhs.state_);
    }
    return *this;
}
inline Status& Status::operator=(Status&& rhs) noexcept {
    std::swap(state_, rhs.state_);
    return *this;
}

}