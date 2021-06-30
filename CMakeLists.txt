cmake_minimum_required(VERSION 3.5)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)

project(uvgrtp)

option(DISABLE_CRYPTO "Do not build uvgRTP with crypto enabled")
option(PTHREADS_PATH  "Path to POSIX threads static library")
option(CRYPTOPP_PATH  "Path to Crypto++ static library")

add_library(uvgrtp STATIC
    src/clock.cc
    src/crypto.cc
    src/dispatch.cc
    src/frame.cc
    src/hostname.cc
    src/lib.cc
    src/media_stream.cc
    src/mingw_inet.cc
    src/multicast.cc
    src/pkt_dispatch.cc
    src/poll.cc
    src/queue.cc
    src/random.cc
    src/rtcp.cc
    src/rtp.cc
    src/runner.cc
    src/session.cc
    src/socket.cc
    src/zrtp.cc
    src/holepuncher.cc
    src/formats/media.cc
    src/formats/h26x.cc
    src/formats/h264.cc
    src/formats/h265.cc
    src/formats/h266.cc
    src/zrtp/zrtp_receiver.cc
    src/zrtp/hello.cc
    src/zrtp/hello_ack.cc
    src/zrtp/commit.cc
    src/zrtp/dh_kxchng.cc
    src/zrtp/confirm.cc
    src/zrtp/confack.cc
    src/zrtp/error.cc
    src/zrtp/zrtp_message.cc
    src/srtp/base.cc
    src/srtp/srtp.cc
    src/srtp/srtcp.cc
)

target_include_directories(uvgrtp
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

set(LIBRARY_PATHS "")

if (PTHREADS_PATH)
    separate_arguments(LIBRARY_PATHS NATIVE_COMMAND ${PTHREADS_PATH})
endif (PTHREADS_PATH)

if (CRYPTOPP_PATH AND NOT DISABLE_CRYPTO)
    separate_arguments(LIBRARY_PATHS NATIVE_COMMAND "${LIBRARY_PATHS} ${CRYPTOPP_PATH}")
endif (CRYPTOPP_PATH AND NOT DISABLE_CRYPTO)

if (UNIX)
    set(COMPILER_FLAGS "-Wall -Wextra -Wuninitialized -Wshadow -O2 -g -march=native")
    
    if (DISABLE_CRYPTO)
        string(APPEND COMPILER_FLAGS " -D__RTP_NO_CRYPTO__")
    endif(DISABLE_CRYPTO)

    # Check the getrandom() function exists
    include(CheckCXXSymbolExists)
    check_cxx_symbol_exists(getrandom sys/random.h HAVE_GETRANDOM)

    if(HAVE_GETRANDOM)
        add_compile_definitions(HAVE_GETRANDOM=1)
    endif()

    # if (NOT "${LIBRARY_PATHS}" STREQUAL "")
    #     add_custom_command(TARGET uvgrtp POST_BUILD
    #         COMMAND ar crsT ARGS libuvgrtp_thin.a libuvgrtp.a ${LIBRARY_PATHS}
    #         BYPRODUCTS libuvgrtp_thin.a
    #         COMMENT "Creating combined archive"
    #     )
    #     install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libuvgrtp_thin.a
    #             RENAME libuvgrtp.a
    #             DESTINATION /usr/local/lib
    #     )
    # else (NOT "${LIBRARY_PATHS}" STREQUAL "")
        install(TARGETS uvgrtp
                ARCHIVE
                DESTINATION /usr/local/lib
        )
    # endif (NOT "${LIBRARY_PATHS}" STREQUAL "")

    install(DIRECTORY include/ DESTINATION /usr/local/include/uvgrtp
            FILES_MATCHING PATTERN "*.hh"
    )

    # Find pkg-config installed in the system
    find_package(PkgConfig REQUIRED)
    if(PkgConfig_FOUND)
        # Check PKG_CONFIG_PATH, if not defined, use /usr/local/lib/pkgconfig
        if(NOT DEFINED ENV{PKG_CONFIG_PATH})
            set( PKG_CONFIG_PATH "/usr/local/lib/pkgconfig")
            message("PKG_CONFIG_PATH is not found. Set it with ${PKG_CONFIG_PATH}")
        endif(NOT DEFINED ENV{PKG_CONFIG_PATH})

        # Find crypto++
        pkg_search_module(CRYPTOPP libcrypto++)

        # Generate and install .pc file
        configure_file("uvgrtp.pc.in" "uvgrtp.pc" @ONLY)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/uvgrtp.pc DESTINATION ${PKG_CONFIG_PATH}/)
    endif(PkgConfig_FOUND)
endif (UNIX)

if (WIN32)
    if(MSVC)
	    # /Zc:__cplusplus makes it so the correct c++ version is defined 
        set(COMPILER_FLAGS "/O2 /Zc:__cplusplus")
    else(MSVC)
        set(COMPILER_FLAGS "-O2")
    endif(MSVC)
    SET(OUT_DIR ${CMAKE_BINARY_DIR}/Debug)

    if (DISABLE_CRYPTO)
        if(MSVC)
            string(APPEND COMPILER_FLAGS " /D__RTP_NO_CRYPTO__")
        else(MSVC)
            string(APPEND COMPILER_FLAGS " -D__RTP_NO_CRYPTO__")
        endif(MSVC)
    endif(DISABLE_CRYPTO)

    # if (NOT "${LIBRARY_PATHS}" STREQUAL "")
    #     add_custom_command(TARGET uvgrtp POST_BUILD
    #         COMMAND lib /out:${OUT_DIR}/uvgrtp.lib ${OUT_DIR}/uvgrtp.lib ${LIBRARY_PATHS}
    #         BYPRODUCTS uvgrtp.lib
    #         COMMENT "Creating combined archive"
    #     )
    # else (NOT "${LIBRARY_PATHS}" STREQUAL "")
        install(TARGETS uvgrtp
                ARCHIVE
                DESTINATION ${PROJECT_BINARY_DIR}/lib
        )
    # endif (NOT "${LIBRARY_PATHS}" STREQUAL "")

    install(DIRECTORY include/ DESTINATION ${PROJECT_BINARY_DIR}/include
            FILES_MATCHING PATTERN "*.hh"
    )
endif (WIN32)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMPILER_FLAGS}")
