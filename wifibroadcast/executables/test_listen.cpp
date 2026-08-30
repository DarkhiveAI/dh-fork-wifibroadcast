//
// Created by consti10 on 07.10.23.
// Uses WBTxRx to listen to all (openhd and non openhd) traffic
//
#include <algorithm>
#include <cstdlib>
#include <unistd.h>

#include "../src/HelperSources/WifiCardHelper.hpp"
#include "../src/WBTxRx.h"
#include "../src/wifibroadcast_spdlog.h"

int main(int argc, char *const *argv) {
  std::string card = "wlxac9e17596103";
  std::string card_arg;
  bool list_cards = false;
  int freq_mhz = 0;
  std::string ht_mode_arg;
  const bool interactive = (argc == 1);
  bool pcap_setdirection = true;
  int duration_seconds = 0;
  int opt;
  while ((opt = getopt(argc, argv, "w:lf:H:dt:")) != -1) {
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
      case 'd':
        pcap_setdirection = false;
        break;
      case 't':
        duration_seconds = atoi(optarg);
        break;
      default: /* '?' */
      show_usage:
        fprintf(stderr,
                "test_listen %s [-w iface|index] [-l] [-f freq_mhz]\n"
                "  [-H HT20|HT40+|HT40-] [-d] [-t seconds]\n",
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
  {
    std::string err;
    if (!wifibroadcast::wifi_card_helper::ensure_monitor_mode(card, &err)) {
      fprintf(stderr, "Failed to enter monitor mode: %s\n", err.c_str());
      return 1;
    }
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
  options_txrx.rx_radiotap_debug_level = 3;
  options_txrx.advanced_debugging_rx = true;
  auto radiotap_header_holder_tx = std::make_shared<RadiotapHeaderTxHolder>();
  std::shared_ptr<WBTxRx> txrx =
      std::make_shared<WBTxRx>(cards, options_txrx, radiotap_header_holder_tx);

  txrx->start_receiving();

  auto lastLog = std::chrono::steady_clock::now();
  const auto started = std::chrono::steady_clock::now();
  while (duration_seconds <= 0 ||
         std::chrono::steady_clock::now() - started <
             std::chrono::seconds(duration_seconds)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // auto txStats=txrx->get_tx_stats();
    auto rxStats = txrx->get_rx_stats();
    auto rx_stats_card0 = txrx->get_rx_stats_for_card(0);
    auto rx_rf_stats_card0 = txrx->get_rx_rf_stats_for_card(0);
    // std::cout<<txStats<<"\n";
    std::cout << rxStats << "\n";
    std::cout << rx_stats_card0 << std::endl;
    std::cout << rx_rf_stats_card0 << std::endl;
  }
  std::cout << "FINAL_RX_STATS " << txrx->get_rx_stats() << "\n";
  std::cout << "FINAL_RX_CARD_STATS " << txrx->get_rx_stats_for_card(0)
            << "\n";
  std::cout << "FINAL_RX_RF_STATS " << txrx->get_rx_rf_stats_for_card(0)
            << "\n";
  return 0;
}
