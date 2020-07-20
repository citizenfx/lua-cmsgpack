/*
** See LICENSE.
*/
#ifndef lua_cmsgpacklib_h
#define lua_cmsgpacklib_h

#include <luaconf.h>
#include <lua.h>

#define LUACMSGPACK_NAME "lua-msgpack-c"
#define LUACMSGPACK_VERSION "lua-msgpack-c 1.0.0"
#define LUACMSGPACK_COPYRIGHT "Copyright (C) 2012, Salvatore Sanfilippo; 2020, Gottfried Leibniz"
#define LUACMSGPACK_DESCRIPTION "msgpack-c bindings for Lua"

#if !defined(LUAMOD_API)  /* LUA_VERSION_NUM == 501 */
  #define LUAMOD_API LUALIB_API
#endif

/*
** pack(...): receives any number of arguments and MessagePacks their values.
** Returning the encoded string.
**
** RULES: See mp_setoption.
** NOTES: A lua_Number is converted into an integer type if floor(num) == num;
**    Otherwise, a lua_Number will always be packed as a double to avoid a loss
**    of precision.
*/
LUALIB_API int mp_pack (lua_State *L);

/*
** unpack(encoded_string [, offset [, limit [, end_position]]]): Unpack all
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
** unpack(encoded_string): ABI compatible unpack, ignore additional arguments.
*/
LUALIB_API int mp_unpack_compat (lua_State *L);

/*
** next(encoded_string [, position [, limit [, end_position ]]]: Unpack all
** elements, up to a potential limit, from a msgpack encoded string. Returning
** the position in the string where the decoding ended (or 0 for completion)
** and all decoded objects, e.g,
**
**  local position,element = 1,nil
**  while position ~= 0 do
**    position,element = msgpack.next(encoded_string, position, 1)
**  end
*/
LUALIB_API int mp_unpack_next (lua_State *L);

/*
** new(): Create a new packer userdata that can be used to msgpack Lua values.
**  Metamethods:
**    __len: Return the length of the current msgpack encoded string.
**    __tostring: Return the current msgpack encoded string.
**    __concat: Append another msgpack encoded strings to the packer.
**    __call, __add, __shl: (>= 5.3): Encode, and append, the provided Lua values.
**    __index: Functions of the form: f(packer, [, value [, ... [, value]...]])
**      Where the values are casted to the named type:
**        "nil",
**        "any",
**        "boolean", "true", "false",
**        "fix_uint8", "fix_uint16", "fix_uint32", "fix_uint64",
**        "fix_int8", "fix_int16", "fix_int32", "fix_int64",
**        "uint8", "uint16", "uint32", "uint64",
**        "int8", "int16", "int32", "int64",
**        "char", "signed_char", "unsigned_char",
**        "short", "integer", "long", "long_long",
**        "unsigned_short", "unsigned_int", "unsigned_long", "unsigned_long_long",
**        "signed_int16", "signed_int32", "signed_int64",
**        "integer", "signed", "unsigned",
**        "float", "double", "number",
**        "_string", "string_compat", "string", "binary",
**        "_table", "map", "array", "table",
**
**  Example Usage:
**    ud = msgpack.new()
**    ud(1, 2, math.pi) -- Append; current state: { 1, 2, math.pi }.
**    ud .. tostring(ud) -- Duplicate; current state: { 1, 2, math.pi, 1, 2, math.pi }.
**    ud::float(4.0) -- Append; current state: { 1, 2, math.pi, 1, 2, math.pi, 4.0f }.
**    msgpack.unpack(tostring(ud)) -- Unpacks the current msgpack stream
*/
LUALIB_API int mp_packer_new (lua_State *L);

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
** settype(association) : Associate the name of a Lua type (see: lua_typename)
** to a encoder table, and possibly a unique MessagePack extension
** type identifier.
**
**  The "association" can either be an extension-type identifier (integer),
**  e.g., cmsgpack.settype("function", 0x10); or an additional encoder table:
**    m.settype("function", {
**      __pack = function(self, t)
**        return cmsgpack.pack( ... )
**      end,
**      __unpack = function(s, t)
**        local ... = cmsgpack.unpack
**          return function() -- An iterator factory
**            -- Do something with ...
**          end
**      end,
**    })
**
** NOTES:
**  Implemented for only LUA_TUSERDATA, LUA_TTHREAD, and LUA_TFUNCTION. The
**  performance impact of a metatable lookup for each primitive type is to much.
**
** RETURN:
**  The encoder-table passed as an argument.
*/
LUALIB_API int mp_set_type_extension (lua_State *L);

/* Get the encoder table associated to the name of a Lua type. */
LUALIB_API int mp_get_type_extension (lua_State *L);

/*
** BOOLEAN:
**  unsigned - Encode integers as unsigned values when possible, i.e., positive
**    lua_Integers are msgpacked as unsigned int; this is default for
**    lua-MessagePack.
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
