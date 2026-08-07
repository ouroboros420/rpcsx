cmake_minimum_required(VERSION 3.20)

find_program(CCACHE_PATH ccache HINTS ENV PATH)
if(CCACHE_PATH)
    message(STATUS "Using ccache: ${CCACHE_PATH}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PATH}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PATH}")
endif()

project(rpcsx)

if(CMAKE_CROSSCOMPILING)
    if(${CMAKE_SYSTEM_NAME} STREQUAL "Windows" AND NOT ${CMAKE_HOST_SYSTEM_NAME} STREQUAL "Windows" AND ${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang")
        message(STATUS "!!! Cross compilation for Windows !!!")
        add_link_options(-fuse-ld=lld -static-libgcc -static-libstdc++)
    else()
        message(FATAL "Cross compilation for Windows is supported with Clang compiler only")
    endif()
endif()


if (CMAKE_CROSSCOMPILING)
    function(rpcsx_setup_crosscompiling)
        add_custom_command(
            OUTPUT "${RPCSX_NATIVE_ROOT_DIR}/CMakeCache.txt"
            COMMAND ${CMAKE_COMMAND} -G "${CMAKE_GENERATOR}" 
                -B ${RPCSX_NATIVE_ROOT_DIR}
                -DCMAKE_MAKE_PROGRAM="${CMAKE_MAKE_PROGRAM}"
                -DCMAKE_C_COMPILER_LAUNCHER="${CMAKE_C_COMPILER_LAUNCHER}"
                -DCMAKE_CXX_COMPILER_LAUNCHER="${CMAKE_CXX_COMPILER_LAUNCHER}"
                -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="${RPCSX_NATIVE_ROOT_DIR}/bin"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT "Configuring native project..."
            USES_TERMINAL
        )

        unset(CMAKE_EXECUTABLE_SUFFIX)

        include(Platform/${CMAKE_HOST_SYSTEM_NAME} OPTIONAL RESULT_VARIABLE platformIncluded)
        if (platformIncluded)
            set(RPCSX_HOST_EXECUTABLE_SUFFIX ${CMAKE_EXECUTABLE_SUFFIX} PARENT_SCOPE)
        endif()
    endfunction()

    set(RPCSX_NATIVE_ROOT_DIR ${CMAKE_BINARY_DIR}/native)
    rpcsx_setup_crosscompiling()
    add_custom_target(RPCSX_CONFIGURE_NATIVE DEPENDS "${RPCSX_NATIVE_ROOT_DIR}/CMakeCache.txt")
endif()

function(import_native_executable_target target)
    set(options)
    set(one_value_keywords EXECUTABLE)
    set(multi_value_keywords)

    cmake_parse_arguments(
        arg
        "${options}"
        "${one_value_keywords}"
        "${multi_value_keywords}"
        ${ARGN}
    )

    if (CMAKE_CROSSCOMPILING)
        if (NOT DEFINED arg_EXECUTABLE)
            set(arg_EXECUTABLE ${target})
        endif()
        set(binary_path "${RPCSX_NATIVE_ROOT_DIR}/bin/${arg_EXECUTABLE}${RPCSX_HOST_EXECUTABLE_SUFFIX}")
        set(${target}_BINARY_PATH "${binary_path}" PARENT_SCOPE)

        if (NOT TARGET ${target}_NATIVE)
            if (CMAKE_GENERATOR MATCHES "Make")
                set(build_command "$(MAKE)" "-C" "${RPCSX_NATIVE_ROOT_DIR}" "${target}")
            else()
                set(build_command ${CMAKE_COMMAND} --build ${bin_dir} --target ${target} --parallel)
            endif()

            add_custom_command(OUTPUT "${binary_path}"
                COMMAND ${build_command}
                DEPENDS RPCSX_CONFIGURE_NATIVE
                WORKING_DIRECTORY "${RPCSX_NATIVE_ROOT_DIR}"
                COMMENT "Building native ${target}..."
                USES_TERMINAL
            )

            add_custom_target(${target}_NATIVE DEPENDS ${binary_path})
        endif()
    else()
        set(${target}_BINARY_PATH "$<TARGET_FILE:${target}>" PARENT_SCOPE)

        if (NOT TARGET ${target}_NATIVE)
            add_custom_target(${target}_NATIVE)
        endif()
    endif()
endfunction()

function(add_precompiled_vulkan_spirv target)
    import_native_executable_target(glslang-standalone EXECUTABLE glslang)

    add_library(${target} INTERFACE)
    set(SPIRV_GEN_ROOT_DIR "spirv-gen/include/")
    set(SPIRV_GEN_DIR "${SPIRV_GEN_ROOT_DIR}/shaders")

    cmake_path(ABSOLUTE_PATH SPIRV_GEN_ROOT_DIR BASE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} NORMALIZE OUTPUT_VARIABLE outputrootdir)
    cmake_path(ABSOLUTE_PATH SPIRV_GEN_DIR BASE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} NORMALIZE OUTPUT_VARIABLE outputdir)
    file(MAKE_DIRECTORY ${outputrootdir})
    file(MAKE_DIRECTORY ${outputdir})
    target_include_directories(${target} INTERFACE ${outputrootdir})

    foreach(input IN LISTS ARGN)
        cmake_path(GET input FILENAME inputname)
        cmake_path(REPLACE_EXTENSION inputname LAST_ONLY .h OUTPUT_VARIABLE outputname)
        cmake_path(APPEND outputdir ${outputname} OUTPUT_VARIABLE outputpath)
        cmake_path(REMOVE_EXTENSION inputname LAST_ONLY OUTPUT_VARIABLE varname)

        string(REPLACE "." "_" varname ${varname})
        string(PREPEND varname "spirv_")

        add_custom_command(
            OUTPUT ${outputpath}
            COMMAND ${glslang-standalone_BINARY_PATH} -V --target-env vulkan1.2 --vn "${varname}" -o "${outputpath}" "${CMAKE_CURRENT_SOURCE_DIR}/${input}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${input}" glslang-standalone_NATIVE
            COMMENT "Generating ${outputname}..."
        )

        set(subtarget ".${target}-subtarget-${outputname}")
        add_custom_target(${subtarget} DEPENDS ${outputpath})
        add_dependencies(${target} ${subtarget})
    endforeach()
endfunction()

function(target_base_address target address)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE off)

    if(CMAKE_CXX_COMPILER_LINKER_ID MATCHES "^(LLD|MOLD)$" OR MINGW)
        target_link_options(${target} PUBLIC "LINKER:--image-base=${address}")
    else()
        target_link_options(${target} PUBLIC "LINKER:-Ttext-segment,${address}")
    endif()
endfunction()


option(WITH_PS3 "Enable PS3 emulation support" OFF)
option(WITH_PS4 "Enable PS4 emulation support" ON)

option(COMPILE_GLFW "Compile GLFW" OFF)
option(COMPILE_VULKAN_LOADER "Compile Vulkan Loader" OFF)

option(WITHOUT_OPENGL "Disable OpenGL" OFF)
option(WITHOUT_OPENGLEW "Disable OpenGLEW" OFF)
option(WITHOUT_OPENAL "Disable OpenAL" OFF)
option(COMPILE_FFMPEG "Compile FFmpeg" ON)
option(USE_FS_GS_SYSCALL "Use the arch_prctl syscall to interact with fsbase and gsbase instead of x86 instructions. Used for compatibility with some systems, but comes at a performance cost." OFF)

if(USE_FS_GS_SYSCALL)
    add_definitions(-DUSE_FS_GS_SYSCALL)
endif()

if(APPLE OR WIN32)
    set(USE_SYSTEM_OPENAL_DEFAULT OFF)
else()
    set(USE_SYSTEM_OPENAL_DEFAULT ON)
endif()

# rpcs3 options
option(USE_NATIVE_INSTRUCTIONS "USE_NATIVE_INSTRUCTIONS makes rpcs3 compile with -march=native, which is useful for local builds, but not good for packages." ON)
option(WITH_LLVM "Enable usage of LLVM library" ON)
option(BUILD_LLVM "Build LLVM from git submodule" OFF)
option(STATIC_LINK_LLVM "Link against LLVM statically. This will get set to ON if you build LLVM from the submodule." OFF)
option(USE_FAUDIO "FAudio audio backend" ON)
option(USE_LIBEVDEV "libevdev-based joystick support" ON)
option(USE_DISCORD_RPC "Discord rich presence integration" OFF)
option(USE_VULKAN "Vulkan render backend" ON)
option(USE_PRECOMPILED_HEADERS "Use precompiled headers" OFF)
option(USE_SDL "Enables SDL input handler" ON)
option(USE_SYSTEM_CUBEB "Prefer system cubeb instead of the builtin one" OFF)
option(USE_SYSTEM_CURL "Prefer system Curl instead of the prebuild one" ON)
option(USE_SYSTEM_FAUDIO "Prefer system FAudio instead of the builtin one" OFF)
option(USE_SYSTEM_FFMPEG "Prefer system ffmpeg instead of the prebuild one" OFF)
option(USE_SYSTEM_PROTOBUF "Prefer system protobuf instead of the builtin one" OFF)
option(USE_SYSTEM_GLSLANG "Prefer system glslang instead of the builtin one" OFF)
option(USE_SYSTEM_HIDAPI "Prefer system hidapi instead of the builtin one" OFF)
option(USE_SYSTEM_LIBPNG "Prefer system libpng instead of the builtin one" OFF)
option(USE_SYSTEM_LIBUSB "Prefer system libusb instead of the builtin one" OFF)
option(USE_SYSTEM_MINIUPNPC "Prefer system MiniUPnPc instead of the builtin one" OFF)
option(USE_SYSTEM_MVK "Prefer system MoltenVK instead of the builtin one" OFF)
option(USE_SYSTEM_OPENAL "Prefer system OpenAL instead of the prebuild one" ${USE_SYSTEM_OPENAL_DEFAULT})
option(USE_SYSTEM_OPENCV "Prefer system OpenCV instead of the builtin one" ON)
option(USE_SYSTEM_PUGIXML "Prefer system pugixml instead of the builtin one" OFF)
option(USE_SYSTEM_RTMIDI "Prefer system RtMidi instead of the builtin one" OFF)
option(USE_SYSTEM_SDL "Prefer system SDL instead of the builtin one" ON)
option(USE_SYSTEM_VULKAN_MEMORY_ALLOCATOR "Prefer system Vulkan Memory Allocator instead of the builtin one" OFF)
option(USE_SYSTEM_WOLFSSL "Prefer system wolfSSL instead of the builtin one" OFF)
option(USE_SYSTEM_ZLIB "Prefer system ZLIB instead of the builtin one" ON)
option(USE_SYSTEM_ZSTD "Prefer system zstd instead of the builtin one" OFF)
option(HAS_MEMORY_BREAKPOINTS "Add support for memory breakpoints to the interpreter" OFF)
option(USE_LTO "Use LTO for building" ON)
option(BUILD_RPCS3_TESTS "Build RPCS3 unit tests." OFF)
option(RUN_RPCS3_TESTS "Run RPCS3 unit tests. Requires BUILD_RPCS3_TESTS" OFF)
option(USE_GAMEMODE "Choose whether to enable GameMode features or not." ON)

if (NOT WITH_PS3)
    set(WITHOUT_OPENGL on)
    set(WITHOUT_OPENGLEW on)
    set(WITHOUT_OPENAL on)
    set(WITH_LLVM off)
    set(USE_FAUDIO off)
    set(USE_LIBEVDEV off)
    set(USE_DISCORD_RPC off)
    set(USE_SYSTEM_ZLIB off)
    set(USE_VULKAN off)
    set(USE_PRECOMPILED_HEADERS off)
    set(USE_SDL off)
    set(USE_SYSTEM_SDL off)
    set(USE_SYSTEM_FFMPEG off)
    set(USE_SYSTEM_OPENAL off)
    set(USE_SYSTEM_CURL off)
    set(USE_SYSTEM_OPENCV off)
    set(HAS_MEMORY_BREAKPOINTS off)
endif()

set(CMAKE_CXX_EXTENSIONS off)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED on)
set(CMAKE_BUILD_RPATH_USE_ORIGIN on)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
        message(FATAL_ERROR "RPCS3 requires at least gcc-13.")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.0)
        message(FATAL_ERROR "RPCS3 requires at least clang-19.0.")
    endif()
endif()

include(CheckCXXCompilerFlag)

get_property(IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(IS_MULTI_CONFIG)
    set(USE_DISCORD_RPC OFF CACHE BOOL "Discord RPC is only available with single-config generator" FORCE)
else()
    if(NOT CMAKE_BUILD_TYPE)
        message(STATUS "No build type selected, default to Release")
        set(CMAKE_BUILD_TYPE "Release")
    endif()
endif()

if(CMAKE_BUILD_TYPE MATCHES "Debug" AND NOT MSVC)
    add_compile_definitions(_DEBUG)
endif()

if(MSVC)
    # NOTE: kept in RPCSX (upstream dropped it in v0.0.41) because
    # 3rdparty/llvm/CMakeLists.txt still selects the MT/MD prebuilt LLVM archive
    # based on USE_MSVC_STATIC_CRT.
    option(USE_MSVC_STATIC_CRT "Use static MSVC C runtime" OFF)

    if(NOT IS_MULTI_CONFIG)
        if(NOT(CMAKE_BUILD_TYPE MATCHES "Release" AND USE_MSVC_STATIC_CRT))
            set(USE_DISCORD_RPC OFF CACHE BOOL "Discord RPC is only available in Release and static CRT build." FORCE)
        endif()
    endif()

    if(USE_MSVC_STATIC_CRT)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    endif()

    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/MP>")
endif()

if(APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")
    list(APPEND CMAKE_SYSTEM_PREFIX_PATH "/opt/homebrew")
endif()

if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/wd4530;/utf-8>") # C++ exception handler used, but unwind semantics are not enabled
endif()

include(CheckFunctionExists)

add_subdirectory(3rdparty EXCLUDE_FROM_ALL)
add_subdirectory(rx EXCLUDE_FROM_ALL)
add_subdirectory(test)

include(3rdparty/llvm/CMakeLists.txt)

if (NOT RX_TAG)
    set(RX_TAG 0)
endif()

if (NOT RX_TAG_VERSION)
    set(RX_TAG_VERSION 0)
endif()

target_compile_definitions(rx PRIVATE
    RX_TAG=${RX_TAG}
    RX_TAG_VERSION=${RX_TAG_VERSION}
)

if (WITH_PS4)
    find_package(nlohmann_json CONFIG)
    add_subdirectory(tools)
endif()

## Look for Gamemode if its installed on Linux
if(LINUX)
    ## User chooses whether to Enable GameMode features or not
    if(USE_GAMEMODE)
        add_compile_definitions(GAMEMODE_AVAILABLE)
    endif()
endif()

add_subdirectory(rpcsx)
add_subdirectory(kernel)

if (WITH_PS3)
    include(ConfigureCompiler)
    add_subdirectory(rpcs3)
    add_subdirectory(ps3fw)
endif()
