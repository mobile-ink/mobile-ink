cmake_minimum_required(VERSION 3.13)
project(mobileink)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Node modules path passed from Gradle
set(NODE_MODULES_DIR ${NODE_MODULES_DIR})

# Skia paths from react-native-skia
set(SKIA_DIR "${NODE_MODULES_DIR}/@shopify/react-native-skia")
set(SKIA_INCLUDE_DIR "${SKIA_DIR}/cpp/skia")
set(SKIA_LIB_DIR "${SKIA_DIR}/libs/android/${ANDROID_ABI}")

# Shared C++ drawing engine from this package root.
set(NATIVE_DRAWING_CPP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../cpp")

message(STATUS "mobileink: NODE_MODULES_DIR=${NODE_MODULES_DIR}")
message(STATUS "mobileink: SKIA_DIR=${SKIA_DIR}")
message(STATUS "mobileink: NATIVE_DRAWING_CPP_DIR=${NATIVE_DRAWING_CPP_DIR}")
message(STATUS "mobileink: ANDROID_ABI=${ANDROID_ABI}")

# Include directories
include_directories(
    ${SKIA_INCLUDE_DIR}
    ${SKIA_INCLUDE_DIR}/include
    ${SKIA_INCLUDE_DIR}/modules/pathops/include
    ${NATIVE_DRAWING_CPP_DIR}
)

# Add source files
add_library(
    mobileink
    SHARED
    ${NATIVE_DRAWING_CPP_DIR}/SkiaDrawingEngine.cpp
    ${NATIVE_DRAWING_CPP_DIR}/DrawingSelection.cpp
    ${NATIVE_DRAWING_CPP_DIR}/BackgroundRenderer.cpp
    ${NATIVE_DRAWING_CPP_DIR}/DrawingSerialization.cpp
    ${NATIVE_DRAWING_CPP_DIR}/PathRenderer.cpp
    ${NATIVE_DRAWING_CPP_DIR}/EraserRenderer.cpp
    ${NATIVE_DRAWING_CPP_DIR}/StrokeSplitter.cpp
    ${NATIVE_DRAWING_CPP_DIR}/BatchExporter.cpp
    ${NATIVE_DRAWING_CPP_DIR}/ActiveStrokeRenderer.cpp
    jni_bridge.cpp
)

# Link Skia library
add_library(skia STATIC IMPORTED)
set_target_properties(skia PROPERTIES IMPORTED_LOCATION "${SKIA_LIB_DIR}/libskia.a")

add_library(pathops STATIC IMPORTED)
set_target_properties(pathops PROPERTIES IMPORTED_LOCATION "${SKIA_LIB_DIR}/libpathops.a")

# Link libraries
target_link_libraries(
    mobileink
    skia
    pathops
    android
    log
    GLESv2
    EGL
    jnigraphics
)
