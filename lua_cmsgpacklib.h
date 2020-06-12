/*
** See LICENSE.
*/
#ifndef lua_cmsgpacklib_h
#define lua_cmsgpacklib_h

#include <luaconf.h>
#include <lua.h>

#define LUACMSGPACK_NAME "lua-msgpack-c"
#define LUACMSGPACK_VERSION "lua-msgpack-c 0.1.0"
#define LUACMSGPACK_COPYRIGHT "Copyright (C) 2012, Salvatore Sanfilippo; 2020, Gottfried Leibniz"
#define LUACMSGPACK_DESCRIPTION "msgpack-c bindings for Lua"

#if !defined(LUAMOD_API)  /* LUA_VERSION_NUM == 501 */
  #define LUAMOD_API LUALIB_API
#endif

/*
** pack(...): receives any number of arguments and MessagePacks their values.
** Returning 1, corresponding to the encoded string/
**
** RULES:
**  (0) Empty tables are encoded as arrays.
**  (1) A table is converted into a MessagePack iff all of the table keys are
**    composed of incrementing integers starting at 1.
**  (2) A lua_Number is converted into an integer type if floor(num) == num;
**    Otherwise, a lua_NUmber will always be packed as a double to avoid a loss
**    of precision.
*/
LUALIB_API int mp_pack (lua_State *L);

/*
** unpack(encoded_string [, offset [, limit [, substring]]]): Unpack all
** elements, up to a potential limit, from a msgpack encoded string. Returning
** the number of unpacked Lua objects.
**
**  offset: offset within the encoded string to start decoding.
**  limit: number of Lua objects to decode, 0 to decode the entire string.
**  substring: length of the encoded substring (starting at offset).
**
** NOTES:
**  The 'offset' & 'substring' arguments are sugar to avoid the additional sub()
**  call when unpacking concatenated messages.
**
** RULES:
**  (0) (<5.3) When 64-bit integers are converted back into lua_Number, it is
**    possible the resulting number will only be an approximation to the
**    original number. This is unavoidable due to the nature of floating point
**    types.
*/
LUALIB_API int mp_unpack (lua_State *L);

LUAMOD_API int luaopen_cmsgpack (lua_State *L);

#endif
