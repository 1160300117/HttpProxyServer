cmake_minimum_required(VERSION 3.12)
project(experiment1)

set(CMAKE_CXX_STANDARD 11)

add_executable(experiment1 main.cpp)

if(MINGW)
    target_link_libraries(experiment1 ws2_32)
endif()