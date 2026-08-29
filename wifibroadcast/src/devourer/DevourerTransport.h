#pragma once

#ifdef WIFIBROADCAST_WITH_DEVOURER

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct Packet;

namespace wifibroadcast::devourer_transport {

struct Channel {
  int frequency_mhz = 0;
  int width_mhz = 20;
};

class Transport {
 public:
  using RxCallback = std::function<void(int, const Packet&)>;
  using FatalCallback = std::function<void(int)>;

  Transport(std::vector<std::string> interface_names, Channel channel);
  ~Transport();
  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;

  bool open();
  void close();
  bool send(int card_index, const uint8_t* data, int length);
  bool set_channel(Channel channel);
  void set_tx_power_index_override(int card_index, int index);
  void start_rx(RxCallback callback, FatalCallback fatal_callback);
  void stop_rx();

 private:
  struct Card;
  std::vector<std::string> m_interface_names;
  std::vector<std::unique_ptr<Card>> m_cards;
  Channel m_channel;
};

}  // namespace wifibroadcast::devourer_transport

#endif
