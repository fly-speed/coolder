#pragma once
#include "record_codec.h"
#include <openssl/rand.h>

namespace webcool { namespace ai { namespace identifiers {
inline bool valid_id(const std::string& id) {
    if (id.size() != 32) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        if (record_codec::hex_value(id[i]) < 0) return false;
    }
    return true;
}
// A 128-bit cryptographically random ID. RNG failure stays visible as empty.
inline std::string new_id() {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) return "";
    return record_codec::hex_encode(bytes, sizeof(bytes));
}
}}}
