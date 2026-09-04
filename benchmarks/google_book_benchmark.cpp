#include <benchmark/benchmark.h>

#include "book/order_book.hpp"

static void AddCancel(benchmark::State& state) {
  book::OrderBook book(1024, 1024);
  std::uint64_t id = 1;
  for (auto _ : state) {
    const auto price = static_cast<itch::Price>(1'850'000 + id % 100);
    benchmark::DoNotOptimize(book.add(id, itch::Side::buy, 100, price));
    benchmark::DoNotOptimize(book.cancel(id, 100));
    ++id;
  }
  state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(AddCancel);
BENCHMARK_MAIN();
