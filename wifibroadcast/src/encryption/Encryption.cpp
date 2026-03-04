//
// Created by consti10 on 13.08.23.
//

#include "Encryption.h"

#include <cstring>

wb::KeyPairTxRx wb::generate_keypair_random() {
  KeyPairTxRx ret{};
  crypto_box_keypair(ret.key_1.public_key.data(), ret.key_1.secret_key.data());
  crypto_box_keypair(ret.key_2.public_key.data(), ret.key_2.secret_key.data());
  return ret;
}

std::array<uint8_t, crypto_aead_chacha20poly1305_KEYBYTES>
wb::create_onetimeauth_subkey(const uint64_t& nonce,
                              const std::array<uint8_t, 32U>& session_key) {
  // sub-key for this packet
  std::array<uint8_t, 32> subkey{};
  // We only have an 8 byte nonce, this should be enough entropy
  std::array<uint8_t, 16> nonce_buf{0};
  memcpy(nonce_buf.data(), (uint8_t*)&nonce, 8);
  crypto_core_hchacha20(subkey.data(), nonce_buf.data(), session_key.data(),
                        nullptr);
  return subkey;
}
