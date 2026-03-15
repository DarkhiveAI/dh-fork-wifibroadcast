//
// Created by Codex on 14.03.2026.
//

#ifndef WIFIBROADCAST_WIFI_CARD_HELPER_H
#define WIFIBROADCAST_WIFI_CARD_HELPER_H

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
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

// Forward declarations for helpers used below.
inline std::optional<int> read_sysfs_int(const char *path);
inline int channel_to_frequency_maybe(int channel);
struct OpenhdOverridePaths;
inline std::optional<OpenhdOverridePaths> detect_openhd_override_paths();

inline std::optional<std::string> run_command_output(
    const std::string &command) {
#if defined(__linux__)
  std::array<char, 256> buffer{};
  std::string result;
  FILE *pipe = popen(command.c_str(), "r");
  if (!pipe) {
    return std::nullopt;
  }
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) !=
         nullptr) {
    result.append(buffer.data());
  }
  const int rc = pclose(pipe);
  if (rc != 0 && result.empty()) {
    return std::nullopt;
  }
  return result;
#else
  (void)command;
  return std::nullopt;
#endif
}

inline std::optional<int> get_current_frequency_mhz(
    const std::string &iface) {
#if defined(__linux__)
  const auto override_paths = detect_openhd_override_paths();
  if (override_paths.has_value()) {
    const auto channel_opt = read_sysfs_int(override_paths->channel);
    if (channel_opt.has_value() && channel_opt.value() > 0) {
      const int freq = channel_to_frequency_maybe(channel_opt.value());
      if (freq > 0) {
        return freq;
      }
    }
  }
  const auto out_opt = run_command_output("iw dev " + iface + " info");
  if (!out_opt.has_value()) {
    return std::nullopt;
  }
  const auto &out = out_opt.value();
  std::istringstream iss(out);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.find("channel ") == std::string::npos) {
      continue;
    }
    auto left = line.find('(');
    auto right = line.find(" MHz");
    if (left == std::string::npos || right == std::string::npos ||
        right <= left + 1) {
      continue;
    }
    const std::string number = trim_copy(line.substr(left + 1, right - left - 1));
    if (is_number(number)) {
      return std::stoi(number);
    }
  }
  return std::nullopt;
#else
  (void)iface;
  return std::nullopt;
#endif
}

inline std::optional<std::string> get_device_mode(
    const std::string &iface) {
#if defined(__linux__)
  const auto out_opt = run_command_output("iw dev " + iface + " info");
  if (!out_opt.has_value()) {
    return std::nullopt;
  }
  std::istringstream iss(out_opt.value());
  std::string line;
  while (std::getline(iss, line)) {
    const auto pos = line.find("type ");
    if (pos == std::string::npos) {
      continue;
    }
    std::string mode = trim_copy(line.substr(pos + 5));
    if (!mode.empty()) {
      return mode;
    }
  }
  return std::nullopt;
#else
  (void)iface;
  return std::nullopt;
#endif
}

inline bool run_command_simple(const std::string &command) {
  const int ret = std::system(command.c_str());
  return ret == 0;
}

inline bool ensure_monitor_mode(const std::string &iface,
                                std::string *error_out) {
#if defined(__linux__)
  const auto mode_opt = get_device_mode(iface);
  if (mode_opt.has_value() && mode_opt.value() == "monitor") {
    return true;
  }
  if (!run_command_simple("ip link set dev " + iface + " down")) {
    if (error_out) {
      *error_out = "failed to bring interface down";
    }
    return false;
  }
  if (!run_command_simple("iw dev " + iface + " set monitor otherbss")) {
    if (error_out) {
      *error_out = "failed to set monitor mode";
    }
    return false;
  }
  if (!run_command_simple("ip link set dev " + iface + " up")) {
    if (error_out) {
      *error_out = "failed to bring interface up";
    }
    return false;
  }
  const auto updated_mode = get_device_mode(iface);
  if (updated_mode.has_value() && updated_mode.value() == "monitor") {
    return true;
  }
  if (error_out) {
    *error_out = "device not in monitor mode after update";
  }
  return false;
#else
  (void)iface;
  if (error_out) {
    *error_out = "monitor mode not supported on this platform";
  }
  return false;
#endif
}

inline std::string prompt_select_card(
    const std::vector<std::string> &detected_cards) {
  if (detected_cards.size() == 1) {
    std::cout << "Using only detected card: " << detected_cards.front()
              << "\n";
    return detected_cards.front();
  }
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

inline int frequency_to_channel_maybe(uint32_t freq_mhz) {
  if (freq_mhz == 2484) {
    return 14;
  }
  if (freq_mhz >= 2412 && freq_mhz <= 2472 &&
      ((freq_mhz - 2412) % 5 == 0)) {
    return 1 + static_cast<int>((freq_mhz - 2412) / 5);
  }
  if (freq_mhz >= 5000 && ((freq_mhz - 5000) % 5 == 0)) {
    return static_cast<int>((freq_mhz - 5000) / 5);
  }
  return -1;
}

inline int channel_to_frequency_maybe(int channel) {
  if (channel == 14) {
    return 2484;
  }
  if (channel >= 1 && channel <= 13) {
    return 2407 + (channel * 5);
  }
  if (channel > 0) {
    return 5000 + (channel * 5);
  }
  return -1;
}

inline std::optional<int> read_sysfs_int(const char *path) {
  if (path == nullptr) {
    return std::nullopt;
  }
  std::ifstream ifs(path);
  if (!ifs.is_open()) {
    return std::nullopt;
  }
  int value = 0;
  ifs >> value;
  return ifs.fail() ? std::nullopt : std::optional<int>(value);
}

struct OpenhdOverridePaths {
  const char *channel = nullptr;
  const char *channel_width = nullptr;
  const char *tx_power = nullptr;
  const char *force_bw80_for_bw40 = nullptr;
};

inline std::optional<OpenhdOverridePaths> detect_openhd_override_paths() {
#if defined(__linux__)
  const OpenhdOverridePaths candidates[] = {
      {"/sys/module/88XXau_ohd/parameters/openhd_override_channel",
       "/sys/module/88XXau_ohd/parameters/openhd_override_channel_width",
       "/sys/module/88XXau_ohd/parameters/openhd_override_tx_power_index",
       nullptr},
      {"/sys/module/88x2bu_ohd/parameters/openhd_override_channel",
       "/sys/module/88x2bu_ohd/parameters/openhd_override_channel_width",
       "/sys/module/88x2bu_ohd/parameters/openhd_override_tx_power_mbm",
       nullptr},
      {"/sys/module/88x2cu_ohd/parameters/openhd_override_channel",
       "/sys/module/88x2cu_ohd/parameters/openhd_override_channel_width",
       "/sys/module/88x2cu_ohd/parameters/openhd_override_tx_power_mbm",
       nullptr},
      {"/sys/module/88x2eu_ohd/parameters/openhd_override_channel",
       "/sys/module/88x2eu_ohd/parameters/openhd_override_channel_width",
       "/sys/module/88x2eu_ohd/parameters/openhd_override_tx_power_mbm",
       "/sys/module/88x2eu_ohd/parameters/rtw_force_tx_rf_bw_80_for_bw40"},
  };
  for (const auto &entry : candidates) {
    std::error_code ec;
    if (entry.channel && std::filesystem::exists(entry.channel, ec)) {
      return entry;
    }
  }
#endif
  return std::nullopt;
}

inline bool write_sysfs_value(const char *path, const std::string &value) {
  if (path == nullptr) {
    return false;
  }
  std::ofstream ofs(path);
  if (!ofs.is_open()) {
    return false;
  }
  ofs << value;
  return true;
}

inline std::string default_ht_mode_for_bw(int channel_width_mhz) {
  if (channel_width_mhz == 40) {
    return "HT40+";
  }
  return "HT20";
}

inline bool apply_openhd_override_frequency(const std::string &iface,
                                            uint32_t freq_mhz,
                                            int channel_width_mhz,
                                            std::string ht_mode,
                                            std::string *error_out) {
#if defined(__linux__)
  const auto paths_opt = detect_openhd_override_paths();
  if (!paths_opt.has_value()) {
    return false;
  }
  const auto paths = paths_opt.value();
  const int channel = frequency_to_channel_maybe(freq_mhz);
  if (channel <= 0) {
    if (error_out) {
      *error_out = "Unsupported frequency for override.";
    }
    return false;
  }
  if (ht_mode.empty()) {
    ht_mode = default_ht_mode_for_bw(channel_width_mhz);
  }
  const int width_override =
      (channel_width_mhz == 40) ? 1
      : (channel_width_mhz == 10) ? 6
      : (channel_width_mhz == 5)  ? 5
      : (channel_width_mhz == 80) ? 2
                                  : 0;

  if (!write_sysfs_value(paths.channel, std::to_string(channel))) {
    if (error_out) {
      *error_out = "Failed to write override channel.";
    }
    return false;
  }
  if (paths.channel_width != nullptr) {
    write_sysfs_value(paths.channel_width, std::to_string(width_override));
  }
  if (paths.force_bw80_for_bw40 != nullptr) {
    const int force_val = (channel_width_mhz == 40) ? 1 : 0;
    write_sysfs_value(paths.force_bw80_for_bw40, std::to_string(force_val));
  }

  const bool is_2g = freq_mhz < 3000;
  uint32_t dummy_freq = is_2g ? 2412 : 5180;
  if (channel_width_mhz == 40) {
    if (ht_mode == "HT40-") {
      dummy_freq = is_2g ? 2432 : 5200;
    } else {
      dummy_freq = is_2g ? 2412 : 5180;
    }
  }
  std::stringstream cmd;
  cmd << "iw dev " << iface << " set freq " << dummy_freq << " " << ht_mode;
  const int ret = std::system(cmd.str().c_str());
  if (ret != 0) {
    if (error_out) {
      *error_out = "iw command failed after override.";
    }
    return false;
  }
  return true;
#else
  (void)iface;
  (void)freq_mhz;
  (void)channel_width_mhz;
  (void)ht_mode;
  if (error_out) {
    *error_out = "OpenHD override not supported on this platform.";
  }
  return false;
#endif
}

inline bool apply_iw_freq_and_ht(const std::string &iface, int freq_mhz,
                                 const std::string &normalized_ht_mode,
                                 std::string *error_out) {
  if (freq_mhz <= 0) {
    return true;
  }
#if defined(__linux__)
  {
    std::string monitor_err;
    if (!ensure_monitor_mode(iface, &monitor_err)) {
      if (error_out) {
        *error_out = "monitor mode required: " + monitor_err;
      }
      return false;
    }
  }
  const int bw =
      ht_mode_to_bandwidth(normalized_ht_mode.empty() ? "HT20"
                                                      : normalized_ht_mode);
  if (apply_openhd_override_frequency(iface, static_cast<uint32_t>(freq_mhz),
                                      bw, normalized_ht_mode, error_out)) {
    return true;
  }
#endif
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
