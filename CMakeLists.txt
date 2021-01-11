cmake_minimum_required(VERSION 2.8.0 FATAL_ERROR)

project(lua-cmsgpack C)
IF (CMAKE_C_COMPILER MATCHES ".*clang")
    SET (CMAKE_COMPILER_IS_CLANGCC 1)
ENDIF ()

# compile in release  mode by default
IF (NOT CMAKE_BUILD_TYPE)
    SET (CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
ENDIF ()

OPTION (LUACMSGPACK_SAFE "Wrap API functions in pcall" OFF)
IF (LUACMSGPACK_SAFE)
    ADD_DEFINITIONS (-DLUACMSGPACK_SAFE)
ENDIF ()

OPTION (LUACMSGPACK_COMPAT "Compatible pack()" OFF)
IF (LUACMSGPACK_COMPAT)
    ADD_DEFINITIONS (-DLUACMSGPACK_COMPAT)
ENDIF ()

OPTION (LUA_COMPILED_AS_HPP "Library compiled for Lua with C++ linkage" OFF)
IF (LUA_COMPILED_AS_HPP)
    ADD_DEFINITIONS (-DLUA_COMPILED_AS_HPP)
ENDIF ()

OPTION (LUACMSGPACK_EXPERIMENTAL "Experimental performance tweaks" OFF)
IF (LUACMSGPACK_EXPERIMENTAL)
    ADD_DEFINITIONS (-DLUACMSGPACK_EXPERIMENTAL)
ENDIF ()

IF (UNIX)
    IF (APPLE)
        SET (PLAT "macosx")
        SET (LINK_FLAGS "-bundle -undefined dynamic_lookup -all_load")
    ELSE (APPLE)
        SET (PLAT "linux")
        SET (LINK_FLAGS "-shared")
    ENDIF (APPLE)

    # Compiler options taken from Lua: lua/blob/master/makefile
    IF (CMAKE_COMPILER_IS_CLANGCC)
        ADD_COMPILE_OPTIONS (
            -ferror-limit=5
            -fno-common
            #-Weverything
            #-Wno-gnu-zero-variadic-macro-arguments
            #-Wno-covered-switch-default
            #-Wno-reserved-id-macro
            #-Wno-c++98-compat
            #-Wno-c++98-compat-pedantic
        )
    ELSEIF (CMAKE_COMPILER_IS_GNUCC)
        ADD_COMPILE_OPTIONS (
            -fmax-errors=5
            -Wduplicated-cond
            -Wduplicated-branches
            -Wrestrict
            -Wnull-dereference
            -Wlogical-op
        )
    ENDIF ()

    ADD_COMPILE_OPTIONS (
        -fPIC
        -Wall
        -Wextra
        -Wshadow
        -Wsign-compare
        -Wundef
        -Wwrite-strings
        -Wredundant-decls
        -Wdouble-promotion
        -Wunused-parameter
        -Wno-comment
        -Wno-unused-function
        -Wno-array-bounds
        -Wconversion
        -Wno-sign-conversion
        -Wstrict-overflow=2
        -Wformat=2
        -Wcast-qual
        #-Wdisabled-optimization
        #-Wlogical-op
        #-Wno-aggressive-loop-optimizations
        # the next warnings might be useful sometimes, but usually they generate too much noise
        #-Werror
    )

    IF (NOT LUA_COMPILED_AS_HPP)
        # c_master is a C-only branch.
        ADD_COMPILE_OPTIONS (
            -Wdeclaration-after-statement
            -Wmissing-prototypes
            -Wnested-externs
            -Wstrict-prototypes
            -Wc++-compat
            -Wold-style-definition
        )
    ENDIF ()

    OPTION (LUACMSGPACK_DEBUG "Include ltests.h" OFF)
    IF (LUACMSGPACK_DEBUG)
        ADD_COMPILE_OPTIONS (-O0 -g)
        ADD_DEFINITIONS (-DLUACMSGPACK_DEBUG)
        ADD_DEFINITIONS (-DLUA_USER_H="ltests.h")
    ELSE ()
        ADD_COMPILE_OPTIONS (-O3)
    ENDIF ()

    OPTION (LUACMSGPACK_BIT32 "32bit compile" OFF)
    IF (LUACMSGPACK_BIT32)
        ADD_DEFINITIONS (-DLUACMSGPACK_BIT32)
        IF ("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
            SET (CMAKE_CXX_FLAGS "-m32 ${CMAKE_CXX_FLAGS}")
            SET (CMAKE_C_FLAGS "-m32 ${CMAKE_C_FLAGS}")
            SET (CMAKE_EXE_LINKER_FLAGS "-m32 ${CMAKE_EXE_LINKER_FLAGS}")
        ELSEIF ("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang")
            SET (CMAKE_CXX_FLAGS "-m32 ${CMAKE_CXX_FLAGS}")
            SET (CMAKE_C_FLAGS "-m32 ${CMAKE_C_FLAGS}")
            SET (CMAKE_EXE_LINKER_FLAGS "-m32 ${CMAKE_EXE_LINKER_FLAGS}")
        ENDIF ()
    ENDIF ()
ELSE (UNIX)
    IF (WIN32)
        SET (PLAT "win32")
        ADD_DEFINITIONS (-DLUA_BUILD_AS_DLL -DLUA_LIB -D_CRT_SECURE_NO_WARNINGS)
        ADD_COMPILE_OPTIONS ( /W4 )
    ELSE(WIN32)
        SET (PLAT "unsupported")
    ENDIF (WIN32)
ENDIF (UNIX)

#-------------------------------------------------------------------------------
# Lua dependency
#-------------------------------------------------------------------------------
IF (NOT LUA_INCLUDE_DIR OR (WIN32 AND NOT LUA_LIBRARIES))
    SET (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")
    FIND_PACKAGE (Lua REQUIRED)
ENDIF ()
#-------------------------------------------------------------------------------

#-------------------------------------------------------------------------------
# msgpack-c external dependency
#-------------------------------------------------------------------------------

# Need to ensure msgpack-c is compiled with -fPIC.
IF (UNIX)
    SET (CMAKE_C_FLAGS "-fPIC ${CMAKE_C_FLAGS}")
ENDIF ()

SET (MSGPACK_C ${CMAKE_BINARY_DIR}/msgpack-c)
SET (MSGPACK_C_DIR ${MSGPACK_C}/repo)
SET (MSGPACK_C_LIB_DIR ${MSGPACK_C_DIR})
SET (MSGPACK_C_INCLUDE_DIR ${MSGPACK_C_DIR}/include)
IF (LUACMSGPACK_BIT32)
    SET (MSGPACK_C_32BIT -DMSGPACK_32BIT=ON)
ELSE ()
    SET (MSGPACK_C_32BIT "")
ENDIF ()

include(ExternalProject)
ExternalProject_Add(msgpack
    PREFIX ${MSGPACK_C}
    GIT_REPOSITORY https://github.com/msgpack/msgpack-c.git
    GIT_TAG "c_master"
    CMAKE_ARGS += -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                  -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                  -DMSGPACK_BUILD_EXAMPLES=OFF
                  -DMSGPACK_BUILD_TESTS=OFF
                  -DBUILD_SHARED_LIBS=OFF
                  -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
                  ${MSGPACK_C_32BIT}
    BUILD_COMMAND make
    INSTALL_COMMAND ""
    TEST_COMMAND ""
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
    BINARY_DIR ${MSGPACK_C_DIR}
    SOURCE_DIR ${MSGPACK_C_DIR}
    STEP_TARGETS build
    EXCLUDE_FROM_ALL TRUE
    BUILD_BYPRODUCTS ${MSGPACK_C_LIB_DIR}/libmsgpackc.a
)

ADD_LIBRARY (libmsgpackc STATIC SHARED IMPORTED)
SET_TARGET_PROPERTIES (libmsgpackc PROPERTIES IMPORTED_LOCATION ${MSGPACK_C_LIB_DIR}/libmsgpackc.a)
SET_PROPERTY (TARGET msgpack PROPERTY IMPORTED_LOCATION libmsgpackc)

#-------------------------------------------------------------------------------

INCLUDE_DIRECTORIES (${LUA_INCLUDE_DIR} ${MSGPACK_C_INCLUDE_DIR})
FILE (GLOB_RECURSE sourceFiles ./src/*.c)
FILE (GLOB_RECURSE headerFiles ./src/*.h)
SET (SOURCES ${sourceFiles} ${headerFiles})
IF (LUA_COMPILED_AS_HPP)
    ENABLE_LANGUAGE (CXX)
    SET_SOURCE_FILES_PROPERTIES (${sourceFiles} PROPERTIES LANGUAGE CXX)
ENDIF ()

ADD_LIBRARY (cmsgpack MODULE ${SOURCES})
ADD_DEPENDENCIES (libmsgpackc msgpack)
IF (LUACMSGPACK_BIT32)
    SET_TARGET_PROPERTIES (cmsgpack PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
ENDIF ()

# Win32 modules need to be linked to the Lua library.
IF (WIN32)
    TARGET_INCLUDE_DIRECTORIES (cmsgpack PRIVATE ${INCLUDE_DIRECTORIES})
    TARGET_LINK_LIBRARIES (cmsgpack ${LUA_LIBRARIES})
ENDIF ()

IF (LINK_FLAGS)
    SET_TARGET_PROPERTIES (cmsgpack PROPERTIES LINK_FLAGS "${LINK_FLAGS}")
ENDIF ()

SET_TARGET_PROPERTIES (cmsgpack PROPERTIES PREFIX "")

TARGET_LINK_LIBRARIES (cmsgpack ${_MODULE_LINK} libmsgpackc)
IF (CMAKE_INSTALL_PREFIX)
  INSTALL (TARGETS cmsgpack DESTINATION "${CMAKE_INSTALL_PREFIX}/lib")
ENDIF (CMAKE_INSTALL_PREFIX)