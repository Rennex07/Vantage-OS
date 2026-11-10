cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(vantage_native_tests LANGUAGES C)
  include(CTest)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/VantageHost.cmake")

if(NOT BUILD_TESTING)
  return()
endif()

find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)

# Production paths are rooted at the repository; local paths name test bodies.
function(vantage_native_test target name group)
  add_executable(${target} ${ARGN})
  vantage_test_target(${target})
  add_test(NAME ${name} COMMAND ${target})
  set_tests_properties(${name} PROPERTIES LABELS "${group}" TIMEOUT 120)
  if(group STREQUAL "drivers")
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
  endif()
endfunction()

set(ROOT "${VANTAGE_ROOT}")

# Each public header must compile without relying on another include's order.
file(GLOB_RECURSE PUBLIC_HEADERS CONFIGURE_DEPENDS "${ROOT}/include/vantage/*.h")
set(HEADER_CHECKS)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/header-checks")
foreach(header IN LISTS PUBLIC_HEADERS)
  file(RELATIVE_PATH include_path "${ROOT}/include" "${header}")
  string(REPLACE "/" "_" check_name "${include_path}")
  set(check "${CMAKE_CURRENT_BINARY_DIR}/header-checks/${check_name}.c")
  file(GENERATE OUTPUT "${check}" CONTENT "#include <${include_path}>\ntypedef int vantage_header_contract;\n")
  list(APPEND HEADER_CHECKS "${check}")
endforeach()
add_library(header_contracts OBJECT ${HEADER_CHECKS})
vantage_host_target(header_contracts)

vantage_native_test(hardware_test hardware_mailbox drivers
  drivers/hardware.c "${ROOT}/drivers/hw/wait.c" "${ROOT}/drivers/hw/property.c")
vantage_native_test(v3d_test v3d_commands_and_mmu drivers
  drivers/v3d.c "${ROOT}/drivers/video/v3d.c" "${ROOT}/drivers/hw/wait.c")
vantage_native_test(wifi_test wifi_firmware_and_frames drivers
  drivers/wifi.c "${ROOT}/drivers/wifi/cyw43455.c" "${ROOT}/drivers/wifi/nvram.c")
vantage_native_test(sdio_test sdio_host drivers
  drivers/sdio.c "${ROOT}/drivers/wifi/sdio.c" "${ROOT}/drivers/hw/wait.c")
vantage_native_test(sd_controller_test sd_controller drivers
  drivers/sd_controller.c "${ROOT}/drivers/storage/sd.c")
vantage_native_test(delays_test delays drivers
  drivers/delays.c "${ROOT}/platform/pi4/timer.c")
vantage_native_test(touch_driver_test touch_driver drivers
  drivers/touch.c "${ROOT}/drivers/input/touch.c")
vantage_native_test(serial_input_test serial_input drivers
  drivers/serial_input.c "${ROOT}/platform/pi4/io.c" "${ROOT}/drivers/input/key.c")
vantage_native_test(gpio_test gpio_pins drivers
  drivers/gpio.c "${ROOT}/platform/pi4/io.c")

set(FILE_SOURCES "${ROOT}/kernel/fs/files.c" "${ROOT}/kernel/fs/storage.c")
vantage_native_test(files_test persistent_files storage
  fs/files.c ${FILE_SOURCES} "${ROOT}/kernel/fs/file_syscall.c")
vantage_native_test(storage_test storage_recovery storage
  fs/storage.c "${ROOT}/kernel/fs/storage.c")
add_executable(storage_host_test fs/storage_host.c
  "${ROOT}/sim/platform/storage_host.c" ${FILE_SOURCES})
vantage_test_target(storage_host_test)
if(MSVC)
  target_compile_definitions(storage_host_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME storage_process_reboot COMMAND "${Python3_EXECUTABLE}" -B
  "${ROOT}/tests/tools/storage_host_roundtrip.py" "$<TARGET_FILE:storage_host_test>")
set_tests_properties(storage_process_reboot PROPERTIES LABELS storage TIMEOUT 120)

add_library(mm_test_core STATIC
  "${ROOT}/kernel/mm/ranges.c" "${ROOT}/kernel/mm/pages.c"
  "${ROOT}/kernel/mm/heap.c" "${ROOT}/kernel/mm/fdt.c" "${ROOT}/kernel/mm/boot.c")
vantage_host_target(mm_test_core)
foreach(name pages allocators fdt)
  vantage_native_test(mm_${name}_test mm_${name} memory mm/${name}.c)
  target_link_libraries(mm_${name}_test PRIVATE mm_test_core)
endforeach()

vantage_native_test(task_smp_test scheduler_four_core scheduler
  sched/task_smp.c "${ROOT}/kernel/sched/task.c" "${ROOT}/kernel/sched/self_test.c"
  "${ROOT}/kernel/parallel/worker.c")
target_link_libraries(task_smp_test PRIVATE Threads::Threads)

vantage_native_test(terminal_test terminal_model user
  user/terminal.c "${ROOT}/user/lib/user_lib.c" "${ROOT}/user/apps/terminal/terminal.c")
vantage_native_test(user_memory_test user_memory user
  user/memory.c "${ROOT}/arch/armv8/mmu_tables.c" "${ROOT}/kernel/syscall/dispatch.c"
  ${FILE_SOURCES} "${ROOT}/kernel/fs/file_syscall.c")
vantage_native_test(abi_test syscall_abi user user/abi.c)

add_test(NAME python_tools COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
  -s "${ROOT}/tests/tools" -v)
set_tests_properties(python_tools PROPERTIES LABELS tooling TIMEOUT 120)
