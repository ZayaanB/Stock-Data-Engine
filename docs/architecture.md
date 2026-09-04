# Architecture

The decoder and book are separate layers. `FramedDecoder` owns only a 64-byte partial-message buffer and accepts arbitrary byte spans. `parse_message` returns stack-resident typed variants. This keeps file framing, wire validation, and market-state mutation independently testable.

`OrderBook` is a single-symbol, single-writer engine. Startup allocates three fixed arrays: orders, price levels, and lookup slots. Orders and levels use index-based free lists. The lookup table uses linear probing and backward-shift deletion, preventing tombstone accumulation on long-running feeds. Each price level owns an intrusive doubly-linked FIFO queue, aggregate quantity, and order count. Bid and ask levels are separate AVL trees.

Operation complexity:

| Operation | Expected cost |
|---|---|
| Order ID lookup | O(1) expected |
| Add / replace price level | O(log L) |
| Execute / cancel / delete | O(1), plus O(log L) if a level empties |
| Best bid / ask | O(log L) traversal |
| Top N depth | O(log L + N) |

The fixed capacities are a deliberate production-style control: memory use is predictable, pointers remain stable, and exhaustion is visible. One engine per instrument also avoids contaminating best-price state across symbols. A future dispatcher can route adds by stock and later lifecycle messages by order-ID ownership.
