# Resolve in each caller's directory scope, even when helpers already exist.
get_filename_component(VANTAGE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include_guard(GLOBAL)

# Keep host language, include paths and warning policy identical across suites.
# Device builds use Make and never receive VANTAGE_SIM or host libraries.
function(vantage_host_target target)
  set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
  target_compile_definitions(${target} PUBLIC VANTAGE_SIM)
  target_include_directories(${target} PUBLIC "${VANTAGE_ROOT}/include")
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  endif()
endfunction()

function(vantage_test_target target)
  vantage_host_target(${target})
  # Tests may inspect private subsystem state and share fixtures.
  target_include_directories(${target} PRIVATE
    "${VANTAGE_ROOT}" "${VANTAGE_ROOT}/tests/support")
endfunction()
