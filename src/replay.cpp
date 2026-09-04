#include "replay/replay.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <stdexcept>

namespace replay {
namespace {
double percentile(const std::vector<std::uint64_t>& values, double probability) {
  if (values.empty()) return 0.0;
  const auto index = static_cast<std::size_t>(probability * static_cast<double>(values.size() - 1));
  return static_cast<double>(values[index]);
}
}  // namespace

Result run_file(const std::filesystem::path& path, const Config& config) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open input file: " + path.string());
  }

  book::OrderBook order_book(config.max_orders, config.max_levels, config.symbol);
  itch::FramedDecoder decoder;
  std::vector<std::uint64_t> samples;
  samples.reserve(config.latency_samples);
  std::array<std::byte, 1 << 20> buffer{};
  std::uint64_t seen = 0;

  const auto start = std::chrono::steady_clock::now();
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count <= 0) break;

    const auto bytes = std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(count));
    if (!decoder.consume(bytes, [&](const itch::Message& message) {
          const bool should_sample = config.sample_every != 0 && seen % config.sample_every == 0 &&
                                     samples.size() < samples.capacity();
          const auto before = std::chrono::steady_clock::now();
          order_book.process(message);
          if (should_sample) {
            const auto elapsed = std::chrono::steady_clock::now() - before;
            samples.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
          }
          ++seen;
        })) {
      break;
    }
  }

  if (input.bad()) throw std::runtime_error("I/O error while reading: " + path.string());
  decoder.finish();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const double seconds = std::chrono::duration<double>(elapsed).count();
  std::sort(samples.begin(), samples.end());

  Result result{};
  result.messages = decoder.stats().decoded;
  result.seconds = seconds;
  result.messages_per_second =
      seconds != 0.0 ? static_cast<double>(result.messages) / seconds : 0.0;
  result.decoder = decoder.stats();
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
}  // namespace replay
