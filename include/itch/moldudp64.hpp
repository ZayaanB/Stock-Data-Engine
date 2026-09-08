#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace itch {

struct MoldHeader {
  std::array<char, 10> session{};
  std::uint64_t sequence{};
  std::uint16_t message_count{};

  bool heartbeat() const noexcept { return message_count == 0; }
  bool end_of_session() const noexcept { return message_count == 0xffff; }
};

enum class MoldError {
  none,
  header_too_short,
  invalid_control_packet,
  truncated_message_length,
  truncated_message,
  trailing_bytes,
};

struct MoldResult {
  MoldError error{MoldError::none};
  MoldHeader header{};
  std::uint16_t messages_decoded{};

  explicit operator bool() const noexcept { return error == MoldError::none; }
};

class MoldDecoder {
 public:
  template <class Callback>
  MoldResult decode(std::span<const std::byte> packet, Callback&& callback) const noexcept {
    return decode(packet, std::forward<Callback>(callback),
                  [](const MoldHeader&) noexcept { return true; });
  }

  template <class Callback, class PacketGate>
  MoldResult decode(std::span<const std::byte> packet, Callback&& callback,
                    PacketGate&& gate) const noexcept {
    MoldResult result{};
    if (packet.size() < header_size) {
      result.error = MoldError::header_too_short;
      return result;
    }

    for (std::size_t index = 0; index < result.header.session.size(); ++index)
      result.header.session[index] = static_cast<char>(packet[index]);
    result.header.sequence = read_u64(packet.data() + 10);
    result.header.message_count = read_u16(packet.data() + 18);

    if (result.header.heartbeat() || result.header.end_of_session()) {
      if (packet.size() != header_size) result.error = MoldError::invalid_control_packet;
      if (result.error == MoldError::none) gate(result.header);
      return result;
    }

    // Validate the entire datagram before dispatching any message so malformed
    // packets cannot partially mutate downstream state.
    std::size_t position = header_size;
    for (std::uint16_t index = 0; index < result.header.message_count; ++index) {
      if (packet.size() - position < 2) {
        result.error = MoldError::truncated_message_length;
        return result;
      }
      const auto length = read_u16(packet.data() + position);
      position += 2;
      if (packet.size() - position < length) {
        result.error = MoldError::truncated_message;
        return result;
      }
      position += length;
    }
    if (position != packet.size()) {
      result.error = MoldError::trailing_bytes;
      return result;
    }
    if (!gate(result.header)) return result;

    position = header_size;
    for (std::uint16_t index = 0; index < result.header.message_count; ++index) {
      const auto length = read_u16(packet.data() + position);
      position += 2;
      callback(packet.subspan(position, length), result.header.sequence + index);
      position += length;
      ++result.messages_decoded;
    }
    return result;
  }

 private:
  static constexpr std::size_t header_size = 20;

  static std::uint16_t read_u16(const std::byte* bytes) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) |
                                      static_cast<std::uint16_t>(bytes[1]));
  }
  static std::uint64_t read_u64(const std::byte* bytes) noexcept {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index)
      value = (value << 8) | static_cast<std::uint8_t>(bytes[index]);
    return value;
  }
};

enum class SequenceStatus {
  initialized,
  in_order,
  gap,
  rewind,
  new_session,
};

struct SequenceResult {
  SequenceStatus status{SequenceStatus::initialized};
  std::uint64_t expected{};
  std::uint64_t received{};
  std::uint64_t missing{};
};

class SequenceTracker {
 public:
  SequenceResult observe(const MoldHeader& header) noexcept;
  void reset() noexcept;
  std::uint64_t expected() const noexcept { return expected_; }

 private:
  std::array<char, 10> session_{};
  std::uint64_t expected_{};
  bool initialized_{};
};

const char* to_string(MoldError error) noexcept;
const char* to_string(SequenceStatus status) noexcept;

}  // namespace itch
