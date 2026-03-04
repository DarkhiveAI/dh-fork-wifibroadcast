
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
#include "../src/encryption/EncryptionFsUtils.h"

/**
 * Generates a new tx rx keypair and saves it to file for later use.
 */
int main(int argc, char *const *argv) {
  if (argc > 1) {
    std::cerr << "Usage: wfb-keygen (no arguments)" << std::endl;
    return 1;
  }
  std::cout << "Generating random txrx keypair" << std::endl;
  wb::KeyPairTxRx keyPairTxRx = wb::generate_keypair_random();
  auto res = wb::write_keypair_to_file(keyPairTxRx, "txrx.key");
  if (res) {
    std::cout << "Wrote keypair to file" << std::endl;
    return 0;
  } else {
    std::cout << "Cannot write keypair to file" << std::endl;
    return -1;
  }
}
