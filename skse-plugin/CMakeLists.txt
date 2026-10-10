cmake_minimum_required(VERSION 3.21)

if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
	set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "")

project(CHIMBFABridge VERSION 1.0.0 LANGUAGES CXX)

# CommonLibSSE NG (alandtse/CommonLibVR, branch ng) built from source. 7.x carries the
# Address Library v5 flag that SKSE 2.3+ requires on Skyrim 1.7.99 and later; the vcpkg
# port (3.6.0) predates it. Clone it to extern/CommonLibVR or pass -DCOMMONLIBSSE_DIR=.
set(COMMONLIBSSE_DIR "${CMAKE_SOURCE_DIR}/extern/CommonLibVR" CACHE PATH "Path to the CommonLibSSE NG source tree")
if(NOT EXISTS "${COMMONLIBSSE_DIR}/CMakeLists.txt")
	message(FATAL_ERROR "CommonLibSSE NG not found at ${COMMONLIBSSE_DIR}")
endif()
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK OFF CACHE BOOL "" FORCE)
add_subdirectory("${COMMONLIBSSE_DIR}" CommonLibSSE)

# The plugin declaration (SKSEPluginInfo) lives in src/main.cpp instead of the
# add_commonlibsse_plugin() generated file: the generated declaration drops the
# Address Library v5 bit.
add_library(${PROJECT_NAME} SHARED
	src/main.cpp
	src/Bridge.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_link_libraries(${PROJECT_NAME} PUBLIC CommonLibSSE::CommonLibSSE PRIVATE bcrypt winhttp)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

target_compile_definitions(${PROJECT_NAME} PRIVATE
	_UNICODE
	UNICODE
)

target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/Zi>)
target_link_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)

set(_compile_plugins_dir "${CMAKE_SOURCE_DIR}/Compile/SKSE/Plugins")
set_target_properties(${PROJECT_NAME} PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${_compile_plugins_dir}"
	RUNTIME_OUTPUT_DIRECTORY_DEBUG "${_compile_plugins_dir}"
	RUNTIME_OUTPUT_DIRECTORY_RELEASE "${_compile_plugins_dir}"
	LIBRARY_OUTPUT_DIRECTORY "${_compile_plugins_dir}"
	LIBRARY_OUTPUT_DIRECTORY_DEBUG "${_compile_plugins_dir}"
	LIBRARY_OUTPUT_DIRECTORY_RELEASE "${_compile_plugins_dir}"
)
