cmake_minimum_required(VERSION 3.20)
project(sesame-null-output CXX)

# Standalone build of one example plugin. Needs only the SDK header.
add_library(sesame-null-output SHARED null-output.cc)
target_include_directories(sesame-null-output PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
set_target_properties(sesame-null-output PROPERTIES PREFIX "" CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_VISIBILITY_PRESET hidden)
