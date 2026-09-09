#include <array>
#include <cstddef>

#include "itch/parser.hpp"

int main() {
  std::array<std::byte, 12> message{};
  message[0] = static_cast<std::byte>('S');
  return itch::parse_message(message) ? 0 : 1;
}
