cmake_minimum_required(VERSION 3.16)
project(vantage_host LANGUAGES C)

include(CTest)
option(VANTAGE_BUILD_SIMULATOR "Build the SDL2 desktop application" OFF)

if(BUILD_TESTING)
  add_subdirectory(tests/native)
endif()

if(VANTAGE_BUILD_SIMULATOR)
  add_subdirectory(sim)
endif()
