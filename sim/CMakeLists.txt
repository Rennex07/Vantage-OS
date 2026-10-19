cmake_minimum_required(VERSION 3.16)
project(vantage_sim LANGUAGES C)

set(CMAKE_C_STANDARD 99)

find_package(SDL2 REQUIRED)

# The simulator shares its application and UI code with the device. Only the
# platform backend differs, so src/drivers/{display,I2C,touch,io,mmu}.c and
# src/boot.s are deliberately left out: they contain the MMIO.
add_executable(vantage_sim
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/main.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/drivers/graphics.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/environment/windowManager.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/platform/display_sim.c"
)

target_include_directories(vantage_sim PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../includes"
  ${SDL2_INCLUDE_DIRS}
)

# SDL2's Visual C++ package ships headers flat in include/, but sources
# include <SDL2/SDL.h>. Mirror them into a build-local SDL2/ subdirectory so
# both that layout and a normal one (MinGW, vcpkg, MSYS2) work unmodified.
if(NOT EXISTS "${SDL2_INCLUDE_DIR}/SDL2/SDL.h")
  set(SDL2_COMPAT_DIR "${CMAKE_BINARY_DIR}/sdl2-compat/SDL2")
  file(COPY "${SDL2_INCLUDE_DIR}/" DESTINATION "${SDL2_COMPAT_DIR}"
       FILES_MATCHING PATTERN "SDL*.h")
  target_include_directories(vantage_sim PRIVATE "${CMAKE_BINARY_DIR}/sdl2-compat")
endif()

target_link_libraries(vantage_sim PRIVATE ${SDL2_LIBRARIES})

# ${SDL2_LIBRARIES} only carries the full path to SDL2.lib. SDL2main.lib sits in
# the same directory, so add the library directories explicitly.
get_filename_component(SDL2_LIBROOT "${SDL2_INCLUDE_DIR}" DIRECTORY)
file(GLOB SDL2_LIBDIRS "${SDL2_LIBROOT}/lib" "${SDL2_LIBROOT}/lib/*")
target_link_directories(vantage_sim PRIVATE ${SDL2_LIBDIRS})

if(WIN32)
  # shell32 for SDL2main's CommandLineToArgvW, user32 for the DPI-awareness calls
  # in display_sim.c.
  target_link_libraries(vantage_sim PRIVATE SDL2main shell32 user32)
endif()

# Put SDL2.dll next to the executable so the simulator runs from the build dir.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(SDL2_ARCH "x64")
else()
  set(SDL2_ARCH "x86")
endif()
file(GLOB SDL2_DLL "${SDL2_LIBROOT}/lib/${SDL2_ARCH}/SDL2.dll")
if(SDL2_DLL)
  add_custom_command(TARGET vantage_sim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SDL2_DLL}" "$<TARGET_FILE_DIR:vantage_sim>"
    COMMENT "Copying SDL2.dll next to vantage_sim.exe"
  )
else()
  message(WARNING "SDL2.dll not found under ${SDL2_LIBROOT}/lib/${SDL2_ARCH}; you may need it on PATH to run.")
endif()