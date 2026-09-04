#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "itch/messages.hpp"

namespace itch {

struct DecoderStats {
  std::uint64_t decoded{}, malformed{}, unsupported{};
};

// Decodes the standard ITCH file framing: a two-byte big-endian length followed by one message.
// Partial frames may be split across consume() calls. The callback must not retain the
// span/message.
class FramedDecoder {
 public:
  static constexpr std::size_t max_message_size = 64;

  template <class Callback>
  bool consume(std::span<const std::byte> input, Callback&& callback) noexcept {
    while (!input.empty()) {
      if (length_bytes_ < 2) {
        length_buf_[length_bytes_++] = input.front();
        input = input.subspan(1);
        if (length_bytes_ != 2) continue;
        expected_ = (static_cast<std::uint16_t>(length_buf_[0]) << 8) |
                    static_cast<std::uint16_t>(length_buf_[1]);
        if (expected_ == 0 || expected_ > max_message_size) {
          ++stats_.malformed;
          reset();
          return false;
        }
      }
      const auto take = std::min<std::size_t>(expected_ - used_, input.size());
      std::copy_n(input.begin(), take, message_buf_.begin() + static_cast<std::ptrdiff_t>(used_));
      used_ += take;
      input = input.subspan(take);
      if (used_ == expected_) {
        auto result = parse_message({message_buf_.data(), expected_});
        if (result) {
          ++stats_.decoded;
          callback(result.message);
        } else if (result.error == ParseError::unsupported_type)
          ++stats_.unsupported;
        else
          ++stats_.malformed;
        reset();
      }
    }
    return true;
  }

  bool finish() noexcept {
    if (length_bytes_ == 0 && used_ == 0) return true;
    ++stats_.malformed;
    reset();
    return false;
  }
  const DecoderStats& stats() const noexcept { return stats_; }

 private:
  void reset() noexcept {
    length_bytes_ = 0;
    expected_ = 0;
    used_ = 0;
  }
  std::array<std::byte, 2> length_buf_{};
  std::array<std::byte, max_message_size> message_buf_{};
  std::size_t length_bytes_{}, expected_{}, used_{};
  DecoderStats stats_{};
};

}  // namespace itch
