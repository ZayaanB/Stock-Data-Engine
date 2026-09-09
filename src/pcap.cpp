#include "replay/pcap.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "itch/moldudp64.hpp"

namespace replay {
namespace {

std::uint16_t be16(const std::byte* bytes) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) |
                                    static_cast<std::uint16_t>(bytes[1]));
}

std::uint32_t read32(const std::byte* bytes, bool little) noexcept {
  std::uint32_t value = 0;
  if (little) {
    for (int index = 3; index >= 0; --index)
      value = (value << 8) | static_cast<std::uint8_t>(bytes[index]);
  } else {
    for (int index = 0; index < 4; ++index)
      value = (value << 8) | static_cast<std::uint8_t>(bytes[index]);
  }
  return value;
}

std::uint16_t read16(const std::byte* bytes, bool little) noexcept {
  if (little)
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[0]) |
                                      (static_cast<std::uint16_t>(bytes[1]) << 8));
  return be16(bytes);
}

double percentile(const std::vector<std::uint64_t>& values, double probability) {
  if (values.empty()) return 0.0;
  const auto index = static_cast<std::size_t>(probability * static_cast<double>(values.size() - 1));
  return static_cast<double>(values[index]);
}

}  // namespace

UdpPayload extract_udp_payload(std::span<const std::byte> packet,
                               std::uint32_t link_type) noexcept {
  std::size_t position = 0;
  if (link_type == 1) {
    if (packet.size() < 14) return {PacketError::truncated_ethernet, {}};
    auto ether_type = be16(packet.data() + 12);
    position = 14;
    while (ether_type == 0x8100 || ether_type == 0x88a8) {
      if (packet.size() - position < 4) return {PacketError::truncated_ethernet, {}};
      ether_type = be16(packet.data() + position + 2);
      position += 4;
    }
    if (ether_type != 0x0800) return {PacketError::unsupported_network_protocol, {}};
  } else if (link_type != 101) {
    return {PacketError::unsupported_link_type, {}};
  }

  if (packet.size() - position < 20) return {PacketError::truncated_ip, {}};
  const auto version_and_ihl = static_cast<std::uint8_t>(packet[position]);
  if ((version_and_ihl >> 4) != 4) return {PacketError::unsupported_network_protocol, {}};
  const std::size_t ip_header_length = (version_and_ihl & 0x0fU) * 4U;
  if (ip_header_length < 20 || packet.size() - position < ip_header_length)
    return {PacketError::truncated_ip, {}};
  const auto total_length = be16(packet.data() + position + 2);
  if (total_length < ip_header_length + 8 || packet.size() - position < total_length)
    return {PacketError::truncated_ip, {}};
  const auto fragment = be16(packet.data() + position + 6);
  if ((fragment & 0x3fffU) != 0) return {PacketError::fragmented_ip, {}};
  if (static_cast<std::uint8_t>(packet[position + 9]) != 17) return {PacketError::non_udp, {}};

  position += ip_header_length;
  const auto udp_length = be16(packet.data() + position + 4);
  if (udp_length < 8 || udp_length > total_length - ip_header_length ||
      packet.size() - position < udp_length)
    return {PacketError::truncated_udp, {}};
  return {PacketError::none, packet.subspan(position + 8, udp_length - 8)};
}

Result run_pcap(const std::filesystem::path& path, const Config& config, PcapStats& pcap_stats) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open PCAP file: " + path.string());

  std::array<std::byte, 24> global{};
  input.read(reinterpret_cast<char*>(global.data()), static_cast<std::streamsize>(global.size()));
  if (input.gcount() != static_cast<std::streamsize>(global.size()))
    throw std::runtime_error("truncated PCAP global header");
  const auto magic = read32(global.data(), true);
  const bool little = magic == 0xa1b2c3d4U || magic == 0xa1b23c4dU;
  const bool big = magic == 0xd4c3b2a1U || magic == 0x4d3cb2a1U;
  if (!little && !big) throw std::runtime_error("unsupported PCAP magic");
  if (read16(global.data() + 4, little) != 2 || read16(global.data() + 6, little) != 4)
    throw std::runtime_error("unsupported PCAP version");
  const auto link_type = read32(global.data() + 20, little);

  book::OrderBook order_book(config.max_orders, config.max_levels, config.symbol);
  itch::MoldDecoder mold_decoder;
  itch::SequenceTracker sequence_tracker;
  itch::DecoderStats decoder_stats{};
  std::vector<std::uint64_t> samples;
  samples.reserve(config.latency_samples);
  std::array<std::byte, 65'536> packet{};
  std::array<std::byte, 16> record{};
  std::uint64_t seen = 0;
  bool sequence_valid = true;

  const auto start = std::chrono::steady_clock::now();
  for (;;) {
    input.read(reinterpret_cast<char*>(record.data()), static_cast<std::streamsize>(record.size()));
    if (input.gcount() == 0) break;
    if (input.gcount() != static_cast<std::streamsize>(record.size()))
      throw std::runtime_error("truncated PCAP record header");
    ++pcap_stats.packets;
    const auto captured_length = read32(record.data() + 8, little);
    if (captured_length > packet.size())
      throw std::runtime_error("PCAP packet exceeds 65536-byte safety limit");
    input.read(reinterpret_cast<char*>(packet.data()),
               static_cast<std::streamsize>(captured_length));
    if (input.gcount() != static_cast<std::streamsize>(captured_length))
      throw std::runtime_error("truncated PCAP packet data");

    const auto udp =
        extract_udp_payload(std::span<const std::byte>(packet.data(), captured_length), link_type);
    if (!udp) {
      if (udp.error == PacketError::non_udp ||
          udp.error == PacketError::unsupported_network_protocol)
        ++pcap_stats.skipped_packets;
      else
        ++pcap_stats.malformed_packets;
      continue;
    }
    ++pcap_stats.udp_packets;
    const auto mold = mold_decoder.decode(
        udp.bytes,
        [&](std::span<const std::byte> bytes, std::uint64_t) {
          const auto parsed = itch::parse_message(bytes);
          if (!parsed) {
            if (parsed.error == itch::ParseError::unsupported_type)
              ++decoder_stats.unsupported;
            else
              ++decoder_stats.malformed;
            return;
          }
          ++decoder_stats.decoded;
          const bool should_sample = config.sample_every != 0 && seen % config.sample_every == 0 &&
                                     samples.size() < samples.capacity();
          const auto before = std::chrono::steady_clock::now();
          order_book.process(parsed.message);
          if (should_sample) {
            const auto elapsed = std::chrono::steady_clock::now() - before;
            samples.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
          }
          ++seen;
        },
        [&](const itch::MoldHeader& header) {
          const auto sequence = sequence_tracker.observe(header);
          if (sequence.status == itch::SequenceStatus::gap) {
            ++pcap_stats.sequence_gaps;
            pcap_stats.missing_messages += sequence.missing;
            sequence_valid = false;
            return false;
          }
          if (sequence.status == itch::SequenceStatus::rewind) {
            ++pcap_stats.sequence_rewinds;
            return false;
          }
          if (sequence.status == itch::SequenceStatus::new_session) sequence_valid = true;
          return sequence_valid;
        });
    if (!mold) {
      ++pcap_stats.mold_errors;
      continue;
    }
    ++pcap_stats.mold_packets;
    if (!mold.header.end_of_session()) pcap_stats.mold_messages += mold.header.message_count;
    if (mold.header.heartbeat()) ++pcap_stats.heartbeats;
    if (mold.header.end_of_session()) ++pcap_stats.end_of_session;
  }
  if (input.bad()) throw std::runtime_error("I/O error while reading PCAP");

  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::sort(samples.begin(), samples.end());
  Result result{};
  result.messages = decoder_stats.decoded;
  result.seconds = seconds;
  result.messages_per_second =
      seconds != 0.0 ? static_cast<double>(result.messages) / seconds : 0.0;
  result.decoder = decoder_stats;
  result.book = order_book.stats();
  result.bid = order_book.best_bid();
  result.ask = order_book.best_ask();
  result.latency = {
      percentile(samples, 0.50),
      percentile(samples, 0.90),
      percentile(samples, 0.99),
      percentile(samples, 0.999),
      samples.empty() ? 0.0 : static_cast<double>(samples.back()),
  };
  return result;
}

const char* to_string(PacketError error) noexcept {
  switch (error) {
    case PacketError::none:
      return "none";
    case PacketError::unsupported_link_type:
      return "unsupported link type";
    case PacketError::truncated_ethernet:
      return "truncated Ethernet frame";
    case PacketError::unsupported_network_protocol:
      return "unsupported network protocol";
    case PacketError::truncated_ip:
      return "truncated IPv4 packet";
    case PacketError::fragmented_ip:
      return "fragmented IPv4 packet";
    case PacketError::non_udp:
      return "non-UDP packet";
    case PacketError::truncated_udp:
      return "truncated UDP datagram";
  }
  return "unknown";
}

}  // namespace replay
