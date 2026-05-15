cmake_minimum_required(VERSION 3.25 FATAL_ERROR)
set(CMAKE_PROJECT_VERSION 0.1.0)

project(viam-camera-zivid
    VERSION ${CMAKE_PROJECT_VERSION}
    DESCRIPTION "Viam Module for Zivid 3D Cameras"
    LANGUAGES CXX
)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()

include(GNUInstallDirs)

# Default install prefix to a local build directory unless overridden.
if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "" FORCE)
endif()

# Relocatable RPATH: binary in <prefix>/bin finds libs in <prefix>/lib.
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
set(CMAKE_SKIP_BUILD_RPATH FALSE)

file(RELATIVE_PATH BIN_TO_LIB
    ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}
    ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR})
if(APPLE)
    list(PREPEND CMAKE_INSTALL_RPATH "@loader_path/${BIN_TO_LIB}")
else()
    list(PREPEND CMAKE_INSTALL_RPATH "$ORIGIN/${BIN_TO_LIB}")
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
    src/zivid_stitcher.cpp
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

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

#
# Package meta.json (configured from meta.json.in) into the install tree.
#
configure_file(
    meta.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/meta.json
    @ONLY
)

install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/meta.json
    DESTINATION .
)

install(
    PROGRAMS first_run.sh
    DESTINATION .
)

#
# CPack: produce module.tar.gz via `cmake --build <build> --target package`.
#
set(CPACK_PACKAGE_NAME "viam-camera-zivid")
set(CPACK_PACKAGE_FILE_NAME "module")
set(CPACK_GENERATOR "TGZ")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY 0)
include(CPack)
