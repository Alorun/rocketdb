#include "dbformat.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "../util/coding.h"
#include "../util/logging.h"

namespace rocketdb {

static uint64_t PackSequenceAndType(uint64_t seq, ValueType t) {
    assert(seq <= kMaxSequenceNumber);
    assert(t <= kValueTypeForSeek);
    return (seq << 8) | t;
}

void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
    result->append(key.user_key.data(), key.user_key.size());
    PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
}

std::string ParsedInternalKey::DebugString() const {
    std::ostringstream ss;
    ss << '\'' << EscapeString(user_key.ToString()) << "' @ " << sequence << " : " << static_cast<int>(type);
    return ss.str();
}

std::string InternalKey::DebugString() const {
    ParsedInternalKey parsed;
    if (ParseInternalKey(rep_, &parsed)) {
        return parsed.DebugString();
    }
    std::ostringstream ss;
    ss << "(bad)" << EscapeString(rep_);
    return ss.str();
}

const char* InternalKeyComparator::Name() const{
    return "rocketdb.InternalKeyComparator";
}

int InternalKeyComparator::Compare(const Slice& akey, const Slice& bkey) const {
    int r = user_comparator_->Compare(ExtractUserKey(akey), ExtractUserKey(bkey));
    // It is the same key, compare the value of seq
    if (r == 0) {
        const uint64_t anum = DecodeFixed64(akey.data() + akey.size() - 8);
        const uint64_t bnum = DecodeFixed64(bkey.data() + bkey.size() - 8);
        if (anum > bnum) {
            r = -1;
        } else if (anum < bnum) {
            r = +1;
        }
    }
    return r;
}

void InternalKeyComparator::FindShortestSeparator(std::string* start, const Slice& limit) const {
    Slice user_start = ExtractUserKey(*start);
    Slice user_limit = ExtractUserKey(limit);
    std::string tmp(user_start.data(), user_start.size());
    user_comparator_->FindShortestSeparator(&tmp, user_limit);
    if (tmp.size() < user_start.size() && user_comparator_->Compare(user_start, tmp)) {
        PutFixed64(&tmp, PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
        assert(this->Compare(*start, tmp) < 0);
        assert(this->Compare(tmp, limit) < 0);
        start->swap(tmp);
    }
}

void InternalKeyComparator::FindShortSuccessor(std::string* key) const {
    Slice user_key = ExtractUserKey(*key);
    std::string tmp(user_key.data(), user_key.size());
    user_comparator_->FindShortSuccessor(&tmp);
    if (tmp.size() < user_key.size() && user_comparator_->Compare(user_key, tmp) < 0) {
        PutFixed64(&tmp, PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
        assert(this->Compare(*key, tmp) < 0);
        key->swap(tmp);
    }
}

const char* InternalFilterPolicy::Name() const { return user_policy_->Name(); }

void InternalFilterPolicy::CreateFilter(const Slice* keys, int n, std::string* dst) const {
    Slice* mkey = const_cast<Slice*>(keys);
    for (int i = 0; i < n; i++) {
        mkey[i] = ExtractUserKey(keys[i]);
    }

    user_policy_->CreateFilter(keys, n, dst);
}

bool InternalFilterPolicy::KeyMayMatch(const Slice& key, const Slice& f) const {
    return user_policy_->KeyMayMatch(ExtractUserKey(key), f);
}

LookupKey::LookupKey(const Slice& user_key, SequenceNumber s) {
    size_t usize = user_key.size();
    size_t needed = usize + 13;
    char* dst;
    if (needed <= sizeof(space_)) {
        dst = space_;
    } else {
        dst = new char[needed];
    }
    start_ = dst;
    dst = EncodeVarint32(dst, usize + 8);
    kstart_ = dst;
    std::memcpy(dst, user_key.data(), usize);
    dst += usize;
    EncodeFixed64(dst, PackSequenceAndType(s, kValueTypeForSeek));
    dst += 8;
    end_ = dst;
}

}