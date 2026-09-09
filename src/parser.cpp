#include <algorithm>
#include <cstring>

#include "itch/messages.hpp"

namespace itch {
namespace {

class Reader {
 public:
  explicit Reader(std::span<const std::byte> b) : b_(b) {}
  std::uint8_t u8() noexcept { return static_cast<std::uint8_t>(b_[pos_++]); }
  std::uint16_t u16() noexcept {
    auto a = u8();
    auto b = u8();
    return static_cast<std::uint16_t>((a << 8) | b);
  }
  std::uint32_t u32() noexcept {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | u8();
    return v;
  }
  std::uint64_t u48() noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | u8();
    return v;
  }
  std::uint64_t u64() noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | u8();
    return v;
  }
  template <std::size_t N>
  std::array<char, N> chars() noexcept {
    std::array<char, N> out{};
    for (auto& c : out) c = static_cast<char>(u8());
    return out;
  }
  Header header() noexcept { return {u16(), u16(), u48()}; }

 private:
  std::span<const std::byte> b_;
  std::size_t pos_{1};
};

bool read_side(Reader& r, Side& side) noexcept {
  const auto c = static_cast<char>(r.u8());
  if (c == 'B') {
    side = Side::buy;
    return true;
  }
  if (c == 'S') {
    side = Side::sell;
    return true;
  }
  return false;
}

}  // namespace

std::size_t expected_message_length(char t) noexcept {
  switch (t) {
    case 'S':
      return 12;
    case 'R':
      return 39;
    case 'H':
      return 25;
    case 'Y':
      return 20;
    case 'L':
      return 26;
    case 'V':
      return 35;
    case 'W':
      return 12;
    case 'K':
      return 28;
    case 'J':
      return 35;
    case 'h':
      return 21;
    case 'A':
      return 36;
    case 'F':
      return 40;
    case 'E':
      return 31;
    case 'C':
      return 36;
    case 'X':
      return 23;
    case 'D':
      return 19;
    case 'U':
      return 35;
    case 'P':
      return 44;
    case 'Q':
      return 40;
    case 'B':
      return 19;
    case 'I':
      return 50;
    case 'N':
      return 20;
    default:
      return 0;
  }
}

ParseResult parse_message(std::span<const std::byte> b) noexcept {
  if (b.empty()) return {ParseError::empty, {SystemEvent{}}};
  const char type = static_cast<char>(b[0]);
  const auto length = expected_message_length(type);
  if (!length) return {ParseError::unsupported_type, {SystemEvent{}}};
  if (b.size() != length) return {ParseError::wrong_length, {SystemEvent{}}};
  Reader r(b);
  const auto h = r.header();
  switch (type) {
    case 'S':
      return {ParseError::none, SystemEvent{h, static_cast<char>(r.u8())}};
    case 'R': {
      StockDirectory m{};
      m.header = h;
      m.stock = r.chars<8>();
      m.market_category = static_cast<char>(r.u8());
      m.financial_status = static_cast<char>(r.u8());
      m.round_lot_size = r.u32();
      m.round_lots_only = static_cast<char>(r.u8());
      m.issue_classification = static_cast<char>(r.u8());
      m.issue_subtype = r.chars<2>();
      m.authenticity = static_cast<char>(r.u8());
      m.short_sale_threshold = static_cast<char>(r.u8());
      m.ipo_flag = static_cast<char>(r.u8());
      m.luld_tier = static_cast<char>(r.u8());
      m.etp_flag = static_cast<char>(r.u8());
      m.etp_leverage_factor = r.u32();
      m.inverse_indicator = static_cast<char>(r.u8());
      return {ParseError::none, m};
    }
    case 'H': {
      StockTradingAction m{};
      m.header = h;
      m.stock = r.chars<8>();
      m.trading_state = static_cast<char>(r.u8());
      m.reserved = static_cast<char>(r.u8());
      m.reason = r.chars<4>();
      return {ParseError::none, m};
    }
    case 'Y': {
      RegShoRestriction m{};
      m.header = h;
      m.stock = r.chars<8>();
      m.action = static_cast<char>(r.u8());
      return {ParseError::none, m};
    }
    case 'L': {
      MarketParticipantPosition m{};
      m.header = h;
      m.mpid = r.chars<4>();
      m.stock = r.chars<8>();
      m.primary_market_maker = static_cast<char>(r.u8());
      m.market_maker_mode = static_cast<char>(r.u8());
      m.market_participant_state = static_cast<char>(r.u8());
      return {ParseError::none, m};
    }
    case 'V': {
      MwcbDeclineLevel m{};
      m.header = h;
      m.level_one = r.u64();
      m.level_two = r.u64();
      m.level_three = r.u64();
      return {ParseError::none, m};
    }
    case 'W':
      return {ParseError::none, MwcbStatus{h, static_cast<char>(r.u8())}};
    case 'K': {
      IpoQuotingPeriod m{};
      m.header = h;
      m.stock = r.chars<8>();
      m.release_time = r.u32();
      m.release_qualifier = static_cast<char>(r.u8());
      m.ipo_price = r.u32();
      return {ParseError::none, m};
    }
    case 'J': {
      LuldAuctionCollar m{};
      m.header = h;
      m.stock = r.chars<8>();
      m.reference_price = r.u32();
      m.upper_price = r.u32();
      m.lower_price = r.u32();
      m.extension = r.u32();
      return {ParseError::none, m};
    }
    case 'h': {
      OperationalHalt m{};
      m.header = h;
      m.stock = r.chars<8>();
      m.action = static_cast<char>(r.u8());
      return {ParseError::none, m};
    }
    case 'A': {
      AddOrder m{};
      m.header = h;
      m.order_id = r.u64();
      if (!read_side(r, m.side)) return {ParseError::invalid_side, {SystemEvent{}}};
      m.quantity = r.u32();
      m.stock = r.chars<8>();
      m.price = r.u32();
      return {ParseError::none, m};
    }
    case 'F': {
      AddOrderMpid m{};
      m.header = h;
      m.order_id = r.u64();
      if (!read_side(r, m.side)) return {ParseError::invalid_side, {SystemEvent{}}};
      m.quantity = r.u32();
      m.stock = r.chars<8>();
      m.price = r.u32();
      m.attribution = r.chars<4>();
      return {ParseError::none, m};
    }
    case 'E': {
      OrderExecuted m{};
      m.header = h;
      m.order_id = r.u64();
      m.executed = r.u32();
      m.match_number = r.u64();
      return {ParseError::none, m};
    }
    case 'C': {
      OrderExecutedWithPrice m{};
      m.header = h;
      m.order_id = r.u64();
      m.executed = r.u32();
      m.match_number = r.u64();
      m.printable = static_cast<char>(r.u8());
      m.execution_price = r.u32();
      return {ParseError::none, m};
    }
    case 'X': {
      OrderCancel m{};
      m.header = h;
      m.order_id = r.u64();
      m.canceled = r.u32();
      return {ParseError::none, m};
    }
    case 'D': {
      OrderDelete m{};
      m.header = h;
      m.order_id = r.u64();
      return {ParseError::none, m};
    }
    case 'U': {
      OrderReplace m{};
      m.header = h;
      m.original_order_id = r.u64();
      m.new_order_id = r.u64();
      m.quantity = r.u32();
      m.price = r.u32();
      return {ParseError::none, m};
    }
    case 'P': {
      Trade m{};
      m.header = h;
      m.order_id = r.u64();
      if (!read_side(r, m.side)) return {ParseError::invalid_side, {SystemEvent{}}};
      m.quantity = r.u32();
      m.stock = r.chars<8>();
      m.price = r.u32();
      m.match_number = r.u64();
      return {ParseError::none, m};
    }
    case 'Q': {
      CrossTrade m{};
      m.header = h;
      m.quantity = r.u64();
      m.stock = r.chars<8>();
      m.price = r.u32();
      m.match_number = r.u64();
      m.cross_type = static_cast<char>(r.u8());
      return {ParseError::none, m};
    }
    case 'B':
      return {ParseError::none, BrokenTrade{h, r.u64()}};
    case 'I': {
      Noii m{};
      m.header = h;
      m.paired_quantity = r.u64();
      m.imbalance_quantity = r.u64();
      m.imbalance_direction = static_cast<char>(r.u8());
      m.stock = r.chars<8>();
      m.far_price = r.u32();
      m.near_price = r.u32();
      m.current_reference_price = r.u32();
      m.cross_type = static_cast<char>(r.u8());
      m.price_variation_indicator = static_cast<char>(r.u8());
      return {ParseError::none, m};
    }
    case 'N': {
      RetailPriceImprovement m{};
      m.header = h;
      m.stock = r.chars<8>();
      m.interest_flag = static_cast<char>(r.u8());
      return {ParseError::none, m};
    }
    default:
      break;
  }
  return {ParseError::unsupported_type, {SystemEvent{}}};
}

}  // namespace itch
