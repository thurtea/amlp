cmake_minimum_required(VERSION 3.20)
project(amlp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

option(AMLP_BUILD_TESTS "Build unit tests" ON)

add_subdirectory(src/core)
add_subdirectory(src/config)
add_subdirectory(src/compiler)
add_subdirectory(src/vm)
add_subdirectory(src/security)
add_subdirectory(src/object)
add_subdirectory(src/net)
add_subdirectory(src/scheduler)
add_subdirectory(src/persist)
add_subdirectory(src/efun)
add_subdirectory(src/apply)
add_subdirectory(src/dialect)

add_executable(amlp src/main.cpp)
target_include_directories(amlp PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(amlp
    PRIVATE
        core config compiler vm object efun apply dialect net scheduler persist
)

if(AMLP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
