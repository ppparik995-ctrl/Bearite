cmake_minimum_required(VERSION 3.18)
project(emotemaker C)

add_library(emotemaker SHARED src/mod.c)

target_include_directories(emotemaker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(emotemaker log)
