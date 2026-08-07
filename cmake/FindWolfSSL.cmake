# Stub find-module: wolfssl is always built from 3rdparty/wolfssl here, so this
# only has to satisfy curl's find_package(WolfSSL REQUIRED).
#
# Deliberately NOT upstream's version, which sets
#     WOLFSSL_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/3rdparty/wolfssl
# The wolfssl revision from v0.0.41 onward DELETES any source-tree
# wolfssl/options.h and generates it into its binary dir instead, so pointing
# curl at the source tree makes curl/lib/md5.c fail with
# "'wolfssl/options.h' file not found" (it includes that header unconditionally
# whenever USE_WOLFSSL is defined).
#
# Passing dummies here instead makes curl link the wolfssl TARGET via
# WOLFSSL_LIBRARIES and inherit its PUBLIC include directories - which
# 3rdparty/wolfssl/CMakeLists.txt extends with the generated header's location.
set(WOLFSSL_LIBRARY ON)
set(WOLFSSL_INCLUDE_DIR ON)
set(WOLFSSL_LIBRARIES wolfssl)
set(WOLFSSL_FOUND TRUE)
