/*
** See Copyright Notice at the end of this file
*/
#ifndef lua_cmsgpacklib_h
#define lua_cmsgpacklib_h

#include <lua.h>

#define LUA_MSGPACK_NAME "luamsgpack-c"
#define LUA_MSGPACK_VERSION "luamsgpack-c 1.2.5"
#define LUA_MSGPACK_COPYRIGHT "Copyright (C) 2021, Gottfried Leibniz"
#define LUA_MSGPACK_DESCRIPTION "msgpack-c bindings for Lua"

#if defined(__cplusplus)
extern "C" {
#endif

/*
** {==================================================================
** Library
** ===================================================================
*/

#define LUA_MSGPACK_LIBNAME "msgpack"
LUAMOD_API int (luaopen_cmsgpack) (lua_State *L);

/* }================================================================== */

/*
** {==================================================================
** C/C++ API
** ===================================================================
*/

/*
** pack(...): receives any number of arguments and msgpack.pack their values.
** Placing the encoded string onto the Lua stack and returning 1.
**
** @RULES: See mp_setoption.
**
** @NOTE: A lua_Number is converted into an integer type if floor(num) == num;
**    Otherwise, a lua_Number will always be packed as a double to avoid a loss
**    of precision.
*/
LUALIB_API int mp_pack (lua_State *L);

/*
** pack_args(...): receives any number of arguments and packs their values as an
** array; ensuring a subsequent table.unpack(msgpack.unpack()) can be passed
** directly to a Lua function call with proper handling of intermediate nil
** values.
**
** By default tables, e.g., {...}, are packed with the 'without_hole' flag,
** meaning arrays with nil values are to be encoded as maps. This conflicts with
** function parameters and required sequences.
*/
LUALIB_API int mp_pack_args (lua_State *L);

/*
** unpack(encoded_string [, offset [, limit [, end_position]]]): Unpack all
** elements, up to a potential limit, from a msgpack encoded string. Returning
** the number of unpacked objects placed onto the Lua stack.
**
** @PARAM offset: offset within the encoded string to start decoding.
** @PARAM limit: number of Lua objects to decode, 0 to decode the entire string.
** @PARAM substring: length of the encoded substring (starting at offset).
** @RETURN
**
** @RULES:
**  (<5.3) When 64-bit integers are converted back into lua_Number, it is
**  possible the resulting number will only be an approximation to the original
**  number. This is unavoidable due to the nature of floating point types.
**
** @NOTE:
**  The 'offset' & 'substring' arguments are sugar to avoid the additional sub()
**  call when unpacking concatenated messages.
*/
LUALIB_API int mp_unpack (lua_State *L);

/*
** unpack(encoded_string): MessagePack.lua ABI compatible unpack: ignore
** additional function arguments.
*/
LUALIB_API int mp_unpack_compat (lua_State *L);

/*
** next(encoded_string [, position [, limit [, end_position ]]]): Unpack all
** elements, up to a potential limit, from a msgpack encoded string. Placing
** (1) the position in the string where the decoding ended, 0 for completion;
** and (2) and all decoded objects (up to limit). And returning the number of
** objects placed onto the Lua stack.
**
** Lua Example:
**  local position,element = 1,nil
**  while position ~= 0 do
**    position,element = msgpack.next(encoded_string, position, 1)
**  end
*/
LUALIB_API int mp_unpack_next (lua_State *L);

/*
** new(): Create, and place onto the Lua stack, a new userdata that can be used
**  to pack Lua values; returning 1. The userdata has the following metamethods
**  defined:
**
**    __len: Return the length of the current msgpack encoded string.
**
**    __tostring: Return the current msgpack encoded string.
**
**    __concat: Append another msgpack encoded strings to the packer.
**
**    __call, __add, __shl(>= 5.3): Encode, and append, the provided Lua values.
**
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
**    ud:float(4.0) -- Append; current state: { 1, 2, math.pi, 1, 2, math.pi, 4.0f }.
**    msgpack.unpack(tostring(ud)) -- Unpacks the current msgpack stream
**
** @RETURN one, corresponding to the userdata placed onto the Lua stack.
*/
LUALIB_API int mp_packer_new (lua_State *L);

/*
** extend(encoder_table): Register an extension-type. The encoder_table is often
** a metatable with additional metamethods for serializing tables/userdata:
**
**   __ext: Unique msgpack extension identifier. Note that applications can only
**    assign 0 to 127 to store application-specific type information.
**
**   __pack: An encoder function:
**              encoding[, handled_header] = f(self, type)
**    where "type" is the extension type identifier (allowing one function
**    handling multiple encodings). The 'handled_header' result is an optional
**    field to tell the encoder that the serialization function 'f' handled
**    packing its extension header
**
**   __unpack: A decoder function: value = f(encoded_string) that is the inverse
**    to __pack. Note, 'f' may only return one value as the custom extension
**    types may be used to encode key/values in tables/arrays.
**
** E.g.,
**   metatable = {
**     __ext = 0x15,  -- Extension type identifier
**
**     __pack = function(self, type) -- Object Serialization
**       return msgpack.pack(self.x, self.y, self.z)
**     end,
**
**     __unpack = function(encoded, type) -- Factory
**       local x,y,z = msgpack.unpack(encoded)
**       return setmetatable({x = x, y = y, z = z}, metatable)
**     end,
**
**    --[[ Other Metamethods --]]
**  }
**
** @RETURN: Placing the encoder-table passed as an argument on top of the stack
**  and returning 1.
*/
LUALIB_API int mp_set_extension (lua_State *L);

/*
** Get the extension-type definition, often a metatable, for encoding/decoding
** tables/userdata definitions.
*/
LUALIB_API int mp_get_extension (lua_State *L);

/*
** Explicitly remove the msgpack extension definition for all type identifiers
** provided to this function.
*/
LUALIB_API int mp_clear_extension (lua_State *L);

/*
** settype(association) : Associate the name of a Lua type (see: lua_typename)
** to a encoder table, and possibly a unique MessagePack extension
** type identifier.
**
**  The "association" can either be an extension-type identifier (integer),
**  e.g., msgpack.settype("function", 0x10); or an additional encoder table:
**    m.settype("function", {
**
**      __pack = function(self, t)
**        return msgpack.pack( ... )
**      end,
**
**      __unpack = function(s, t)
**        local ... = msgpack.unpack
**          return function() -- An iterator factory
**            -- Do something with ...
**          end
**      end,
**    })
**
** @NOTE:
**  Implemented for only LUA_TUSERDATA, LUA_TTHREAD, and LUA_TFUNCTION. The
**  performance impact of a metatable lookup for each primitive type is to much.
**
** @RETURN: Placing the encoder-table passed as an argument on top of the stack
**  and returning 1.
*/
LUALIB_API int mp_set_type_extension (lua_State *L);

/* Get the encoder table associated to the name of a Lua type. */
LUALIB_API int mp_get_type_extension (lua_State *L);

/* Returns msgpack.null */
LUALIB_API int mp_null (lua_State *L);

/*
** BOOLEAN:
**  'unsigned': Encode integers as unsigned values when possible, i.e., positive
**    lua_Integers are packed as unsigned int; this is default for lua-MessagePack.
**  'integer': Encodes lua_Number's as, possibly unsigned, integers, regardless
**    of type
**  'float': Encodes a lua_Number as float, regardless of type.
**  'double': Encodes a lua_Number as double, regardless of type.
**
**  'string_compat': Use MessagePack v4's spec for encoding strings.
**  'string_binary': Encode strings using the binary tag.
**
**  'always_as_map': Encode all tables as a sequence of <key, value> pairs.
**  'without_hole': Only contiguous arrays (i.e., [1, N] all contain non-nil elements)
**    to be encoded as arrays.
**  'with_hole': Allow tables to be encoded as arrays iff all keys are positive
**    integers, inserting "nil"s when encoding to satisfy the array type.
**  'empty_table_as_array': empty tables encoded as arrays. Beware, when
**    'always_as_map' is enabled, this flag is forced to disabled (and persists).
**
**  'sentinel': Replace 'nil' values with a 'sentinel' value during unpacking.
**    The packer will always replace sentinel's with null during packing.
**
**  'ignore_invalid': Ignore invalid types (i.e., ones without 'type' extensions)
**    during encoding by packing 'nil' instead of throwing an error.
**
**  'small_lua': lua-MessagePack compatibility field.
**  'full64bits': lua-MessagePack compatibility field.
**  'long_double': lua-MessagePack compatibility field.
*/
LUALIB_API int mp_setoption (lua_State *L);
LUALIB_API int mp_getoption (lua_State *L);

/* }================================================================== */

#if defined(__cplusplus)
}
#endif

/******************************************************************************
* luamsgpack-c
* Copyright (C) 2021 - gottfriedleibniz
* Copyright (C) 2012 - Salvatore Sanfilippo (https://github.com/antirez/lua-cmsgpack)
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************/

#endif
