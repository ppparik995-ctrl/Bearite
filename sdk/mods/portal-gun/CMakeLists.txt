cmake_minimum_required(VERSION 3.18)
project(portalgun C)

add_library(portalgun SHARED src/mod.c)

target_include_directories(portalgun PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(portalgun log)
