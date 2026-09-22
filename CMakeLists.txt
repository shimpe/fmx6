cmake_minimum_required(VERSION 3.16)
project(FMX6UGens LANGUAGES CXX)

if(NOT SC_PATH)
    message(FATAL_ERROR "Set SC_PATH to the SuperCollider source tree, e.g. cmake -DSC_PATH=/path/to/supercollider ..")
endif()

if(NOT EXISTS "${SC_PATH}/include/plugin_interface/SC_PlugIn.h")
    message(FATAL_ERROR "SC_PlugIn.h not found under ${SC_PATH}/include/plugin_interface")
endif()

add_library(FMX6 MODULE FMX6.cpp)

target_compile_features(FMX6 PRIVATE cxx_std_17)
target_include_directories(FMX6 PRIVATE
    "${SC_PATH}/include/plugin_interface"
    "${SC_PATH}/include/common"
    "${SC_PATH}/common"
)

set_target_properties(FMX6 PROPERTIES PREFIX "")

if(APPLE OR WIN32)
    set_target_properties(FMX6 PROPERTIES SUFFIX ".scx")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(FMX6 PRIVATE -fvisibility=hidden)
endif()

# Keep the language class next to the compiled plugin for easy installation.
add_custom_command(TARGET FMX6 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:FMX6>/Classes"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Classes/FMX6.sc"
        "$<TARGET_FILE_DIR:FMX6>/Classes/FMX6.sc"
)
