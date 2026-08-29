#include "DevourerTransport.h"

#ifdef WIFIBROADCAST_WITH_DEVOURER

#include <libusb-1.0/libusb.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include "DeviceConfig.h"
#include "IRtlDevice.h"
#include "RxPacket.h"
#include "SelectedChannel.h"
#include "UsbDeviceLock.h"
#include "UsbOpen.h"
#include "WiFiDriver.h"
#include "logger.h"

namespace wifibroadcast::devourer_transport {
namespace {

struct UsbAddress {
  uint8_t bus = 0;
  uint8_t device = 0;
};

std::optional<int> read_int(const std::filesystem::path& path) {
  std::ifstream input(path);
  int value = 0;
  if (!(input >> value)) return std::nullopt;
  return value;
}

std::optional<UsbAddress> usb_address_for_interface(const std::string& name) {
  static constexpr const char* prefix = "devourer-usb-";
  if (name.rfind(prefix, 0) == 0) {
    const auto separator = name.find('-', std::char_traits<char>::length(prefix));
    if (separator != std::string::npos) {
      try {
        const int bus = std::stoi(
            name.substr(std::char_traits<char>::length(prefix),
                        separator - std::char_traits<char>::length(prefix)));
        const int device = std::stoi(name.substr(separator + 1));
        if (bus > 0 && bus <= 255 && device > 0 && device <= 255) {
          return UsbAddress{static_cast<uint8_t>(bus),
                            static_cast<uint8_t>(device)};
        }
      } catch (...) {
      }
    }
    return std::nullopt;
  }
  std::error_code ec;
  auto path = std::filesystem::canonical(
      std::filesystem::path("/sys/class/net") / name / "device", ec);
  if (ec) return std::nullopt;
  for (int depth = 0; depth < 12 && !path.empty(); ++depth) {
    const auto bus = read_int(path / "busnum");
    const auto device = read_int(path / "devnum");
    if (bus && device && *bus > 0 && *bus <= 255 && *device > 0 &&
        *device <= 255) {
      return UsbAddress{static_cast<uint8_t>(*bus),
                        static_cast<uint8_t>(*device)};
    }
    const auto parent = path.parent_path();
    if (parent == path) break;
    path = parent;
  }
  return std::nullopt;
}

libusb_device_handle* open_address(libusb_context* context,
                                   const UsbAddress& address) {
  libusb_device** devices = nullptr;
  const auto count = libusb_get_device_list(context, &devices);
  if (count < 0) return nullptr;
  libusb_device_handle* handle = nullptr;
  for (ssize_t i = 0; i < count; ++i) {
    auto* device = devices[i];
    if (libusb_get_bus_number(device) != address.bus ||
        libusb_get_device_address(device) != address.device) {
      continue;
    }
    if (libusb_open(device, &handle) != 0) handle = nullptr;
    break;
  }
  libusb_free_device_list(devices, 1);
  return handle;
}

std::optional<SelectedChannel> selected_channel(Channel channel) {
  int number = -1;
  if (channel.frequency_mhz == 2484) {
    number = 14;
  } else if (channel.frequency_mhz >= 2412 &&
             channel.frequency_mhz <= 2472 &&
             (channel.frequency_mhz - 2407) % 5 == 0) {
    number = (channel.frequency_mhz - 2407) / 5;
  } else if (channel.frequency_mhz >= 5000 &&
             (channel.frequency_mhz - 5000) % 5 == 0) {
    number = (channel.frequency_mhz - 5000) / 5;
  }
  ChannelWidth_t width;
  uint8_t channel_offset = 0;
  switch (channel.width_mhz) {
    case 5:
      width = CHANNEL_WIDTH_5;
      break;
    case 10:
      width = CHANNEL_WIDTH_10;
      break;
    case 20:
      width = CHANNEL_WIDTH_20;
      break;
    case 40:
      width = CHANNEL_WIDTH_40;
      // Devourer needs the primary-20 position to tune the RF center for
      // HT40. OpenHD's supported channel list alternates HT40+ and HT40-
      // within each standard four-channel block. ChannelOffset follows the
      // HAL convention: 1 = HT40+ (secondary above), 2 = HT40- (below).
      if (number <= 14) {
        channel_offset = ((number - 1) / 4) % 2 == 0 ? 1 : 2;
      } else if (number <= 36 || number >= 181) {
        channel_offset = 1;
      } else if (number >= 149) {
        channel_offset = ((number - 1) / 4) % 2 == 1 ? 1 : 2;
      } else {
        channel_offset = (number / 4) % 2 == 1 ? 1 : 2;
      }
      break;
    case 80:
      width = CHANNEL_WIDTH_80;
      break;
    default:
      return std::nullopt;
  }
  if (number < 0 || number > 255) return std::nullopt;
  return SelectedChannel{static_cast<uint8_t>(number), channel_offset, width};
}

}  // namespace

struct Transport::Card {
  std::string interface_name;
  std::shared_ptr<Logger> logger = std::make_shared<Logger>();
  libusb_context* context = nullptr;
  libusb_device_handle* handle = nullptr;
  int interface_number = -1;
  std::shared_ptr<devourer::UsbDeviceLock> lock;
  std::unique_ptr<IRtlDevice> device;
  std::thread rx_thread;
  std::mutex control_mutex;

  ~Card() { close(); }

  bool open(Channel channel) {
    const auto address = usb_address_for_interface(interface_name);
    if (!address) {
      logger->error("Cannot resolve {} to a USB device", interface_name);
      return false;
    }
    if (libusb_init(&context) != 0) return false;
    libusb_set_option(context, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
    handle = open_address(context, *address);
    if (!handle) {
      logger->error("Cannot open USB device for {}", interface_name);
      close();
      return false;
    }
    interface_number = devourer::find_wifi_interface(handle);
    const int rc = devourer::claim_interface_reset_reopen(
        context, handle, logger, true, lock);
    if (rc != 0) {
      logger->error("Cannot claim {} (libusb rc={})", interface_name, rc);
      close();
      return false;
    }
    interface_number = devourer::find_wifi_interface(handle);
    devourer::DeviceConfig config;
    config.rx.enable_with_tx = true;
    WiFiDriver factory(logger);
    device = factory.CreateRtlDevice(handle, context, lock, config);
    const auto selected = selected_channel(channel);
    if (!device || !selected) {
      logger->error("Unsupported adapter or channel for {}", interface_name);
      close();
      return false;
    }
    try {
      device->InitWrite(*selected);
    } catch (const std::exception& ex) {
      logger->error("Devourer bring-up failed for {}: {}", interface_name,
                    ex.what());
      close();
      return false;
    }
    return true;
  }

  void close() {
    stop_rx();
    if (device) {
      try {
        device->Stop();
      } catch (...) {
      }
      device.reset();
    }
    if (handle) {
      if (interface_number >= 0) {
        libusb_release_interface(handle, interface_number);
        libusb_attach_kernel_driver(handle, interface_number);
      }
      libusb_close(handle);
      handle = nullptr;
    }
    lock.reset();
    if (context) {
      libusb_exit(context);
      context = nullptr;
    }
  }

  void stop_rx() {
    if (device) device->StopRxLoop();
    if (rx_thread.joinable()) rx_thread.join();
  }
};

Transport::Transport(std::vector<std::string> interface_names, Channel channel)
    : m_interface_names(std::move(interface_names)), m_channel(channel) {}

Transport::~Transport() { close(); }

bool Transport::open() {
  close();
  for (const auto& name : m_interface_names) {
    auto card = std::make_unique<Card>();
    card->interface_name = name;
    card->logger->set_level(Logger::Level::Info);
    if (!card->open(m_channel)) {
      close();
      return false;
    }
    m_cards.push_back(std::move(card));
  }
  return !m_cards.empty();
}

void Transport::close() {
  stop_rx();
  m_cards.clear();
}

bool Transport::send(int card_index, const uint8_t* data, int length) {
  if (card_index < 0 || card_index >= static_cast<int>(m_cards.size()) ||
      !data || length <= 0) {
    return false;
  }
  return m_cards[card_index]->device->send_packet(
      data, static_cast<size_t>(length));
}

bool Transport::set_channel(Channel channel) {
  const auto selected = selected_channel(channel);
  if (!selected) return false;
  try {
    for (auto& card : m_cards) {
      std::lock_guard<std::mutex> guard(card->control_mutex);
      card->device->SetMonitorChannel(*selected);
    }
  } catch (const std::exception&) {
    return false;
  }
  m_channel = channel;
  return true;
}

void Transport::set_tx_power_index_override(const int card_index,
                                            const int index) {
  if (card_index < 0 || card_index >= static_cast<int>(m_cards.size())) return;
  auto& card = m_cards[card_index];
  std::lock_guard<std::mutex> guard(card->control_mutex);
  card->device->SetTxPowerIndexOverride(index);
}

std::optional<devourer::ThermalStatus> Transport::get_thermal_status(
    const int card_index) {
  if (card_index < 0 || card_index >= static_cast<int>(m_cards.size())) {
    return std::nullopt;
  }
  auto& card = m_cards[card_index];
  std::lock_guard<std::mutex> guard(card->control_mutex);
  if (!card->device) return std::nullopt;
  return card->device->GetThermalStatus();
}

void Transport::start_rx(RxCallback callback, FatalCallback fatal_callback) {
  for (int i = 0; i < static_cast<int>(m_cards.size()); ++i) {
    auto* card = m_cards[i].get();
    if (card->rx_thread.joinable()) continue;
    card->rx_thread = std::thread([card, i, callback, fatal_callback]() {
      try {
        card->device->StartRxLoop(
            [i, callback](const Packet& packet) { callback(i, packet); });
      } catch (const std::exception& ex) {
        card->logger->error("RX loop failed for {}: {}", card->interface_name,
                            ex.what());
        if (fatal_callback) fatal_callback(ENODEV);
      }
    });
  }
}

void Transport::stop_rx() {
  for (auto& card : m_cards) {
    if (card->device) card->device->StopRxLoop();
  }
  for (auto& card : m_cards) {
    if (card->rx_thread.joinable()) card->rx_thread.join();
  }
}

}  // namespace wifibroadcast::devourer_transport

#endif
