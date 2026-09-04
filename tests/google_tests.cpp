#include <gtest/gtest.h>

#include "book/order_book.hpp"
#include "itch/messages.hpp"

TEST(OrderBook, MaintainsFifoAndAggregates) {
  book::OrderBook book(8, 8);

  ASSERT_EQ(book.add(1, itch::Side::buy, 100, 1'000), book::BookError::none);
  ASSERT_EQ(book.add(2, itch::Side::buy, 200, 1'000), book::BookError::none);
  ASSERT_NE(book.find(1), nullptr);
  ASSERT_NE(book.find(2), nullptr);
  EXPECT_EQ(book.find(1)->next, book.find(2));
  EXPECT_EQ(book.best_bid().quantity, 300U);
}

TEST(OrderBook, AppliesLifecycleEvents) {
  book::OrderBook book(8, 8);

  ASSERT_EQ(book.add(1, itch::Side::sell, 100, 1'100), book::BookError::none);
  EXPECT_EQ(book.execute(1, 25), book::BookError::none);
  EXPECT_EQ(book.cancel(1, 25), book::BookError::none);
  EXPECT_EQ(book.replace(1, 2, 40, 1'050), book::BookError::none);
  EXPECT_EQ(book.erase(2), book::BookError::none);
  EXPECT_FALSE(book.best_ask());
}

TEST(Parser, RejectsWrongLength) {
  const std::byte incomplete_add[]{std::byte{'A'}};
  EXPECT_EQ(itch::parse_message(incomplete_add).error, itch::ParseError::wrong_length);
}
