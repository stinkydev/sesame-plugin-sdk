# Example native plugins. Built on its own, this builds every example into
# <build>/<Config>/plugins/<name>/, ready to copy into a Sesame plugin directory.
# The Sesame server build includes it so its tests can load them.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20)
  project(sesame-plugin-sdk-examples CXX)
  set(SESAME_EXAMPLE_PLUGIN_DIR ${CMAKE_BINARY_DIR}/$<CONFIG>/plugins)
endif()

set(SESAME_PLUGIN_SDK_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Each plugin builds into a package folder of its own, next to its manifest and assets.
function(sesame_add_example_plugin name dir)
  list(TRANSFORM ARGN PREPEND ${dir}/ OUTPUT_VARIABLE sources)
  add_library(${name} SHARED ${sources})
  target_include_directories(${name} PRIVATE ${SESAME_PLUGIN_SDK_INCLUDE_DIR})
  set_target_properties(${name} PROPERTIES
    PREFIX ""
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY ${SESAME_EXAMPLE_PLUGIN_DIR}/${name}
    RUNTIME_OUTPUT_DIRECTORY ${SESAME_EXAMPLE_PLUGIN_DIR}/${name}
    FOLDER "Plugins")
  file(GLOB assets ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/plugin.json ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.svg)
  foreach(asset ${assets})
    add_custom_command(TARGET ${name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${asset} $<TARGET_FILE_DIR:${name}>)
  endforeach()
endfunction()

sesame_add_example_plugin(sesame-color-generator examples/color-generator color-generator.cc)
sesame_add_example_plugin(sesame-null-output examples/null-output null-output.cc)
sesame_add_example_plugin(sesame-capture-sim examples/capture-sim capture-sim.cc test-pattern.cc)

# The examples stay out of the sesame target so installers never pick them up; build this to run them.
add_custom_target(sesame-example-plugins DEPENDS sesame-color-generator sesame-null-output sesame-capture-sim)
