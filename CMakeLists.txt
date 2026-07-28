cmake_minimum_required(VERSION 3.20)
project(credis VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Extra build type: -DCMAKE_BUILD_TYPE=ASan
set(CMAKE_CXX_FLAGS_ASAN "-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined"
    CACHE STRING "Flags used by the C++ compiler for ASan builds" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_ASAN "-fsanitize=address,undefined"
    CACHE STRING "Flags used by the linker for ASan builds" FORCE)
mark_as_advanced(CMAKE_CXX_FLAGS_ASAN CMAKE_EXE_LINKER_FLAGS_ASAN)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

# --- core library -------------------------------------------------------------
file(GLOB_RECURSE CREDIS_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/src/net/*.cpp"
     "${CMAKE_SOURCE_DIR}/src/resp/*.cpp"
     "${CMAKE_SOURCE_DIR}/src/store/*.cpp"
     "${CMAKE_SOURCE_DIR}/src/commands/*.cpp"
     "${CMAKE_SOURCE_DIR}/src/util/*.cpp"
     "${CMAKE_SOURCE_DIR}/src/server.cpp")

add_library(credis_core STATIC ${CREDIS_SOURCES})
target_include_directories(credis_core PUBLIC "${CMAKE_SOURCE_DIR}/src")

# --- server -------------------------------------------------------------------
add_executable(credis-server src/main.cpp)
target_link_libraries(credis-server PRIVATE credis_core)

# --- tests --------------------------------------------------------------------
enable_testing()
file(GLOB_RECURSE CREDIS_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/*.cpp")
add_executable(credis-tests ${CREDIS_TEST_SOURCES})
target_link_libraries(credis-tests PRIVATE credis_core)
target_include_directories(credis-tests PRIVATE "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME unit COMMAND credis-tests)

add_test(NAME integration
         COMMAND "${CMAKE_SOURCE_DIR}/tests/integration.sh" "$<TARGET_FILE:credis-server>")
