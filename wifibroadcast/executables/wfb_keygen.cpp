
// Copyright (C) 2017, 2018 Vasily Evseenko <svpcom@p2ptech.org>
// 2020 Constantin Geier

/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; version 3.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License along
 *   with this program; if not, write to the Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <iostream>

#include "../src/encryption/Encryption.h"

/**
 * Generates a new tx/rx keypair and prints it as raw bytes (hex).
 * (Key files are not used by OpenHD community builds.)
 */
int main(int argc, char *const *argv) {
  if (argc > 1) {
    std::cerr << "Usage: wfb-keygen (no arguments)" << std::endl;
    return 1;
  }
  std::cout << "Generating random txrx keypair (hex)" << std::endl;
  wb::KeyPairTxRx keyPairTxRx = wb::generate_keypair_random();
  const auto raw = wb::KeyPairTxRx::as_raw(keyPairTxRx);
  std::ios_base::fmtflags f(std::cout.flags());
  for (size_t i = 0; i < raw.size(); ++i) {
    std::cout << std::hex << std::uppercase
              << static_cast<int>(raw[i] >> 4)
              << static_cast<int>(raw[i] & 0xF);
  }
  std::cout.flags(f);
  std::cout << std::endl;
  return 0;
}
