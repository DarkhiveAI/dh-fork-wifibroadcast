//
// Created by consti10 on 27.06.23.
//

#include <algorithm>
#include <cstdlib>

#include "../src/HelperSources/WifiCardHelper.hpp"
#include "../src/WBStreamRx.h"
#include "../src/WBStreamTx.h"
#include "../src/WBTxRx.h"
#include "../src/wifibroadcast_spdlog.h"
#include "RandomBufferPot.hpp"

int main(int argc, char *const *argv) {
  std::string card = "wlxac9e17596103";
  std::string card_arg;
  bool list_cards = false;
  int freq_mhz = 0;
  std::string ht_mode_arg;
  int mcs_override = -1;
  const bool interactive = (argc == 1);
  bool pcap_setdirection = true;
  int opt;
  while ((opt = getopt(argc, argv, "w:lf:H:m:d")) != -1) {
    switch (opt) {
      case 'w':
        card_arg = optarg;
        break;
      case 'l':
        list_cards = true;
        break;
      case 'f':
        freq_mhz = atoi(optarg);
        break;
      case 'H':
        ht_mode_arg = optarg;
        break;
      case 'm':
        mcs_override = atoi(optarg);
        break;
      case 'd':
        pcap_setdirection = false;
        break;
      default: /* '?' */
      show_usage:
        fprintf(
            stderr,
            "test_txrx %s [-w iface|index] [-l] [-f freq_mhz]\n"
            "  [-H HT20|HT40+|HT40-] [-m mcs] [-d]\n",
            argv[0]);
        exit(1);
    }
  }
  const auto detected_cards =
      wifibroadcast::wifi_card_helper::list_wifi_cards();
  if (list_cards) {
    std::cout << wifibroadcast::wifi_card_helper::format_card_list(
        detected_cards);
    return 0;
  }
  if (interactive) {
    card = wifibroadcast::wifi_card_helper::prompt_select_card(detected_cards);
    freq_mhz = wifibroadcast::wifi_card_helper::prompt_int(
        "Set frequency MHz (empty to skip): ", 0, true);
    ht_mode_arg = wifibroadcast::wifi_card_helper::read_line(
        "HT mode (HT20/HT40+/HT40-, empty to skip): ");
    mcs_override = wifibroadcast::wifi_card_helper::prompt_int(
        "MCS index (empty to skip): ", -1, true);
  }
  if (!interactive && !card_arg.empty()) {
    if (wifibroadcast::wifi_card_helper::is_number(card_arg)) {
      if (detected_cards.empty()) {
        fprintf(stderr,
                "No wifi cards detected, cannot resolve index %s\n",
                card_arg.c_str());
        return 1;
      }
      const size_t idx = static_cast<size_t>(std::stoul(card_arg));
      if (idx >= detected_cards.size()) {
        fprintf(stderr, "Wifi card index %zu out of range (0..%zu)\n", idx,
                detected_cards.size() - 1);
        return 1;
      }
      card = detected_cards[idx];
    } else {
      card = card_arg;
      if (!detected_cards.empty() &&
          std::find(detected_cards.begin(), detected_cards.end(), card) ==
              detected_cards.end()) {
        fprintf(stderr,
                "Warning: interface %s not in detected list, continuing\n",
                card.c_str());
      }
    }
  } else if (!interactive && !detected_cards.empty()) {
    card = detected_cards.front();
  }
  const auto normalized_ht =
      wifibroadcast::wifi_card_helper::normalize_ht_mode(ht_mode_arg);
  if (!ht_mode_arg.empty() && normalized_ht.empty()) {
    fprintf(stderr, "Invalid HT mode: %s\n", ht_mode_arg.c_str());
    return 1;
  }
  if (freq_mhz > 0) {
    std::string err;
    if (!wifibroadcast::wifi_card_helper::apply_iw_freq_and_ht(
            card, freq_mhz, normalized_ht, &err)) {
      fprintf(stderr, "Failed to set frequency: %s\n", err.c_str());
    }
  }

  std::vector<wifibroadcast::WifiCard> cards;
  wifibroadcast::WifiCard tmp_card{card, 1};
  cards.push_back(tmp_card);
  WBTxRx::Options options_txrx{};
  // options_txrx.pcap_rx_set_direction= false;
  options_txrx.pcap_rx_set_direction = pcap_setdirection;
  options_txrx.log_all_received_validated_packets = true;
  auto radiotap_header_holder_tx = std::make_shared<RadiotapHeaderTxHolder>();
  const int bw_from_ht =
      wifibroadcast::wifi_card_helper::ht_mode_to_bandwidth(normalized_ht);
  if (bw_from_ht > 0) {
    radiotap_header_holder_tx->update_channel_width(bw_from_ht);
  }
  if (mcs_override >= 0) {
    radiotap_header_holder_tx->update_mcs_index(mcs_override);
  }
  std::shared_ptr<WBTxRx> txrx =
      std::make_shared<WBTxRx>(cards, options_txrx, radiotap_header_holder_tx);

  const bool enable_fec = true;
  WBStreamTx::Options options_tx{};
  options_tx.radio_port = 10;
  options_tx.enable_fec = enable_fec;
  auto radiotap_header_holder_rx = std::make_shared<RadiotapHeaderTxHolder>();
  if (bw_from_ht > 0) {
    radiotap_header_holder_rx->update_channel_width(bw_from_ht);
  }
  if (mcs_override >= 0) {
    radiotap_header_holder_rx->update_mcs_index(mcs_override);
  }
  std::unique_ptr<WBStreamTx> wb_tx =
      std::make_unique<WBStreamTx>(txrx, options_tx, radiotap_header_holder_rx);

  WBStreamRx::Options options_rx{};
  options_rx.radio_port = 10;
  options_rx.enable_fec = enable_fec;
  std::unique_ptr<WBStreamRx> wb_rx =
      std::make_unique<WBStreamRx>(txrx, options_rx);
  auto console = wifibroadcast::log::create_or_get("out_cb");
  auto cb = [&console](const uint8_t *payload, const std::size_t payloadSize) {
    console->debug("Got data {}", payloadSize);
  };
  wb_rx->set_callback(cb);

  txrx->start_receiving();

  const auto randomBufferPot = std::make_unique<RandomBufferPot>(1000, 1024);

  auto lastLog = std::chrono::steady_clock::now();
  while (true) {
    for (int i = 0; i < 100; i++) {
      auto dummy_packet = randomBufferPot->getBuffer(i);
      // txrx->tx_inject_packet(0,dummy_packet->data(),dummy_packet->size());
      if (enable_fec) {
        wb_tx->try_enqueue_block({dummy_packet}, 10, 10);
      } else {
        wb_tx->try_enqueue_packet(dummy_packet);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      const auto elapsed_since_last_log =
          std::chrono::steady_clock::now() - lastLog;
      if (elapsed_since_last_log > std::chrono::seconds(1)) {
        lastLog = std::chrono::steady_clock::now();
        auto txStats = txrx->get_tx_stats();
        auto rxStats = txrx->get_rx_stats();
        auto rx_stats_card0 = txrx->get_rx_stats_for_card(0);
        std::cout << txStats << "\n";
        std::cout << rxStats << "\n";
        std::cout << rx_stats_card0 << std::endl;
      }
    }
  }
}
