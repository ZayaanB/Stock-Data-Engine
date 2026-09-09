#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace queue {

template <class T, std::size_t Capacity>
class SpscRing {
  static_assert(Capacity >= 2, "SPSC capacity must be at least two");
  static_assert((Capacity & (Capacity - 1)) == 0, "SPSC capacity must be a power of two");
  static_assert(std::is_nothrow_move_assignable_v<T>, "SPSC items must be nothrow move assignable");

 public:
  static constexpr std::size_t capacity() noexcept { return Capacity; }

  bool try_push(const T& value) noexcept {
    const auto head = producer_head_.load(std::memory_order_relaxed);
    if (head - consumer_tail_.load(std::memory_order_acquire) >= Capacity) return false;
    storage_[head & mask] = value;
    producer_head_.store(head + 1, std::memory_order_release);
    return true;
  }

  bool try_push(T&& value) noexcept {
    const auto head = producer_head_.load(std::memory_order_relaxed);
    if (head - consumer_tail_.load(std::memory_order_acquire) >= Capacity) return false;
    storage_[head & mask] = std::move(value);
    producer_head_.store(head + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(T& output) noexcept {
    const auto tail = consumer_tail_.load(std::memory_order_relaxed);
    if (tail == producer_head_.load(std::memory_order_acquire)) return false;
    output = std::move(storage_[tail & mask]);
    consumer_tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  bool empty() const noexcept {
    return consumer_tail_.load(std::memory_order_acquire) ==
           producer_head_.load(std::memory_order_acquire);
  }

  std::size_t size() const noexcept {
    return static_cast<std::size_t>(producer_head_.load(std::memory_order_acquire) -
                                    consumer_tail_.load(std::memory_order_acquire));
  }

 private:
  static constexpr std::uint64_t mask = Capacity - 1;
  static constexpr std::size_t cache_line = 64;

  alignas(cache_line) std::array<T, Capacity> storage_{};
  alignas(cache_line) std::atomic<std::uint64_t> producer_head_{};
  alignas(cache_line) std::atomic<std::uint64_t> consumer_tail_{};
};

}  // namespace queue
