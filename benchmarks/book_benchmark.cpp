#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

#include "book/order_book.hpp"

int main(int argc, char** argv) {
  const std::uint64_t operations = argc > 1 ? std::stoull(argv[1]) : 10'000'000;
  book::OrderBook order_book(1'024, 1'024);
  std::uint64_t sink = 0;

  const auto start = std::chrono::steady_clock::now();
  for (std::uint64_t id = 1; id <= operations / 2; ++id) {
    const auto side = id % 2 != 0 ? itch::Side::buy : itch::Side::sell;
    const auto price = static_cast<itch::Price>(1'850'000 + id % 1'000);
    order_book.add(id, side, 100, price);
    order_book.cancel(id, 100);
    sink += order_book.stats().active_orders;
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  const double operation_count = static_cast<double>(operations);

  std::cout << std::fixed << std::setprecision(2) << "operations=" << operations
            << " seconds=" << seconds << " throughput_mops=" << operation_count / seconds / 1e6
            << " ns_per_operation=" << seconds * 1e9 / operation_count << " sink=" << sink << '\n';
}
