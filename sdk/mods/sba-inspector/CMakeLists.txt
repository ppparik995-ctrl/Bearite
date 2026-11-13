cmake_minimum_required(VERSION 3.18)
project(sbainspector C)

add_library(sbainspector SHARED src/mod.c)

target_include_directories(sbainspector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(sbainspector log)
