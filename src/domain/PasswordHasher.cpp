#include "domain/PasswordHasher.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace bank {
namespace {

// Manual hex encoding keeps the bytes human-readable in the audit trail and
// avoids null bytes in text-based storage.
std::string bytes_to_hex(const unsigned char* data, std::size_t size)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        const auto byte = static_cast<unsigned>(data[i]);
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0Fu]);
    }
    return out;
}

std::vector<unsigned char> hex_to_bytes(std::string_view hex)
{
    if (hex.size() % 2 != 0) {
        throw std::invalid_argument{"PasswordHash: salt is not valid hex"};
    }

    const auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
        throw std::invalid_argument{"PasswordHash: salt contains a non-hex digit"};
    };

    std::vector<unsigned char> bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        bytes.push_back(static_cast<unsigned char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    }
    return bytes;
}

PasswordHash derive(std::string_view pin, const std::vector<unsigned char>& salt)
{
    PasswordHash out;
    out.algorithm = PasswordHasher::algorithm_name;

    std::array<unsigned char, static_cast<std::size_t>(PasswordHasher::digest_len_bytes)> digest{};
    const int rc = PKCS5_PBKDF2_HMAC(
        pin.data(), static_cast<int>(pin.size()),
        salt.data(), static_cast<int>(salt.size()),
        PasswordHasher::iterations,
        EVP_sha256(),
        static_cast<int>(digest.size()),
        digest.data());
    if (rc != 1) {
        throw std::runtime_error{"PasswordHasher: PBKDF2 derivation failed"};
    }

    out.salt = bytes_to_hex(salt.data(), salt.size());
    out.digest = bytes_to_hex(digest.data(), digest.size());
    return out;
}

} // namespace

PasswordHash PasswordHasher::create(std::string_view pin)
{
    std::array<unsigned char, static_cast<std::size_t>(salt_len_bytes)> salt{};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
        throw std::runtime_error{"PasswordHasher: CSPRNG failure"};
    }

    std::vector<unsigned char> bytes{salt.begin(), salt.end()};
    return derive(pin, bytes);
}

PasswordHash PasswordHasher::create_with_salt(std::string_view pin, std::string_view salt_hex)
{
    return derive(pin, hex_to_bytes(salt_hex));
}

bool PasswordHasher::verify(std::string_view pin, const PasswordHash& stored)
{
    if (stored.algorithm != algorithm_name || stored.digest.empty()) {
        return false;
    }

    try {
        const auto salt = hex_to_bytes(stored.salt);
        return derive(pin, salt).digest == stored.digest;
    } catch (const std::exception&) {
        // A malformed stored record is not a valid credential.
        return false;
    }
}

} // namespace bank