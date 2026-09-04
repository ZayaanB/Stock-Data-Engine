#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "itch/messages.hpp"

namespace book {

enum class BookError {
  none,
  duplicate_order,
  unknown_order,
  invalid_quantity,
  order_pool_exhausted,
  lookup_full,
  level_pool_exhausted,
  wrong_symbol
};

struct Order;
struct PriceLevel {
  itch::Price price{};
  std::uint64_t total_quantity{};
  std::uint32_t order_count{};
  Order* head{};
  Order* tail{};
  std::int32_t left{-1}, right{-1}, parent{-1}, free_next{-1};
  std::int8_t height{1};
  bool active{};
};

struct Order {
  itch::OrderId id{};
  itch::Price price{};
  itch::Quantity quantity{};
  itch::Side side{};
  std::int32_t level{-1};
  Order* prev{};
  Order* next{};
  std::int32_t free_next{-1};
  bool active{};
};

struct Quote {
  itch::Price price{};
  std::uint64_t quantity{};
  std::uint32_t orders{};
  explicit operator bool() const noexcept { return orders != 0; }
};
struct DepthLevel {
  itch::Price price{};
  std::uint64_t quantity{};
  std::uint32_t orders{};
};
struct BookStats {
  std::uint64_t accepted{}, ignored{}, rejected{};
  std::size_t active_orders{}, active_levels{};
};

class OrderBook {
 public:
  OrderBook(std::size_t max_orders, std::size_t max_levels, std::string_view symbol = {});
  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;

  BookError add(itch::OrderId, itch::Side, itch::Quantity, itch::Price) noexcept;
  BookError execute(itch::OrderId, itch::Quantity) noexcept;
  BookError cancel(itch::OrderId, itch::Quantity) noexcept;
  BookError erase(itch::OrderId) noexcept;
  BookError replace(itch::OrderId old_id, itch::OrderId new_id, itch::Quantity,
                    itch::Price) noexcept;
  BookError process(const itch::Message&) noexcept;

  Quote best_bid() const noexcept;
  Quote best_ask() const noexcept;
  std::size_t depth(itch::Side, std::span<DepthLevel>) const noexcept;
  const Order* find(itch::OrderId) const noexcept;
  const BookStats& stats() const noexcept { return stats_; }
  std::string_view symbol() const noexcept { return symbol_; }

 private:
  struct LookupSlot {
    itch::OrderId id{};
    Order* order{};
    enum State : std::uint8_t { empty, occupied, tombstone } state{empty};
  };
  static constexpr std::int32_t none = -1;

  Order* acquire_order() noexcept;
  void release_order(Order*) noexcept;
  bool lookup_insert(itch::OrderId, Order*) noexcept;
  void lookup_erase(itch::OrderId) noexcept;
  Order* lookup_find(itch::OrderId) noexcept;
  const Order* lookup_find(itch::OrderId) const noexcept;
  std::size_t hash(itch::OrderId) const noexcept;

  std::int32_t level_find(std::int32_t root, itch::Price) const noexcept;
  std::int32_t level_acquire(itch::Price) noexcept;
  void level_release(std::int32_t) noexcept;
  std::int32_t level_get_or_add(itch::Side, itch::Price, BookError&) noexcept;
  void level_remove(itch::Side, std::int32_t) noexcept;
  void append(PriceLevel&, Order*) noexcept;
  void unlink(PriceLevel&, Order*) noexcept;
  BookError reduce(Order*, itch::Quantity) noexcept;

  int height(std::int32_t) const noexcept;
  void update(std::int32_t) noexcept;
  void replace_child(std::int32_t& root, std::int32_t parent, std::int32_t old_child,
                     std::int32_t new_child) noexcept;
  std::int32_t rotate_left(std::int32_t& root, std::int32_t) noexcept;
  std::int32_t rotate_right(std::int32_t& root, std::int32_t) noexcept;
  void rebalance_up(std::int32_t& root, std::int32_t) noexcept;
  std::int32_t extreme(std::int32_t root, bool maximum) const noexcept;
  bool symbol_matches(const itch::Stock&) const noexcept;

  std::vector<Order> orders_;
  std::vector<PriceLevel> levels_;
  std::vector<LookupSlot> lookup_;
  std::int32_t free_order_{none}, free_level_{none};
  std::int32_t bid_root_{none}, ask_root_{none};
  char symbol_[9]{};
  std::size_t symbol_length_{};
  BookStats stats_{};
};

const char* to_string(BookError) noexcept;

}  // namespace book
