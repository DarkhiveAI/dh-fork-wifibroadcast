#ifndef WIFIBROADCAST_EXTERNAL_CRYPTO_PLUGIN_H
#define WIFIBROADCAST_EXTERNAL_CRYPTO_PLUGIN_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "openhd_crypto_plugin.h"

namespace wb {

class ExternalCryptoPlugin {
 public:
  ExternalCryptoPlugin() = default;
  ExternalCryptoPlugin(const ExternalCryptoPlugin&) = delete;
  ExternalCryptoPlugin& operator=(const ExternalCryptoPlugin&) = delete;
  ~ExternalCryptoPlugin();

  bool load(const std::string& path, bool is_air);
  bool is_loaded() const { return m_handle != nullptr; }
  uint32_t wire_format_id() const {
    return m_provider ? m_provider->wire_format_id : (m_legacy_encrypt ? 1U : 0U);
  }
  const std::string& loaded_path() const { return m_loaded_path; }

  bool encrypt(const uint8_t* in, size_t in_len, uint8_t* out,
               size_t* out_len, uint64_t* key_id) const;
  bool decrypt(const uint8_t* in, size_t in_len, uint8_t* out,
               size_t* out_len, uint64_t key_id) const;

 private:
  using legacy_init_fn = int (*)(int);
  using legacy_shutdown_fn = void (*)();
  using legacy_encrypt_fn = int (*)(const uint8_t*, size_t, uint8_t*, size_t*,
                                    uint64_t*);
  using legacy_decrypt_fn = int (*)(const uint8_t*, size_t, uint8_t*, size_t*,
                                    uint64_t);

  void* m_handle = nullptr;
  std::string m_loaded_path;
  const openhd_crypto_provider* m_provider = nullptr;
  legacy_shutdown_fn m_legacy_shutdown = nullptr;
  legacy_encrypt_fn m_legacy_encrypt = nullptr;
  legacy_decrypt_fn m_legacy_decrypt = nullptr;
};

}  // namespace wb

#endif  // WIFIBROADCAST_EXTERNAL_CRYPTO_PLUGIN_H
