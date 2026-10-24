cmake_minimum_required(VERSION 3.22.1)
project(igi_native CXX)

add_library(igi_native SHARED native_lib.cpp)
target_compile_features(igi_native PRIVATE cxx_std_17)
target_link_libraries(igi_native GLESv3 log)
