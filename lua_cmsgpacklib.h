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

/*
** extend(encoder_table): Register an extension-type. The encoder_table is often
** a metatable with additional metamethods for serializing tables/userdata:
**   __ext: Unique msgpack extension identifier. Note that applications can only
**          assign 0 to 127 to store application-specific type information.
**   __pack: An encoder function: f(self, type): Where "type" is the extension
**           type identifier (one function handling multiple encodings).
**   __unpack: A decoder function: f(encoded string)
**
** E.g.,
**   metatable = {
**     __ext = 0x15,  -- Extension type identifier
**
**     __pack = function(self, type) -- Object Serialization
**       return cmsgpack.pack(self.x, self.y, self.z)
**     end,
**
**     __unpack = function(encoded, type) -- Factory
**       local x,y,z = cmsgpack.unpack(encoded)
**       return setmetatable({x = x, y = y, z = z}, metatable)
**     end,
**  }
**
** The encoder also handles the returning of a second boolean argument, telling
** the encoder that the serialization function handled packing its extension
** header
**
** This function will throw an error if attempting to register over an already
** existing definition (even if it's the same metatable)/. and it returns
** zero.
**
** RETURN:
**  The encoder-table passed as an argument.
*/
LUALIB_API int mp_set_extension (lua_State *L);

/*
** Get the extension-type definition, often a metatable, for encoding/decoding
** tables/userdata definitions.
*/
LUALIB_API int mp_get_extension (lua_State *L);

/*
** Explicitly remove the msgpack extension definition for each of the type
** identifiers provided to this function; returning zero.
*/
LUALIB_API int mp_clear_extension (lua_State *L);

/*
** BOOLEAN:
**  unsigned - Encode integers as unsigned integers/values.
**  integer - Encodes lua_Number's as, possibly unsigned, integers, regardless of type.
**  float - Encodes lua_Number's as float, regardless of type.
**  double - Encodes lua_Number's as double, regardless of type.
**  string_compat: Use MessagePack v4's spec for encoding strings.
**  string_binary: Encode strings using the binary tag.
**  empty_table_as_array: empty tables encoded as arrays. Beware, when
**    'always_as_map' is enabled, this flag is forced to disabled (and persists).
**  without_hole: Only contiguous arrays (i.e., [1, N] all contain non-nil elements)
**    to be encoded as arrays.
**  with_hole: Allow tables to be encoded as arrays iff all keys are positive
**    integers, inserting "nil"s when encoding to satisfy the array type.
**  always_as_map: Encode all tables as a sequence of <key, value> pairs.
**  small_lua: Compat
**  full64bits: Compat
**  long_double: Compat
*/
LUALIB_API int mp_setoption (lua_State *L);
LUALIB_API int mp_getoption (lua_State *L);

LUAMOD_API int luaopen_cmsgpack (lua_State *L);

#endif
