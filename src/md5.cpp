#include "md5.h"

#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace ustb {
namespace {

constexpr size_t kMd5Len = 16;

}  // namespace

std::string md5_hex(const void* data, size_t len) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::string out;
  DWORD cb_hash = 0;
  DWORD cb_data = 0;

  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) !=
      0) {
    return out;
  }
  if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&cb_hash),
                        sizeof(cb_hash), &cb_data, 0) != 0 ||
      cb_hash != kMd5Len) {
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
  }
  if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
  }
  if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
                     static_cast<ULONG>(len), 0) != 0) {
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
  }
  unsigned char digest[kMd5Len]{};
  if (BCryptFinishHash(hash, digest, kMd5Len, 0) != 0) {
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
  }
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);

  static const char* hex = "0123456789abcdef";
  out.resize(kMd5Len * 2);
  for (size_t i = 0; i < kMd5Len; ++i) {
    out[i * 2] = hex[digest[i] >> 4];
    out[i * 2 + 1] = hex[digest[i] & 0xf];
  }
  return out;
}

std::string md5_hex(const std::string& s) {
  return md5_hex(s.data(), s.size());
}

}  // namespace ustb
