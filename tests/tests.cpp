#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "book/order_book.hpp"
#include "itch/moldudp64.hpp"
#include "itch/parser.hpp"
#include "queue/spsc_ring.hpp"
#include "replay/pcap.hpp"
#include "replay/replay.hpp"

static std::atomic<std::uint64_t> allocation_count{0};
void* operator new(std::size_t n) {
  ++allocation_count;
  if (void* p = std::malloc(n)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
int failures = 0;
#define CHECK(x)                                                              \
  do {                                                                        \
    if (!(x)) {                                                               \
      std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #x "\n"; \
      ++failures;                                                             \
    }                                                                         \
  } while (0)

class Bytes {
 public:
  void u8(std::uint8_t value) { data.push_back(static_cast<std::byte>(value)); }
  void u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value >> 8));
    u8(static_cast<std::uint8_t>(value));
  }
  void little_u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value));
    u8(static_cast<std::uint8_t>(value >> 8));
  }
  void little_u32(std::uint32_t value) {
    for (int shift = 0; shift <= 24; shift += 8) u8(static_cast<std::uint8_t>(value >> shift));
  }
  void u32(std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) u8(static_cast<std::uint8_t>(value >> shift));
  }
  void u48(std::uint64_t value) {
    for (int shift = 40; shift >= 0; shift -= 8) u8(static_cast<std::uint8_t>(value >> shift));
  }
  void u64(std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) u8(static_cast<std::uint8_t>(value >> shift));
  }
  void chars(const char* value, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) u8(static_cast<std::uint8_t>(value[index]));
  }
  void header(char type) {
    u8(static_cast<std::uint8_t>(type));
    u16(7);
    u16(9);
    u48(123'456);
  }

  std::vector<std::byte> data;
};

void parser_tests() {
  Bytes b;
  b.header('A');
  b.u64(42);
  b.u8('B');
  b.u32(500);
  b.chars("AAPL    ", 8);
  b.u32(1'842'100);
  auto r = itch::parse_message(b.data);
  CHECK(r);
  CHECK(std::holds_alternative<itch::AddOrder>(r.message));
  auto m = std::get<itch::AddOrder>(r.message);
  CHECK(m.order_id == 42);
  CHECK(m.quantity == 500);
  CHECK(m.price == 1'842'100);
  CHECK(m.header.timestamp == 123456);
  CHECK(itch::stock_view(m.stock) == "AAPL");
  const auto valid_add = b.data;
  b.data.pop_back();
  CHECK(itch::parse_message(b.data).error == itch::ParseError::wrong_length);
  Bytes bad;
  bad.header('A');
  bad.u64(1);
  bad.u8('Z');
  bad.u32(1);
  bad.chars("AAPL    ", 8);
  bad.u32(1);
  CHECK(itch::parse_message(bad.data).error == itch::ParseError::invalid_side);
  std::vector<std::byte> framed{std::byte{0}, std::byte{36}};
  framed.insert(framed.end(), b.data.begin(), b.data.end());  // deliberately incomplete
  itch::FramedDecoder d;
  int called = 0;
  d.consume(std::span(framed).first(5), [&](auto&) { ++called; });
  d.consume(std::span(framed).subspan(5), [&](auto&) { ++called; });
  CHECK(called == 0);
  CHECK(!d.finish());
  CHECK(d.stats().malformed == 1);
  std::vector<std::byte> valid_frame{std::byte{0}, std::byte{36}};
  valid_frame.insert(valid_frame.end(), valid_add.begin(), valid_add.end());
  itch::FramedDecoder fragmented;
  fragmented.consume(std::span(valid_frame).first(7), [&](auto&) { ++called; });
  fragmented.consume(std::span(valid_frame).subspan(7), [&](auto&) { ++called; });
  CHECK(fragmented.finish());
  CHECK(fragmented.stats().decoded == 1);
  CHECK(called == 1);
  CHECK(itch::expected_message_length('S') == 12);
  CHECK(itch::expected_message_length('R') == 39);
  CHECK(itch::expected_message_length('H') == 25);
  CHECK(itch::expected_message_length('Y') == 20);
  CHECK(itch::expected_message_length('L') == 26);
  CHECK(itch::expected_message_length('V') == 35);
  CHECK(itch::expected_message_length('W') == 12);
  CHECK(itch::expected_message_length('K') == 28);
  CHECK(itch::expected_message_length('J') == 35);
  CHECK(itch::expected_message_length('h') == 21);
  CHECK(itch::expected_message_length('F') == 40);
  CHECK(itch::expected_message_length('E') == 31);
  CHECK(itch::expected_message_length('C') == 36);
  CHECK(itch::expected_message_length('X') == 23);
  CHECK(itch::expected_message_length('D') == 19);
  CHECK(itch::expected_message_length('U') == 35);
  CHECK(itch::expected_message_length('P') == 44);
  CHECK(itch::expected_message_length('Q') == 40);
  CHECK(itch::expected_message_length('B') == 19);
  CHECK(itch::expected_message_length('I') == 50);
  CHECK(itch::expected_message_length('N') == 20);
  for (const char type : std::string("SRHYLVWKJhAFECXDUPQBIN")) {
    std::vector<std::byte> message(itch::expected_message_length(type));
    message[0] = static_cast<std::byte>(type);
    if (type == 'A' || type == 'F' || type == 'P') message[19] = std::byte{'B'};
    CHECK(itch::parse_message(message));
  }
  std::array<std::byte, 1> unsupported{std::byte{'Z'}};
  CHECK(itch::parse_message(unsupported).error == itch::ParseError::unsupported_type);
}

void mold_tests() {
  Bytes system_event;
  system_event.header('S');
  system_event.u8('O');

  Bytes packet;
  packet.chars("SESSION001", 10);
  packet.u64(100);
  packet.u16(2);
  packet.u16(static_cast<std::uint16_t>(system_event.data.size()));
  packet.data.insert(packet.data.end(), system_event.data.begin(), system_event.data.end());
  packet.u16(static_cast<std::uint16_t>(system_event.data.size()));
  packet.data.insert(packet.data.end(), system_event.data.begin(), system_event.data.end());

  itch::MoldDecoder decoder;
  std::uint64_t expected_sequence = 100;
  std::size_t callbacks = 0;
  const auto result =
      decoder.decode(packet.data, [&](std::span<const std::byte> message, std::uint64_t sequence) {
        CHECK(sequence == expected_sequence++);
        CHECK(itch::parse_message(message));
        ++callbacks;
      });
  CHECK(result);
  CHECK(result.messages_decoded == 2);
  CHECK(callbacks == 2);
  const auto callbacks_before_gate = callbacks;
  const auto gated = decoder.decode(
      packet.data, [&](auto, auto) { ++callbacks; }, [](const itch::MoldHeader&) { return false; });
  CHECK(gated);
  CHECK(gated.messages_decoded == 0);
  CHECK(callbacks == callbacks_before_gate);

  itch::SequenceTracker tracker;
  CHECK(tracker.observe(result.header).status == itch::SequenceStatus::initialized);
  auto next = result.header;
  next.sequence = 102;
  CHECK(tracker.observe(next).status == itch::SequenceStatus::in_order);
  next.sequence = 106;
  const auto gap = tracker.observe(next);
  CHECK(gap.status == itch::SequenceStatus::gap);
  CHECK(gap.missing == 2);
  next.sequence = 104;
  CHECK(tracker.observe(next).status == itch::SequenceStatus::rewind);
  next.session[0] = 'X';
  CHECK(tracker.observe(next).status == itch::SequenceStatus::new_session);

  auto truncated = packet.data;
  truncated.pop_back();
  CHECK(decoder.decode(truncated, [](auto, auto) {}).error == itch::MoldError::truncated_message);
  auto trailing = packet.data;
  trailing.push_back(std::byte{});
  CHECK(decoder.decode(trailing, [](auto, auto) {}).error == itch::MoldError::trailing_bytes);

  Bytes heartbeat;
  heartbeat.chars("SESSION001", 10);
  heartbeat.u64(102);
  heartbeat.u16(0);
  CHECK(decoder.decode(heartbeat.data, [](auto, auto) {}).header.heartbeat());
  heartbeat.data.push_back(std::byte{});
  CHECK(decoder.decode(heartbeat.data, [](auto, auto) {}).error ==
        itch::MoldError::invalid_control_packet);

  const auto ip_length = static_cast<std::uint16_t>(20 + 8 + packet.data.size());
  const auto udp_length = static_cast<std::uint16_t>(8 + packet.data.size());
  std::vector<std::byte> ethernet(14 + 20 + 8);
  ethernet[12] = std::byte{0x08};
  ethernet[13] = std::byte{0x00};
  ethernet[14] = std::byte{0x45};
  ethernet[16] = static_cast<std::byte>(ip_length >> 8);
  ethernet[17] = static_cast<std::byte>(ip_length);
  ethernet[23] = std::byte{17};
  ethernet[38] = static_cast<std::byte>(udp_length >> 8);
  ethernet[39] = static_cast<std::byte>(udp_length);
  ethernet.insert(ethernet.end(), packet.data.begin(), packet.data.end());
  const auto udp = replay::extract_udp_payload(ethernet, 1);
  CHECK(udp);
  CHECK(udp.bytes.size() == packet.data.size());
  CHECK(decoder.decode(udp.bytes, [](auto, auto) {}));

  Bytes pcap;
  pcap.little_u32(0xa1b2c3d4);
  pcap.little_u16(2);
  pcap.little_u16(4);
  pcap.little_u32(0);
  pcap.little_u32(0);
  pcap.little_u32(65'535);
  pcap.little_u32(1);
  pcap.little_u32(1);
  pcap.little_u32(0);
  pcap.little_u32(static_cast<std::uint32_t>(ethernet.size()));
  pcap.little_u32(static_cast<std::uint32_t>(ethernet.size()));
  pcap.data.insert(pcap.data.end(), ethernet.begin(), ethernet.end());
  const auto path = std::filesystem::temp_directory_path() / "itch-pcap-test.pcap";
  {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(pcap.data.data()),
                 static_cast<std::streamsize>(pcap.data.size()));
  }
  replay::Config config;
  config.symbol = "AAPL";
  config.max_orders = 8;
  config.max_levels = 8;
  config.latency_samples = 0;
  config.sample_every = 0;
  replay::PcapStats stats;
  const auto replay_result = replay::run_pcap(path, config, stats);
  CHECK(replay_result.messages == 2);
  CHECK(stats.packets == 1);
  CHECK(stats.mold_packets == 1);
  CHECK(stats.mold_messages == 2);
  CHECK(stats.sequence_gaps == 0);
  std::filesystem::remove(path);
}

void spsc_tests() {
  queue::SpscRing<std::uint64_t, 8> small;
  const auto allocations_before = allocation_count.load();
  for (std::uint64_t value = 0; value < 8; ++value) CHECK(small.try_push(value));
  CHECK(!small.try_push(9));
  std::uint64_t output = 0;
  for (std::uint64_t value = 0; value < 8; ++value) {
    CHECK(small.try_pop(output));
    CHECK(output == value);
  }
  CHECK(!small.try_pop(output));
  CHECK(allocation_count.load() == allocations_before);

  constexpr std::uint64_t count = 1'000'000;
  queue::SpscRing<std::uint64_t, 1 << 12> concurrent;
  std::thread producer([&] {
    for (std::uint64_t value = 0; value < count; ++value)
      while (!concurrent.try_push(value)) std::this_thread::yield();
  });
  for (std::uint64_t expected = 0; expected < count; ++expected) {
    while (!concurrent.try_pop(output)) std::this_thread::yield();
    CHECK(output == expected);
  }
  producer.join();
  CHECK(concurrent.empty());
}

void threaded_replay_test() {
  Bytes add;
  add.header('A');
  add.u64(42);
  add.u8('B');
  add.u32(500);
  add.chars("AAPL    ", 8);
  add.u32(1'842'100);
  Bytes remove;
  remove.header('D');
  remove.u64(42);

  Bytes file;
  file.u16(static_cast<std::uint16_t>(add.data.size()));
  file.data.insert(file.data.end(), add.data.begin(), add.data.end());
  file.u16(static_cast<std::uint16_t>(remove.data.size()));
  file.data.insert(file.data.end(), remove.data.begin(), remove.data.end());
  const auto path = std::filesystem::temp_directory_path() / "itch-threaded-test.bin";
  {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(file.data.data()),
                 static_cast<std::streamsize>(file.data.size()));
  }

  replay::Config config;
  config.symbol = "AAPL";
  config.max_orders = 8;
  config.max_levels = 8;
  config.latency_samples = 0;
  config.sample_every = 0;
  const auto single = replay::run_file(path, config);
  const auto threaded = replay::run_file_threaded(path, config);
  CHECK(single.messages == threaded.messages);
  CHECK(single.decoder.malformed == threaded.decoder.malformed);
  CHECK(single.book.active_orders == threaded.book.active_orders);
  CHECK(single.book.active_levels == threaded.book.active_levels);
  CHECK(single.book.rejected == threaded.book.rejected);
  CHECK(single.bid.price == threaded.bid.price);
  CHECK(single.ask.price == threaded.ask.price);
  std::filesystem::remove(path);
}

void malformed_fuzz_test() {
  std::mt19937_64 random(0x49544348);
  std::array<std::byte, 128> bytes{};
  itch::MoldDecoder mold_decoder;
  for (std::size_t iteration = 0; iteration < 100'000; ++iteration) {
    const auto length = static_cast<std::size_t>(random() % bytes.size());
    for (std::size_t index = 0; index < length; ++index)
      bytes[index] = static_cast<std::byte>(random());
    const auto input = std::span<const std::byte>(bytes.data(), length);
    static_cast<void>(itch::parse_message(input));
    static_cast<void>(mold_decoder.decode(input, [](auto, auto) {}));
  }
}

void lifecycle_tests() {
  book::OrderBook b(8, 8);
  CHECK(b.add(1, itch::Side::buy, 100, 1000) == book::BookError::none);
  CHECK(b.add(2, itch::Side::buy, 200, 1000) == book::BookError::none);
  CHECK(b.find(1)->next == b.find(2));
  CHECK(b.best_bid().quantity == 300);
  CHECK(b.cancel(1, 40) == book::BookError::none);
  CHECK(b.best_bid().quantity == 260);
  CHECK(b.execute(1, 60) == book::BookError::none);
  CHECK(!b.find(1));
  CHECK(b.best_bid().quantity == 200);
  CHECK(b.replace(2, 3, 150, 1100) == book::BookError::none);
  CHECK(!b.find(2));
  CHECK(b.find(3));
  CHECK(b.best_bid().price == 1100);
  CHECK(b.erase(3) == book::BookError::none);
  CHECK(!b.best_bid());
  CHECK(b.add(4, itch::Side::sell, 100, 1200) == book::BookError::none);
  CHECK(b.cancel(4, 101) == book::BookError::invalid_quantity);
  CHECK(b.add(4, itch::Side::sell, 1, 1200) == book::BookError::duplicate_order);
  CHECK(b.erase(999) == book::BookError::unknown_order);
}

void best_and_pool_tests() {
  book::OrderBook b(2, 2);
  CHECK(b.add(1, itch::Side::buy, 1, 100) == book::BookError::none);
  CHECK(b.add(2, itch::Side::buy, 2, 200) == book::BookError::none);
  CHECK(b.add(3, itch::Side::buy, 1, 300) == book::BookError::order_pool_exhausted);
  CHECK(b.best_bid().price == 200);
  CHECK(b.erase(2) == book::BookError::none);
  CHECK(b.best_bid().price == 100);
  CHECK(b.add(3, itch::Side::sell, 3, 300) == book::BookError::none);
  CHECK(b.best_ask().price == 300);
  CHECK(b.erase(1) == book::BookError::none);
  CHECK(b.erase(3) == book::BookError::none);
  CHECK(b.stats().active_levels == 0);
}

void atomic_replace_test() {
  book::OrderBook b(4, 1);
  CHECK(b.add(1, itch::Side::buy, 10, 100) == book::BookError::none);
  CHECK(b.add(2, itch::Side::buy, 20, 100) == book::BookError::none);
  CHECK(b.replace(1, 3, 10, 200) == book::BookError::level_pool_exhausted);
  CHECK(b.find(1));
  CHECK(!b.find(3));
  CHECK(b.best_bid().quantity == 30);
}

void allocation_free_test() {
  book::OrderBook b(1024, 256);
  auto before = allocation_count.load();
  for (std::uint64_t i = 1; i <= 100000; ++i) {
    const auto price = static_cast<itch::Price>(1000 + i % 100);
    CHECK(b.add(i, itch::Side::buy, 100, price) == book::BookError::none);
    CHECK(b.cancel(i, 100) == book::BookError::none);
  }
  CHECK(allocation_count.load() == before);
}

struct RefOrder {
  itch::Side side;
  itch::Quantity quantity;
  itch::Price price;
};

void differential_test() {
  book::OrderBook book(512, 256);
  std::unordered_map<std::uint64_t, RefOrder> orders;
  std::map<itch::Price, std::uint64_t> bids;
  std::map<itch::Price, std::uint64_t> asks;
  std::mt19937_64 random(12'345);
  std::uint64_t next_id = 1;

  const auto check = [&] {
    CHECK(book.stats().active_orders == orders.size());
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    if (bids.empty())
      CHECK(!bid);
    else {
      CHECK(bid.price == bids.rbegin()->first);
      CHECK(bid.quantity == bids.rbegin()->second);
    }
    if (asks.empty())
      CHECK(!ask);
    else {
      CHECK(ask.price == asks.begin()->first);
      CHECK(ask.quantity == asks.begin()->second);
    }

    std::array<book::DepthLevel, 128> actual{};
    auto count = book.depth(itch::Side::buy, actual);
    CHECK(count == bids.size());
    std::size_t index = 0;
    for (auto iterator = bids.rbegin(); iterator != bids.rend(); ++iterator) {
      CHECK(actual[index].price == iterator->first);
      CHECK(actual[index].quantity == iterator->second);
      ++index;
    }
    count = book.depth(itch::Side::sell, actual);
    CHECK(count == asks.size());
    index = 0;
    for (const auto& [price, quantity] : asks) {
      CHECK(actual[index].price == price);
      CHECK(actual[index].quantity == quantity);
      ++index;
    }
  };

  for (int step = 0; step < 20'000; ++step) {
    const bool should_add = orders.empty() || (orders.size() < 300 && random() % 100 < 58);
    if (should_add) {
      const auto id = next_id++;
      const auto side = random() % 2 != 0 ? itch::Side::buy : itch::Side::sell;
      const auto quantity = static_cast<itch::Quantity>(1 + random() % 1'000);
      const auto price = static_cast<itch::Price>(1'000 + random() % 100);
      CHECK(book.add(id, side, quantity, price) == book::BookError::none);
      orders[id] = {side, quantity, price};
      (side == itch::Side::buy ? bids : asks)[price] += quantity;
    } else {
      auto iterator = orders.begin();
      std::advance(iterator, static_cast<std::ptrdiff_t>(random() % orders.size()));
      const auto [id, order] = *iterator;
      auto& levels = order.side == itch::Side::buy ? bids : asks;
      const auto action = random() % 3;
      if (action == 0 && order.quantity > 1) {
        const auto quantity = static_cast<itch::Quantity>(1 + random() % (order.quantity - 1));
        CHECK(book.cancel(id, quantity) == book::BookError::none);
        levels[order.price] -= quantity;
        iterator->second.quantity -= quantity;
      } else if (action == 1) {
        const auto new_id = next_id++;
        const auto quantity = static_cast<itch::Quantity>(1 + random() % 1'000);
        const auto price = static_cast<itch::Price>(1'000 + random() % 100);
        CHECK(book.replace(id, new_id, quantity, price) == book::BookError::none);
        if ((levels[order.price] -= order.quantity) == 0) levels.erase(order.price);
        orders.erase(iterator);
        orders[new_id] = {order.side, quantity, price};
        (order.side == itch::Side::buy ? bids : asks)[price] += quantity;
      } else {
        CHECK(book.erase(id) == book::BookError::none);
        if ((levels[order.price] -= order.quantity) == 0) levels.erase(order.price);
        orders.erase(iterator);
      }
    }
    if (step % 31 == 0) check();
  }
  check();
}
}  // namespace

int main() {
  parser_tests();
  mold_tests();
  spsc_tests();
  threaded_replay_test();
  malformed_fuzz_test();
  lifecycle_tests();
  best_and_pool_tests();
  atomic_replace_test();
  allocation_free_test();
  differential_test();
  if (failures) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "All tests passed\n";
}
