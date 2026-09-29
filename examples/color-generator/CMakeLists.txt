cmake_minimum_required(VERSION 3.20)
project(sesame-color-generator CXX)

# Standalone build of one example plugin. Needs only the SDK header.
add_library(sesame-color-generator SHARED color-generator.cc)
target_include_directories(sesame-color-generator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
set_target_properties(sesame-color-generator PROPERTIES PREFIX "" CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_VISIBILITY_PRESET hidden)
