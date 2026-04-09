cmake_minimum_required(VERSION 3.25 FATAL_ERROR)
set(CMAKE_PROJECT_VERSION 0.1.0)

project(viam-camera-zivid
    DESCRIPTION "Viam Module for Zivid 3D Cameras"
    LANGUAGES CXX
)

if(APPLE)
    list(PREPEND CMAKE_INSTALL_RPATH "@loader_path/../lib")
else()
    list(PREPEND CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

find_package(Zivid REQUIRED)

find_package(viam-cpp-sdk QUIET)
if(NOT viam-cpp-sdk_FOUND)
    message(FATAL_ERROR "viam-cpp-sdk not found. Run: conan install . --build=missing")
endif()

find_package(stb REQUIRED)

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/zivid_camera.cpp
    src/zivid_discovery.cpp
    src/zivid_handeye.cpp
)

target_include_directories(${PROJECT_NAME}
    PRIVATE src
)

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        Zivid::Core
        viam-cpp-sdk::viamsdk
        stb::stb
        Threads::Threads
        ${CMAKE_DL_LIBS}
)

install(TARGETS ${PROJECT_NAME} DESTINATION .)
install(FILES etc/meta.json DESTINATION .)
