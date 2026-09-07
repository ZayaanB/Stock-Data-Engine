#include "itch/moldudp64.hpp"

#include <algorithm>

namespace itch {

SequenceResult SequenceTracker::observe(const MoldHeader& header) noexcept {
  const auto advances =
      header.end_of_session() ? 0U : static_cast<std::uint64_t>(header.message_count);
  if (!initialized_) {
    session_ = header.session;
    expected_ = header.sequence + advances;
    initialized_ = true;
    return {SequenceStatus::initialized, header.sequence, header.sequence, 0};
  }
  if (header.session != session_) {
    session_ = header.session;
    const auto previous_expected = expected_;
    expected_ = header.sequence + advances;
    return {SequenceStatus::new_session, previous_expected, header.sequence, 0};
  }

  SequenceResult result{SequenceStatus::in_order, expected_, header.sequence, 0};
  if (header.sequence > expected_) {
    result.status = SequenceStatus::gap;
    result.missing = header.sequence - expected_;
  } else if (header.sequence < expected_) {
    result.status = SequenceStatus::rewind;
  }

  const auto packet_end = header.sequence + advances;
  if (packet_end > expected_) expected_ = packet_end;
  return result;
}

void SequenceTracker::reset() noexcept {
  session_ = {};
  expected_ = 0;
  initialized_ = false;
}

const char* to_string(MoldError error) noexcept {
  switch (error) {
    case MoldError::none:
      return "none";
    case MoldError::header_too_short:
      return "header too short";
    case MoldError::invalid_control_packet:
      return "invalid control packet";
    case MoldError::truncated_message_length:
      return "truncated message length";
    case MoldError::truncated_message:
      return "truncated message";
    case MoldError::trailing_bytes:
      return "trailing bytes";
  }
  return "unknown";
}

const char* to_string(SequenceStatus status) noexcept {
  switch (status) {
    case SequenceStatus::initialized:
      return "initialized";
    case SequenceStatus::in_order:
      return "in order";
    case SequenceStatus::gap:
      return "gap";
    case SequenceStatus::rewind:
      return "rewind";
    case SequenceStatus::new_session:
      return "new session";
  }
  return "unknown";
}

}  // namespace itch
