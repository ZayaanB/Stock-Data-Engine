#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
enum class State : std::uint8_t { empty, occupied, tombstone };
struct Slot {
  std::uint64_t id{};
  State state{};
};

class Table {
 public:
  Table(std::size_t capacity, bool backward_shift)
      : slots_(capacity), backward_shift_(backward_shift) {}

  bool find(std::uint64_t id) const {
    auto position = hash(id);
    for (std::size_t probe = 0; probe < slots_.size(); ++probe) {
      const auto& slot = slots_[position];
      if (slot.state == State::empty) return false;
      if (slot.state == State::occupied && slot.id == id) return true;
      position = (position + 1) % slots_.size();
    }
    return false;
  }

  void insert(std::uint64_t id) {
    auto position = hash(id);
    auto first_tombstone = slots_.size();
    for (std::size_t probe = 0; probe < slots_.size(); ++probe) {
      auto& slot = slots_[position];
      if (slot.state == State::tombstone && first_tombstone == slots_.size())
        first_tombstone = position;
      if (slot.state == State::empty) {
        slots_[first_tombstone == slots_.size() ? position : first_tombstone] = {id,
                                                                                 State::occupied};
        return;
      }
      position = (position + 1) % slots_.size();
    }
    slots_[first_tombstone] = {id, State::occupied};
  }

  void erase(std::uint64_t id) {
    auto position = hash(id);
    while (slots_[position].state != State::empty) {
      if (slots_[position].state == State::occupied && slots_[position].id == id) {
        if (backward_shift_)
          erase_with_shift(position);
        else
          slots_[position].state = State::tombstone;
        return;
      }
      position = (position + 1) % slots_.size();
    }
  }

 private:
  std::size_t hash(std::uint64_t value) const {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    return static_cast<std::size_t>(value % slots_.size());
  }

  void erase_with_shift(std::size_t hole) {
    for (auto scan = (hole + 1) % slots_.size(); slots_[scan].state != State::empty;
         scan = (scan + 1) % slots_.size()) {
      const auto home = hash(slots_[scan].id);
      const bool crosses = hole <= scan ? home <= hole || home > scan : home <= hole && home > scan;
      if (crosses) {
        slots_[hole] = slots_[scan];
        hole = scan;
      }
    }
    slots_[hole] = {};
  }

  std::vector<Slot> slots_;
  bool backward_shift_;
};

double run(bool backward_shift, std::uint64_t operations, std::uint64_t& sink) {
  Table table(4'093, backward_shift);
  const auto start = std::chrono::steady_clock::now();
  for (std::uint64_t id = 1; id <= operations; ++id) {
    sink += table.find(id) ? 1U : 0U;
    table.insert(id);
    sink += table.find(id) ? 1U : 0U;
    table.erase(id);
  }
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t operations = argc > 1 ? std::stoull(argv[1]) : 100'000;
  std::uint64_t sink = 0;
  const double tombstone_seconds = run(false, operations, sink);
  const double shift_seconds = run(true, operations, sink);
  const double count = static_cast<double>(operations);
  std::cout << std::fixed << std::setprecision(2) << "operations=" << operations << '\n'
            << "tombstone_ns_per_cycle=" << tombstone_seconds * 1e9 / count << '\n'
            << "backward_shift_ns_per_cycle=" << shift_seconds * 1e9 / count << '\n'
            << "speedup=" << tombstone_seconds / shift_seconds << "x\n"
            << "sink=" << sink << '\n';
}
