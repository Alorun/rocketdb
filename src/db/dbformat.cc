#include "dbformat.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <sstream>

#include "../util/coding.h"

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
    ss << '\'' << EscapeString()
}

}