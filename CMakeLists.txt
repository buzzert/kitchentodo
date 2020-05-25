cmake_minimum_required(VERSION 2.8.9)
project (todo)

file (GLOB SOURCES src/*.c)

add_executable (kitchentodo ${SOURCES})
target_link_libraries (kitchentodo PUBLIC -lXm -lXt)
target_compile_options (kitchentodo PRIVATE -Wno-cast-qual)
