#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {
class Writer {
 public:
  void u8(std::uint8_t value) { buffer_[position_++] = value; }
  void u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value >> 8));
    u8(static_cast<std::uint8_t>(value));
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
  void chars(std::string_view value, std::size_t width) {
    for (std::size_t index = 0; index < width; ++index)
      u8(static_cast<std::uint8_t>(index < value.size() ? value[index] : ' '));
  }
  void header(char type, std::uint64_t timestamp) {
    u8(static_cast<std::uint8_t>(type));
    u16(1);
    u16(0);
    u48(timestamp);
  }
  void frame(std::ofstream& output) {
    const unsigned char size[2] = {
        static_cast<unsigned char>(position_ >> 8),
        static_cast<unsigned char>(position_),
    };
    output.write(reinterpret_cast<const char*>(size), 2);
    output.write(reinterpret_cast<const char*>(buffer_.data()),
                 static_cast<std::streamsize>(position_));
    position_ = 0;
  }

 private:
  std::array<unsigned char, 64> buffer_{};
  std::size_t position_{};
};

void add(Writer& writer, std::ofstream& output, std::uint64_t id, std::uint64_t timestamp,
         char side, std::uint32_t quantity, std::uint32_t price) {
  writer.header('A', timestamp);
  writer.u64(id);
  writer.u8(static_cast<std::uint8_t>(side));
  writer.u32(quantity);
  writer.chars("AAPL", 8);
  writer.u32(price);
  writer.frame(output);
}

void cancel(Writer& writer, std::ofstream& output, std::uint64_t id, std::uint64_t timestamp,
            std::uint32_t quantity) {
  writer.header('X', timestamp);
  writer.u64(id);
  writer.u32(quantity);
  writer.frame(output);
}

void delete_order(Writer& writer, std::ofstream& output, std::uint64_t id,
                  std::uint64_t timestamp) {
  writer.header('D', timestamp);
  writer.u64(id);
  writer.frame(output);
}

void execute(Writer& writer, std::ofstream& output, std::uint64_t id, std::uint64_t timestamp,
             std::uint32_t quantity) {
  writer.header('E', timestamp);
  writer.u64(id);
  writer.u32(quantity);
  writer.u64(id + 9'000'000);
  writer.frame(output);
}

void replace(Writer& writer, std::ofstream& output, std::uint64_t old_id, std::uint64_t new_id,
             std::uint64_t timestamp, std::uint32_t quantity, std::uint32_t price) {
  writer.header('U', timestamp);
  writer.u64(old_id);
  writer.u64(new_id);
  writer.u32(quantity);
  writer.u32(price);
  writer.frame(output);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: generate_feed OUTPUT MESSAGE_COUNT "
                 "[sequential|mixed]\n";
    return 2;
  }
  const std::uint64_t count = std::stoull(argv[2]);
  const std::string_view mode = argc > 3 ? argv[3] : "mixed";
  if (mode != "mixed" && mode != "sequential") {
    std::cerr << "Workload must be 'mixed' or 'sequential'\n";
    return 2;
  }

  std::ofstream output(argv[1], std::ios::binary);
  if (!output) {
    std::cerr << "Cannot create output\n";
    return 1;
  }

  Writer writer;
  std::uint64_t emitted = 0;
  std::uint64_t id = 1;
  std::uint64_t timestamp = 1;
  while (emitted < count) {
    const char side = (id / 4) % 2 != 0 ? 'B' : 'S';
    const auto offset = static_cast<std::uint32_t>((id % 100) * 100);
    const auto quantity = static_cast<std::uint32_t>(100 + id % 900);
    const std::uint32_t price = side == 'B' ? 1'850'000 - offset : 1'850'100 + offset;
    add(writer, output, id, timestamp++, side, quantity, price);
    ++emitted;
    if (mode == "sequential" || emitted == count) {
      ++id;
      continue;
    }

    switch (id % 4) {
      case 0:
        cancel(writer, output, id, timestamp++, quantity);
        break;
      case 1:
        execute(writer, output, id, timestamp++, quantity);
        break;
      case 2:
        delete_order(writer, output, id, timestamp++);
        break;
      default:
        replace(writer, output, id, id + 1'000'000'000, timestamp++, 50,
                side == 'B' ? price - 100 : price + 100);
        break;
    }
    ++emitted;
    ++id;
  }

  if (!output) {
    std::cerr << "Error writing output\n";
    return 1;
  }
  std::cout << "Wrote " << emitted << " framed ITCH messages to " << argv[1] << '\n';
}
