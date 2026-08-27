
cmake_minimum_required(VERSION 3.22.1)

project("XrGoggle")

find_package(OpenXR REQUIRED CONFIG)

add_library(${CMAKE_PROJECT_NAME} SHARED
        XrGoggleSession.cpp
        jni_bindings.cpp)

target_link_libraries(${CMAKE_PROJECT_NAME}
        OpenXR::openxr_loader
        OpenXR::headers
        android
        EGL
        GLESv3
        log)

set_property(TARGET ${CMAKE_PROJECT_NAME} PROPERTY CXX_STANDARD 20)
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -fno-omit-frame-pointer)
