# Builds libglasses_bridge.so — the JNI bridge between UxSpace and the open
# viture-v2 driver.
#
# The upstream project links VITURE's proprietary libglasses.so here. This fork
# links a Rust driver instead, which speaks the Gen2 wire protocol directly.
# Build it with:
#
#   cargo build --release --target aarch64-linux-android
#   cp target/aarch64-linux-android/release/libviture_v2.so \
#      Android/glasses/src/main/jniLibs/arm64-v8a/
cmake_minimum_required(VERSION 3.22.1)
project(glasses_bridge)

set(JNILIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}")

# The Rust driver, consumed as a prebuilt shared library.
add_library(viture_v2 SHARED IMPORTED)
# IMPORTED_NO_SONAME makes CMake link by name (-lviture_v2) rather than by
# absolute path. Belt and braces alongside the soname the crate now sets: either
# alone is enough, and getting it wrong bakes a build-machine path into the
# bridge, which then fails to dlopen on the device.
set_target_properties(viture_v2 PROPERTIES
    IMPORTED_LOCATION "${JNILIBS_DIR}/libviture_v2.so"
    IMPORTED_NO_SONAME TRUE)

add_library(glasses_bridge SHARED glasses_bridge.cpp)

target_link_libraries(glasses_bridge
    viture_v2
    log
    android)
