//
// Created by consti10 on 25.07.23.
//

#include <algorithm>
#include <cstdlib>
#include <thread>

#include <ncurses.h>

#include "../src/WBStreamRx.h"
#include "../src/WBStreamTx.h"
#include "../src/WBTxRx.h"
#include "../src/HelperSources/WifiCardHelper.hpp"
#include "../src/wifibroadcast_spdlog.h"
#include "DummyStreamGenerator.hpp"
#include "RandomBufferPot.hpp"
#include "Rates.hpp"

// Utility / benchmark executable to find the maximum injection rate possible
// for the card given a MCS index
// It works by increasing the injection rate (injected bitrate / packets per
// second) until there are so called "TX ERRORS", aka the driver tx packet queue
// is running full

// static constexpr auto TEST_PACKETS_SIZE=1024;
//  Video in openhd is fragmented into packets of this size - and in general, is
//  by far the biggest bitrate producer Therefore, we use this packet size
//  during testing - Note that a smaller packet size reduces bitrate due to more
//  overhead.
static constexpr auto TEST_PACKETS_SIZE = 1440;

struct TestResult {
  int mcs_index;
  int pass_pps_set;
  int pass_pps_measured;
  int pass_bps_measured;
  int fail_pps_set;
  int fail_pps_measured;
  int fail_bps_measured;
};

static int clamp_payload_size(int size_bytes) {
  if (size_bytes < 1) {
    return 1;
  }
  if (size_bytes > WBTxRx::MAX_PACKET_PAYLOAD_SIZE) {
    return WBTxRx::MAX_PACKET_PAYLOAD_SIZE;
  }
  return size_bytes;
}

static int calculate_target_pps(double rate_mbit, int payload_size) {
  if (rate_mbit <= 0.0) {
    return 1;
  }
  if (payload_size < 1) {
    payload_size = 1;
  }
  const double rate_bps = rate_mbit * 1000.0 * 1000.0;
  const int pps =
      static_cast<int>(rate_bps / (static_cast<double>(payload_size) * 8.0));
  return std::max(1, pps);
}

struct UiLayout {
  WINDOW *header = nullptr;
  WINDOW *settings = nullptr;
  WINDOW *stats = nullptr;
  WINDOW *help = nullptr;
  WINDOW *footer = nullptr;
  int rows = 0;
  int cols = 0;
};

static void destroy_layout(UiLayout &layout) {
  if (layout.header) delwin(layout.header);
  if (layout.settings) delwin(layout.settings);
  if (layout.stats) delwin(layout.stats);
  if (layout.help) delwin(layout.help);
  if (layout.footer) delwin(layout.footer);
  layout = UiLayout{};
}

static UiLayout create_layout() {
  UiLayout layout{};
  getmaxyx(stdscr, layout.rows, layout.cols);
  const int header_h = 3;
  const int footer_h = 2;
  const int help_h = 7;
  const int body_h = std::max(4, layout.rows - header_h - footer_h - help_h);
  const int half_w = std::max(20, layout.cols / 2);
  layout.header = newwin(header_h, layout.cols, 0, 0);
  layout.settings = newwin(body_h, half_w, header_h, 0);
  layout.stats = newwin(body_h, layout.cols - half_w, header_h, half_w);
  layout.help = newwin(help_h, layout.cols, header_h + body_h, 0);
  layout.footer = newwin(footer_h, layout.cols,
                         header_h + body_h + help_h, 0);
  return layout;
}

static void init_colors() {
  if (!has_colors()) {
    return;
  }
  start_color();
  use_default_colors();
  init_pair(1, COLOR_CYAN, -1);
  init_pair(2, COLOR_GREEN, -1);
  init_pair(3, COLOR_YELLOW, -1);
  init_pair(4, COLOR_RED, -1);
  init_pair(5, COLOR_MAGENTA, -1);
}

static void draw_bar(WINDOW *win, int y, int x, int width, double value,
                     double max_value, int color_pair) {
  if (width <= 0) {
    return;
  }
  const double ratio =
      (max_value <= 0.0) ? 0.0 : std::min(1.0, value / max_value);
  const int filled = static_cast<int>(ratio * width);
  wattron(win, COLOR_PAIR(color_pair));
  for (int i = 0; i < width; i++) {
    mvwaddch(win, y, x + i, (i < filled) ? '#' : '-');
  }
  wattroff(win, COLOR_PAIR(color_pair));
}

static int select_band_ui() {
  nodelay(stdscr, false);
  keypad(stdscr, true);
  int rows = 0;
  int cols = 0;
  getmaxyx(stdscr, rows, cols);
  const std::string title = "Select Band (Enter to choose, q to cancel)";
  const std::vector<std::string> bands = {"2.4 GHz", "5.8 GHz"};
  int idx = 0;
  while (true) {
    erase();
    mvprintw(0, 0, "%s", title.c_str());
    for (int i = 0; i < static_cast<int>(bands.size()); i++) {
      if (i == idx) {
        attron(A_REVERSE);
      }
      mvprintw(2 + i, 2, "%s", bands[i].c_str());
      if (i == idx) {
        attroff(A_REVERSE);
      }
    }
    refresh();
    const int ch = getch();
    if (ch == 'q' || ch == 27) {
      nodelay(stdscr, true);
      return -1;
    }
    if (ch == KEY_UP) {
      idx = (idx == 0) ? static_cast<int>(bands.size() - 1) : idx - 1;
    } else if (ch == KEY_DOWN) {
      idx = (idx + 1) % static_cast<int>(bands.size());
    } else if (ch == '\n' || ch == KEY_ENTER) {
      nodelay(stdscr, true);
      return idx;
    }
  }
}

static int select_frequency_ui(const std::vector<int> &frequencies,
                               int current_value) {
  if (frequencies.empty()) {
    return -1;
  }
  nodelay(stdscr, false);
  keypad(stdscr, true);
  int rows = 0;
  int cols = 0;
  getmaxyx(stdscr, rows, cols);
  int idx = 0;
  for (int i = 0; i < static_cast<int>(frequencies.size()); i++) {
    if (frequencies[i] == current_value) {
      idx = i;
      break;
    }
  }
  while (true) {
    erase();
    mvprintw(0, 0, "Select Frequency (Enter to choose, q to cancel)");
    const int list_start_y = 2;
    const int list_height = std::max(4, rows - list_start_y - 2);
    const int max_start =
        std::max(0, static_cast<int>(frequencies.size()) - list_height);
    int start = idx - list_height / 2;
    if (start < 0) start = 0;
    if (start > max_start) start = max_start;
    for (int i = 0; i < list_height; i++) {
      const int item_index = start + i;
      if (item_index >= static_cast<int>(frequencies.size())) {
        break;
      }
      if (item_index == idx) {
        attron(A_REVERSE);
      }
      mvprintw(list_start_y + i, 2, "%d MHz", frequencies[item_index]);
      if (item_index == idx) {
        attroff(A_REVERSE);
      }
    }
    refresh();
    const int ch = getch();
    if (ch == 'q' || ch == 27) {
      nodelay(stdscr, true);
      return -1;
    }
    if (ch == KEY_UP) {
      idx = (idx == 0) ? static_cast<int>(frequencies.size() - 1) : idx - 1;
    } else if (ch == KEY_DOWN) {
      idx = (idx + 1) % static_cast<int>(frequencies.size());
    } else if (ch == KEY_NPAGE) {
      idx = std::min(static_cast<int>(frequencies.size() - 1),
                     idx + list_height);
    } else if (ch == KEY_PPAGE) {
      idx = std::max(0, idx - list_height);
    } else if (ch == '\n' || ch == KEY_ENTER) {
      nodelay(stdscr, true);
      return frequencies[idx];
    }
  }
}

static std::vector<int> get_freqs_2g() {
  return {2412, 2417, 2422, 2427, 2432, 2437, 2442,
          2447, 2452, 2457, 2462, 2467, 2472, 2484};
}

static std::vector<int> get_freqs_5g() {
  return {5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320,
          5500, 5520, 5540, 5560, 5580, 5600, 5620, 5640,
          5660, 5680, 5700, 5720, 5745, 5765, 5785, 5805,
          5825, 5845, 5865};
}

static std::string suggest_ht40_mode(int freq_mhz) {
  const int channel =
      wifibroadcast::wifi_card_helper::frequency_to_channel_maybe(freq_mhz);
  if (channel <= 0) {
    return "HT40+";
  }
  if (freq_mhz < 3000) {
    return (channel <= 7) ? "HT40+" : "HT40-";
  }
  return (channel % 8 == 4) ? "HT40+" : "HT40-";
}

static std::string ui_prompt_line(UiLayout &layout, const std::string &prompt) {
  nodelay(stdscr, false);
  werase(layout.footer);
  box(layout.footer, 0, 0);
  mvwprintw(layout.footer, 0, 2, "Input");
  mvwprintw(layout.footer, 1, 2, "%s", prompt.c_str());
  wrefresh(layout.footer);
  echo();
  curs_set(1);
  char buf[128] = {};
  wgetnstr(layout.footer, buf, sizeof(buf) - 1);
  noecho();
  curs_set(0);
  nodelay(stdscr, true);
  return wifibroadcast::wifi_card_helper::trim_copy(buf);
}

static void render_ui(const UiLayout &layout, const std::string &card,
                      int target_freq_mhz, int current_freq_mhz,
                      const std::string &device_mode,
                      const std::string &override_info,
                      const std::string &ht_mode, int bandwidth, int mcs,
                      int payload_size, double target_rate_mbit, int target_pps,
                      const WBTxRx::TxStats &txstats, int cannot_keep_up,
                      bool injecting, const std::string &status_line) {
  if (layout.rows < 20 || layout.cols < 60) {
    erase();
    mvprintw(0, 0, "Terminal too small for UI.");
    refresh();
    return;
  }

  werase(layout.header);
  werase(layout.settings);
  werase(layout.stats);
  werase(layout.help);
  werase(layout.footer);

  box(layout.header, 0, 0);
  wattron(layout.header, COLOR_PAIR(1));
  mvwprintw(layout.header, 1, 2,
            "WiFi Injection Monitor  (q to quit)");
  wattroff(layout.header, COLOR_PAIR(1));

  box(layout.settings, 0, 0);
  mvwprintw(layout.settings, 0, 2, "Settings");
  mvwprintw(layout.settings, 1, 2, "Card: %s", card.c_str());
  mvwprintw(layout.settings, 2, 2, "Mode: %s",
            device_mode.empty() ? "-" : device_mode.c_str());
  mvwprintw(layout.settings, 3, 2, "Override: %s",
            override_info.empty() ? "-" : override_info.c_str());
  if (current_freq_mhz > 0) {
    if (target_freq_mhz > 0) {
      mvwprintw(layout.settings, 4, 2, "Freq: %d MHz (target %d)",
                current_freq_mhz, target_freq_mhz);
    } else {
      mvwprintw(layout.settings, 4, 2, "Freq: %d MHz", current_freq_mhz);
    }
  } else if (target_freq_mhz > 0) {
    mvwprintw(layout.settings, 4, 2, "Freq: %d MHz", target_freq_mhz);
  } else {
    mvwprintw(layout.settings, 4, 2, "Freq: -");
  }
  mvwprintw(layout.settings, 5, 2, "HT: %s",
            ht_mode.empty() ? "-" : ht_mode.c_str());
  mvwprintw(layout.settings, 6, 2, "BW: %d MHz", bandwidth);
  mvwprintw(layout.settings, 7, 2, "MCS: %d", mcs);
  mvwprintw(layout.settings, 8, 2, "Payload: %d bytes", payload_size);
  mvwprintw(layout.settings, 9, 2, "Target: %.2f Mbit/s", target_rate_mbit);
  mvwprintw(layout.settings, 10, 2, "Target PPS: %d", target_pps);

  box(layout.stats, 0, 0);
  mvwprintw(layout.stats, 0, 2, "Live Stats");
  mvwprintw(layout.stats, 1, 2, "Injecting: %s", injecting ? "YES" : "NO");
  mvwprintw(layout.stats, 2, 2, "TX PPS: %d",
            txstats.curr_packets_per_second);
  mvwprintw(layout.stats, 3, 2, "TX bitrate: %s",
            StringHelper::bitrate_readable(
                txstats.curr_bits_per_second_excluding_overhead)
                .c_str());
  mvwprintw(layout.stats, 4, 2, "TX errors: %d",
            txstats.count_tx_injections_error_hint);
  mvwprintw(layout.stats, 5, 2, "Cannot keep up: %d", cannot_keep_up);

  const double target_bps = target_rate_mbit * 1000.0 * 1000.0;
  const double curr_bps =
      static_cast<double>(txstats.curr_bits_per_second_excluding_overhead);
  const int bar_width = std::max(10, layout.cols / 3);
  mvwprintw(layout.stats, 7, 2, "Rate:");
  draw_bar(layout.stats, 7, 8, bar_width, curr_bps, target_bps, 2);
  mvwprintw(layout.stats, 8, 2, "PPS:");
  draw_bar(layout.stats, 8, 8, bar_width,
           static_cast<double>(txstats.curr_packets_per_second),
           static_cast<double>(target_pps), 5);

  box(layout.help, 0, 0);
  mvwprintw(layout.help, 0, 2, "Controls");
  mvwprintw(layout.help, 1, 2, "m/M: MCS -/+   b: toggle BW 20/40");
  mvwprintw(layout.help, 2, 2, "f: set frequency   h: set HT mode");
  mvwprintw(layout.help, 3, 2, "r: set target rate (Mbit/s)");
  mvwprintw(layout.help, 4, 2, "p: set payload size (bytes)");
  mvwprintw(layout.help, 5, 2, "s: start/stop injection");

  box(layout.footer, 0, 0);
  mvwprintw(layout.footer, 0, 2, "Status");
  if (!status_line.empty()) {
    mvwprintw(layout.footer, 1, 2, "%s", status_line.c_str());
  } else {
    mvwprintw(layout.footer, 1, 2, "OK");
  }

  wrefresh(layout.header);
  wrefresh(layout.settings);
  wrefresh(layout.stats);
  wrefresh(layout.help);
  wrefresh(layout.footer);
}

static int run_ncurses_ui(const std::string &card, int initial_freq_mhz,
                          const std::string &initial_ht_mode,
                          std::shared_ptr<WBTxRx> txrx,
                          std::shared_ptr<RadiotapHeaderTxHolder> hdr,
                          int initial_payload_size, int initial_mcs,
                          double initial_rate_mbit) {
  int freq_mhz = initial_freq_mhz;
  std::string ht_mode = initial_ht_mode;
  int bandwidth = wifibroadcast::wifi_card_helper::ht_mode_to_bandwidth(
      ht_mode.empty() ? "HT20" : ht_mode);
  if (bandwidth <= 0) {
    bandwidth = 20;
  }
  int mcs = std::max(0, initial_mcs);
  int payload_size = clamp_payload_size(initial_payload_size);
  double target_rate_mbit = initial_rate_mbit > 0.0 ? initial_rate_mbit : 10.0;
  int target_pps = calculate_target_pps(target_rate_mbit, payload_size);
  bool injecting = true;
  std::string status_line;

  hdr->update_channel_width(bandwidth);
  hdr->update_mcs_index(mcs);
  {
    std::string err;
    if (!wifibroadcast::wifi_card_helper::ensure_monitor_mode(card, &err)) {
      status_line = "Monitor mode required: " + err;
      injecting = false;
    }
  }

  auto tx_cb = [&txrx, &hdr](const uint8_t *data, int data_len) {
    const auto radiotap_header = hdr->thread_safe_get();
    const bool encrypt = false;
    txrx->tx_inject_packet(10, data, data_len, radiotap_header, encrypt);
  };

  auto stream_generator =
      std::make_unique<DummyStreamGenerator>(tx_cb, payload_size);
  stream_generator->set_target_pps(target_pps);
  if (injecting) {
    stream_generator->start();
  }

  initscr();
  cbreak();
  noecho();
  keypad(stdscr, true);
  nodelay(stdscr, true);
  curs_set(0);
  init_colors();

  UiLayout layout = create_layout();

  bool running = true;
  int current_freq_mhz = -1;
  std::string device_mode;
  std::string override_info;
  bool rx_running = true;
  auto last_freq_poll = std::chrono::steady_clock::now();
  while (running) {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    if (rows != layout.rows || cols != layout.cols) {
      destroy_layout(layout);
      layout = create_layout();
    }
    int ch = getch();
    if (ch != ERR) {
      status_line.clear();
      switch (ch) {
        case 'q':
          running = false;
          break;
        case 'm':
          if (mcs > 0) {
            mcs--;
            hdr->update_mcs_index(mcs);
          }
          break;
        case 'M':
          if (mcs < 31) {
            mcs++;
            hdr->update_mcs_index(mcs);
          }
          break;
        case 'b':
          bandwidth = (bandwidth == 20) ? 40 : 20;
          hdr->update_channel_width(bandwidth);
          if (bandwidth == 20) {
            ht_mode = "HT20";
          } else {
            const int freq_for_hint =
                (freq_mhz > 0) ? freq_mhz : current_freq_mhz;
            ht_mode = suggest_ht40_mode(freq_for_hint);
          }
          break;
        case 'f': {
          if (injecting) {
            stream_generator->stop();
          }
          if (rx_running) {
            txrx->stop_receiving();
            rx_running = false;
          }
          const int band = select_band_ui();
          if (band >= 0) {
            const auto freqs = (band == 0) ? get_freqs_2g() : get_freqs_5g();
            const int selected =
                select_frequency_ui(freqs, freq_mhz > 0 ? freq_mhz
                                                        : current_freq_mhz);
            if (selected > 0) {
              freq_mhz = selected;
              if (bandwidth == 40) {
                ht_mode = suggest_ht40_mode(freq_mhz);
                hdr->update_channel_width(bandwidth);
              }
              std::string err;
              if (!wifibroadcast::wifi_card_helper::apply_iw_freq_and_ht(
                      card, freq_mhz, ht_mode, &err)) {
                status_line = "Failed to set frequency: " + err;
              }
            }
          }
          if (!rx_running) {
            txrx->start_receiving();
            rx_running = true;
          }
          if (injecting) {
            stream_generator->start();
          }
          break;
        }
        case 'h': {
          if (injecting) {
            stream_generator->stop();
          }
          if (rx_running) {
            txrx->stop_receiving();
            rx_running = false;
          }
          const auto input =
              ui_prompt_line(layout, "HT mode (HT20/HT40+/HT40-): ");
          if (!input.empty()) {
            const auto normalized =
                wifibroadcast::wifi_card_helper::normalize_ht_mode(input);
            if (normalized.empty()) {
              status_line = "Invalid HT mode.";
            } else {
              ht_mode = normalized;
              const int bw =
                  wifibroadcast::wifi_card_helper::ht_mode_to_bandwidth(
                      ht_mode);
              if (bw > 0) {
                bandwidth = bw;
                hdr->update_channel_width(bandwidth);
              }
              if (freq_mhz > 0) {
                std::string err;
                if (!wifibroadcast::wifi_card_helper::apply_iw_freq_and_ht(
                        card, freq_mhz, ht_mode, &err)) {
                  status_line = "Failed to set HT mode: " + err;
                }
              }
            }
          }
          if (!rx_running) {
            txrx->start_receiving();
            rx_running = true;
          }
          if (injecting) {
            stream_generator->start();
          }
          break;
        }
        case 'r': {
          const auto input = ui_prompt_line(layout, "Target rate Mbit/s: ");
          if (!input.empty()) {
            const double new_rate = strtod(input.c_str(), nullptr);
            if (new_rate > 0.0) {
              target_rate_mbit = new_rate;
              target_pps =
                  calculate_target_pps(target_rate_mbit, payload_size);
              stream_generator->set_target_pps(target_pps);
            } else {
              status_line = "Invalid rate.";
            }
          }
          break;
        }
        case 'p': {
          const auto input = ui_prompt_line(layout, "Payload size bytes: ");
          if (!input.empty()) {
            const int new_size = clamp_payload_size(atoi(input.c_str()));
            if (new_size > 0) {
              payload_size = new_size;
              stream_generator->set_packet_size(payload_size);
              target_pps =
                  calculate_target_pps(target_rate_mbit, payload_size);
              stream_generator->set_target_pps(target_pps);
            } else {
              status_line = "Invalid payload size.";
            }
          }
          break;
        }
        case 's':
          injecting = !injecting;
          if (injecting) {
            std::string err;
            if (!wifibroadcast::wifi_card_helper::ensure_monitor_mode(
                    card, &err)) {
              injecting = false;
              status_line = "Monitor mode required: " + err;
            } else {
              stream_generator->start();
            }
          } else {
            stream_generator->stop();
          }
          break;
        default:
          break;
      }
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - last_freq_poll > std::chrono::seconds(1)) {
      const auto override_paths =
          wifibroadcast::wifi_card_helper::detect_openhd_override_paths();
      if (override_paths.has_value()) {
        const auto ch_opt =
            wifibroadcast::wifi_card_helper::read_sysfs_int(
                override_paths->channel);
        const auto bw_opt =
            wifibroadcast::wifi_card_helper::read_sysfs_int(
                override_paths->channel_width);
        if (ch_opt.has_value() && ch_opt.value() > 0) {
          if (bw_opt.has_value()) {
            override_info =
                "ch " + std::to_string(ch_opt.value()) +
                " w " + std::to_string(bw_opt.value());
          } else {
            override_info = "ch " + std::to_string(ch_opt.value());
          }
        } else {
          override_info.clear();
        }
      } else {
        override_info.clear();
      }
      const auto freq_opt =
          wifibroadcast::wifi_card_helper::get_current_frequency_mhz(card);
      if (freq_opt.has_value()) {
        current_freq_mhz = freq_opt.value();
      }
      const auto mode_opt =
          wifibroadcast::wifi_card_helper::get_device_mode(card);
      if (mode_opt.has_value()) {
        device_mode = mode_opt.value();
      }
      last_freq_poll = now;
    }

    const auto txstats = txrx->get_tx_stats();
    render_ui(layout, card, freq_mhz, current_freq_mhz, device_mode,
              override_info, ht_mode, bandwidth, mcs, payload_size,
              target_rate_mbit, target_pps, txstats,
              stream_generator->n_times_cannot_keep_up_wanted_pps, injecting,
              status_line);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }

  stream_generator->stop();
  destroy_layout(layout);
  endwin();
  return 0;
}

static TestResult increase_pps_until_fail(
    std::shared_ptr<WBTxRx> txrx, std::shared_ptr<RadiotapHeaderTxHolder> hdr,
    const int mcs, const int pps_start, const int pps_increment) {
  auto m_console = wifibroadcast::log::create_or_get("main");
  m_console->info("Testing MCS {}", mcs);
  hdr->update_mcs_index(mcs);

  int last_passed_pps_set = 0;
  int last_passed_pps_measured = 0;
  int last_passed_bps_measured = 0;
  // 7*1000 packets per second are a lot (way over 50MBit/s) but we can reach it
  // in ideal scenarios have a limit here though to not run infinitely
  for (int pps = pps_start; pps < 7 * 1000; pps += pps_increment) {
    auto tx_cb = [&txrx, &hdr](const uint8_t* data, int data_len) {
      const auto radiotap_header = hdr->thread_safe_get();
      const bool encrypt = false;
      txrx->tx_inject_packet(10, data, data_len, radiotap_header, encrypt);
    };
    auto stream_generator =
        std::make_unique<DummyStreamGenerator>(tx_cb, TEST_PACKETS_SIZE);
    stream_generator->set_target_pps(pps);
    std::this_thread::sleep_for(
        std::chrono::seconds(1));  // give driver time to empty queue
    txrx->tx_reset_stats();
    stream_generator->start();
    m_console->info("Testing MCS {} with {} pps", mcs, pps);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    const auto txstats = txrx->get_tx_stats();
    if (txstats.count_tx_injections_error_hint > 0 ||
        stream_generator->n_times_cannot_keep_up_wanted_pps > 10) {
      m_console->info("TX errors {} n_times_cannot_keep_up_wanted_pps {}",
                      txstats.count_tx_injections_error_hint,
                      stream_generator->n_times_cannot_keep_up_wanted_pps);
      // TX errors
      m_console->info("Got TX errors at set:{} actual: {} pps {} pps", pps,
                      txstats.curr_packets_per_second,
                      txstats.curr_packets_per_second);
      TestResult result{};
      result.mcs_index = mcs;
      result.pass_pps_set = last_passed_pps_set;
      result.pass_pps_measured = last_passed_pps_measured;
      result.pass_bps_measured = last_passed_bps_measured;
      result.fail_pps_set = pps;
      result.fail_pps_measured = txstats.curr_packets_per_second;
      result.fail_bps_measured =
          txstats.curr_bits_per_second_excluding_overhead;
      return result;
    } else {
      m_console->info("MCS {} passed {} - measured {} {}", mcs, pps,
                      txstats.curr_packets_per_second,
                      StringHelper::bitrate_readable(
                          txstats.curr_bits_per_second_excluding_overhead));
      m_console->info("{}", WBTxRx::tx_stats_to_string(txstats));
      last_passed_pps_set = pps;
      last_passed_pps_measured = txstats.curr_packets_per_second;
      last_passed_bps_measured =
          txstats.curr_bits_per_second_excluding_overhead;
    }
  }
  // assert(false);
  return {mcs, 0, 0, 0, 0, 0};
}

static void calculate_max_possible_pps_quick(
    std::shared_ptr<WBTxRx> txrx, std::shared_ptr<RadiotapHeaderTxHolder> hdr,
    const int mcs) {
  auto m_console = wifibroadcast::log::create_or_get("main");
  m_console->info("Testing MCS {}", mcs);
  hdr->update_mcs_index(mcs);
  auto tx_cb = [&txrx, &hdr](const uint8_t* data, int data_len) {
    const auto radiotap_header = hdr->thread_safe_get();
    const bool encrypt = false;
    txrx->tx_inject_packet(10, data, data_len, radiotap_header, encrypt);
  };
  auto stream_generator =
      std::make_unique<DummyStreamGenerator>(tx_cb, TEST_PACKETS_SIZE);
  stream_generator->set_target_pps(10 * 1000);
  std::this_thread::sleep_for(
      std::chrono::seconds(1));  // give driver time to empty queue
  txrx->tx_reset_stats();
  stream_generator->start();
  std::this_thread::sleep_for(std::chrono::seconds(4));
  auto stats = txrx->get_tx_stats();
  m_console->info("MCS {} max {} {}", mcs, stats.curr_packets_per_second,
                  StringHelper::bitrate_readable(
                      stats.curr_bits_per_second_excluding_overhead));
}

static std::string validate_specific_rate(
    std::shared_ptr<WBTxRx> txrx, std::shared_ptr<RadiotapHeaderTxHolder> hdr,
    const int mcs, const int rate_kbits, int duration_seconds = 10) {
  auto m_console = wifibroadcast::log::create_or_get("main");
  const auto rate_bps =
      (rate_kbits * 1000) + 10;  // add a bit more to actually hit the target
  const auto pps = rate_bps / (TEST_PACKETS_SIZE * 8);
  m_console->info("Validating {} - {}", mcs, pps);
  hdr->update_mcs_index(mcs);
  auto tx_cb = [&txrx, &hdr](const uint8_t* data, int data_len) {
    const auto radiotap_header = hdr->thread_safe_get();
    const bool encrypt = false;
    txrx->tx_inject_packet(10, data, data_len, radiotap_header, encrypt);
  };
  auto stream_generator =
      std::make_unique<DummyStreamGenerator>(tx_cb, TEST_PACKETS_SIZE);
  stream_generator->set_target_pps(pps);
  std::this_thread::sleep_for(
      std::chrono::seconds(1));  // give driver time to empty queue
  txrx->tx_reset_stats();
  stream_generator->start();
  if (duration_seconds < 1) {
    duration_seconds = 1;
  }
  std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
  const auto txstats = txrx->get_tx_stats();
  std::stringstream ss;
  if (txstats.count_tx_injections_error_hint > 0 ||
      stream_generator->n_times_cannot_keep_up_wanted_pps > 10) {
    ss << fmt::format("MCS {} didn't pass {}/{} measured {}-{}\n", mcs, pps,
                      StringHelper::bitrate_readable(rate_bps),
                      txstats.curr_packets_per_second,
                      StringHelper::bitrate_readable(
                          txstats.curr_bits_per_second_excluding_overhead));
    ss << fmt::format("{}", WBTxRx::tx_stats_to_string(txstats));
  } else {
    ss << fmt::format("MCS {} passed {}/{} measured {}-{}\n", mcs, pps,
                      StringHelper::bitrate_readable(rate_bps),
                      txstats.curr_packets_per_second,
                      StringHelper::bitrate_readable(
                          txstats.curr_bits_per_second_excluding_overhead));
    // ss<<fmt::format("{}",WBTxRx::tx_stats_to_string(txstats));
  }
  m_console->info(ss.str());
  return ss.str();
}
static void validate_rtl8812au_rates(
    std::shared_ptr<WBTxRx> txrx, std::shared_ptr<RadiotapHeaderTxHolder> hdr,
    const bool is_40mhz) {
  hdr->update_channel_width(is_40mhz ? 40 : 20);
  std::stringstream log;
  for (int mcs = 0; mcs < 12; mcs++) {
    const auto rate = wifibroadcast::get_practical_rate_5G(mcs);
    const auto rate_kbits =
        (is_40mhz ? rate.rate_40mhz_kbits : rate.rate_20mhz_kbits);
    const auto res = validate_specific_rate(txrx, hdr, mcs, rate_kbits);
    log << res << "\n";
  }
  wifibroadcast::log::get_default()->debug("\n{}", log.str());
}

static void print_test_results_rough(
    const std::vector<TestResult>& test_results) {
  auto m_console = wifibroadcast::log::create_or_get("main");
  for (const auto& result : test_results) {
    m_console->debug("MCS {} PASSED: {}-{}-{} FAILED {}-{}-{}",
                     result.mcs_index, result.pass_pps_set,
                     result.pass_pps_measured,
                     StringHelper::bitrate_readable(result.pass_bps_measured),
                     result.fail_pps_set, result.fail_pps_measured,
                     StringHelper::bitrate_readable(result.fail_bps_measured));
  }
}
static void print_test_results_and_theoretical(
    const std::vector<TestResult>& test_results, bool is_40mhz) {
  auto m_console = wifibroadcast::log::create_or_get("main");
  for (const auto& result : test_results) {
    const auto theoretical =
        wifibroadcast::get_theoretical_rate_5G(result.mcs_index);
    const int rate_kbits =
        is_40mhz ? theoretical.rate_40mhz_kbits : theoretical.rate_20mhz_kbits;
    m_console->debug("MCS {} PASSED {}--{}", result.mcs_index,
                     StringHelper::bitrate_readable(result.pass_bps_measured),
                     StringHelper::bitrate_readable(rate_kbits * 1000));
  }
}

static std::vector<TestResult> all_mcs_increase_pps_until_fail(
    std::shared_ptr<WBTxRx> txrx, std::shared_ptr<RadiotapHeaderTxHolder> hdr,
    const int pps_increment, const int max_mcs = 12) {
  assert(max_mcs >= 0);
  assert(max_mcs <= 32);
  std::vector<TestResult> ret;
  auto m_console = wifibroadcast::log::create_or_get("main");
  // Since we use increasing MCS, start where the last measurement failed to
  // speed up testing
  int pps_start = 500;
  for (int mcs = 0; mcs < max_mcs; mcs++) {
    // at MCS8 we loop around regarding rate
    if (mcs % 8 == 0) {
      pps_start = 500;
    }
    if (pps_start <= 0) {
      m_console->warn("Didn't pass a prev. rate");
      pps_start = 500;
    }
    auto res =
        increase_pps_until_fail(txrx, hdr, mcs, pps_start, pps_increment);
    print_test_results_rough({res});
    // start where the last mcs successfully passed
    pps_start = res.pass_pps_set;
    ret.push_back(res);
    /*auto res_rough_fine=
    increase_pps_until_fail_fine_adjust(txrx,mcs,pps_start,400); auto
    rough=res_rough_fine.rough; auto fine=res_rough_fine.fine;
    pps_start=rough.pass_pps_set;
    ret.push_back(fine);*/
  }
  return ret;
}

void long_test(std::shared_ptr<WBTxRx> txrx,
               std::shared_ptr<RadiotapHeaderTxHolder> hdr, bool use_40mhz) {
  auto m_console = wifibroadcast::log::create_or_get("main");
  const int freq_w = use_40mhz ? 40 : 20;
  m_console->info("Long test {}", freq_w);
  hdr->update_channel_width(freq_w);
  const int mcs_max = 12;
  const auto res_first =
      all_mcs_increase_pps_until_fail(txrx, hdr, 50, mcs_max);
  const auto res_second =
      all_mcs_increase_pps_until_fail(txrx, hdr, 50, mcs_max);
  const auto res_third =
      all_mcs_increase_pps_until_fail(txrx, hdr, 50, mcs_max);
  m_console->info("First run:");
  print_test_results_rough(res_first);
  m_console->info("Second run:");
  print_test_results_rough(res_second);
  m_console->info("Third run:");
  print_test_results_rough(res_third);
  m_console->info("---------------------------");
  m_console->info("First run:");
  print_test_results_and_theoretical(res_first, use_40mhz);
  m_console->info("Second run:");
  print_test_results_and_theoretical(res_second, use_40mhz);
  m_console->info("Third run:");
  print_test_results_and_theoretical(res_third, use_40mhz);
  for (int i = 0; i < res_first.size(); i++) {
    m_console->info(
        "MCS {} possible {}--{}--{}", res_first.at(i).mcs_index,
        StringHelper::bitrate_readable(res_first.at(i).pass_bps_measured),
        StringHelper::bitrate_readable(res_second.at(i).pass_bps_measured),
        StringHelper::bitrate_readable(res_third.at(i).pass_bps_measured));
  }
}

void test_rates_and_print_results(std::shared_ptr<WBTxRx> txrx,
                                  std::shared_ptr<RadiotapHeaderTxHolder> hdr,
                                  bool use_40mhz) {
  const int freq_w = use_40mhz ? 40 : 20;
  hdr->update_channel_width(freq_w);
  const auto res_20mhz = all_mcs_increase_pps_until_fail(txrx, hdr, 20);
  print_test_results_rough(res_20mhz);
  print_test_results_and_theoretical(res_20mhz, false);
}

int main(int argc, char* const* argv) {
  // std::string card="wlxac9e17596103";
  std::string card = "wlx200db0c3a53c";
  std::string card_arg;
  bool list_cards = false;
  int freq_mhz = 0;
  std::string ht_mode_arg;
  int mcs_override = -1;
  double rate_mbit = -1.0;
  int duration_seconds = 10;
  int payload_size = TEST_PACKETS_SIZE;
  bool ui_mode = (argc == 1);
  int opt;
  while ((opt = getopt(argc, argv, "w:lf:H:m:r:t:p:U")) != -1) {
    switch (opt) {
      case 'w':
        card_arg = optarg;
        break;
      case 'l':
        list_cards = true;
        break;
      case 'U':
        ui_mode = true;
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
      case 'r':
        rate_mbit = strtod(optarg, nullptr);
        break;
      case 't':
        duration_seconds = atoi(optarg);
        break;
      case 'p':
        payload_size = atoi(optarg);
        break;
      default: /* '?' */
      show_usage:
        fprintf(stderr,
                "injection rate test %s [-w iface|index] [-l] [-f freq_mhz]\n"
                "  [-H HT20|HT40+|HT40-] [-m mcs] [-r rate_mbit] [-t seconds]\n"
                "  [-p payload_bytes] [-U]\n",
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
  if (!card_arg.empty()) {
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
  } else if (ui_mode) {
    card = wifibroadcast::wifi_card_helper::prompt_select_card(detected_cards);
  } else if (!detected_cards.empty()) {
    card = detected_cards.front();
  }
  auto normalized_ht =
      wifibroadcast::wifi_card_helper::normalize_ht_mode(ht_mode_arg);
  if (ui_mode && normalized_ht.empty()) {
    normalized_ht = "HT20";
  }
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
  std::cout << "Running on card " << card << "\n";

  // Create the Tx-RX
  std::vector<wifibroadcast::WifiCard> cards;
  wifibroadcast::WifiCard tmp_card{card, 1};
  cards.push_back(tmp_card);
  WBTxRx::Options options_txrx{};
  // options_txrx.pcap_rx_set_direction= false;
  options_txrx.log_all_received_validated_packets = false;
  options_txrx.tx_without_pcap = true;

  auto radiotap_header = std::make_shared<RadiotapHeaderTxHolder>();
  const int bw_from_ht =
      wifibroadcast::wifi_card_helper::ht_mode_to_bandwidth(normalized_ht);
  if (bw_from_ht > 0) {
    radiotap_header->update_channel_width(bw_from_ht);
  }
  std::shared_ptr<WBTxRx> txrx =
      std::make_shared<WBTxRx>(cards, options_txrx, radiotap_header);
  // No idea if and what effect stbc and ldpc have on the rate, but openhd
  // enables them if possible by default since they greatly increase range /
  // resiliency
  radiotap_header->update_stbc(true);
  radiotap_header->update_ldpc(true);
  // short GI interval gives slightly higher rates, but also decreases
  // resiliency
  radiotap_header->update_guard_interval(false);

  txrx->start_receiving();

  auto m_console = wifibroadcast::log::create_or_get("main");
  std::vector<TestResult> m_test_results;

  WBTxRx::OUTPUT_DATA_CALLBACK cb =
      [](uint64_t nonce, int wlan_index, const uint8_t radioPort,
         const uint8_t* data, const std::size_t data_len) {
        // std::string message((const char*)data,data_len);
        // fmt::print("Got packet[{}]\n",message);
      };
  txrx->rx_register_callback(cb);

  if (ui_mode) {
    const int initial_mcs = (mcs_override >= 0) ? mcs_override : 3;
    const double initial_rate = (rate_mbit > 0.0) ? rate_mbit : 10.0;
    return run_ncurses_ui(card, freq_mhz, normalized_ht, txrx, radiotap_header,
                          payload_size, initial_mcs, initial_rate);
  }

  // long_test(txrx, false);
  if (rate_mbit > 0.0 || mcs_override >= 0) {
    if (rate_mbit <= 0.0 || mcs_override < 0) {
      fprintf(stderr,
              "Both -m (mcs) and -r (rate_mbit) must be provided together\n");
      return 1;
    }
    const int rate_kbits =
        static_cast<int>(rate_mbit * 1000.0 + 0.5);
    const auto res = validate_specific_rate(txrx, radiotap_header, mcs_override,
                                            rate_kbits, duration_seconds);
    std::cout << res << "\n";
    return 0;
  }

  test_rates_and_print_results(txrx, radiotap_header, bw_from_ht == 40);
  // test_rates_and_print_results(txrx, true);

  // validate_rtl8812au_rates(txrx, false);

  /*const auto res_40mhz= all_mcs_increase_pps_until_fail(txrx);
  print_test_results_rough(res_40mhz);
  m_console->info("20Mhz:");
  print_test_results_rough(res_20mhz);
  m_console->info("40Mhz:");
  print_test_results_rough(res_40mhz);*/
}
