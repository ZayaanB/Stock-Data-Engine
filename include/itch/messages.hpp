#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>

namespace itch {

using OrderId = std::uint64_t;
using Price = std::uint32_t;  // ITCH prices are fixed point with four decimals.
using Quantity = std::uint32_t;

enum class Side : std::uint8_t { buy, sell };

struct Header {
  std::uint16_t stock_locate{};
  std::uint16_t tracking_number{};
  std::uint64_t timestamp{};  // 48-bit nanoseconds since midnight on the wire.
};

using Stock = std::array<char, 8>;

constexpr std::string_view stock_view(const Stock& stock) noexcept {
  std::size_t n = stock.size();
  while (n && stock[n - 1] == ' ') --n;
  return {stock.data(), n};
}

struct SystemEvent {
  Header header;
  char event_code{};
};
struct StockDirectory {
  Header header;
  Stock stock{};
  char market_category{}, financial_status{};
  std::uint32_t round_lot_size{};
  char round_lots_only{}, issue_classification{};
  std::array<char, 2> issue_subtype{};
  char authenticity{}, short_sale_threshold{}, ipo_flag{}, luld_tier{}, etp_flag{};
  std::uint32_t etp_leverage_factor{};
  char inverse_indicator{};
};
struct StockTradingAction {
  Header header;
  Stock stock{};
  char trading_state{}, reserved{};
  std::array<char, 4> reason{};
};
struct RegShoRestriction {
  Header header;
  Stock stock{};
  char action{};
};
struct MarketParticipantPosition {
  Header header;
  std::array<char, 4> mpid{};
  Stock stock{};
  char primary_market_maker{}, market_maker_mode{}, market_participant_state{};
};
struct MwcbDeclineLevel {
  Header header;
  std::uint64_t level_one{}, level_two{}, level_three{};
};
struct MwcbStatus {
  Header header;
  char breached_level{};
};
struct IpoQuotingPeriod {
  Header header;
  Stock stock{};
  std::uint32_t release_time{};
  char release_qualifier{};
  Price ipo_price{};
};
struct LuldAuctionCollar {
  Header header;
  Stock stock{};
  Price reference_price{}, upper_price{}, lower_price{};
  std::uint32_t extension{};
};
struct OperationalHalt {
  Header header;
  Stock stock{};
  char action{};
};
struct AddOrder {
  Header header;
  OrderId order_id{};
  Side side{};
  Quantity quantity{};
  Stock stock{};
  Price price{};
};
struct AddOrderMpid : AddOrder {
  std::array<char, 4> attribution{};
};
struct OrderExecuted {
  Header header;
  OrderId order_id{};
  Quantity executed{};
  std::uint64_t match_number{};
};
struct OrderExecutedWithPrice : OrderExecuted {
  char printable{};
  Price execution_price{};
};
struct OrderCancel {
  Header header;
  OrderId order_id{};
  Quantity canceled{};
};
struct OrderDelete {
  Header header;
  OrderId order_id{};
};
struct OrderReplace {
  Header header;
  OrderId original_order_id{}, new_order_id{};
  Quantity quantity{};
  Price price{};
};
struct Trade {
  Header header;
  OrderId order_id{};
  Side side{};
  Quantity quantity{};
  Stock stock{};
  Price price{};
  std::uint64_t match_number{};
};
struct CrossTrade {
  Header header;
  std::uint64_t quantity{};
  Stock stock{};
  Price price{};
  std::uint64_t match_number{};
  char cross_type{};
};
struct BrokenTrade {
  Header header;
  std::uint64_t match_number{};
};
struct Noii {
  Header header;
  std::uint64_t paired_quantity{}, imbalance_quantity{};
  char imbalance_direction{};
  Stock stock{};
  Price far_price{}, near_price{}, current_reference_price{};
  char cross_type{}, price_variation_indicator{};
};
struct RetailPriceImprovement {
  Header header;
  Stock stock{};
  char interest_flag{};
};

using Message =
    std::variant<SystemEvent, StockDirectory, StockTradingAction, RegShoRestriction,
                 MarketParticipantPosition, MwcbDeclineLevel, MwcbStatus, IpoQuotingPeriod,
                 LuldAuctionCollar, OperationalHalt, AddOrder, AddOrderMpid, OrderExecuted,
                 OrderExecutedWithPrice, OrderCancel, OrderDelete, OrderReplace, Trade, CrossTrade,
                 BrokenTrade, Noii, RetailPriceImprovement>;

enum class ParseError { none, empty, unsupported_type, wrong_length, invalid_side };
struct ParseResult {
  ParseError error{ParseError::none};
  Message message{SystemEvent{}};
  explicit operator bool() const noexcept { return error == ParseError::none; }
};

ParseResult parse_message(std::span<const std::byte> bytes) noexcept;
std::size_t expected_message_length(char type) noexcept;

}  // namespace itch
