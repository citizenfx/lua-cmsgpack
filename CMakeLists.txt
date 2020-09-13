cmake_minimum_required(VERSION 2.8.0 FATAL_ERROR)

project(lua-cmsgpack C)
if (CMAKE_C_COMPILER MATCHES ".*clang")
    set(CMAKE_COMPILER_IS_CLANGCC 1)
endif()

# compile in release  mode by default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif()

option (LUACMSGPACK_SAFE "Wrap API functions in pcall" OFF)
if(LUACMSGPACK_SAFE)
    add_definitions(-DLUACMSGPACK_SAFE)
endif()

option (LUACMSGPACK_COMPAT "Compatible pack()" OFF)
if(LUACMSGPACK_COMPAT)
    add_definitions(-DLUACMSGPACK_COMPAT)
endif()

option (LUACMSGPACK_STATIC_NIL "Store null reference in a static variable (no longer reentrant)" OFF)
if (LUACMSGPACK_STATIC_NIL)
    add_definitions(-DLUACMSGPACK_STATIC_NIL)
endif()

if(UNIX)
    if(APPLE)
        set(PLAT "macosx")
        set(LINK_FLAGS "-bundle -undefined dynamic_lookup -all_load")
    else(APPLE)
        set(PLAT "linux")
        set(LINK_FLAGS "-shared")
    endif(APPLE)

    # Compiler options taken from Lua: lua/blob/master/makefile
    if (CMAKE_COMPILER_IS_CLANGCC)
        add_compile_options(
            -ferror-limit=5
            -fno-common
            #-Weverything
            #-Wno-gnu-zero-variadic-macro-arguments
            #-Wno-covered-switch-default
            #-Wno-reserved-id-macro
        )
    elseif (CMAKE_COMPILER_IS_GNUCC)
        add_compile_options(
            -fmax-errors=5
            -Wduplicated-cond
            -Wduplicated-branches
            -Wrestrict
            -Wnull-dereference
            -Wlogical-op
        )
    endif()

    add_compile_options(
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

        # c_master is a C-only branch.
        -Wdeclaration-after-statement
        -Wmissing-prototypes
        -Wnested-externs
        -Wstrict-prototypes
        -Wc++-compat
        -Wold-style-definition
    )

    option (LUACMSGPACK_DEBUG "Include ltests.h" OFF)
    if(LUACMSGPACK_DEBUG)
        add_compile_options(-O0 -g)
        add_definitions(-DLUACMSGPACK_DEBUG)
        add_definitions(-DLUA_USER_H="ltests.h")
    else()
        add_compile_options(-O3)
    endif()

    option (LUACMSGPACK_BIT32 "32bit compile" OFF)
    if(LUACMSGPACK_BIT32)
        add_definitions(-DLUACMSGPACK_BIT32)
        if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
            set (CMAKE_CXX_FLAGS "-m32 ${CMAKE_CXX_FLAGS}")
            set (CMAKE_C_FLAGS "-m32 ${CMAKE_C_FLAGS}")
            set (CMAKE_EXE_LINKER_FLAGS "-m32 ${CMAKE_EXE_LINKER_FLAGS}")
        elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang")
            set (CMAKE_CXX_FLAGS "-m32 ${CMAKE_CXX_FLAGS}")
            set (CMAKE_C_FLAGS "-m32 ${CMAKE_C_FLAGS}")
            set (CMAKE_EXE_LINKER_FLAGS "-m32 ${CMAKE_EXE_LINKER_FLAGS}")
        endif()
    endif()
else(UNIX)
    if(WIN32)
        set(PLAT "win32")
        add_definitions(-DLUA_BUILD_AS_DLL -DLUA_LIB -D_CRT_SECURE_NO_WARNINGS)
    else(WIN32)
        set(PLAT "unsupported")
    endif(WIN32)
endif(UNIX)

#-------------------------------------------------------------------------------
# Lua dependency
#-------------------------------------------------------------------------------
if(NOT LUA_INCLUDE_DIR OR (WIN32 AND NOT LUA_LIBRARIES))
    set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")
    find_package(Lua REQUIRED)
endif()
#-------------------------------------------------------------------------------

#-------------------------------------------------------------------------------
# msgpack-c external dependency
#-------------------------------------------------------------------------------
set (CMAKE_C_FLAGS "-fPIC ${CMAKE_C_FLAGS}")
set(MSGPACK_C ${CMAKE_BINARY_DIR}/msgpack-c)
set(MSGPACK_C_DIR ${MSGPACK_C}/repo)
set(MSGPACK_C_LIB_DIR ${MSGPACK_C_DIR})
set(MSGPACK_C_INCLUDE_DIR ${MSGPACK_C_DIR}/include)
if(LUACMSGPACK_BIT32)
    set(MSGPACK_C_32BIT -DMSGPACK_32BIT=ON)
else()
    set(MSGPACK_C_32BIT "")
endif()

# Need to ensure msgpack-c is compiled with -fPIC.
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
add_library(libmsgpackc STATIC SHARED IMPORTED)
set_target_properties(libmsgpackc PROPERTIES IMPORTED_LOCATION ${MSGPACK_C_LIB_DIR}/libmsgpackc.a)
add_dependencies(libmsgpackc msgpack)
set_property(TARGET msgpack PROPERTY IMPORTED_LOCATION libmsgpackc)
#-------------------------------------------------------------------------------

include_directories(${LUA_INCLUDE_DIR} ${MSGPACK_C_INCLUDE_DIR})
set(SOURCES src/lua_pack_template.h src/lua_cmsgpack.h src/lua_cmsgpacklib.h src/lua_cmsgpack.c )
add_library(cmsgpack MODULE ${SOURCES} )

if (LINK_FLAGS)
    set_target_properties(cmsgpack PROPERTIES
        LINK_FLAGS ${LINK_FLAGS}
    )
endif()

set_target_properties(cmsgpack PROPERTIES PREFIX "")

if(WIN32)
    # Win32 modules need to be linked to the Lua library.
    set(_MODULE_LINK ${LUA_LIBRARY} ${_MODULE_LINK})
endif()

if(LUACMSGPACK_BIT32)
    set_target_properties(cmsgpack PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()

target_link_libraries(cmsgpack ${_MODULE_LINK} libmsgpackc)
if (CMAKE_INSTALL_PREFIX)
  install(TARGETS cmsgpack DESTINATION "${CMAKE_INSTALL_PREFIX}/lib")
endif(CMAKE_INSTALL_PREFIX)