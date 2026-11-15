cmake_minimum_required(VERSION 3.18)
project(ragdoll C)

add_library(ragdoll SHARED src/mod.c)

target_include_directories(ragdoll PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(ragdoll log)
