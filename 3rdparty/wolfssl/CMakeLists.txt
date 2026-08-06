if(USE_SYSTEM_WOLFSSL)
	message(STATUS "RPCS3: using shared wolfssl")
	pkg_check_modules(WolfSSL REQUIRED IMPORTED_TARGET wolfssl>=4.7.0)
	add_library(wolfssl INTERFACE)
	target_link_libraries(wolfssl INTERFACE PkgConfig::WolfSSL)
else()
	set(WOLFSSL_TLS13 OFF CACHE INTERNAL "")
	set(WOLFSSL_SHA3 ON CACHE INTERNAL "")
	set(WOLFSSL_SHAKE256 ON CACHE INTERNAL "")
	set(WOLFSSL_BASE64_ENCODE OFF CACHE INTERNAL "")
	set(WOLFSSL_DES3 ON CACHE INTERNAL "")
	set(WOLFSSL_PWDBASED ON CACHE INTERNAL "")
	set(WOLFSSL_FAST_MATH ON CACHE INTERNAL "")
	set(WOLFSSL_EXAMPLES OFF CACHE INTERNAL "")
	set(WOLFSSL_CRYPT_TESTS OFF CACHE INTERNAL "")
	set(WOLFSSL_ASYNC_THREADS OFF CACHE INTERNAL "")
	set(WOLFSSL_BUILD_OUT_OF_TREE ON CACHE INTERNAL "")
	set(WOLFSSL_SNI ON CACHE INTERNAL "")
	set(WOLFSSL_OPENSSLEXTRA ON CACHE INTERNAL "")
	set(WOLFSSL_ALT_CERT_CHAINS ON CACHE INTERNAL "")

	add_subdirectory(wolfssl EXCLUDE_FROM_ALL)

	target_compile_definitions(wolfssl PUBLIC WOLFSSL_DES_ECB HAVE_WRITE_DUP FP_MAX_BITS=8192 WOLFSSL_USE_OPTIONS_H)

	# The wolfssl revision v0.0.41 bumped to deletes any source-tree
	# wolfssl/options.h and generates it under its own binary dir instead, but
	# does not add that dir to its PUBLIC include path. curl/lib/md5.c includes
	# <wolfssl/options.h> unconditionally whenever USE_WOLFSSL is defined - it
	# does not honour WOLFSSL_NO_OPTIONS_H - so without this the build fails
	# with "'wolfssl/options.h' file not found". Export the generated header's
	# directory so every consumer of the wolfssl target inherits it.
	target_include_directories(wolfssl PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/wolfssl>)
	if (WIN32)
		target_include_directories(wolfssl PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/extra/win32>)
	endif()
endif()
