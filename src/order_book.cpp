#include "book/order_book.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace book {
namespace {

std::size_t validate_capacity(std::size_t value, const char* name) {
  constexpr auto max_index = static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
  if (value == 0 || value > max_index)
    throw std::invalid_argument(std::string(name) + " must be between 1 and INT32_MAX");
  return value;
}

}  // namespace

OrderBook::OrderBook(std::size_t max_orders, std::size_t max_levels, std::string_view symbol)
    : orders_(validate_capacity(max_orders, "max_orders")),
      levels_(validate_capacity(max_levels, "max_levels")) {
  if (symbol.size() > 8) throw std::invalid_argument("ITCH symbols cannot exceed 8 characters");
  const std::size_t lookup_size = std::max<std::size_t>(8, max_orders * 2 + 1);
  lookup_.resize(lookup_size);
  for (std::size_t i = 0; i < orders_.size(); ++i)
    orders_[i].free_next = i + 1 < orders_.size() ? static_cast<std::int32_t>(i + 1) : none;
  for (std::size_t i = 0; i < levels_.size(); ++i)
    levels_[i].free_next = i + 1 < levels_.size() ? static_cast<std::int32_t>(i + 1) : none;
  free_order_ = orders_.empty() ? none : 0;
  free_level_ = levels_.empty() ? none : 0;
  symbol_length_ = std::min<std::size_t>(8, symbol.size());
  if (symbol_length_ != 0) std::memcpy(symbol_, symbol.data(), symbol_length_);
}

std::size_t OrderBook::hash(itch::OrderId x) const noexcept {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return static_cast<std::size_t>(x % lookup_.size());
}

Order* OrderBook::lookup_find(itch::OrderId id) noexcept {
  return const_cast<Order*>(std::as_const(*this).lookup_find(id));
}
const Order* OrderBook::lookup_find(itch::OrderId id) const noexcept {
  auto pos = hash(id);
  for (std::size_t n = 0; n < lookup_.size(); ++n, pos = (pos + 1) % lookup_.size()) {
    const auto& s = lookup_[pos];
    if (s.state == LookupSlot::empty) return nullptr;
    if (s.state == LookupSlot::occupied && s.id == id) return s.order;
  }
  return nullptr;
}
const Order* OrderBook::find(itch::OrderId id) const noexcept { return lookup_find(id); }

bool OrderBook::lookup_insert(itch::OrderId id, Order* order) noexcept {
  auto pos = hash(id), first_tomb = lookup_.size();
  for (std::size_t n = 0; n < lookup_.size(); ++n, pos = (pos + 1) % lookup_.size()) {
    auto& s = lookup_[pos];
    if (s.state == LookupSlot::occupied && s.id == id) return false;
    if (s.state == LookupSlot::tombstone && first_tomb == lookup_.size()) first_tomb = pos;
    if (s.state == LookupSlot::empty) {
      auto& out = lookup_[first_tomb == lookup_.size() ? pos : first_tomb];
      out = {id, order, LookupSlot::occupied};
      return true;
    }
  }
  if (first_tomb != lookup_.size()) {
    lookup_[first_tomb] = {id, order, LookupSlot::occupied};
    return true;
  }
  return false;
}
void OrderBook::lookup_erase(itch::OrderId id) noexcept {
  auto pos = hash(id);
  for (std::size_t n = 0; n < lookup_.size(); ++n, pos = (pos + 1) % lookup_.size()) {
    auto& s = lookup_[pos];
    if (s.state == LookupSlot::empty) return;
    if (s.state == LookupSlot::occupied && s.id == id) {
      // Backward-shift deletion preserves probe chains without accumulating tombstones.
      auto hole = pos;
      for (auto scan = (pos + 1) % lookup_.size(); lookup_[scan].state != LookupSlot::empty;
           scan = (scan + 1) % lookup_.size()) {
        const auto home = hash(lookup_[scan].id);
        const bool crosses =
            hole <= scan ? (home <= hole || home > scan) : (home <= hole && home > scan);
        if (crosses) {
          lookup_[hole] = lookup_[scan];
          hole = scan;
        }
      }
      lookup_[hole] = {};
      return;
    }
  }
}

Order* OrderBook::acquire_order() noexcept {
  if (free_order_ == none) return nullptr;
  auto* o = &orders_[free_order_];
  free_order_ = o->free_next;
  *o = {};
  o->level = none;
  o->free_next = none;
  o->active = true;
  return o;
}
void OrderBook::release_order(Order* o) noexcept {
  auto i = static_cast<std::int32_t>(o - orders_.data());
  *o = {};
  o->level = none;
  o->free_next = free_order_;
  free_order_ = i;
}

int OrderBook::height(std::int32_t i) const noexcept { return i == none ? 0 : levels_[i].height; }
void OrderBook::update(std::int32_t i) noexcept {
  levels_[i].height =
      static_cast<std::int8_t>(1 + std::max(height(levels_[i].left), height(levels_[i].right)));
}
void OrderBook::replace_child(std::int32_t& root, std::int32_t p, std::int32_t oldc,
                              std::int32_t newc) noexcept {
  if (p == none)
    root = newc;
  else if (levels_[p].left == oldc)
    levels_[p].left = newc;
  else
    levels_[p].right = newc;
  if (newc != none) levels_[newc].parent = p;
}
std::int32_t OrderBook::rotate_left(std::int32_t& root, std::int32_t x) noexcept {
  auto y = levels_[x].right, beta = levels_[y].left, p = levels_[x].parent;
  replace_child(root, p, x, y);
  levels_[y].left = x;
  levels_[x].parent = y;
  levels_[x].right = beta;
  if (beta != none) levels_[beta].parent = x;
  update(x);
  update(y);
  return y;
}
std::int32_t OrderBook::rotate_right(std::int32_t& root, std::int32_t y) noexcept {
  auto x = levels_[y].left, beta = levels_[x].right, p = levels_[y].parent;
  replace_child(root, p, y, x);
  levels_[x].right = y;
  levels_[y].parent = x;
  levels_[y].left = beta;
  if (beta != none) levels_[beta].parent = y;
  update(y);
  update(x);
  return x;
}
void OrderBook::rebalance_up(std::int32_t& root, std::int32_t i) noexcept {
  while (i != none) {
    update(i);
    auto balance = height(levels_[i].left) - height(levels_[i].right);
    std::int32_t top = i;
    if (balance > 1) {
      if (height(levels_[levels_[i].left].left) < height(levels_[levels_[i].left].right))
        rotate_left(root, levels_[i].left);
      top = rotate_right(root, i);
    } else if (balance < -1) {
      if (height(levels_[levels_[i].right].right) < height(levels_[levels_[i].right].left))
        rotate_right(root, levels_[i].right);
      top = rotate_left(root, i);
    }
    i = levels_[top].parent;
  }
}
std::int32_t OrderBook::level_find(std::int32_t i, itch::Price p) const noexcept {
  while (i != none) {
    if (p == levels_[i].price) return i;
    i = p < levels_[i].price ? levels_[i].left : levels_[i].right;
  }
  return none;
}
std::int32_t OrderBook::level_acquire(itch::Price p) noexcept {
  if (free_level_ == none) return none;
  auto i = free_level_;
  free_level_ = levels_[i].free_next;
  levels_[i] = {};
  levels_[i].price = p;
  levels_[i].left = levels_[i].right = levels_[i].parent = levels_[i].free_next = none;
  levels_[i].height = 1;
  levels_[i].active = true;
  ++stats_.active_levels;
  return i;
}
void OrderBook::level_release(std::int32_t i) noexcept {
  levels_[i] = {};
  levels_[i].left = levels_[i].right = levels_[i].parent = none;
  levels_[i].free_next = free_level_;
  free_level_ = i;
  --stats_.active_levels;
}
std::int32_t OrderBook::level_get_or_add(itch::Side side, itch::Price p, BookError& err) noexcept {
  auto& root = side == itch::Side::buy ? bid_root_ : ask_root_;
  auto found = level_find(root, p);
  if (found != none) return found;
  auto n = level_acquire(p);
  if (n == none) {
    err = BookError::level_pool_exhausted;
    return none;
  }
  if (root == none) {
    root = n;
    return n;
  }
  auto cur = root, parent = none;
  while (cur != none) {
    parent = cur;
    cur = p < levels_[cur].price ? levels_[cur].left : levels_[cur].right;
  }
  levels_[n].parent = parent;
  if (p < levels_[parent].price)
    levels_[parent].left = n;
  else
    levels_[parent].right = n;
  rebalance_up(root, parent);
  return n;
}
std::int32_t OrderBook::extreme(std::int32_t i, bool maximum) const noexcept {
  if (i == none) return none;
  for (;;) {
    auto next = maximum ? levels_[i].right : levels_[i].left;
    if (next == none) return i;
    i = next;
  }
}
void OrderBook::level_remove(itch::Side side, std::int32_t z) noexcept {
  auto& root = side == itch::Side::buy ? bid_root_ : ask_root_;
  std::int32_t rebalance = none;
  if (levels_[z].left == none) {
    rebalance = levels_[z].parent;
    replace_child(root, levels_[z].parent, z, levels_[z].right);
  } else if (levels_[z].right == none) {
    rebalance = levels_[z].parent;
    replace_child(root, levels_[z].parent, z, levels_[z].left);
  } else {
    auto y = extreme(levels_[z].right, false);
    auto old_parent = levels_[y].parent;
    if (old_parent != z) {
      replace_child(root, old_parent, y, levels_[y].right);
      levels_[y].right = levels_[z].right;
      levels_[levels_[y].right].parent = y;
      rebalance = old_parent;
    } else
      rebalance = y;
    replace_child(root, levels_[z].parent, z, y);
    levels_[y].left = levels_[z].left;
    levels_[levels_[y].left].parent = y;
    update(y);
  }
  level_release(z);
  if (rebalance != none) rebalance_up(root, rebalance);
}

void OrderBook::append(PriceLevel& l, Order* o) noexcept {
  o->prev = l.tail;
  o->next = nullptr;
  if (l.tail)
    l.tail->next = o;
  else
    l.head = o;
  l.tail = o;
  l.total_quantity += o->quantity;
  ++l.order_count;
}
void OrderBook::unlink(PriceLevel& l, Order* o) noexcept {
  if (o->prev)
    o->prev->next = o->next;
  else
    l.head = o->next;
  if (o->next)
    o->next->prev = o->prev;
  else
    l.tail = o->prev;
  l.total_quantity -= o->quantity;
  --l.order_count;
}

BookError OrderBook::add(itch::OrderId id, itch::Side side, itch::Quantity q,
                         itch::Price p) noexcept {
  if (q == 0) return BookError::invalid_quantity;
  if (lookup_find(id)) return BookError::duplicate_order;
  auto* o = acquire_order();
  if (!o) return BookError::order_pool_exhausted;
  BookError err = BookError::none;
  auto li = level_get_or_add(side, p, err);
  if (li == none) {
    release_order(o);
    return err;
  }
  o->id = id;
  o->side = side;
  o->quantity = q;
  o->price = p;
  o->level = li;
  if (!lookup_insert(id, o)) {
    if (levels_[li].order_count == 0) level_remove(side, li);
    release_order(o);
    return BookError::lookup_full;
  }
  append(levels_[li], o);
  ++stats_.active_orders;
  return BookError::none;
}
BookError OrderBook::reduce(Order* o, itch::Quantity q) noexcept {
  if (q == 0 || q > o->quantity) return BookError::invalid_quantity;
  auto& l = levels_[o->level];
  l.total_quantity -= q;
  o->quantity -= q;
  if (o->quantity == 0) {
    auto side = o->side;
    auto li = o->level;
    unlink(l, o);  // unlink subtracts zero after quantity reached zero
    lookup_erase(o->id);
    release_order(o);
    --stats_.active_orders;
    if (l.order_count == 0) level_remove(side, li);
  }
  return BookError::none;
}
BookError OrderBook::execute(itch::OrderId id, itch::Quantity q) noexcept {
  auto* o = lookup_find(id);
  return o ? reduce(o, q) : BookError::unknown_order;
}
BookError OrderBook::cancel(itch::OrderId id, itch::Quantity q) noexcept { return execute(id, q); }
BookError OrderBook::erase(itch::OrderId id) noexcept {
  auto* o = lookup_find(id);
  if (!o) return BookError::unknown_order;
  auto side = o->side;
  auto li = o->level;
  auto& l = levels_[li];
  unlink(l, o);
  lookup_erase(id);
  release_order(o);
  --stats_.active_orders;
  if (l.order_count == 0) level_remove(side, li);
  return BookError::none;
}
BookError OrderBook::replace(itch::OrderId oldid, itch::OrderId newid, itch::Quantity q,
                             itch::Price p) noexcept {
  auto* old = lookup_find(oldid);
  if (!old) return BookError::unknown_order;
  if (q == 0) return BookError::invalid_quantity;
  if (newid != oldid && lookup_find(newid)) return BookError::duplicate_order;
  auto side = old->side;
  auto root = side == itch::Side::buy ? bid_root_ : ask_root_;
  if (p != old->price && level_find(root, p) == none && free_level_ == none)
    return BookError::level_pool_exhausted;
  auto e = erase(oldid);
  if (e != BookError::none) return e;
  return add(newid, side, q, p);
}

bool OrderBook::symbol_matches(const itch::Stock& s) const noexcept {
  return symbol_length_ == 0 || (itch::stock_view(s).size() == symbol_length_ &&
                                 std::memcmp(symbol_, s.data(), symbol_length_) == 0);
}
BookError OrderBook::process(const itch::Message& msg) noexcept {
  BookError e = BookError::none;
  std::visit(
      [&](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, itch::AddOrder> || std::is_same_v<T, itch::AddOrderMpid>) {
          if (!symbol_matches(m.stock))
            e = BookError::wrong_symbol;
          else
            e = add(m.order_id, m.side, m.quantity, m.price);
        } else if constexpr (std::is_same_v<T, itch::OrderExecuted> ||
                             std::is_same_v<T, itch::OrderExecutedWithPrice>)
          e = execute(m.order_id, m.executed);
        else if constexpr (std::is_same_v<T, itch::OrderCancel>)
          e = cancel(m.order_id, m.canceled);
        else if constexpr (std::is_same_v<T, itch::OrderDelete>)
          e = erase(m.order_id);
        else if constexpr (std::is_same_v<T, itch::OrderReplace>)
          e = replace(m.original_order_id, m.new_order_id, m.quantity, m.price);
      },
      msg);
  if (e == BookError::none)
    ++stats_.accepted;
  else if (e == BookError::wrong_symbol || e == BookError::unknown_order)
    ++stats_.ignored;
  else
    ++stats_.rejected;
  return e;
}

Quote OrderBook::best_bid() const noexcept {
  auto i = extreme(bid_root_, true);
  return i == none ? Quote{}
                   : Quote{levels_[i].price, levels_[i].total_quantity, levels_[i].order_count};
}
Quote OrderBook::best_ask() const noexcept {
  auto i = extreme(ask_root_, false);
  return i == none ? Quote{}
                   : Quote{levels_[i].price, levels_[i].total_quantity, levels_[i].order_count};
}
std::size_t OrderBook::depth(itch::Side side, std::span<DepthLevel> out) const noexcept {
  auto i = extreme(side == itch::Side::buy ? bid_root_ : ask_root_, side == itch::Side::buy);
  std::size_t n = 0;
  while (i != none && n < out.size()) {
    const auto& l = levels_[i];
    out[n++] = {l.price, l.total_quantity, l.order_count};
    if (side == itch::Side::buy) {
      if (l.left != none)
        i = extreme(l.left, true);
      else {
        auto cur = i, p = l.parent;
        while (p != none && levels_[p].left == cur) {
          cur = p;
          p = levels_[p].parent;
        }
        i = p;
      }
    } else {
      if (l.right != none)
        i = extreme(l.right, false);
      else {
        auto cur = i, p = l.parent;
        while (p != none && levels_[p].right == cur) {
          cur = p;
          p = levels_[p].parent;
        }
        i = p;
      }
    }
  }
  return n;
}

const char* to_string(BookError e) noexcept {
  switch (e) {
    case BookError::none:
      return "none";
    case BookError::duplicate_order:
      return "duplicate order";
    case BookError::unknown_order:
      return "unknown order";
    case BookError::invalid_quantity:
      return "invalid quantity";
    case BookError::order_pool_exhausted:
      return "order pool exhausted";
    case BookError::lookup_full:
      return "lookup full";
    case BookError::level_pool_exhausted:
      return "level pool exhausted";
    case BookError::wrong_symbol:
      return "wrong symbol";
  }
  return "unknown";
}

}  // namespace book
