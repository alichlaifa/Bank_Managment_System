#include <gtest/gtest.h>
#include "domain/Ostream.h"

#include "domain/PasswordHash.h"
#include "domain/PasswordHasher.h"

using bank::PasswordHash;
using bank::PasswordHasher;

TEST(Password, HashedPinsVerifyAndNeverStorePlaintext)
{
    const PasswordHash stored = PasswordHasher::create("1234");
    ASSERT_TRUE(stored.digest != "1234");
    ASSERT_TRUE(PasswordHasher::verify("1234", stored));
    ASSERT_FALSE(PasswordHasher::verify("0000", stored));
    ASSERT_FALSE(PasswordHasher::verify("", stored));
}

TEST(Password, DefaultHashingSaltsAreUniquePerCall)
{
    const auto first = PasswordHasher::create("1234");
    const auto second = PasswordHasher::create("1234");
    ASSERT_TRUE(first.salt != second.salt);
    ASSERT_TRUE(first.digest != second.digest);
    ASSERT_TRUE(first.algorithm == PasswordHasher::algorithm_name);
}

TEST(Password, DeterministicHashingForFixtures)
{
    const auto stored = PasswordHasher::create_with_salt(
        "4321", "00112233445566778899aabbccddeeff");
    ASSERT_TRUE(stored.salt == "00112233445566778899aabbccddeeff");
    ASSERT_TRUE(stored.digest.size() == 64);
    ASSERT_TRUE(PasswordHasher::verify("4321", stored));
    ASSERT_FALSE(PasswordHasher::verify("4322", stored));
}

TEST(Password, MalformedStoredRecordsNeverVerify)
{
    ASSERT_FALSE(PasswordHasher::verify("1234", PasswordHash{"pbkdf2-sha256", "zz", "00"}));
    ASSERT_FALSE(PasswordHasher::verify("1234", PasswordHash{"","", ""}));
    ASSERT_FALSE(PasswordHasher::verify("1234", PasswordHash{"sha1", "10", "20"}));
}
