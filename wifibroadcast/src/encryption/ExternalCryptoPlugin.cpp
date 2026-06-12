#include "ExternalCryptoPlugin.h"

#include <cstdlib>

#include "../wifibroadcast_spdlog.h"

#ifdef __linux__
#include <dlfcn.h>
#endif

namespace {
constexpr const char* kExternalCryptoEnvVar = "OPENHD_VIDEO_CRYPTO_SO";
constexpr const char* kDefaultExternalCryptoPath =
    "/usr/local/lib/openhd/libohd_video_crypto.so";
}

wb::ExternalCryptoPlugin::~ExternalCryptoPlugin() {
#ifdef __linux__
  if (m_shutdown) {
    m_shutdown();
  }
  if (m_handle) {
    dlclose(m_handle);
  }
#endif
}

bool wb::ExternalCryptoPlugin::load(const std::string& path, bool is_air) {
#ifdef __linux__
  if (m_handle != nullptr) {
    return true;
  }

  std::string resolved_path = path;
  if (resolved_path.empty()) {
    const char* env_path = std::getenv(kExternalCryptoEnvVar);
    resolved_path = (env_path && env_path[0] != '\0')
                        ? env_path
                        : kDefaultExternalCryptoPath;
  }

  m_handle = dlopen(resolved_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!m_handle) {
    wifibroadcast::log::get_default()->debug(
        "External crypto plugin not loaded from {}: {}", resolved_path,
        dlerror() ? dlerror() : "unknown error");
    return false;
  }

  m_init = reinterpret_cast<init_fn>(
      dlsym(m_handle, "openhd_video_crypto_init"));
  m_shutdown = reinterpret_cast<shutdown_fn>(
      dlsym(m_handle, "openhd_video_crypto_shutdown"));
  m_encrypt = reinterpret_cast<encrypt_fn>(
      dlsym(m_handle, "openhd_video_crypto_encrypt"));
  m_decrypt = reinterpret_cast<decrypt_fn>(
      dlsym(m_handle, "openhd_video_crypto_decrypt"));

  if (!m_init || !m_shutdown || !m_encrypt || !m_decrypt) {
    wifibroadcast::log::get_default()->warn(
        "External crypto plugin {} is missing required symbols",
        resolved_path);
    dlclose(m_handle);
    m_handle = nullptr;
    m_init = nullptr;
    m_shutdown = nullptr;
    m_encrypt = nullptr;
    m_decrypt = nullptr;
    return false;
  }

  const int rc = m_init(is_air ? 1 : 0);
  if (rc != 0) {
    wifibroadcast::log::get_default()->warn(
        "External crypto plugin init failed with code {}", rc);
    dlclose(m_handle);
    m_handle = nullptr;
    m_init = nullptr;
    m_shutdown = nullptr;
    m_encrypt = nullptr;
    m_decrypt = nullptr;
    return false;
  }

  m_loaded_path = resolved_path;
  wifibroadcast::log::get_default()->info("Loaded external crypto plugin {}",
                                          m_loaded_path);
  return true;
#else
  (void)path;
  (void)is_air;
  return false;
#endif
}

bool wb::ExternalCryptoPlugin::encrypt(const uint8_t* in, size_t in_len,
                                       uint8_t* out, size_t* out_len,
                                       uint64_t* key_id) const {
  if (!m_encrypt) {
    return false;
  }
  return m_encrypt(in, in_len, out, out_len, key_id) == 0;
}

bool wb::ExternalCryptoPlugin::decrypt(const uint8_t* in, size_t in_len,
                                       uint8_t* out, size_t* out_len,
                                       uint64_t key_id) const {
  if (!m_decrypt) {
    return false;
  }
  return m_decrypt(in, in_len, out, out_len, key_id) == 0;
}
