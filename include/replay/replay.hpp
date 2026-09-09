#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "book/order_book.hpp"
#include "itch/parser.hpp"

namespace replay {

struct Config {
  std::size_t max_orders{1'000'000};
  std::size_t max_levels{100'000};
  std::size_t latency_samples{1'000'000};
  std::size_t sample_every{1};
  std::string symbol;
};
struct Percentiles {
  double p50_ns{}, p90_ns{}, p99_ns{}, p999_ns{}, max_ns{};
};
struct Result {
  std::uint64_t messages{};
  double seconds{}, messages_per_second{};
  Percentiles latency{};
  itch::DecoderStats decoder{};
  book::BookStats book{};
  book::Quote bid{}, ask{};
  std::uint64_t producer_spins{}, consumer_spins{};
};

Result run_file(const std::filesystem::path&, const Config&);
Result run_file_threaded(const std::filesystem::path&, const Config&);

}  // namespace replay
