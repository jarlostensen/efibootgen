cmake_minimum_required(VERSION 3.18 FATAL_ERROR)
project(efibootgen VERSION 1.0
      DESCRIPTION "EFI boot image generator"
      LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)

add_executable(efibootgen "efibootgen.cpp" "disktools.cpp")
set_target_properties(efibootgen PROPERTIES 
    CXX_STANDARD 17
)
target_link_libraries(efibootgen stdc++fs)

