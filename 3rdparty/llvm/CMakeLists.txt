
if(WITH_LLVM)
	set(USE_LLVM_VERSION 20.1.3)

	# RPCSX/Android: prefer building the 3rdparty/llvm submodule over downloading
	# a prebuilt.
	#
	# The prebuilt path fetches from RPCSX/llvm-build, which only publishes
	# 19.1.7 / 20.1.0 / 20.1.2 / 20.1.3. Upstream RPCS3 now targets LLVM 21+ APIs
	# (Value::hasUseList, the KnownFPClass header split, the computeKnownFPClass
	# argument order, Module::setTargetTriple taking a Triple), so pinning to
	# 20.1.3 means carrying a version guard for every one of those, forever, and
	# adding more each stage. The submodule is already at the revision upstream
	# pins (llvmorg-22.1.8); ARMSX3 builds exactly that from source for Android
	# arm64, so this is a proven configuration rather than an experiment.
	#
	# Set RPCSX_LLVM_PREBUILT=ON to go back to the download.
	option(RPCSX_LLVM_PREBUILT "Download a prebuilt LLVM instead of building the submodule" OFF)
	if (NOT MSVC)
		check_cxx_compiler_flag("-msse -msse2 -mcx16" COMPILER_X86)
		check_cxx_compiler_flag("-march=armv8-a+lse" COMPILER_ARM)
	else()
		set(COMPILER_X86 on) # TODO
	endif()

	if(BUILD_LLVM)
		message(STATUS "LLVM will be built from the submodule.")

		if (ANDROID)
			if (COMPILER_ARM)
				set(LLVM_TARGETS_TO_BUILD "AArch64" CACHE STRING "Semicolon-separated list of targets to build, or \"all\".")
				set(LLVM_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}-none-linux-android${ANDROID_NATIVE_API_LEVEL}" CACHE STRING "")
			else()
				set(LLVM_TARGETS_TO_BUILD "X86" CACHE STRING "Semicolon-separated list of targets to build, or \"all\".")
			endif()
		else()
			set(LLVM_TARGETS_TO_BUILD "AArch64;X86" CACHE STRING "Semicolon-separated list of targets to build, or \"all\".")
		endif()

		option(LLVM_BUILD_RUNTIME "Build the LLVM runtime libraries." OFF)
		option(LLVM_BUILD_TOOLS "Build the LLVM tools. If OFF, just generate build targets." OFF)
		option(LLVM_INCLUDE_BENCHMARKS "Generate benchmark targets. If OFF, benchmarks can't be built." OFF)
		option(LLVM_INCLUDE_DOCS "Generate build targets for llvm documentation." OFF)
		option(LLVM_INCLUDE_EXAMPLES "Generate build targets for the LLVM examples" OFF)
		option(LLVM_INCLUDE_TESTS "Generate build targets for the LLVM unit tests." OFF)
		option(LLVM_INCLUDE_TOOLS "Generate build targets for the LLVM tools." OFF)
		option(LLVM_INCLUDE_UTILS "Generate build targets for the LLVM utils." OFF)
		option(LLVM_CCACHE_BUILD "Set to ON for a ccache enabled build" OFF)
		set(LLVM_ENABLE_WARNINGS OFF CACHE BOOL "Enable compiler warnings.")

		# For Windows x86 (not Windows AArch64).
		# Check on MSVC is needed due to COMPILER_X86, COMPILER_ARM etc. are not set/supported by the MSVC compiler, if used.
		# Furthermore, the MSVC compiler is not available/supported on Windows AArch64
		if(WIN32 AND (COMPILER_X86 OR MSVC))
			set(LLVM_USE_INTEL_JITEVENTS ON)
		endif()

		set(LLVM_DOWNLOAD_BINARY "")

		if (ANDROID AND NOT RPCSX_LLVM_PREBUILT)
			# Leave LLVM_DOWNLOAD_BINARY empty so the branch below builds the
			# submodule. See the note at the top of this file.
			message(STATUS "LLVM: building submodule from source (no prebuilt above 20.1.3 exists)")
		elseif (ANDROID)
			string(APPEND LLVM_DOWNLOAD_BINARY llvm-android-)

			if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
				string(APPEND LLVM_DOWNLOAD_BINARY arm64-v8a)
			else()
				string(APPEND LLVM_DOWNLOAD_BINARY x64)
			endif()

			string(APPEND LLVM_DOWNLOAD_BINARY .7z)
		elseif ((WIN32 AND MSVC) OR LINUX)
			string(APPEND LLVM_DOWNLOAD_BINARY llvm-)
			if (WIN32)
				string(APPEND LLVM_DOWNLOAD_BINARY windows-)
			else()
				string(APPEND LLVM_DOWNLOAD_BINARY linux-)
			endif()

			if (COMPILER_X86)
				string(APPEND LLVM_DOWNLOAD_BINARY x64-)
			else()
				string(APPEND LLVM_DOWNLOAD_BINARY aarch64-)
			endif()

			if (WIN32)
				if (USE_MSVC_STATIC_CRT)
					string(APPEND LLVM_DOWNLOAD_BINARY MT)
				else()
					string(APPEND LLVM_DOWNLOAD_BINARY MD)
				endif()
			endif()

			string(APPEND LLVM_DOWNLOAD_BINARY .7z)
		endif()

		if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
			if(COMPILER_X86)
				set(LLVM_USE_INTEL_JITEVENTS ON)
			endif()
			set(LLVM_USE_PERF ON)
		endif()

		if (MSVC)
			add_compile_definitions("$<$<COMPILE_LANGUAGE:CXX>:_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS>")
		endif()

		if (LLVM_DOWNLOAD_BINARY STREQUAL "")
			# LLVM needs to be built out-of-tree.
			#
			# Build it into a PERSISTENT directory rather than under this build
			# tree, so that wiping build-android/ does not cost another
			# multi-hour LLVM build. Everything else here is cheap to redo; LLVM
			# is not. Ninja treats the cached tree as up to date, so subsequent
			# configures reuse it with no work.
			#
			# The path is keyed on the LLVM revision and ABI, so bumping the
			# submodule naturally gets a fresh directory instead of a stale mix.
			# Override with -DRPCSX_LLVM_CACHE_DIR=<path>, or the environment
			# variable of the same name.
			if (NOT RPCSX_LLVM_CACHE_DIR)
				set(RPCSX_LLVM_CACHE_DIR "$ENV{RPCSX_LLVM_CACHE_DIR}")
			endif()

			if (NOT RPCSX_LLVM_CACHE_DIR)
				execute_process(
					COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}/llvm" rev-parse --short=12 HEAD
					OUTPUT_VARIABLE RPCSX_LLVM_REV OUTPUT_STRIP_TRAILING_WHITESPACE
					ERROR_QUIET)
				if (NOT RPCSX_LLVM_REV)
					# git can fail here for reasons unrelated to the source being
					# valid (notably "dubious ownership" on Windows). Fall back to
					# the version the source declares rather than lumping every
					# revision into one "unknown" cache directory.
					file(READ "${CMAKE_CURRENT_LIST_DIR}/llvm/cmake/Modules/LLVMVersion.cmake" _rpcsx_llvm_ver_file)
					string(REGEX MATCH "set\\(LLVM_VERSION_MAJOR ([0-9]+)\\)" _m "${_rpcsx_llvm_ver_file}")
					if (CMAKE_MATCH_1)
						set(RPCSX_LLVM_REV "v${CMAKE_MATCH_1}")
					else()
						set(RPCSX_LLVM_REV "unknown")
					endif()
				endif()

				set(RPCSX_LLVM_CACHE_DIR
					"$ENV{USERPROFILE}/.rpcsx-llvm-cache/${RPCSX_LLVM_REV}-${CMAKE_SYSTEM_NAME}-${CMAKE_ANDROID_ARCH_ABI}")
			endif()

			message(STATUS "LLVM build cache: ${RPCSX_LLVM_CACHE_DIR}")
			if (EXISTS "${RPCSX_LLVM_CACHE_DIR}/lib/cmake/llvm")
				message(STATUS "LLVM: reusing cached build (delete that directory to force a rebuild)")
			else()
				message(STATUS "LLVM: no cache yet - this first build takes a long time, later ones reuse it")
			endif()

			# Cross-compiling LLVM configures a SECOND, host-targeted build tree
			# to produce llvm-tblgen/llvm-min-tblgen, because those tools must
			# RUN on this machine to generate sources for the target build.
			# That sub-configure inherits no compiler settings, and the NDK
			# clang cannot build host binaries (it targets
			# x86_64-w64-windows-gnu but ships no Windows libc headers), so it
			# dies with "No CMAKE_C_COMPILER could be found". Point it at a real
			# host toolchain -- WinLibs MinGW-w64 GCC, which the FFmpeg
			# cross-build already depends on for exactly the same reason.
			# Recompute on every configure when WE are the ones who set the
			# flags (tracked by the OWNED marker). Guarding purely on "is it
			# empty" would freeze the first value into the cache forever, so
			# later fixes here would silently never apply. A value the user
			# supplied is left alone.
			if (CMAKE_CROSSCOMPILING AND (NOT CROSS_TOOLCHAIN_FLAGS_NATIVE OR RPCSX_NATIVE_FLAGS_OWNED))
				if (NOT RPCSX_HOST_TOOLCHAIN_BIN)
					set(RPCSX_HOST_TOOLCHAIN_BIN "$ENV{RPCSX_HOST_TOOLCHAIN_BIN}")
				endif()
				if (NOT RPCSX_HOST_TOOLCHAIN_BIN)
					file(GLOB _rpcsx_host_bins
						"$ENV{LOCALAPPDATA}/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.*/mingw64/bin")
					if (_rpcsx_host_bins)
						list(GET _rpcsx_host_bins 0 RPCSX_HOST_TOOLCHAIN_BIN)
					endif()
				endif()

				find_program(RPCSX_HOST_CC  NAMES gcc HINTS "${RPCSX_HOST_TOOLCHAIN_BIN}")
				find_program(RPCSX_HOST_CXX NAMES g++ HINTS "${RPCSX_HOST_TOOLCHAIN_BIN}")

				if (NOT RPCSX_HOST_CC OR NOT RPCSX_HOST_CXX)
					message(FATAL_ERROR
						"Building LLVM from source while cross-compiling needs a HOST C/C++ compiler "
						"to build llvm-tblgen, and none was found.\n"
						"  Install one:  winget install BrechtSanders.WinLibs.POSIX.UCRT\n"
						"  Or point at one:  -DRPCSX_HOST_TOOLCHAIN_BIN=<dir containing gcc/g++>\n"
						"  Or skip the source build:  -DRPCSX_LLVM_PREBUILT=ON")
				endif()

				message(STATUS "LLVM host tools (tblgen) compiler: ${RPCSX_HOST_CXX}")
				set(_rpcsx_native_flags
					"-DCMAKE_C_COMPILER=${RPCSX_HOST_CC}"
					"-DCMAKE_CXX_COMPILER=${RPCSX_HOST_CXX}"
					"-DCMAKE_ASM_COMPILER=${RPCSX_HOST_CC}"
					# Link the host tools statically. A MinGW-built tblgen
					# otherwise needs libstdc++-6/libgcc_s_seh-1/libwinpthread-1
					# from the compiler's bin directory, which is not on PATH -
					# it then dies with exit 127 and NO error output, which reads
					# like a tblgen crash rather than a missing DLL. Static also
					# keeps the cached binary usable from any shell.
					"-DCMAKE_EXE_LINKER_FLAGS=-static")

				# LLVM forwards PYTHON_EXECUTABLE to the NATIVE build but not
				# Python3_EXECUTABLE, so its find_package(Python3) searches PATH
				# and lands on the pyenv shim (a .bat CMake cannot exec), failing
				# with 'Cannot use the interpreter'. Forward the real one.
				if (Python3_EXECUTABLE)
					list(APPEND _rpcsx_native_flags "-DPython3_EXECUTABLE=${Python3_EXECUTABLE}")
				endif()

				set(CROSS_TOOLCHAIN_FLAGS_NATIVE "${_rpcsx_native_flags}"
					CACHE STRING "Toolchain flags for the native (host) LLVM tools build" FORCE)
				set(RPCSX_NATIVE_FLAGS_OWNED ON CACHE INTERNAL
					"CROSS_TOOLCHAIN_FLAGS_NATIVE was set by us, not the user")
			endif()

			# LLVM's GetHostTriple.cmake cannot infer a triple when cross-
			# compiling from a Windows host with a non-MSVC/non-MinGW compiler
			# (our NDK clang): it warns "unable to determine host target triple"
			# and leaves LLVM_HOST_TRIPLE empty, so llvm-config.h emits
			# '#undef LLVM_HOST_TRIPLE' and lib/TargetParser fails to compile.
			#
			# "Host" here means the machine LLVM will RUN on, i.e. the Android
			# device. Use the same triples jit_compiler::triple1()/triple2()
			# hardcode for Android, so the build agrees with runtime.
			# (The old prebuilt shipped LLVM_HOST_TRIPLE="x86_64-unknown-linux-gnu"
			# and LLVM_NATIVE_ARCH=X86 in an arm64 library -- harmless only
			# because RPCSX never consults them on Android.)
			if (NOT LLVM_HOST_TRIPLE)
				if (CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
					set(_rpcsx_llvm_triple "aarch64-unknown-linux-android")
				elseif (CMAKE_ANDROID_ARCH_ABI STREQUAL "x86_64")
					set(_rpcsx_llvm_triple "x86_64-unknown-linux-android")
				elseif (CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi-v7a")
					set(_rpcsx_llvm_triple "armv7-unknown-linux-androideabi")
				elseif (CMAKE_ANDROID_ARCH_ABI STREQUAL "x86")
					set(_rpcsx_llvm_triple "i686-unknown-linux-android")
				endif()

				if (_rpcsx_llvm_triple)
					message(STATUS "LLVM host triple (inferred for ${CMAKE_ANDROID_ARCH_ABI}): ${_rpcsx_llvm_triple}")
					set(LLVM_HOST_TRIPLE "${_rpcsx_llvm_triple}" CACHE STRING "" FORCE)
					set(LLVM_DEFAULT_TARGET_TRIPLE "${_rpcsx_llvm_triple}" CACHE STRING "" FORCE)
				endif()
			endif()

			# MLIR is deliberately NOT built: nothing links 3rdparty_mlir and no
			# source references MLIR_AVAILABLE. The prebuilt tarball simply
			# bundled it. Building it would add a lot of time for nothing.
			set(LLVM_ENABLE_PROJECTS "" CACHE STRING "" FORCE)

			add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/llvm/llvm" "${RPCSX_LLVM_CACHE_DIR}" EXCLUDE_FROM_ALL)
			set(LLVM_DIR "${RPCSX_LLVM_CACHE_DIR}/lib/cmake/llvm/")
			set(MLIR_DIR "${RPCSX_LLVM_CACHE_DIR}/lib/cmake/mlir/")
		else()
			set(LLVM_DOWNLOAD_LINK https://github.com/RPCSX/llvm-build/releases/download/${USE_LLVM_VERSION})

			if (NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/${USE_LLVM_VERSION}-${LLVM_DOWNLOAD_BINARY}" AND
				NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/${USE_LLVM_VERSION}-${LLVM_DOWNLOAD_BINARY}.unpacked")
				message(STATUS "Downloading LLVM")
				file(DOWNLOAD ${LLVM_DOWNLOAD_LINK}/${LLVM_DOWNLOAD_BINARY}
					"${CMAKE_CURRENT_BINARY_DIR}/${USE_LLVM_VERSION}-${LLVM_DOWNLOAD_BINARY}.tmp" SHOW_PROGRESS
					STATUS FILE_STATUS)
				list(GET FILE_STATUS 0 STATUS_CODE)
				if (NOT STATUS_CODE EQUAL 0)
					file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/${USE_LLVM_VERSION}-${LLVM_DOWNLOAD_BINARY}.tmp")
					message(FATAL_ERROR "Failed to download LLVM")
				endif()

				file(RENAME
					"${CMAKE_CURRENT_BINARY_DIR}/${USE_LLVM_VERSION}-${LLVM_DOWNLOAD_BINARY}.tmp"
					"${CMAKE_CURRENT_BINARY_DIR}/${USE_LLVM_VERSION}-${LLVM_DOWNLOAD_BINARY}"
				)
			endif()

			if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/${USE_LLVM_VERSION}-${LLVM_DOWNLOAD_BINARY}.unpacked")
				file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${USE_LLVM_VERSION}-${LLVM_DOWNLOAD_BINARY}.dir")
				execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf "${CMAKE_CURRENT_BINARY_DIR}/${USE_LLVM_VERSION}-${LLVM_DOWNLOAD_BINARY}"
					WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${USE_LLVM_VERSION}-${LLVM_DOWNLOAD_BINARY}.dir" RESULT_VARIABLE STATUS_CODE)

				if (NOT STATUS_CODE EQUAL 0)
					message(FATAL_ERROR "Failed to unpack LLVM")
				endif()
				file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${USE_LLVM_VERSION}-${LLVM_DOWNLOAD_BINARY}.unpacked")
				file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/${USE_LLVM_VERSION}-${LLVM_DOWNLOAD_BINARY}")
			endif()

			file(GLOB LLVM_ROOT_DIR_LIST LIST_DIRECTORIES true "${CMAKE_CURRENT_BINARY_DIR}/${USE_LLVM_VERSION}-${LLVM_DOWNLOAD_BINARY}.dir/*")
			list(GET LLVM_ROOT_DIR_LIST 0 LLVM_ROOT_DIR)
			set(LLVM_DIR "${LLVM_ROOT_DIR}/lib/cmake/llvm")
			set(MLIR_DIR "${LLVM_ROOT_DIR}/lib/cmake/mlir")

			if (NOT EXISTS "${LLVM_DIR}")
				message(FATAL_ERROR "Failed to locate LLVM: ${LLVM_ROOT_DIR}")
			endif()
			
			if (NOT EXISTS "${MLIR_DIR}")
				message(FATAL_ERROR "Failed to locate MLIR: ${LLVM_ROOT_DIR}")
			endif()

			if (NOT ANDROID)
				set(Clang_DIR "${LLVM_ROOT_DIR}/lib/cmake/clang")
				if (NOT EXISTS "${Clang_DIR}")
					message(FATAL_ERROR "Failed to locate Clang: ${LLVM_ROOT_DIR}")
				endif()
			endif()
		endif()

		set(STATIC_LINK_LLVM ON CACHE BOOL "Link against LLVM statically. This will get set to ON if you build LLVM from the submodule." FORCE)

		# USE_LLVM_VERSION describes the PREBUILT. When we build the submodule
		# the version is whatever upstream pinned, so do not constrain it.
		if (LLVM_DOWNLOAD_BINARY STREQUAL "")
			find_package(LLVM CONFIG)
			find_package(MLIR CONFIG)
		else()
			find_package(LLVM ${USE_LLVM_VERSION} CONFIG)
			find_package(MLIR ${USE_LLVM_VERSION} CONFIG)
		endif()

		if(NOT LLVM_FOUND)
			message(FATAL_ERROR "Couldn't build LLVM from the submodule. You might need to run `git submodule update --init`")
		endif()

		# MLIR is only required for the prebuilt path, which ships it. See above.
		if(NOT MLIR_FOUND AND NOT LLVM_DOWNLOAD_BINARY STREQUAL "")
			message(FATAL_ERROR "Prebuilt LLVM is missing MLIR")
		endif()

		if (NOT ANDROID)
			find_package(Clang ${USE_LLVM_VERSION} CONFIG)
			if(NOT Clang_FOUND)
				message(FATAL_ERROR "Couldn't build Clang from the submodule. You might need to run `git submodule update --init`")
			endif()
		endif()
	else()
		message(STATUS "Using prebuilt or system LLVM")

		if (LLVM_DIR AND NOT IS_ABSOLUTE "${LLVM_DIR}")
			# change relative LLVM_DIR to be relative to the source dir
			set(LLVM_DIR ${CMAKE_SOURCE_DIR}/${LLVM_DIR})
		endif()

		if (MLIR_DIR AND NOT IS_ABSOLUTE "${MLIR_DIR}")
			set(MLIR_DIR ${CMAKE_SOURCE_DIR}/${MLIR_DIR})
		endif()

		if (Clang_DIR AND NOT IS_ABSOLUTE "${Clang_DIR}")
			set(Clang_DIR ${CMAKE_SOURCE_DIR}/${Clang_DIR})
		endif()

		find_package(LLVM CONFIG)
		find_package(MLIR CONFIG)

		if (NOT LLVM_FOUND)
			message(FATAL_ERROR "Can't find LLVM libraries from the CMAKE_PREFIX_PATH path or LLVM_DIR. Enable BUILD_LLVM option to build LLVM from included as a git submodule.")
		endif()

		if (LLVM_VERSION VERSION_LESS 18)
			message(FATAL_ERROR "Found LLVM version ${LLVM_VERSION}. Required version 18 or above.")
		endif()

		if (NOT MLIR_FOUND)
			message(FATAL_ERROR "Can't find MLIR libraries from the CMAKE_PREFIX_PATH path or MLIR_DIR: ${MLIR_DIR}")
		endif()


		if (NOT ANDROID)
			find_package(Clang CONFIG)

			if (NOT Clang_FOUND)
				message(FATAL_ERROR "Can't find Clang from the CMAKE_PREFIX_PATH path or Clang_DIR.")
			endif()
		endif()
	endif()

	if (STATIC_LINK_LLVM)
		if (NOT DEFINED LLVM_TARGETS_TO_BUILD)
			if(COMPILER_ARM)
				set(LLVM_TARGETS_TO_BUILD "AArch64" CACHE STRING "Semicolon-separated list of targets to build, or \"all\".")
			else()
				set(LLVM_TARGETS_TO_BUILD "X86" CACHE STRING "Semicolon-separated list of targets to build, or \"all\".")
			endif()
		endif()

		# For Windows x86 (not Windows AArch64) only when BUILD_LLVM is enabled and
		# for Linux x86 (not Linux AArch64) even if BUILD_LLVM is disabled (precompiled llvm used)
		if(LLVM_USE_INTEL_JITEVENTS OR (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND COMPILER_X86))
			list (APPEND LLVM_ADDITIONAL_LIBS IntelJITEvents)
		endif()

		# For Linux even if BUILD_LLVM is disabled (precompiled llvm used)
		# if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		# 	list (APPEND LLVM_ADDITIONAL_LIBS PerfJITEvents)
		# endif()

		llvm_map_components_to_libnames(LLVM_LIBS
			${LLVM_TARGETS_TO_BUILD}
			${LLVM_ADDITIONAL_LIBS}
			Core
			ExecutionEngine
			MCJIT
			Passes
		)

		set(MLIR_LIBS MLIRIR MLIRInferTypeOpInterface MLIRFuncDialect MLIRSCFDialect MLIRSCFToControlFlow MLIRAffineAnalysis MLIRAsyncToLLVM)
	else()
		if (MLIR_FOUND)
			set(LLVM_LIBS LLVM MLIR)
		else()
			set(LLVM_LIBS LLVM)
		endif()
	endif()

	list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}")
	list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
	
	include(TableGen)
	include(AddLLVM)
	if (MLIR_FOUND)
		include(AddMLIR)
	endif()

	if (NOT ANDROID)
		list(APPEND CMAKE_MODULE_PATH "${CLANG_CMAKE_DIR}")
		include(AddClang)

		get_target_property(CLANG_EXECUTABLE clang LOCATION)
	endif()
	# include(HandleLLVMOptions)

	add_library(3rdparty_llvm INTERFACE)
	target_link_libraries(3rdparty_llvm INTERFACE ${LLVM_LIBS})
	target_include_directories(3rdparty_llvm SYSTEM INTERFACE ${LLVM_INCLUDE_DIRS})
	separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
	target_compile_definitions(3rdparty_llvm INTERFACE ${LLVM_DEFINITIONS_LIST} LLVM_AVAILABLE)

	# Defined unconditionally so consumers still link, but only carries MLIR
	# when MLIR was actually built (prebuilt path). Nothing links it today.
	add_library(3rdparty_mlir INTERFACE)
	target_link_libraries(3rdparty_mlir INTERFACE 3rdparty_llvm)
	if (MLIR_FOUND)
		target_link_libraries(3rdparty_mlir INTERFACE ${MLIR_LIBS})
		target_include_directories(3rdparty_mlir INTERFACE ${MLIR_INCLUDE_DIRS})
		separate_arguments(MLIR_DEFINITIONS_LIST NATIVE_COMMAND ${MLIR_DEFINITIONS})
		target_compile_definitions(3rdparty_mlir INTERFACE ${MLIR_DEFINITIONS_LIST} MLIR_AVAILABLE)
	endif()

	add_library(3rdparty::llvm ALIAS 3rdparty_llvm)
	add_library(3rdparty::mlir ALIAS 3rdparty_mlir)
else()
	add_library(3rdparty::llvm ALIAS 3rdparty_dummy_lib)
	add_library(3rdparty::mlir ALIAS 3rdparty_dummy_lib)
endif()
