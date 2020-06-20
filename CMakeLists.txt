cmake_minimum_required(VERSION 2.8.9)
project (todo)

file (GLOB SOURCES src/*.c)

add_executable (kitchentodo ${SOURCES})
target_compile_options (kitchentodo PRIVATE -Wno-unused-parameter)
target_link_libraries (kitchentodo PUBLIC -lXm -lXt -lpthread)
target_compile_options (kitchentodo PRIVATE -Wno-cast-qual)
