#pragma once

#include <filesystem>
#include <string>
#include <unistd.h>

// RAII owner of a disposable temp directory shared by persistence tests.
class TempDir {
public:
    explicit TempDir(const std::string& tag)
    {
        path = std::filesystem::temp_directory_path();
        path /= "bank_" + tag + "_" + std::to_string(::getpid()) + "_" + std::to_string(next_id_++);
        std::filesystem::create_directories(path);
    }

    ~TempDir()
    {
        std::filesystem::remove_all(path);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::filesystem::path path;

private:
    static int next_id_;
};