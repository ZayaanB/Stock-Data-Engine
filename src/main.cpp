#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "replay/pcap.hpp"
#include "replay/replay.hpp"

namespace {
void usage() {
  std::cout << "Usage: itch_order_book --input FILE [options]\n"
               "Options:\n"
               "  --symbol SYMBOL       Required; process one symbol (maximum 8 characters)\n"
               "  --format FORMAT       Input format: itch (default) or pcap\n"
               "  --max-orders N        Preallocated order capacity\n"
               "  --max-levels N        Preallocated price-level capacity\n"
               "  --sample-every N      Time every Nth message; 0 disables sampling\n"
               "  --latency-samples N   Maximum number of retained samples\n"
               "  --threaded            Use reader and book threads with an SPSC queue\n"
               "  --benchmark           Accepted for compatibility; replay always reports metrics\n"
               "  --help                Show this help\n";
}

void print_quote(const char* name, const book::Quote& quote) {
  std::cout << name << ": ";
  if (!quote) {
    std::cout << "empty\n";
    return;
  }
  std::cout << quote.price / 10'000 << '.' << std::setfill('0') << std::setw(4)
            << quote.price % 10'000 << std::setfill(' ') << " x " << quote.quantity << " ("
            << quote.orders << " orders)\n";
}
}  // namespace

int main(int argc, char** argv) {
  replay::Config config;
  std::string input;
  std::string format = "itch";
  bool threaded = false;

  try {
    for (int index = 1; index < argc; ++index) {
      const std::string_view argument = argv[index];
      const auto value = [&]() -> std::string_view {
        if (++index >= argc) throw std::invalid_argument("missing option value");
        return argv[index];
      };

      if (argument == "--input")
        input = value();
      else if (argument == "--symbol")
        config.symbol = value();
      else if (argument == "--format")
        format = value();
      else if (argument == "--max-orders")
        config.max_orders = std::stoull(std::string(value()));
      else if (argument == "--max-levels")
        config.max_levels = std::stoull(std::string(value()));
      else if (argument == "--sample-every")
        config.sample_every = std::stoull(std::string(value()));
      else if (argument == "--latency-samples")
        config.latency_samples = std::stoull(std::string(value()));
      else if (argument == "--threaded")
        threaded = true;
      else if (argument == "--benchmark")
        continue;
      else if (argument == "--help") {
        usage();
        return 0;
      } else {
        throw std::invalid_argument("unknown option: " + std::string(argument));
      }
    }

    if (input.empty()) {
      usage();
      return 2;
    }
    if (config.symbol.empty())
      throw std::invalid_argument("--symbol is required for a single-book replay");
    if (format != "itch" && format != "pcap")
      throw std::invalid_argument("--format must be 'itch' or 'pcap'");
    if (threaded && format == "pcap")
      throw std::invalid_argument("--threaded currently supports framed ITCH files only");

    replay::PcapStats pcap_stats{};
    const auto result = format == "pcap" ? replay::run_pcap(input, config, pcap_stats)
                        : threaded       ? replay::run_file_threaded(input, config)
                                         : replay::run_file(input, config);
    std::cout << std::fixed << std::setprecision(2) << "Messages processed: " << result.messages
              << "\nProcessing time: " << result.seconds
              << " sec\nThroughput: " << result.messages_per_second / 1e6
              << " M messages/sec\n\nLatency (sampled):\n"
              << "p50    " << result.latency.p50_ns << " ns\n"
              << "p90    " << result.latency.p90_ns << " ns\n"
              << "p99    " << result.latency.p99_ns << " ns\n"
              << "p99.9  " << result.latency.p999_ns << " ns\n"
              << "max    " << result.latency.max_ns << " ns\n\n"
              << "Decoder malformed: " << result.decoder.malformed
              << "\nDecoder unsupported: " << result.decoder.unsupported
              << "\nBook active orders: " << result.book.active_orders
              << "\nBook rejected: " << result.book.rejected << '\n';
    if (threaded)
      std::cout << "Producer spins: " << result.producer_spins
                << "\nConsumer spins: " << result.consumer_spins << '\n';
    if (format == "pcap")
      std::cout << "PCAP packets: " << pcap_stats.packets
                << "\nMoldUDP64 packets: " << pcap_stats.mold_packets
                << "\nMoldUDP64 messages: " << pcap_stats.mold_messages
                << "\nSequence gaps: " << pcap_stats.sequence_gaps
                << "\nMissing messages: " << pcap_stats.missing_messages
                << "\nSequence rewinds: " << pcap_stats.sequence_rewinds
                << "\nMalformed PCAP packets: " << pcap_stats.malformed_packets
                << "\nMold errors: " << pcap_stats.mold_errors << '\n';
    print_quote("Best Bid", result.bid);
    print_quote("Best Ask", result.ask);
    return result.decoder.malformed != 0 || result.book.rejected != 0 ||
                   pcap_stats.malformed_packets != 0 || pcap_stats.mold_errors != 0 ||
                   pcap_stats.sequence_gaps != 0 || pcap_stats.sequence_rewinds != 0
               ? 1
               : 0;
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
}
