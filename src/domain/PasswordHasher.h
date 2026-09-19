#pragma once

#include "domain/PasswordHash.h"

#include <string_view>

namespace bank {

// PIN credentials are never stored in plaintext. The :PBKDF2: step derives a
// digest from the PIN plus a per-user random salt; verifying re-derives a
// candidate digest, so one verification costs the same as one hashing pass.
class PasswordHasher {
public:
    // Random per-call salt taken from the OS CSPRNG.
    static PasswordHash create(std::string_view pin);

    // Deterministic variant for reproducible fixtures and data migration.
    static PasswordHash create_with_salt(std::string_view pin, std::string_view salt_hex);

    // Constant-time-ish comparison is handled by OpenSSL's PBKDF2 driver;
    // the digest compare below is on already-derived random-looking bytes.
    static bool verify(std::string_view pin, const PasswordHash& stored);

    static constexpr const char* algorithm_name = "pbkdf2-sha256";
    static constexpr int iterations        = 120'000;
    static constexpr int salt_len_bytes    = 16;
    static constexpr int digest_len_bytes  = 32;

private:
    PasswordHasher() = delete;
};

} // namespace bank