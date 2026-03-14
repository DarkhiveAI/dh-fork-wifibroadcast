//
// Created by Codex on 14.03.2026.
//

#ifndef WIFIBROADCAST_WIFI_CARD_HELPER_H
#define WIFIBROADCAST_WIFI_CARD_HELPER_H

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <pcap/pcap.h>
#include <sstream>
#include <string>
#include <vector>

namespace wifibroadcast::wifi_card_helper {

inline std::vector<std::string> list_wireless_interfaces_sysfs() {
  std::vector<std::string> ret;
#if defined(__linux__)
  std::error_code ec;
  const std::filesystem::path net_root("/sys/class/net");
  if (!std::filesystem::exists(net_root, ec)) {
    return ret;
  }
  for (const auto &entry :
       std::filesystem::directory_iterator(net_root, ec)) {
    if (ec) {
      break;
    }
    const auto iface = entry.path().filename().string();
    const auto wireless_dir = entry.path() / "wireless";
    const auto phy_dir = entry.path() / "phy80211";
    if (std::filesystem::exists(wireless_dir, ec) ||
        std::filesystem::exists(phy_dir, ec)) {
      ret.push_back(iface);
    }
  }
#endif
  return ret;
}

inline std::vector<std::string> list_interfaces_pcap() {
  std::vector<std::string> ret;
  pcap_if_t *alldevs = nullptr;
  char errbuf[PCAP_ERRBUF_SIZE] = {};
  if (pcap_findalldevs(&alldevs, errbuf) != 0 || alldevs == nullptr) {
    return ret;
  }
  for (auto *d = alldevs; d != nullptr; d = d->next) {
    if (d->name != nullptr) {
      ret.emplace_back(d->name);
    }
  }
  pcap_freealldevs(alldevs);
  return ret;
}

inline std::vector<std::string> list_wifi_cards() {
  auto ret = list_wireless_interfaces_sysfs();
  if (ret.empty()) {
    ret = list_interfaces_pcap();
  }
  std::sort(ret.begin(), ret.end());
  ret.erase(std::unique(ret.begin(), ret.end()), ret.end());
  return ret;
}

inline std::string format_card_list(const std::vector<std::string> &cards) {
  std::stringstream ss;
  if (cards.empty()) {
    ss << "No wifi cards detected.\n";
    return ss.str();
  }
  ss << "Detected wifi cards:\n";
  for (size_t i = 0; i < cards.size(); i++) {
    ss << "  [" << i << "] " << cards[i] << "\n";
  }
  return ss.str();
}

inline bool is_number(const std::string &value) {
  if (value.empty()) {
    return false;
  }
  for (char c : value) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

inline std::string trim_copy(std::string value) {
  size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start]))) {
    start++;
  }
  size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    end--;
  }
  return value.substr(start, end - start);
}

inline std::string read_line(const std::string &prompt) {
  std::cout << prompt;
  std::cout.flush();
  std::string line;
  std::getline(std::cin, line);
  return trim_copy(line);
}

inline std::string prompt_select_card(
    const std::vector<std::string> &detected_cards) {
  if (!detected_cards.empty()) {
    std::cout << format_card_list(detected_cards);
    while (true) {
      const auto input =
          read_line("Select card index (empty for 0) or name: ");
      if (input.empty()) {
        return detected_cards.front();
      }
      if (is_number(input)) {
        const size_t idx = static_cast<size_t>(std::stoul(input));
        if (idx < detected_cards.size()) {
          return detected_cards[idx];
        }
        std::cout << "Index out of range. Try again.\n";
        continue;
      }
      return input;
    }
  }
  while (true) {
    const auto input = read_line("Enter wifi interface name: ");
    if (!input.empty()) {
      return input;
    }
  }
}

inline int prompt_int(const std::string &prompt, int default_value,
                      bool allow_empty) {
  while (true) {
    const auto input = read_line(prompt);
    if (input.empty() && allow_empty) {
      return default_value;
    }
    if (is_number(input)) {
      return std::stoi(input);
    }
    std::cout << "Invalid number. Try again.\n";
  }
}

inline bool prompt_yes_no(const std::string &prompt, bool default_value) {
  while (true) {
    const auto input = read_line(prompt);
    if (input.empty()) {
      return default_value;
    }
    if (input == "y" || input == "Y" || input == "yes" || input == "YES") {
      return true;
    }
    if (input == "n" || input == "N" || input == "no" || input == "NO") {
      return false;
    }
    std::cout << "Please answer y or n.\n";
  }
}

inline std::string normalize_ht_mode(std::string mode) {
  if (mode.empty()) {
    return {};
  }
  std::string upper;
  upper.reserve(mode.size());
  for (char c : mode) {
    upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  if (upper == "HT+" || upper == "HT40+" || upper == "40+" ||
      upper == "HT40PLUS") {
    return "HT40+";
  }
  if (upper == "HT-" || upper == "HT40-" || upper == "40-" ||
      upper == "HT40MINUS") {
    return "HT40-";
  }
  if (upper == "HT20" || upper == "20" || upper == "HT") {
    return "HT20";
  }
  if (upper == "NOHT") {
    return "NOHT";
  }
  return {};
}

inline int ht_mode_to_bandwidth(const std::string &normalized_ht_mode) {
  if (normalized_ht_mode == "HT40+" || normalized_ht_mode == "HT40-") {
    return 40;
  }
  if (normalized_ht_mode == "HT20" || normalized_ht_mode == "NOHT") {
    return 20;
  }
  return 0;
}

inline bool apply_iw_freq_and_ht(const std::string &iface, int freq_mhz,
                                 const std::string &normalized_ht_mode,
                                 std::string *error_out) {
  if (freq_mhz <= 0) {
    return true;
  }
  std::stringstream cmd;
  cmd << "iw dev " << iface << " set freq " << freq_mhz;
  if (!normalized_ht_mode.empty()) {
    cmd << " " << normalized_ht_mode;
  }
  const auto cmd_str = cmd.str();
  const int ret = std::system(cmd_str.c_str());
  if (ret != 0) {
    if (error_out != nullptr) {
      *error_out = "command failed: " + cmd_str;
    }
    return false;
  }
  return true;
}

}  // namespace wifibroadcast::wifi_card_helper

#endif  // WIFIBROADCAST_WIFI_CARD_HELPER_H
