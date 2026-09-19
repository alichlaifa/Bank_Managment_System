#include <doctest.h>

#include "domain/Password.h"

using bank::PasswordHash;
using bank::PasswordHasher;

TEST_CASE("hashed PINs verify and never store plaintext")
{
    const PasswordHash stored = PasswordHasher::create("1234");
    REQUIRE(stored.digest != "1234");
    REQUIRE(PasswordHasher::verify("1234", stored));
    REQUIRE_FALSE(PasswordHasher::verify("0000", stored));
    REQUIRE_FALSE(PasswordHasher::verify("", stored));
}

TEST_CASE("default hashing salts are unique per call")
{
    const auto first = PasswordHasher::create("1234");
    const auto second = PasswordHasher::create("1234");
    REQUIRE(first.salt != second.salt);
    REQUIRE(first.digest != second.digest);
    REQUIRE(first.algorithm == PasswordHasher::algorithm_name);
}

TEST_CASE("deterministic hashing for fixtures")
{
    const auto stored = PasswordHasher::create_with_salt(
        "4321", "00112233445566778899aabbccddeeff");
    REQUIRE(stored.salt == "00112233445566778899aabbccddeeff");
    REQUIRE(stored.digest.size() == 64);
    REQUIRE(PasswordHasher::verify("4321", stored));
    REQUIRE_FALSE(PasswordHasher::verify("4322", stored));
}

TEST_CASE("malformed stored records never verify")
{
    REQUIRE_FALSE(PasswordHasher::verify("1234", PasswordHash{"pbkdf2-sha256", "zz", "00"}));
    REQUIRE_FALSE(PasswordHasher::verify("1234", PasswordHash{"","", ""}));
    REQUIRE_FALSE(PasswordHasher::verify("1234", PasswordHash{"sha1", "10", "20"}));
}