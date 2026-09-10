#include "replay/pcap.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "itch/moldudp64.hpp"
#include "queue/spsc_ring.hpp"

namespace replay {
namespace {

constexpr std::uint32_t section_header = 0x0a0d0d0aU;
constexpr std::uint32_t interface_description = 1;
constexpr std::uint32_t simple_packet = 3;
constexpr std::uint32_t enhanced_packet = 6;
constexpr std::size_t maximum_packet_size = 65'536;
constexpr std::size_t maximum_block_size = maximum_packet_size + 32;

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

struct CapturedPacket {
  std::span<const std::byte> bytes{};
  std::uint32_t link_type{};
};

class CaptureReader {
 public:
  explicit CaptureReader(const std::filesystem::path& path) : input_(path, std::ios::binary) {
    if (!input_) throw std::runtime_error("cannot open capture file: " + path.string());
    std::array<std::byte, 4> magic{};
    read_exact(magic, "truncated capture header");
    const auto value = read32(magic.data(), true);
    if (value == 0xa1b2c3d4U || value == 0xa1b23c4dU || value == 0xd4c3b2a1U ||
        value == 0x4d3cb2a1U) {
      format_ = Format::pcap;
      little_ = value == 0xa1b2c3d4U || value == 0xa1b23c4dU;
      std::array<std::byte, 20> header{};
      read_exact(header, "truncated PCAP global header");
      if (read16(header.data(), little_) != 2 || read16(header.data() + 2, little_) != 4)
        throw std::runtime_error("unsupported PCAP version");
      classic_link_type_ = read32(header.data() + 16, little_);
      return;
    }
    if (value != section_header) throw std::runtime_error("unsupported capture magic");
    format_ = Format::pcapng;
    consume_section();
  }

  bool next(CapturedPacket& packet) {
    return format_ == Format::pcap ? next_classic(packet) : next_pcapng(packet);
  }

 private:
  enum class Format { pcap, pcapng };
  struct Interface {
    std::uint32_t link_type{};
    std::uint32_t snap_length{};
  };

  template <std::size_t Size>
  void read_exact(std::array<std::byte, Size>& bytes, const char* error) {
    read_exact(std::span<std::byte>(bytes), error);
  }

  void read_exact(std::span<std::byte> bytes, const char* error) {
    input_.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input_.gcount() != static_cast<std::streamsize>(bytes.size()))
      throw std::runtime_error(error);
  }

  bool read_block_type(std::array<std::byte, 4>& type) {
    input_.read(reinterpret_cast<char*>(type.data()), static_cast<std::streamsize>(type.size()));
    if (input_.gcount() == 0) return false;
    if (input_.gcount() != static_cast<std::streamsize>(type.size()))
      throw std::runtime_error("truncated PCAP-NG block type");
    return true;
  }

  static void validate_block_size(std::uint32_t total, std::uint32_t minimum) {
    if (total < minimum || total % 4 != 0) throw std::runtime_error("invalid PCAP-NG block length");
  }

  std::span<const std::byte> read_block_body(std::uint32_t total) {
    if (total > block_.size()) throw std::runtime_error("PCAP-NG block exceeds safety limit");
    const auto remaining = static_cast<std::size_t>(total) - 8;
    read_exact(std::span<std::byte>(block_.data(), remaining), "truncated PCAP-NG block");
    if (read32(block_.data() + remaining - 4, little_) != total)
      throw std::runtime_error("PCAP-NG block length mismatch");
    return std::span<const std::byte>(block_.data(), remaining);
  }

  void skip_block(std::uint32_t total) {
    input_.seekg(static_cast<std::streamoff>(total - 12), std::ios::cur);
    if (!input_) throw std::runtime_error("truncated PCAP-NG block");
    std::array<std::byte, 4> trailing{};
    read_exact(trailing, "truncated PCAP-NG block");
    if (read32(trailing.data(), little_) != total)
      throw std::runtime_error("PCAP-NG block length mismatch");
  }

  void consume_section() {
    std::array<std::byte, 8> prefix{};
    read_exact(prefix, "truncated PCAP-NG section header");
    const auto byte_order_magic = read32(prefix.data() + 4, true);
    if (byte_order_magic == 0x1a2b3c4dU)
      little_ = true;
    else if (byte_order_magic == 0x4d3c2b1aU)
      little_ = false;
    else
      throw std::runtime_error("invalid PCAP-NG byte-order magic");

    const auto total = read32(prefix.data(), little_);
    validate_block_size(total, 28);
    if (total - 12 > block_.size())
      throw std::runtime_error("PCAP-NG section header exceeds safety limit");
    const auto remaining = static_cast<std::size_t>(total) - 12;
    read_exact(std::span<std::byte>(block_.data(), remaining), "truncated PCAP-NG section header");
    if (read16(block_.data(), little_) != 1)
      throw std::runtime_error("unsupported PCAP-NG version");
    if (read32(block_.data() + remaining - 4, little_) != total)
      throw std::runtime_error("PCAP-NG section length mismatch");
    interfaces_.clear();
  }

  bool next_classic(CapturedPacket& packet) {
    std::array<std::byte, 16> record{};
    input_.read(reinterpret_cast<char*>(record.data()),
                static_cast<std::streamsize>(record.size()));
    if (input_.gcount() == 0) return false;
    if (input_.gcount() != static_cast<std::streamsize>(record.size()))
      throw std::runtime_error("truncated PCAP record header");
    const auto captured_length = read32(record.data() + 8, little_);
    const auto original_length = read32(record.data() + 12, little_);
    if (captured_length > maximum_packet_size || captured_length > original_length)
      throw std::runtime_error("invalid PCAP captured length");
    read_exact(std::span<std::byte>(block_.data(), captured_length), "truncated PCAP packet data");
    packet = {std::span<const std::byte>(block_.data(), captured_length), classic_link_type_};
    return true;
  }

  bool next_pcapng(CapturedPacket& packet) {
    for (;;) {
      std::array<std::byte, 4> type_bytes{};
      if (!read_block_type(type_bytes)) return false;
      if (read32(type_bytes.data(), true) == section_header) {
        consume_section();
        continue;
      }

      std::array<std::byte, 4> length_bytes{};
      read_exact(length_bytes, "truncated PCAP-NG block length");
      const auto type = read32(type_bytes.data(), little_);
      const auto total = read32(length_bytes.data(), little_);
      validate_block_size(total, 12);

      if (type == interface_description) {
        validate_block_size(total, 20);
        const auto body = read_block_body(total);
        interfaces_.push_back({read16(body.data(), little_), read32(body.data() + 4, little_)});
        continue;
      }
      if (type == enhanced_packet) {
        validate_block_size(total, 32);
        const auto body = read_block_body(total);
        const auto interface_id = read32(body.data(), little_);
        if (interface_id >= interfaces_.size())
          throw std::runtime_error("PCAP-NG packet references unknown interface");
        const auto captured_length = read32(body.data() + 12, little_);
        const auto original_length = read32(body.data() + 16, little_);
        const auto padded_length = (static_cast<std::size_t>(captured_length) + 3U) & ~3U;
        if (captured_length > maximum_packet_size || captured_length > original_length ||
            20 + padded_length + 4 > body.size())
          throw std::runtime_error("invalid PCAP-NG captured length");
        packet = {body.subspan(20, captured_length), interfaces_[interface_id].link_type};
        return true;
      }
      if (type == simple_packet) {
        validate_block_size(total, 16);
        const auto body = read_block_body(total);
        if (interfaces_.empty()) throw std::runtime_error("PCAP-NG simple packet has no interface");
        const auto original_length = read32(body.data(), little_);
        const auto available = body.size() - 8;
        const auto captured_length =
            std::min({static_cast<std::size_t>(original_length),
                      static_cast<std::size_t>(interfaces_.front().snap_length), available});
        if (captured_length > maximum_packet_size)
          throw std::runtime_error("PCAP-NG packet exceeds safety limit");
        packet = {body.subspan(4, captured_length), interfaces_.front().link_type};
        return true;
      }
      skip_block(total);
    }
  }

  std::ifstream input_;
  Format format_{Format::pcap};
  bool little_{};
  std::uint32_t classic_link_type_{};
  std::array<std::byte, maximum_block_size> block_{};
  std::vector<Interface> interfaces_;
};

void process_message(book::OrderBook& order_book, std::vector<std::uint64_t>& samples,
                     const Config& config, std::uint64_t seen, const itch::Message& message) {
  const bool should_sample = config.sample_every != 0 && seen % config.sample_every == 0 &&
                             samples.size() < samples.capacity();
  const auto before = std::chrono::steady_clock::now();
  order_book.process(message);
  if (should_sample) {
    const auto elapsed = std::chrono::steady_clock::now() - before;
    samples.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  }
}

Result make_result(const itch::DecoderStats& decoder, const book::OrderBook& order_book,
                   std::vector<std::uint64_t>& samples, double seconds) {
  std::sort(samples.begin(), samples.end());
  Result result{};
  result.messages = decoder.decoded;
  result.seconds = seconds;
  result.messages_per_second =
      seconds != 0.0 ? static_cast<double>(result.messages) / seconds : 0.0;
  result.decoder = decoder;
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

template <class Callback>
void decode_capture(const std::filesystem::path& path, const PcapFilter& filter,
                    PcapStats& pcap_stats, itch::DecoderStats& decoder_stats, Callback&& callback) {
  CaptureReader capture(path);
  itch::MoldDecoder mold_decoder;
  itch::SequenceTracker sequence_tracker;
  bool sequence_valid = true;
  CapturedPacket packet;

  while (capture.next(packet)) {
    ++pcap_stats.packets;
    const auto udp = extract_udp_payload(packet.bytes, packet.link_type);
    if (!udp) {
      if (udp.error == PacketError::non_udp ||
          udp.error == PacketError::unsupported_network_protocol)
        ++pcap_stats.skipped_packets;
      else
        ++pcap_stats.malformed_packets;
      continue;
    }
    ++pcap_stats.udp_packets;
    if (!filter.matches(udp)) {
      ++pcap_stats.filtered_packets;
      continue;
    }

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
          callback(parsed.message);
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
}

Result run_pcap_impl(const std::filesystem::path& path, const Config& config, PcapStats& pcap_stats,
                     const PcapFilter& filter, bool threaded) {
  pcap_stats = {};
  book::OrderBook order_book(config.max_orders, config.max_levels, config.symbol);
  itch::DecoderStats decoder_stats{};
  std::vector<std::uint64_t> samples;
  samples.reserve(config.latency_samples);
  std::uint64_t producer_spins = 0;
  std::uint64_t consumer_spins = 0;
  const auto start = std::chrono::steady_clock::now();

  if (!threaded) {
    std::uint64_t seen = 0;
    decode_capture(path, filter, pcap_stats, decoder_stats, [&](const itch::Message& message) {
      process_message(order_book, samples, config, seen++, message);
    });
  } else {
    constexpr std::size_t queue_capacity = 1 << 16;
    queue::SpscRing<itch::Message, queue_capacity> message_queue;
    std::atomic<bool> producer_done{false};
    std::exception_ptr producer_error;
    std::thread producer([&] {
      try {
        decode_capture(path, filter, pcap_stats, decoder_stats, [&](const itch::Message& message) {
          while (!message_queue.try_push(message)) {
            ++producer_spins;
            std::this_thread::yield();
          }
        });
      } catch (...) {
        producer_error = std::current_exception();
      }
      producer_done.store(true, std::memory_order_release);
    });

    itch::Message message;
    std::uint64_t seen = 0;
    while (!producer_done.load(std::memory_order_acquire) || !message_queue.empty()) {
      if (message_queue.try_pop(message))
        process_message(order_book, samples, config, seen++, message);
      else {
        ++consumer_spins;
        std::this_thread::yield();
      }
    }
    producer.join();
    if (producer_error) std::rethrow_exception(producer_error);
  }

  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  auto result = make_result(decoder_stats, order_book, samples, seconds);
  result.producer_spins = producer_spins;
  result.consumer_spins = consumer_spins;
  return result;
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
  const auto source_port = be16(packet.data() + position);
  const auto destination_port = be16(packet.data() + position + 2);
  const auto udp_length = be16(packet.data() + position + 4);
  if (udp_length < 8 || udp_length > total_length - ip_header_length ||
      packet.size() - position < udp_length)
    return {PacketError::truncated_udp, {}};
  return {PacketError::none, packet.subspan(position + 8, udp_length - 8), source_port,
          destination_port};
}

Result run_pcap(const std::filesystem::path& path, const Config& config, PcapStats& pcap_stats,
                const PcapFilter& filter) {
  return run_pcap_impl(path, config, pcap_stats, filter, false);
}

Result run_pcap_threaded(const std::filesystem::path& path, const Config& config,
                         PcapStats& pcap_stats, const PcapFilter& filter) {
  return run_pcap_impl(path, config, pcap_stats, filter, true);
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
