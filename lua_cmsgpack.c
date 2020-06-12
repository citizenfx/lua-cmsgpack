/*
** See LICENSE.
*/
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <limits.h>

/* Before Lua includes to account for potential Lua debug definitions*/
#include "lua_cmsgpack.h"
#include "lua_cmsgpacklib.h"

#include <luaconf.h>
#include <lua.h>
#include <lauxlib.h>

#include <msgpack.h>
#include <msgpack/sysdep.h>

/* Check if float or double can be an integer without loss of precision */
#define IS_INT_TYPE_EQUIVALENT(x, T) (!isinf(x) && (T)(x) == (x))
#define IS_INT64_EQUIVALENT(x) IS_INT_TYPE_EQUIVALENT(x, int64_t)
#define IS_INT_EQUIVALENT(x) IS_INT_TYPE_EQUIVALENT(x, int)

/*
** Default chunk msgpack_zone chunk size.
**
** @TODO: Experiment with changing this (and other values) based on some input
**        heuristic.
*/
#define LUACMSGPACK_ZONE_CHUNK_SIZE 256

/* Lua 5.4 changed the definition of lua_newuserdata; no uservalues required */
#if LUA_VERSION_NUM >= 504
  #define mp_newuserdata(L, s) lua_newuserdatauv((L), (s), 0)
#elif LUA_VERSION_NUM >= 501 && LUA_VERSION_NUM <= 503
  #define mp_newuserdata(L, s) lua_newuserdata((L), (s))
#else
  #error unsupported Lua version
#endif

#if LUA_VERSION_NUM < 502
  #define lua_absindex(L, i) ((i) > 0 || (i) <= LUA_REGISTRYINDEX ? (i) : lua_gettop(L) + (i) + 1)
static int luaL_getsubtable (lua_State *L, int idx, const char *fname) {
  lua_getfield(L, idx, fname);
  if (lua_istable(L, -1))
    return 1;  /* table already there */
  else {
    lua_pop(L, 1);  /* remove previous result */
    idx = lua_absindex(L, idx);
    lua_newtable(L);
    lua_pushvalue(L, -1);  /* copy to be left at top */
    lua_setfield(L, idx, fname);  /* assign new table to field */
    return 0;  /* false, because did not find table there */
  }
}
#endif

/* Fetch a lua_Int from the registry table */
static lua_Integer mp_getregi (lua_State *L, const char *key, lua_Integer opt) {
  lua_Integer result;

  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUACMSGPACK_REG);
  lua_getfield(L, -1, key);
  result = luaL_optinteger(L, -1, opt);
  lua_pop(L, 2);  /* registry & key */
  return result;
}

/* Push a integer into the registry table at the specified key */
static void mp_setregi (lua_State *L, const char *key, lua_Integer value) {
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUACMSGPACK_REG);
  lua_pushinteger(L, value);
  lua_setfield(L, -2, key);  /* pops value */
  lua_pop(L, 1);  /* getregtable */
}

/* Get a subtable within the registry table */
static inline void mp_getregt (lua_State *L, const char *name) {
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUACMSGPACK_REG);
  if (!luaL_getsubtable(L, -1, name)) {
    /* initialize it */
  }

#if LUA_VERSION_NUM < 503
  lua_pushvalue(L, -2);
  lua_remove(L, -3);
#else
  lua_rotate(L, -2, 1);  /* LUACMSGPACK_REG <-> subtable */
#endif
  lua_pop(L, 1);
}

static int typetoindex (lua_State *L, const char *name) {
  int i, found = -1;
#if LUA_VERSION_NUM == 501  /* Ensure the type name exists ... */
  for (i = 0; i < (LUA_TTHREAD+1); ++i) {
#else
  for (i = 0; i < LUA_NUMTAGS; ++i) {
#endif
    if (strcmp(lua_typename(L, i), name) == 0) {
      found = i;
      break;
    }
  }
  return found;
}

/*
** Decode the msgpack_object and place its contents onto the Lua stack.
** Returning 1 on success, 0 on failure.
**/
static int mp_decode_to_lua_type (lua_State *L, msgpack_object *obj);

/*
** {==================================================================
** msgpack-c bindings
** ===================================================================
*/
#define mp_rel_index(idx, n) (((idx) < 0) ? ((idx) - (n)) : (idx))

/*
** Return true if the table at the specified stack index can be encoded as an
** array, i.e., a table whose keys are (1) integers; (2) begin at one; (3)
** strictly positive; and (4) form a contiguous sequence.
**
** However, with the flag "MP_ARRAY_WITH_HOLES" set, condition (4) is alleviated
** and msgpack can encode "null" in the nil array indices.
*/
static int mp_table_is_an_array (lua_State *L, int idx, lua_Integer flags,
                                                         size_t *array_length) {
  lua_Integer n;
  size_t count = 0, max = 0;
  int stacktop = lua_gettop(L);
  int i_idx = mp_rel_index(idx, 1);

  mp_checkstack(L, 2);
  lua_pushnil(L);
  while (lua_next(L, i_idx)) {  /* [key, value] */
    lua_pop(L, 1);  /* [key] */
    if (mp_isinteger(L, -1)  /* && within range of size_t */
              && ((n = lua_tointeger(L, -1)) >= 1 && ((size_t)n) <= MAX_SIZE)) {
      count++;  /* Is a valid array index ... */
      max = ((size_t)n) > max ? ((size_t)n) : max;
    }
    else {
      lua_settop(L, stacktop);
      return 0;
    }
  }
  *array_length = max;

  lua_settop(L, stacktop);
  if ((flags & MP_ARRAY_WITH_HOLES) == MP_ARRAY_WITH_HOLES)
    return 1;  /* All integer keys, insert nils. */
  return max == count;
}

/*
** Encode the table at the specified stack index as an array.
**
** PARAMETERS:
**  idx - stack (or relative) index of the lua table.
**  level - current recursive/encoding depth, used as a short-hand to avoid an
**    explicit structure to avoid cycles among tables.
**  array_length - precomputed array length; at worst use lua_objlen/lua_rawlen
**    to compute this value.
*/
static void mp_encode_lua_table_as_array (lua_State *L, lua_msgpack *ud,
                                      int idx, int level, size_t array_length) {
  size_t j;
#if LUA_VERSION_NUM < 503
  int i_idx = mp_rel_index(idx, 1);
#endif

  msgpack_pack_array(&ud->u.packed.packer, array_length);
  mp_checkstack(L, 1);
  for (j = 1; j <= array_length; j++) {
#if LUA_VERSION_NUM >= 503
    lua_rawgeti(L, idx, (lua_Integer)j);
#else
    lua_pushinteger(L, (lua_Integer)j);
    lua_rawget(L, i_idx);
#endif
    lua_msgpack_encode(L, ud, -1, level + 1);
    lua_pop(L, 1);  /* pop: t[j] */
  }
}

/*
** Encode the table at the specified stack index as a <key, value> array.
*/
static void mp_encode_lua_table_as_map (lua_State *L, lua_msgpack *ud, int idx,
                                                                    int level) {
  size_t len = 0;
  int i_idx = mp_rel_index(idx, 1);

  /*
  ** Count the number of <key, value> pairs in the table. It's impossible to
  ** single-pass this logic given the limitations in the Lua API and inability
  ** to reserve bytes and retroactively encode the table length.
  */
  mp_checkstack(L, 3);
  lua_pushnil(L);
  while (lua_next(L, i_idx)) {
    len++;
    lua_pop(L, 1);  /* remove value, keep key for next iteration. */
  }

  msgpack_pack_map(&ud->u.packed.packer, len);
  lua_pushnil(L);
  while (lua_next(L, i_idx)) {
    lua_msgpack_encode(L, ud, -2, level + 1);  /* Encode Key */
    lua_msgpack_encode(L, ud, -1, level + 1);  /* Encode Value */
    lua_pop(L, 1);  /* pop value, leave key for next iteration */
  }
}

/*
** Earlier versions of Lua have no explicit integer types, therefore detect if
** floating types can be faithfully casted to an int.
*/
static inline void mp_encode_lua_number (lua_State *L, lua_msgpack *ud, int idx) {
  lua_Number n = lua_tonumber(L, idx);
  lua_Integer flags = ud->flags;
  msgpack_packer *pk = &(ud->u.packed.packer);
#if defined(LUACMSGPACK_BIT32)
  if (IS_INT_EQUIVALENT(n) || (flags & MP_NUMBER_AS_INTEGER)) {
    if (flags & MP_UNSIGNED_INTEGERS)
      msgpack_pack_uint32(pk, (uint32_t)n);
    else
      msgpack_pack_int32(pk, (int32_t)n);
  }
  else {
    if (flags & MP_NUMBER_AS_FLOAT)
      msgpack_pack_float(pk, (float)n);
    else
      msgpack_pack_double(pk, (double)n);
  }
#else
  if (IS_INT64_EQUIVALENT(n) || (flags & MP_NUMBER_AS_INTEGER)) {
    if (flags & MP_UNSIGNED_INTEGERS)
      msgpack_pack_uint64(pk, (uint64_t)n);
    else
      msgpack_pack_int64(pk, (int64_t)n);
  }
  else {
    if (flags & MP_NUMBER_AS_FLOAT)
      msgpack_pack_float(pk, (float)n);
    else
      msgpack_pack_double(pk, (double)n);
  }
#endif
}

static int mp_decode_to_lua_type (lua_State *L, msgpack_object *obj) {
  mp_checkstack(L, 1);
  switch (obj->type) {
    case MSGPACK_OBJECT_NIL:
      lua_pushnil(L);
      break;
    case MSGPACK_OBJECT_BOOLEAN:
      lua_pushboolean(L, obj->via.boolean);
      break;
    case MSGPACK_OBJECT_POSITIVE_INTEGER:
#if LUA_VERSION_NUM >= 503
      if (obj->via.u64 > LUA_MAXINTEGER)
        lua_pushnumber(L, (lua_Number)obj->via.u64);
      else
        lua_pushinteger(L, (lua_Integer)obj->via.u64);
#else
      /* lua_pushinteger: cast(lua_Number, ...) anyway */
      lua_pushnumber(L, (lua_Number)obj->via.u64);
#endif
      break;
    case MSGPACK_OBJECT_NEGATIVE_INTEGER:
      lua_pushinteger(L, (lua_Integer)obj->via.i64);
      break;
    case MSGPACK_OBJECT_FLOAT32:
      lua_pushnumber(L, (lua_Number)((float) obj->via.f64));
      break;
    case MSGPACK_OBJECT_FLOAT64:
      lua_pushnumber(L, (lua_Number)((double)obj->via.f64));
      break;
    case MSGPACK_OBJECT_STR:
      lua_pushlstring(L, obj->via.str.ptr, obj->via.str.size);
      break;
    case MSGPACK_OBJECT_BIN:
      lua_pushlstring(L, obj->via.bin.ptr, obj->via.bin.size);
      break;
    case MSGPACK_OBJECT_ARRAY: {
      uint32_t i = 0;
      msgpack_object_array array = obj->via.array;

      lua_createtable(L, (array.size <= INT_MAX) ? (int)array.size : 0, 0);
      mp_checkstack(L, 1);
      for (i = 0; i < array.size; ++i) {
#if LUA_VERSION_NUM >= 503
        if (mp_decode_to_lua_type(L, &(array.ptr[i])))
          lua_rawseti(L, -2, (lua_Integer)(i + 1));
#else
        lua_pushinteger(L, (lua_Integer)(i + 1));
        if (mp_decode_to_lua_type(L, &(array.ptr[i])))
          lua_rawset(L, -3);
        else
          lua_pop(L, 1);  /* decoded key */
#endif
      }
      break;
    }
    case MSGPACK_OBJECT_MAP: {
      uint32_t i = 0;
      msgpack_object_map map = obj->via.map;

      lua_createtable(L, 0, (map.size <= INT_MAX) ? (int)map.size : 0);
      mp_checkstack(L, 2);
      for (i = 0; i < map.size; ++i) {
        if (mp_decode_to_lua_type(L, &(map.ptr[i].key))) {
          if (!lua_isnil(L, -1) && mp_decode_to_lua_type(L, &(map.ptr[i].val)))
            lua_rawset(L, -3);
          else
            lua_pop(L, 1);  /* decoded key */
        }
      }
      break;
    }
    case MSGPACK_OBJECT_EXT: {
      msgpack_object_ext ext = obj->via.ext;
      mp_getregt(L, LUACMSGPACK_REG_EXT);  /* Fetch the decoding function */
      lua_rawgeti(L, -1, (lua_Integer)ext.type);
      if (lua_type(L, -1) == LUA_TTABLE) {
        lua_getfield(L, -1, LUACMSGPACK_META_DECODE);  /* [table, table, decoder] */
        if (lua_isfunction(L, -1)) {
          lua_insert(L, -3); lua_pop(L, 2); /* [decoder] */
          lua_pushlstring(L, ext.ptr, (size_t)ext.size);  /* [decoder, value] */
          lua_pushinteger(L, (lua_Integer)ext.type);
          lua_call(L, 2, 1);  /* [decoded value] */
        }
        else {
          lua_pop(L, 3);
          return luaL_error(L, "msgpack extension type: invalid decoder!");
        }
      }
      else {
        lua_pop(L, 2);
        lua_pushlstring(L, (const char *)ext.ptr, ext.size);
      }
      break;
    }
    default:
      lua_pushnil(L);
      break;
  }
  return 1;
}

/* }================================================================== */

/*
** {==================================================================
** Core API
** ===================================================================
*/

#if defined(__GNUC__)
  #define popcount __builtin_popcount
#elif defined(_MSC_VER)
  #include <intrin.h>
  #define popcount __popcnt
#else
  int popcount(uint32_t x) {
    uint32_t c = 0;
    for (; x > 0; x &= x -1) c++;
    return c;
  }
#endif

static lua_Integer mp_checktype (lua_State *L, lua_Integer type, int arg) {
  if (!LUACMSGPACK_EXT_VALID(type))
    return luaL_argerror(L, arg, "Invalid extension-type identifier");
  return type;
}

/*
** Return the extension type, if one exists, associated to the object value at
** the specified stack index.
*/
static lua_Integer mp_ext_type (lua_State *L, int idx) {
  lua_Integer type = EXT_INVALID;
#if LUA_VERSION_NUM >= 503
  if (luaL_getmetafield(L, idx, LUACMSGPACK_META_MTYPE) != LUA_TNIL) {
#else
  if (luaL_getmetafield(L, idx, LUACMSGPACK_META_MTYPE) != 0) {
#endif
    if (mp_isinteger(L, -1)) {  /* [table] */
      type = lua_tointeger(L, -1);
      type = LUACMSGPACK_EXT_VALID(type) ? type : EXT_INVALID;
    }
    lua_pop(L, 1);
  }

  return type;
}

/*
** @TODO: Missing level. With a poorly defined extension encoder, cycles can
**  exist and the encoder level isn't propagated.
*/
static int mp_encode_ext_lua_type (lua_State *L, lua_msgpack *ud, int idx, int8_t ext_id) {
  lua_checkstack(L, 5);
  mp_getregt(L, LUACMSGPACK_REG_EXT);  /* [table] */
  lua_rawgeti(L, -1, (lua_Integer)ext_id);

  /* Parse the encoder table */
  if (lua_type(L, -1) == LUA_TTABLE) {  /* [table, table] */
    lua_getfield(L, -1, LUACMSGPACK_META_ENCODE);  /* [table, table, encoder] */
    if (lua_isfunction(L, -1)) {
      msgpack_packer *pk = &(ud->u.packed.packer);

      lua_insert(L, -3); lua_pop(L, 2); /* [encoder] */
      lua_pushvalue(L, mp_rel_index(idx, 1));  /* [encoder, value to encode] */
      lua_pushinteger(L, (lua_Integer)ext_id);  /* [encoder, value, type] */
      lua_call(L, 2, 2);  /* [encoded value] */
      if (lua_type(L, -2) == LUA_TSTRING) {
        size_t len = 0;
        const char *s = lua_tolstring(L, -2, &len);
        /* Second, and optional, return value denotes a custom encoding */
        if (lua_toboolean(L, -1))
          lua_mpbuffer_append(&ud->u.packed.buffer, s, len);
        /*
        ** TODO: If the type identifier has a custom extension identifier,
        ** use it. Otherwise, binary encode. How "extensions" and "types" are
        ** separated is slightly confusing at the moment as the API doesn't
        ** expose the previous "packers" and "unpackers" table
        **
        ** else if (ext_id < 0) {
        **   msgpack_pack_bin(pk, len);
        **   msgpack_pack_bin_body(pk, s, len);
        ** }
        */
        else {
          msgpack_pack_ext(pk, len, ext_id);
          msgpack_pack_ext_body(pk, s, len);
        }
        lua_pop(L, 2);
        return 1;
      }
      lua_pop(L, 2);  /* both returns */
      return luaL_error(L, "invalid encoder result from encoder <%d>", ext_id);
    }
    lua_pop(L, 3);
    return luaL_error(L, "msgpack extension type: invalid encoder!");
  }
  /* Lua type has been associated to an extension type */
  else if (lua_type(L, -1) == LUA_TNUMBER) { /* [table, type] */
    lua_Integer ext = lua_tointeger(L, -1);

    lua_pop(L, 2);  /* [ ] */
    if (ext == ext_id)  /* prevent cycles */
      return luaL_error(L, "msgpack extension type: invalid encoder!");
    else if (!LUACMSGPACK_EXT_VALID(ext))
      return luaL_error(L, "Invalid extension-type identifier");
    else
      return mp_encode_ext_lua_type(L, ud, idx, (int8_t)ext);
  }
  lua_pop(L, 2);
  return 0;
}

LUA_API lua_msgpack *lua_msgpack_create (lua_State *L, lua_Integer flags) {
  lua_msgpack *ud = NULL;

  int mode = flags & MP_MODE;
  lua_Integer options = mp_getregi(L, LUACMSGPACK_REG_OPTIONS, MP_DEFAULT);
  if (popcount((uint32_t)mode) != 1) {
    luaL_error(L, "invalid msgpack flags: %d\n", flags);
    return NULL;
  }
  else if ((ud = (lua_msgpack *)mp_newuserdata(L, sizeof(lua_msgpack))) == NULL) {
    luaL_error(L, "Could not allocate packer UD");
    return NULL;
  }

  if (mode & MP_PACKING) {
    lua_mpbuffer_init(L, &ud->u.packed.buffer);
    msgpack_packer_init(&ud->u.packed.packer, &ud->u.packed.buffer, lua_mpbuffer_append);
  }
  else if (mode & MP_UNPACKING) {
    if (!msgpack_zone_init(&ud->u.unpacked.zone, LUACMSGPACK_ZONE_CHUNK_SIZE)) {
      luaL_error(L, "Could not allocate msgpack_zone_init");
      return NULL;
    }
  }
  else {
    luaL_error(L, "invalid msgpack mode");
    return NULL;
  }

  ud->flags = MP_OPEN | mode | (options & ~MP_MASK_RUNTIME);
  luaL_getmetatable(L, LUACMSGPACK_USERDATA);
  lua_setmetatable(L, -2);
  return ud;
}

LUA_API int lua_msgpack_destroy (lua_State *L, int idx, lua_msgpack *ud) {
  if (ud == NULL) {
    ud = ((lua_msgpack *)luaL_checkudata(L, idx, LUACMSGPACK_USERDATA));
  }

  if ((ud->flags & MP_OPEN)) {
    if (ud->flags & MP_PACKING) {
      ud->u.packed.buffer.L = L;  /* Use active lua_State for alloc/dealloc. */
      lua_mpbuffer_free(&ud->u.packed.buffer);
    }
    else if (ud->flags & MP_UNPACKING) {
      msgpack_zone_destroy(&ud->u.unpacked.zone);
    }
    ud->flags = 0;

    mp_checkstack(L, 1);  /* function can be called from pack/unpack */
    lua_pushnil(L);
    lua_setmetatable(L, idx);  /* Remove metatable, memory already managed */
    return 1;
  }
  return 0;
}

LUA_API void lua_msgpack_extension (lua_State *L, lua_Integer type,
                                 lua_CFunction encoder, lua_CFunction decoder) {
  if (LUACMSGPACK_EXT_VALID(type) && !LUACMSGPACK_EXT_RESERVED(type)) {
    mp_getregt(L, LUACMSGPACK_REG_EXT);
    lua_pushinteger(L, type);

    lua_createtable(L, 0, 3);
    lua_pushinteger(L, type); lua_setfield(L, -2, LUACMSGPACK_META_MTYPE);
    lua_pushcfunction(L, encoder); lua_setfield(L, -2, LUACMSGPACK_META_ENCODE);
    lua_pushcfunction(L, decoder); lua_setfield(L, -2, LUACMSGPACK_META_DECODE);

    lua_rawset(L, -3);
    lua_pop(L, 1);  /* mp_getregt */
  }
  else
    luaL_error(L, "Invalid extension-type identifier");
}

LUA_API void lua_msgpack_encode (lua_State *L, lua_msgpack *ud, int idx, int level) {
  int t = lua_type(L, idx);
  msgpack_packer *pk = &(ud->u.packed.packer);
#if defined(LUACMSGPACK_ERROR_NESTING)
  if (t == LUA_TTABLE && level == LUACMSGPACK_MAX_NESTING) {
    luaL_error(L, "maximum table nesting depth exceeded");
    return;
  }
#else
  if (t == LUA_TTABLE && level == LUACMSGPACK_MAX_NESTING)
    t = LUA_TNIL;
#endif

  switch (t) {
    case LUA_TNIL:
      msgpack_pack_nil(pk);
      break;
    case LUA_TBOOLEAN: {
      if (lua_toboolean(L, idx))
        msgpack_pack_true(pk);
      else
        msgpack_pack_false(pk);
      break;
    }
    case LUA_TNUMBER: {
#if LUA_VERSION_NUM < 503
      mp_encode_lua_number(L, ud, idx);
#else
  #if defined(LUACMSGPACK_BIT32)
      if (lua_isinteger(L, idx)) {
        if (ud->flags & MP_UNSIGNED_INTEGERS)
          msgpack_pack_uint32(pk, (uint32_t)lua_tointeger(L, idx));
        else
          msgpack_pack_int32(pk, (int32_t)lua_tointeger(L, idx));
      }
      else  /* still attempt to pack numeric types as integers when possible. */
        mp_encode_lua_number(L, ud, idx);
  #else
      if (lua_isinteger(L, idx)) {
        if (ud->flags & MP_UNSIGNED_INTEGERS)
          msgpack_pack_uint64(pk, (uint64_t)lua_tointeger(L, idx));
        else
          msgpack_pack_int64(pk, (int64_t)lua_tointeger(L, idx));
      }
      else
        mp_encode_lua_number(L, ud, idx);
  #endif
#endif
      break;
    }
    case LUA_TSTRING: {
      size_t len;
      const char *s = lua_tolstring(L, idx, &len);
      if ((ud->flags & MP_STRING_COMPAT) == MP_STRING_COMPAT) {
        msgpack_pack_v4raw(pk, len);
        msgpack_pack_v4raw_body(pk, s, len);
      }
      else if ((ud->flags & MP_STRING_BINARY) == MP_STRING_BINARY) {
        msgpack_pack_bin(pk, len);
        msgpack_pack_bin_body(pk, s, len);
      }
      else {
        msgpack_pack_str(pk, len);
        msgpack_pack_str_body(pk, s, len);
      }
      break;
    }
    case LUA_TTABLE: {
      size_t array_length = 0;
      lua_Integer type = 0;
      if ((type = mp_ext_type(L, idx)) != EXT_INVALID) {
        if (!mp_encode_ext_lua_type(L, ud, idx, (int8_t)type)) {
          luaL_error(L, "msgpack extension type: not registered!");
          return;
        }
      }
      else if ((ud->flags & MP_ARRAY_AS_MAP) == MP_ARRAY_AS_MAP)
        mp_encode_lua_table_as_map(L, ud, idx, level);
      else if (mp_table_is_an_array(L, idx, ud->flags, &array_length)
                     && (array_length > 0 || (ud->flags & MP_EMPTY_AS_ARRAY))) {
        mp_encode_lua_table_as_array(L, ud, idx, level, array_length);
      }
      else
        mp_encode_lua_table_as_map(L, ud, idx, level);
      break;
    }
    case LUA_TLIGHTUSERDATA: {
      /*
      ** TODO: Improve how light userdata is managed. Ideally, there will be
      **       API function lua_msgpack_type_extension( ..., lua_CFunction,
      **       lua_CFunction) that handles the serialization of C pointers.
      */
      if (!mp_encode_ext_lua_type(L, ud, idx, (int8_t)LUACMSGPACK_LUATYPE_EXT(t))) {
#if defined(LUACMSGPACK_BIT32)
        msgpack_pack_uint32(pk, (uint32_t)lua_touserdata(L, idx));
#else
        msgpack_pack_uint64(pk, (uint64_t)lua_touserdata(L, idx));
#endif
      }
      break;
    }
    case LUA_TUSERDATA:
    case LUA_TTHREAD:
    case LUA_TFUNCTION: {
      lua_Integer type = 0;
      if ((type = mp_ext_type(L, idx)) != EXT_INVALID) {
        if (!mp_encode_ext_lua_type(L, ud, idx, (int8_t)type)) {
          luaL_error(L, "msgpack extension type: not registered!");
          return;
        }
        break;
      }
      else if (mp_encode_ext_lua_type(L, ud, idx, (int8_t)LUACMSGPACK_LUATYPE_EXT(t))) {
        break;
      }
    }  /* FALLTHROUGH */
    default:
      luaL_error(L, "type <%s> cannot be msgpack'd", lua_typename(L, t));
      break;
  }
}

LUA_API int lua_msgpack_decode (lua_State *L, lua_msgpack *ud, const char *s,
                                          size_t len, size_t *offset, int limit,
                          const char **error, msgpack_unpack_return *err_code) {
  int done = 0;
  int object_count = 0;
  msgpack_unpack_return err_res = MSGPACK_UNPACK_SUCCESS;
  const char *err_msg = NULL;  /* luaL_error message on failure */
  while (err_msg == NULL && !done) {
    msgpack_object obj;
    switch ((err_res = msgpack_unpack(s, len, offset, &ud->u.unpacked.zone, &obj))) {
      case MSGPACK_UNPACK_SUCCESS: {
        if ((done = mp_decode_to_lua_type(L, &obj))) {
          ++object_count;
        }
        else {
          err_res = MSGPACK_UNPACK_PARSE_ERROR;
          err_msg = "could not unpack final type";
        }
        break;
      }
      case MSGPACK_UNPACK_EXTRA_BYTES: {
        if (mp_decode_to_lua_type(L, &obj)) {
          done = (++object_count >= limit && limit > 0);
        }
        else {
          err_res = MSGPACK_UNPACK_PARSE_ERROR;
          err_msg = "could not unpack type";
        }
        break;
      }
      case MSGPACK_UNPACK_CONTINUE:
        err_msg = "msgpack format data is incomplete";
        break;
      case MSGPACK_UNPACK_PARSE_ERROR:
        err_msg = "msgpack format data is invalid";
        break;
      case MSGPACK_UNPACK_NOMEM_ERROR:
        err_msg = "msgpack memory allocation failed";
        break;
      default:
        err_res = MSGPACK_UNPACK_PARSE_ERROR;
        err_msg = "msgpack unknown decoder result";
        break;
    }
  }

  *error = err_msg;
  *err_code = err_res;
  return (err_msg == NULL) ? object_count : 0;
}

/* }================================================================== */

/*
** {==================================================================
** Userdata API
** ===================================================================
*/

static int packud_gc (lua_State *L) {
  lua_msgpack_destroy(L, 1, NULL);
  return 0;
}

static const luaL_Reg msgpack_metafuncs[] = {
  { "__gc", packud_gc },
#if LUA_VERSION_NUM >= 504
  { "__close", packud_gc },
#endif
  { NULL, NULL }
};

/* }================================================================== */

/*
** {==================================================================
** MODULE
** ===================================================================
*/

static const char *const opts[] = {
  "unsigned", "integer", "float", "double",
  "string_compat", "string_binary",
  "empty_table_as_array", "without_hole", "with_hole", "always_as_map",
  "small_lua", "full64bits", "long_double", NULL
};

static const int optsnum[] = {
  MP_UNSIGNED_INTEGERS, MP_NUMBER_AS_INTEGER, MP_NUMBER_AS_FLOAT, MP_NUMBER_AS_DOUBLE,
  MP_STRING_COMPAT, MP_STRING_BINARY,
  MP_EMPTY_AS_ARRAY, MP_ARRAY_WITHOUT_HOLES, MP_ARRAY_WITH_HOLES, MP_ARRAY_AS_MAP,
  MP_SMALL_LUA, MP_FULL_64_BITS, MP_LONG_DOUBLE,
};

/*
** If the stack argument is a convertible to a size_t from an lua_Integer,
** returns the size_t. If the argument is absent or is nil, returns def.
** Otherwise, throw an error.
*/
static inline size_t luaL_optsizet (lua_State *L, int arg, size_t def) {
  lua_Integer i;
  if (lua_isnoneornil(L, arg))
    return def;
  else if (!mp_isinteger(L, arg)) { /* 5.1/5.2: Number not an integer */
    luaL_argerror(L, arg, lua_pushfstring(L, "integer expected"));
    return 0;
  }
  else if ((i = lua_tointeger(L, arg)) < 0) {
    luaL_argerror(L, arg, "negative integer argument");
    return 0;
  }
  else if (((size_t)i) > MAX_SIZE) {
    luaL_argerror(L, arg, "invalid integer argument");
    return 0;
  }
  return (size_t)i;
}

LUALIB_API int mp_pack (lua_State *L) {
  lua_msgpack *ud = NULL;
  int top, i, nargs;

  if ((nargs = lua_gettop(L)) == 0)
    return luaL_argerror(L, 0, "MessagePack pack needs input");
  else if (!lua_checkstack(L, nargs))
    return luaL_argerror(L, 0, "too many arguments for MessagePack pack");
  else if (!(ud =  lua_msgpack_create(L, MP_PACKING)))
    return luaL_error(L, "could not allocate packer UD");

  top = lua_gettop(L);
  for (i = 1; i <= nargs; i++) {
    lua_msgpack_encode(L, ud, i, 0);
  }

  lua_pushlstring(L, ud->u.packed.buffer.b, ud->u.packed.buffer.n);
  lua_msgpack_destroy(L, top, ud);  /* memory already managed */
  /* lua_remove(L, top);  let moveresults remove lua_msgpack */
  return 1;
}

static int mp_unpacker (lua_State *L, int include_offset) {
  int top = 0, count = 0, limit = 0;
  size_t len = 0, position = 0, offset = 0, end_position = 0;
  lua_msgpack *ud = NULL;  /* Decoder */

  msgpack_unpack_return err_code = MSGPACK_UNPACK_SUCCESS;
  const char *err_msg = NULL;  /* Error message on decoding failure. */
  const char *s = luaL_checklstring(L, 1, &len);
  position = luaL_optsizet(L, 2, 1);
  limit = (int)luaL_optinteger(L, 3, include_offset ? 1 : 0);
  end_position = luaL_optsizet(L, 4, 0);
  offset = position - 1;

  if (mp_isinteger(L, 2) && lua_tointeger(L, 2) <= 0) {
    lua_pushvalue(L, 2);
    lua_pushnil(L);
    return 2;
  }

  /* @TODO: lua_pushfstring doesn't support %zu's for formatting errors... */
  if (len == 0)  /* edge-case sanitation */
    return 0;
  else if (position == 0)
    return luaL_error(L, "invalid string position: <0>");
  else if (limit < 0)
    return luaL_error(L, "invalid limit");
  else if (offset > len)
    return luaL_error(L, "start offset greater than input length");
  else if (end_position > 0 && end_position < offset)
    return luaL_error(L, "end position less than offset");
  else if (end_position > 0 && end_position > len)
    return luaL_error(L, "ending offset greater than input ending position");

  /* Allocate packer userdata and initialize metatables */
  if (!(ud = lua_msgpack_create(L, MP_UNPACKING)))
    return luaL_error(L, "could not allocate packer UD");

  top = lua_gettop(L);
  len = (end_position == 0) ? len : end_position;
  if ((count = lua_msgpack_decode(L, ud, s, len, &offset, limit, &err_msg, &err_code)) == 0) {
    if (include_offset && err_code == MSGPACK_UNPACK_CONTINUE) {
      lua_settop(L, top);
      lua_pushinteger(L, -(lua_Integer)(offset + 1));
      lua_pushnil(L);
      return 2;
    }

    msgpack_zone_destroy(&ud->u.unpacked.zone); ud->flags = 0;
    return luaL_error(L, err_msg);
  }

  /* Insert the updated string offset at the beginning of the decoded sequence */
  if (include_offset) {
    mp_checkstack(L, 2);
    /*
    ** @TODO: Possibly consider a flag MP_LIMIT_CAP that forces the limit
    ** parameter to be an explicit expectation on the number of results.
    ** Anything less it to be considered in error.
    */
    lua_pushinteger(L, (offset < len) ? (lua_Integer)(offset + 1) : 0);
    lua_insert(L, top + 1);  /* one position above the allocated userdata */
    count++;
  }

  lua_msgpack_destroy(L, top, ud);  /* memory already managed */
  /* lua_remove(L, top);  let moveresults remove lua_msgpack */
  return count;
}

LUALIB_API int mp_unpack (lua_State *L) { return mp_unpacker(L, 0); }

LUALIB_API int mp_unpack_next (lua_State *L) { return mp_unpacker(L, 1); }

LUALIB_API int mp_get_extension (lua_State *L) {
  mp_checktype(L, luaL_checkinteger(L, 1), 1);
  mp_getregt(L, LUACMSGPACK_REG_EXT);
  lua_pushvalue(L, 1);
  lua_rawget(L, -2);
  /* Defensive statement: lua_remove(L, -2); */
  return 1;
}

LUALIB_API int mp_set_extension (lua_State *L) {
  lua_Integer type = 0;
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checkstack(L, 4, "set_extension");

  /* Quickly sanitize extension table */
  lua_getfield(L, 1, LUACMSGPACK_META_MTYPE);
  lua_getfield(L, 1, LUACMSGPACK_META_ENCODE);
  lua_getfield(L, 1, LUACMSGPACK_META_DECODE);

  mp_checktype(L, (type = luaL_checkinteger(L, -3)), 1);
  if (LUACMSGPACK_EXT_RESERVED(type))
    return luaL_argerror(L, 1, "Reserved extension-type identifier");
  else if (!lua_isfunction(L, -1) || !lua_isfunction(L, -2))  /* meta-methods */
    return luaL_argerror(L, 2, "missing pack/unpack metamethods.");
  lua_pop(L, 3);

  /* Ensure extension id isn't already used... */
  mp_getregt(L, LUACMSGPACK_REG_EXT);
  lua_rawgeti(L, -1, type);  /* [ext, value] */
  if (lua_isnil(L, -1)) {
    lua_pushvalue(L, 1);
    lua_rawseti(L, -3, type);  /* Pop: value */
    lua_pop(L, 2);  /* Pop: nil & LUACMSGPACK_REG_EXT */

    lua_pushvalue(L, 1);
    return 1;
  }
  else {
    lua_pop(L, 2);
    return luaL_error(L, "attempting to replace registered msgpack extension");
  }
}

LUALIB_API int mp_clear_extension (lua_State *L) {
  int i, nargs = lua_gettop(L);

  mp_getregt(L, LUACMSGPACK_REG_EXT);
  for (i = 1; i <= nargs; i++) {
    lua_Integer type = 0;
    mp_checktype(L, (type = luaL_checkinteger(L, i)), 1);
    if (LUACMSGPACK_EXT_RESERVED(type))
      return luaL_argerror(L, 1, "Reserved extension-type identifier");

    lua_pushvalue(L, i);
    lua_pushnil(L);
    lua_rawset(L, -3);
  }
  lua_pop(L, 1);  /* LUACMSGPACK_REG_EXT */
  return 0;
}

LUALIB_API int mp_get_type_extension (lua_State *L) {
  int top = lua_gettop(L);
  lua_Integer ltype = typetoindex(L, lua_tostring(L, 1));
  luaL_argcheck(L, ltype != -1, 1, "Lua type");

  mp_getregt(L, LUACMSGPACK_REG_EXT);
  lua_pushinteger(L, LUACMSGPACK_LUATYPE_EXT(ltype));  /* Ensure is array */
  lua_gettable(L, -2);
  if (mp_isinteger(L, -1)) {  /* Associated to an extension type, fetch it */
    lua_Integer ext = lua_tointeger(L, -1); lua_pop(L, 1);
    mp_getregt(L, LUACMSGPACK_REG_EXT);  /* [ext] */
    lua_rawgeti(L, -1, ext);
  }
  lua_insert(L, top + 1);
  lua_pop(L, lua_gettop(L) - top - 1);
  return 1;
}

LUALIB_API int mp_set_type_extension (lua_State *L) {
  lua_Integer ltype = typetoindex(L, lua_tostring(L, 1));
  int t = lua_type(L, 2);
  luaL_argcheck(L, ltype != -1, 1, "Lua type");
  luaL_argcheck(L, t == LUA_TNUMBER || t == LUA_TTABLE, 2, "extension or table");

  mp_getregt(L, LUACMSGPACK_REG_EXT); /* [ext] */
  if (t == LUA_TNUMBER) {
    lua_rawgeti(L, -1, lua_tointeger(L, 2));  /* [ext, encoder] */
    if (lua_isnil(L, -1))
      return luaL_error(L, "attempting to associate to nil msgpack extension");
    lua_pop(L, 1);  /* [ext] */
  }
  else {
    /* Quickly sanitize extension table */
    lua_getfield(L, 2, LUACMSGPACK_META_ENCODE);
    lua_getfield(L, 2, LUACMSGPACK_META_DECODE);  /* [ext, type, func, func] */
    if (!lua_isfunction(L, -1) || !lua_isfunction(L, -2))  /* meta-methods */
      return luaL_argerror(L, 2, "missing pack/unpack metamethods.");
    lua_pop(L, 2);  /* [ext] */
  }

  /* Associate the value to a synthetic Lua extension-type identifier. */
  lua_pushinteger(L, LUACMSGPACK_LUATYPE_EXT(ltype));  /* Ensure is array */
  lua_pushvalue(L, 2);  /* [ext, ltype, association] */
  lua_settable(L, -3);  /* [ext] */
  lua_pop(L, 1);

  lua_pushvalue(L, 2);  /* Return the encoder value */
  return 1;
}

LUALIB_API int mp_setoption (lua_State *L) {
  int opt;
  lua_Integer flags = mp_getregi(L, LUACMSGPACK_REG_OPTIONS, MP_DEFAULT);
  switch ((opt = optsnum[luaL_checkoption(L, 1, NULL, opts)])) {
    case MP_EMPTY_AS_ARRAY:
    case MP_UNSIGNED_INTEGERS:
      luaL_checktype(L, 2, LUA_TBOOLEAN);
      mp_setregi(L, LUACMSGPACK_REG_OPTIONS, lua_toboolean(L, 2) ?
                                                (flags | opt) : (flags & ~opt));
      break;
    case MP_NUMBER_AS_INTEGER:
    case MP_NUMBER_AS_FLOAT:
    case MP_NUMBER_AS_DOUBLE:
      luaL_checktype(L, 2, LUA_TBOOLEAN);
      flags &= lua_toboolean(L, 2) ? ~MP_MASK_NUMBER : MP_MASK_NUMBER;
      flags = lua_toboolean(L, 2) ? (flags | opt) : (flags & ~opt);
      if ((flags & MP_MASK_NUMBER) == 0)
        flags |= (MP_DEFAULT & MP_MASK_NUMBER);

      mp_setregi(L, LUACMSGPACK_REG_OPTIONS, flags);
      break;
    case MP_ARRAY_AS_MAP:
    case MP_ARRAY_WITH_HOLES:
    case MP_ARRAY_WITHOUT_HOLES:
      luaL_checktype(L, 2, LUA_TBOOLEAN);
      flags &= lua_toboolean(L, 2) ? ~MP_MASK_ARRAY : MP_MASK_ARRAY;
      flags = lua_toboolean(L, 2) ? (flags | opt) : (flags & ~opt);
      if ((flags & MP_MASK_ARRAY) == 0)
        flags |= (MP_DEFAULT & MP_MASK_ARRAY);
      if ((flags & MP_ARRAY_AS_MAP) != 0)
        flags &= ~MP_EMPTY_AS_ARRAY;
      mp_setregi(L, LUACMSGPACK_REG_OPTIONS, flags);
      break;
    case MP_STRING_BINARY:
    case MP_STRING_COMPAT:
      luaL_checktype(L, 2, LUA_TBOOLEAN);
      flags &= lua_toboolean(L, 2) ? ~MP_MASK_STRING : MP_MASK_STRING;
      flags = lua_toboolean(L, 2) ? (flags | opt) : (flags & ~opt);
      if ((flags & MP_MASK_STRING) == 0)
        flags |= (MP_DEFAULT & MP_MASK_STRING);

      mp_setregi(L, LUACMSGPACK_REG_OPTIONS, flags);
      break;
    default:
      break;
  }
  return 0;
}

LUALIB_API int mp_getoption (lua_State *L) {
  int opt;
  lua_Integer flag = 0;
  switch ((opt = optsnum[luaL_checkoption(L, 1, NULL, opts)])) {
    case MP_EMPTY_AS_ARRAY:
    case MP_UNSIGNED_INTEGERS:
      flag = mp_getregi(L, LUACMSGPACK_REG_OPTIONS, MP_DEFAULT);
      lua_pushboolean(L, (flag & opt) != 0);
      break;
    case MP_STRING_BINARY:
    case MP_STRING_COMPAT:
      flag = mp_getregi(L, LUACMSGPACK_REG_OPTIONS, MP_DEFAULT) & MP_MASK_STRING;
      lua_pushboolean(L, (((flag == 0) ? (MP_DEFAULT & MP_MASK_STRING) : flag) & opt) != 0);
      break;
    case MP_ARRAY_AS_MAP:
    case MP_ARRAY_WITH_HOLES:
    case MP_ARRAY_WITHOUT_HOLES:
      flag = mp_getregi(L, LUACMSGPACK_REG_OPTIONS, MP_DEFAULT) & MP_MASK_ARRAY;
      lua_pushboolean(L, (((flag == 0) ? (MP_DEFAULT & MP_MASK_ARRAY) : flag) & opt) != 0);
      break;
    case MP_NUMBER_AS_INTEGER:
    case MP_NUMBER_AS_FLOAT:
    case MP_NUMBER_AS_DOUBLE:
      flag = mp_getregi(L, LUACMSGPACK_REG_OPTIONS, MP_DEFAULT) & MP_MASK_NUMBER;
      lua_pushboolean(L, (((flag == 0) ? (MP_DEFAULT & MP_MASK_NUMBER) : flag) & opt) != 0);
      break;
    case MP_SMALL_LUA:
#if defined(LUACMSGPACK_BIT32)
      lua_pushboolean(L, 1);
#else
      lua_pushboolean(L, 0);
#endif
      break;
    case MP_FULL_64_BITS:
#if defined(LUACMSGPACK_BIT32)
      lua_pushboolean(L, 0);
#else
      lua_pushboolean(L, 1);
#endif
      break;
    case MP_LONG_DOUBLE:
#if LUA_VERSION_NUM >= 503
      lua_pushboolean(L, LUA_FLOAT_TYPE == LUA_FLOAT_LONGDOUBLE);
#else
      lua_pushboolean(L, 0);
#endif
      break;
    default:
      return 0;
  }
  return 1;
}

static int mp_set_string (lua_State *L) {
  static const char *const s_opts[] = { "string", "string_compat", "string_binary", NULL };
  static const int s_optsnum[] = { 0x0, MP_STRING_COMPAT, MP_STRING_BINARY };
  lua_Integer flags = mp_getregi(L, LUACMSGPACK_REG_OPTIONS, MP_DEFAULT) & ~MP_MASK_STRING;

  int opt = s_optsnum[luaL_checkoption(L, 1, NULL, s_opts)];
  mp_setregi(L, LUACMSGPACK_REG_OPTIONS, flags | opt);
  return 0;
}

static int mp_set_array (lua_State *L) {
  static const char *const s_opts[] = { "without_hole", "with_hole", "always_as_map", NULL };
  static const int s_optsnum[] = { MP_ARRAY_WITHOUT_HOLES, MP_ARRAY_WITH_HOLES, MP_ARRAY_AS_MAP };
  lua_Integer flags = mp_getregi(L, LUACMSGPACK_REG_OPTIONS, MP_DEFAULT) & ~MP_MASK_ARRAY;

  int opt = s_optsnum[luaL_checkoption(L, 1, NULL, s_opts)];
  flags |= opt;  /* Validate any post-conditions */
  if ((flags & MP_ARRAY_AS_MAP) != 0)
    flags &= ~MP_EMPTY_AS_ARRAY;
  mp_setregi(L, LUACMSGPACK_REG_OPTIONS, flags);
  return 0;
}

static int mp_set_integer (lua_State *L) {
  static const char *const s_opts[] = { "signed", "unsigned", NULL };
  static const int s_optsnum[] = { 0x0, MP_UNSIGNED_INTEGERS };
  lua_Integer flags = mp_getregi(L, LUACMSGPACK_REG_OPTIONS, MP_DEFAULT) & ~MP_UNSIGNED_INTEGERS;

  int opt = s_optsnum[luaL_checkoption(L, 1, NULL, s_opts)];
  mp_setregi(L, LUACMSGPACK_REG_OPTIONS, flags | opt);
  return 0;
}

static int mp_set_number (lua_State *L) {
  static const char *const s_opts[] = { "float", "double", NULL };
  static const int s_optsnum[] = { MP_NUMBER_AS_FLOAT, MP_NUMBER_AS_DOUBLE };
  lua_Integer flags = mp_getregi(L, LUACMSGPACK_REG_OPTIONS, MP_DEFAULT) & ~MP_MASK_NUMBER;

  int opt = s_optsnum[luaL_checkoption(L, 1, NULL, s_opts)];
  mp_setregi(L, LUACMSGPACK_REG_OPTIONS, flags | opt);
  return 0;
}

#if defined(LUACMSGPACK_SAFE)
/* lua_cmsgpack.c */
static int mp_safe (lua_State *L) {
  int argc = lua_gettop(L), err, total_results;

  /*
  ** This adds our function to the bottom of the stack (the "call this function"
  ** position)
  */
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);

  err = lua_pcall(L, argc, LUA_MULTRET, 0);
  total_results = lua_gettop(L);
  if (err) {
    lua_pushnil(L);
    lua_insert(L,-2);
    return 2;
  }
  return total_results;
}

static int mp_issafe (lua_State *L) {
  lua_pushboolean(L, 1);
  return 1;
}
#else
static int mp_issafe (lua_State *L) {
  lua_pushboolean(L, 0);
  return 1;
}
#endif

static const luaL_Reg msgpack_lib[] = {
  { "pack", mp_pack },
  { "unpack", mp_unpack },
  { "next", mp_unpack_next },
  /* Configuration */
  { "setoption", mp_setoption },
  { "getoption", mp_getoption },
  { "safe", mp_issafe },
  /* Compat Configuration */
  { "set_string", mp_set_string },
  { "set_array", mp_set_array },
  { "set_integer", mp_set_integer },
  { "set_number", mp_set_number },
  /* Extension API*/
  { "extend", mp_set_extension },
  { "extend_get", mp_get_extension },
  { "extend_clear", mp_clear_extension },
  { "gettype", mp_get_type_extension },
  { "settype", mp_set_type_extension },
  { NULL, NULL }
};

LUAMOD_API int luaopen_cmsgpack (lua_State *L) {
#if LUA_VERSION_NUM == 501
  luaL_register(L, LUACMSGPACK_NAME, msgpack_lib);
#else
  luaL_newlib(L, msgpack_lib);
#endif

#if defined(LUACMSGPACK_SAFE)
  {
    size_t i;
    /* Wrap all functions in the safe handler */
    for (i = 0; i < (sizeof(msgpack_lib)/sizeof(*msgpack_lib) - 1); i++) {
      lua_getfield(L, -1, msgpack_lib[i].name);
      lua_pushcclosure(L, mp_safe, 1);
      lua_setfield(L, -2, msgpack_lib[i].name);
    }
  }
#endif

  /* metatable for packer userdata */
  if (luaL_newmetatable(L, LUACMSGPACK_USERDATA)) {
#if LUA_VERSION_NUM == 501
    luaL_register(L, NULL, msgpack_metafuncs);
#else
    luaL_setfuncs(L, msgpack_metafuncs, 0);
#endif
  }
  lua_pop(L, 1);  /* pop metatable */

  lua_pushliteral(L, LUACMSGPACK_NAME); lua_setfield(L, -2, "_NAME");
  lua_pushliteral(L, LUACMSGPACK_VERSION); lua_setfield(L, -2, "_VERSION");
  lua_pushliteral(L, LUACMSGPACK_COPYRIGHT); lua_setfield(L, -2, "_COPYRIGHT");
  lua_pushliteral(L, LUACMSGPACK_DESCRIPTION); lua_setfield(L, -2, "_DESCRIPTION");

  /* Register name globally for 5.1 */
#if LUA_VERSION_NUM == 501
  lua_pushvalue(L, -1);
  lua_setglobal(L, "msgpack");
#endif
  return 1;
}

/* }================================================================== */
