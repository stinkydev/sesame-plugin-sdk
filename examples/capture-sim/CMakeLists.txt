cmake_minimum_required(VERSION 3.20)
project(sesame-capture-sim CXX)

# Standalone build of one example plugin. Needs only the SDK header.
add_library(sesame-capture-sim SHARED capture-sim.cc test-pattern.cc)
target_include_directories(sesame-capture-sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
set_target_properties(sesame-capture-sim PROPERTIES PREFIX "" CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_VISIBILITY_PRESET hidden)
