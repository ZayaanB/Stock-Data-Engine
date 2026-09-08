#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#include "replay/replay.hpp"

namespace replay {

enum class PacketError {
  none,
  unsupported_link_type,
  truncated_ethernet,
  unsupported_network_protocol,
  truncated_ip,
  fragmented_ip,
  non_udp,
  truncated_udp,
};

struct UdpPayload {
  PacketError error{PacketError::none};
  std::span<const std::byte> bytes{};
  explicit operator bool() const noexcept { return error == PacketError::none; }
};

struct PcapStats {
  std::uint64_t packets{}, udp_packets{}, skipped_packets{}, malformed_packets{};
  std::uint64_t mold_packets{}, mold_messages{}, mold_errors{}, heartbeats{}, end_of_session{};
  std::uint64_t sequence_gaps{}, missing_messages{}, sequence_rewinds{};
};

UdpPayload extract_udp_payload(std::span<const std::byte> packet, std::uint32_t link_type) noexcept;
Result run_pcap(const std::filesystem::path&, const Config&, PcapStats&);
const char* to_string(PacketError error) noexcept;

}  // namespace replay
