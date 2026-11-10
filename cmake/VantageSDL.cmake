find_package(SDL2 REQUIRED)
add_library(vantage_sdl INTERFACE)
target_include_directories(vantage_sdl INTERFACE ${SDL2_INCLUDE_DIRS})
target_link_libraries(vantage_sdl INTERFACE ${SDL2_LIBRARIES})

# The Visual C++ package has flat headers; sources use <SDL2/SDL.h>.
if(SDL2_INCLUDE_DIR AND NOT EXISTS "${SDL2_INCLUDE_DIR}/SDL2/SDL.h")
  file(COPY "${SDL2_INCLUDE_DIR}/" DESTINATION "${CMAKE_BINARY_DIR}/sdl2-compat/SDL2"
    FILES_MATCHING PATTERN "SDL*.h")
  target_include_directories(vantage_sdl INTERFACE "${CMAKE_BINARY_DIR}/sdl2-compat")
endif()

if(WIN32)
  get_filename_component(SDL2_LIBROOT "${SDL2_INCLUDE_DIR}" DIRECTORY)
  file(GLOB SDL2_LIBDIRS "${SDL2_LIBROOT}/lib" "${SDL2_LIBROOT}/lib/*")
  target_link_directories(vantage_sdl INTERFACE ${SDL2_LIBDIRS})
  target_link_libraries(vantage_sdl INTERFACE SDL2main shell32 user32)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(SDL2_ARCH x64)
  else()
    set(SDL2_ARCH x86)
  endif()
  file(GLOB SDL2_DLL "${SDL2_LIBROOT}/lib/${SDL2_ARCH}/SDL2.dll")
endif()

function(vantage_copy_sdl_runtime target)
  if(SDL2_DLL)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SDL2_DLL}" "$<TARGET_FILE_DIR:${target}>")
  endif()
endfunction()
