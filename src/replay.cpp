#include "replay/replay.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>

#include "queue/spsc_ring.hpp"

namespace replay {
namespace {

double percentile(const std::vector<std::uint64_t>& values, double probability) {
  if (values.empty()) return 0.0;
  const auto index = static_cast<std::size_t>(probability * static_cast<double>(values.size() - 1));
  return static_cast<double>(values[index]);
}

void process_message(book::OrderBook& order_book, std::vector<std::uint64_t>& samples,
                     const Config& config, std::uint64_t seen, const itch::Message& message) {
  const bool should_sample = config.sample_every != 0 && seen % config.sample_every == 0 &&
                             samples.size() < samples.capacity();
  const auto before = std::chrono::steady_clock::now();
  order_book.process(message);
  if (should_sample) {
    const auto elapsed = std::chrono::steady_clock::now() - before;
    samples.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  }
}

Result make_result(const itch::FramedDecoder& decoder, const book::OrderBook& order_book,
                   std::vector<std::uint64_t>& samples, double seconds) {
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

}  // namespace

Result run_file(const std::filesystem::path& path, const Config& config) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open input file: " + path.string());

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
          process_message(order_book, samples, config, seen++, message);
        }))
      break;
  }

  if (input.bad()) throw std::runtime_error("I/O error while reading: " + path.string());
  decoder.finish();
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  return make_result(decoder, order_book, samples, seconds);
}

Result run_file_threaded(const std::filesystem::path& path, const Config& config) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open input file: " + path.string());

  constexpr std::size_t queue_capacity = 1 << 16;
  queue::SpscRing<itch::Message, queue_capacity> message_queue;
  book::OrderBook order_book(config.max_orders, config.max_levels, config.symbol);
  itch::FramedDecoder decoder;
  std::vector<std::uint64_t> samples;
  samples.reserve(config.latency_samples);
  std::atomic<bool> producer_done{false};
  bool input_error = false;
  std::uint64_t producer_spins = 0;
  std::uint64_t consumer_spins = 0;

  const auto start = std::chrono::steady_clock::now();
  std::thread producer([&] {
    std::array<std::byte, 1 << 20> buffer{};
    while (input) {
      input.read(reinterpret_cast<char*>(buffer.data()),
                 static_cast<std::streamsize>(buffer.size()));
      const auto count = input.gcount();
      if (count <= 0) break;
      const auto bytes = std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(count));
      if (!decoder.consume(bytes, [&](const itch::Message& message) {
            while (!message_queue.try_push(message)) {
              ++producer_spins;
              std::this_thread::yield();
            }
          }))
        break;
    }
    if (input.bad()) input_error = true;
    decoder.finish();
    producer_done.store(true, std::memory_order_release);
  });

  itch::Message message;
  std::uint64_t seen = 0;
  while (!producer_done.load(std::memory_order_acquire) || !message_queue.empty()) {
    if (message_queue.try_pop(message)) {
      process_message(order_book, samples, config, seen++, message);
    } else {
      ++consumer_spins;
      std::this_thread::yield();
    }
  }
  producer.join();
  if (input_error) throw std::runtime_error("I/O error while reading: " + path.string());

  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  auto result = make_result(decoder, order_book, samples, seconds);
  result.producer_spins = producer_spins;
  result.consumer_spins = consumer_spins;
  return result;
}

}  // namespace replay
