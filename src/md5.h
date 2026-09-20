#pragma once

#include <string>

namespace ustb {

// Lowercase hex MD5 of the input bytes (32 chars). Empty on failure.
std::string md5_hex(const void* data, size_t len);
std::string md5_hex(const std::string& s);

}  // namespace ustb
