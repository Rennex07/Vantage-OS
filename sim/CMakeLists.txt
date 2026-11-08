cmake_minimum_required(VERSION 3.16)
project(vantage_sim LANGUAGES C)

set(CMAKE_C_STANDARD 99)

find_package(SDL2 REQUIRED)

# Share the application, syscall dispatcher and UI code with the device.
# ARM startup, MMIO drivers and the svc instruction are replaced on desktop.
add_library(vantage_sim_core STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../init/main.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../kernel/fs/files.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../kernel/fs/storage.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../kernel/fs/file_syscall.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../drivers/video/graphics.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../user/env/windowManager.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../user/env/terminal.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../user/env/terminal_app.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../user/lib/user_lib.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/platform/display_sim.c"
)

target_compile_definitions(vantage_sim_core PUBLIC VANTAGE_SIM)

add_executable(vantage_sim "${CMAKE_CURRENT_SOURCE_DIR}/main.c" "${CMAKE_CURRENT_SOURCE_DIR}/platform/storage_host.c")
target_link_libraries(vantage_sim PRIVATE vantage_sim_core)

target_include_directories(vantage_sim_core PUBLIC
  ${SDL2_INCLUDE_DIRS}
)

# SDL2's Visual C++ package ships headers flat in include/, but sources
# include <SDL2/SDL.h>. Mirror them into a build-local SDL2/ subdirectory so
# both that layout and a normal one (MinGW, vcpkg, MSYS2) work unmodified.
if(NOT EXISTS "${SDL2_INCLUDE_DIR}/SDL2/SDL.h")
  set(SDL2_COMPAT_DIR "${CMAKE_BINARY_DIR}/sdl2-compat/SDL2")
  file(COPY "${SDL2_INCLUDE_DIR}/" DESTINATION "${SDL2_COMPAT_DIR}"
       FILES_MATCHING PATTERN "SDL*.h")
  target_include_directories(vantage_sim_core PUBLIC "${CMAKE_BINARY_DIR}/sdl2-compat")
endif()

target_link_libraries(vantage_sim_core PUBLIC ${SDL2_LIBRARIES})

# ${SDL2_LIBRARIES} only carries the full path to SDL2.lib. SDL2main.lib sits in
# the same directory, so add the library directories explicitly.
get_filename_component(SDL2_LIBROOT "${SDL2_INCLUDE_DIR}" DIRECTORY)
file(GLOB SDL2_LIBDIRS "${SDL2_LIBROOT}/lib" "${SDL2_LIBROOT}/lib/*")
target_link_directories(vantage_sim_core PUBLIC ${SDL2_LIBDIRS})

if(WIN32)
  # shell32 for SDL2main's CommandLineToArgvW, user32 for the DPI-awareness calls
  # in display_sim.c.
  target_link_libraries(vantage_sim_core PUBLIC SDL2main shell32 user32)
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

include(CTest)
if(BUILD_TESTING)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../drivers" "${CMAKE_BINARY_DIR}/drivers")
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../kernel/fs" "${CMAKE_BINARY_DIR}/files")
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../kernel/sched" "${CMAKE_BINARY_DIR}/scheduler")
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../mm" "${CMAKE_BINARY_DIR}/mm")
  add_executable(vantage_sim_smoke "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.c")
  target_link_libraries(vantage_sim_smoke PRIVATE vantage_sim_core)
  if(SDL2_DLL)
    add_custom_command(TARGET vantage_sim_smoke POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${SDL2_DLL}" "$<TARGET_FILE_DIR:vantage_sim_smoke>"
    )
  endif()
  add_test(NAME simulator_smoke COMMAND vantage_sim_smoke)
  set_tests_properties(simulator_smoke PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software"
    TIMEOUT 10
  )
  add_executable(touch_driver_test
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/touch_driver.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../drivers/input/touch.c"
  )
  target_compile_definitions(touch_driver_test PRIVATE VANTAGE_SIM)
  add_test(NAME touch_driver COMMAND touch_driver_test)
  add_executable(user_memory_test
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/user_memory.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../arch/armv8/mmu_tables.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../init/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../kernel/fs/files.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../kernel/fs/storage.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../kernel/fs/file_syscall.c"
  )
  target_compile_definitions(user_memory_test PRIVATE VANTAGE_SIM)
  add_test(NAME user_memory COMMAND user_memory_test)
  add_executable(terminal_test
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/terminal.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../user/lib/user_lib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../user/env/terminal.c")
  target_compile_definitions(terminal_test PRIVATE VANTAGE_SIM)
  add_test(NAME terminal_model COMMAND terminal_test)
  add_executable(terminal_storage_test
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/terminal_storage.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/storage_host.c")
  target_link_libraries(terminal_storage_test PRIVATE vantage_sim_core)
  if(SDL2_DLL)
    add_custom_command(TARGET terminal_storage_test POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${SDL2_DLL}" "$<TARGET_FILE_DIR:terminal_storage_test>")
  endif()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME terminal_storage_reboot COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/tools/terminal_storage_roundtrip.py"
    "$<TARGET_FILE:terminal_storage_test>")
  set_tests_properties(terminal_storage_reboot PROPERTIES TIMEOUT 120)
  add_executable(serial_input_test
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/serial_input.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../drivers/io.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../drivers/input/key.c")
  target_compile_definitions(serial_input_test PRIVATE VANTAGE_SIM)
  add_test(NAME serial_input COMMAND serial_input_test)
  add_executable(terminal_smoke "${CMAKE_CURRENT_SOURCE_DIR}/tests/terminal_smoke.c")
  target_link_libraries(terminal_smoke PRIVATE vantage_sim_core)
  if(SDL2_DLL)
    add_custom_command(TARGET terminal_smoke POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${SDL2_DLL}" "$<TARGET_FILE_DIR:terminal_smoke>")
  endif()
  add_test(NAME terminal_smoke COMMAND terminal_smoke)
  set_tests_properties(terminal_smoke PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software" TIMEOUT 15)
  add_executable(editor_touch "${CMAKE_CURRENT_SOURCE_DIR}/tests/editor_touch.c")
  target_link_libraries(editor_touch PRIVATE vantage_sim_core)
  if(SDL2_DLL)
    add_custom_command(TARGET editor_touch POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${SDL2_DLL}" "$<TARGET_FILE_DIR:editor_touch>")
  endif()
  add_test(NAME editor_touch COMMAND editor_touch)
  set_tests_properties(editor_touch PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software" TIMEOUT 15)
  foreach(target terminal_test terminal_storage_test serial_input_test terminal_smoke editor_touch)
    if(MSVC)
      target_compile_options(${target} PRIVATE /W4 /WX)
    else()
      target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
  endforeach()
  add_executable(parallel_test
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/parallel.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../kernel/parallel.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../drivers/video/raster.c"
  )
  target_compile_definitions(parallel_test PRIVATE VANTAGE_SIM)
  target_include_directories(parallel_test PRIVATE ${SDL2_INCLUDE_DIRS} "${CMAKE_BINARY_DIR}/sdl2-compat")
  target_link_libraries(parallel_test PRIVATE ${SDL2_LIBRARIES})
  if(SDL2_DLL)
    add_custom_command(TARGET parallel_test POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${SDL2_DLL}" "$<TARGET_FILE_DIR:parallel_test>"
    )
  endif()
  add_test(NAME parallel_batches COMMAND parallel_test)
  add_test(NAME parallel_missing_worker COMMAND parallel_test --missing)
  add_test(NAME parallel_completion_timeout COMMAND parallel_test --timeout)
  set_tests_properties(parallel_batches parallel_missing_worker parallel_completion_timeout
    PROPERTIES TIMEOUT 20)
endif()
