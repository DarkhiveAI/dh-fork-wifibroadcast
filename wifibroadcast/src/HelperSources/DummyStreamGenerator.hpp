//
// Created by consti10 on 25.07.23.
//

#ifndef WIFIBROADCAST_DUMMYSTREAMGENERATOR_HPP
#define WIFIBROADCAST_DUMMYSTREAMGENERATOR_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

#include "RandomBufferPot.hpp"
#include "SchedulingHelper.hpp"

/**
 * Generates as close as possible a stream of data packets with a target packets
 * per second packet rate.
 */
class DummyStreamGenerator {
 public:
  typedef std::function<void(const uint8_t* data, int data_len)>
      OUTPUT_DATA_CALLBACK;

  DummyStreamGenerator(OUTPUT_DATA_CALLBACK cb, int packet_size)
      : m_cb(std::move(cb)), m_packet_size(packet_size) {
    auto pot = std::make_shared<RandomBufferPot>(1000, packet_size);
    std::atomic_store(&m_random_buffer_pot, pot);
  };
  ~DummyStreamGenerator() { stop(); }

  void set_target_pps(int pps) {
    if (pps < 1) {
      pps = 1;
    }
    m_target_pps.store(pps);
  }
  int get_target_pps() const { return m_target_pps.load(); }
  void set_packet_size(int packet_size) {
    if (packet_size < 1) {
      return;
    }
    m_packet_size.store(packet_size);
    auto pot = std::make_shared<RandomBufferPot>(1000, packet_size);
    std::atomic_store(&m_random_buffer_pot, pot);
  }
  int get_packet_size() const { return m_packet_size.load(); }

  void start() {
    if (m_running.load()) {
      return;
    }
    m_terminate = false;
    m_producer_thread =
        std::make_unique<std::thread>([this]() { loop_generate_data(); });
    m_running.store(true);
  }
  void stop() {
    m_terminate = true;
    if (m_producer_thread) {
      m_producer_thread->join();
      m_producer_thread = nullptr;
    }
    m_running.store(false);
  }
  bool is_running() const { return m_running.load(); }
  void loop_generate_data() {
    SchedulingHelper::set_thread_params_max_realtime("DummyStreamGenerator");
    std::chrono::steady_clock::time_point last_packet =
        std::chrono::steady_clock::now();
    while (!m_terminate) {
      const int target_pps = std::max(1, m_target_pps.load());
      const uint64_t delay_between_packets_ns =
          1000ULL * 1000ULL * 1000ULL / static_cast<uint64_t>(target_pps);
      const auto delay_between_packets =
          std::chrono::nanoseconds(delay_between_packets_ns);
      last_packet = std::chrono::steady_clock::now();
      // wifibroadcast::log::get_default()->debug("Delay between packets:
      // {}",std::chrono::duration_cast<std::chrono::nanoseconds>(delay_between_packets).count());
      auto pot = std::atomic_load(&m_random_buffer_pot);
      auto buff = pot->get_next_buffer();
      m_cb(buff->data(), buff->size());
      const auto next_packet_tp =
          last_packet + delay_between_packets -
          std::chrono::nanoseconds(200);  // minus Xns to better hit the target
      if (std::chrono::steady_clock::now() >= next_packet_tp) {
        // wifibroadcast::log::get_default()->warn("Cannot keep up with the
        // wanted tx pps");
        n_times_cannot_keep_up_wanted_pps++;
      }
      while (std::chrono::steady_clock::now() < next_packet_tp) {
        // busy wait
      }
    }
  }
  int n_times_cannot_keep_up_wanted_pps = 0;

 private:
  const OUTPUT_DATA_CALLBACK m_cb;
  std::atomic<int> m_target_pps = 100;
  std::atomic<int> m_packet_size = 1400;
  std::unique_ptr<std::thread> m_producer_thread;
  std::shared_ptr<RandomBufferPot> m_random_buffer_pot;
  bool m_terminate = false;
  std::atomic<bool> m_running = false;
};

#endif  // WIFIBROADCAST_DUMMYSTREAMGENERATOR_HPP
