cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/tiles-transition.c
    src/tiles-transition.h
    src/vortex-transition.c
    src/vortex-transition.h
)

if(NOT MSVC)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE m)
endif()

option(ENABLE_TESTS "Build the transition geometry tests" OFF)
option(ENABLE_GPU_TESTS "Build the effect check that renders on a real graphics device" OFF)

if(ENABLE_TESTS)
  enable_testing()
  add_executable(test-tiling test/test-tiling.c)
  if(NOT MSVC)
    target_link_libraries(test-tiling PRIVATE m)
  endif()
  add_test(NAME test-tiling COMMAND test-tiling)

  add_executable(test-vortex test/test-vortex.c)
  if(NOT MSVC)
    target_link_libraries(test-vortex PRIVATE m)
  endif()
  add_test(NAME test-vortex COMMAND test-vortex)
endif()

# Not part of the ctest suite: it needs a graphics device, so it is run by hand
# (under xvfb-run on a headless machine) against data/effects/tiles_transition.effect.
if(ENABLE_GPU_TESTS)
  add_executable(gpu-check test/gpu-check.c)
  target_link_libraries(gpu-check PRIVATE OBS::libobs)
  if(NOT MSVC)
    target_link_libraries(gpu-check PRIVATE m)
  endif()
  if(APPLE)
    target_compile_definitions(gpu-check PRIVATE GPU_CHECK_GRAPHICS_MODULE="libobs-opengl.dylib")
  elseif(WIN32)
    target_compile_definitions(gpu-check PRIVATE GPU_CHECK_GRAPHICS_MODULE="libobs-d3d11.dll")
  endif()
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
