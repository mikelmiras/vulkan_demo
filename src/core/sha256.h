// SHA-256 (FIPS 180-4): checking downloads (app/updates.h: an update is only installed when its digest matches)
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace sha256 {

class Hasher {
public:
    Hasher();
    void add(const void* data, size_t size);
    std::string hex();   // the digest as 64 lowercase hex digits (finishes it: add nothing after)

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buffer_[64];
    size_t used_ = 0;
    uint64_t total_ = 0;
};

std::string of(const std::string& bytes);
// A file's digest, read in pieces; "" if it can't be read
std::string ofFile(const std::filesystem::path& file);

}  // namespace sha256
