cmake_minimum_required(VERSION 3.20)
project(itch_package_consumer LANGUAGES CXX)

find_package(itch_order_book 1 CONFIG REQUIRED)

add_executable(itch_package_consumer main.cpp)
target_link_libraries(itch_package_consumer PRIVATE ItchOrderBook::Engine)
