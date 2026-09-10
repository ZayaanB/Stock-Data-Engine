#include <cstddef>
#include <cstdint>
#include <span>

#include "itch/moldudp64.hpp"
#include "itch/parser.hpp"
#include "replay/pcap.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size);
  static_cast<void>(itch::parse_message(bytes));

  itch::MoldDecoder mold;
  static_cast<void>(mold.decode(bytes, [](std::span<const std::byte> message, auto) noexcept {
    static_cast<void>(itch::parse_message(message));
  }));

  static_cast<void>(replay::extract_udp_payload(bytes, 1));
  static_cast<void>(replay::extract_udp_payload(bytes, 101));
  return 0;
}
