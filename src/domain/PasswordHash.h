#pragma once

#include <string>

namespace bank {

struct PasswordHash {
    std::string algorithm;
    std::string salt;   // hex encoded
    std::string digest; // hex encoded
};

} // namespace bank