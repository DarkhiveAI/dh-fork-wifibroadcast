#ifndef WIFIBROADCAST_EXTERNAL_CRYPTO_PLUGIN_H
#define WIFIBROADCAST_EXTERNAL_CRYPTO_PLUGIN_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace wb {

class ExternalCryptoPlugin {
 public:
  ExternalCryptoPlugin() = default;
  ExternalCryptoPlugin(const ExternalCryptoPlugin&) = delete;
  ExternalCryptoPlugin& operator=(const ExternalCryptoPlugin&) = delete;
  ~ExternalCryptoPlugin();

  bool load(const std::string& path, bool is_air);
  bool is_loaded() const { return m_handle != nullptr; }
  const std::string& loaded_path() const { return m_loaded_path; }

  bool encrypt(const uint8_t* in, size_t in_len, uint8_t* out,
               size_t* out_len, uint64_t* key_id) const;
  bool decrypt(const uint8_t* in, size_t in_len, uint8_t* out,
               size_t* out_len, uint64_t key_id) const;

 private:
  using init_fn = int (*)(int is_air);
  using shutdown_fn = void (*)();
  using encrypt_fn = int (*)(const uint8_t* in, size_t in_len, uint8_t* out,
                             size_t* out_len, uint64_t* key_id);
  using decrypt_fn = int (*)(const uint8_t* in, size_t in_len, uint8_t* out,
                             size_t* out_len, uint64_t key_id);

  void* m_handle = nullptr;
  std::string m_loaded_path;
  init_fn m_init = nullptr;
  shutdown_fn m_shutdown = nullptr;
  encrypt_fn m_encrypt = nullptr;
  decrypt_fn m_decrypt = nullptr;
};

}  // namespace wb

#endif  // WIFIBROADCAST_EXTERNAL_CRYPTO_PLUGIN_H
