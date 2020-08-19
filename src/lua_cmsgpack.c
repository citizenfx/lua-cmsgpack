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

/*
** Default chunk msgpack_zone chunk size.
**
** @TODO: Experiment with changing this (and other values) based on some input
**        heuristic.
*/
#define LUACMSGPACK_ZONE_CHUNK_SIZE 256

/*
** Threshold for mp_table_is_an_array: If a table has an integer key greater
** than this value, ensure at least half of the keys within the table have
** elements to be encoded as an array; this addition is technically not
** one-to-one with MessagePack.lua.
*/
#define MP_TABLE_CUTOFF 16

/* Lua 5.4 changed the definition of lua_newuserdata */
#if LUA_VERSION_NUM >= 504
  #define mp_newuserdata(L, s) lua_newuserdatauv((L), (s), 1)
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
  for (i = 0; i < (LUA_TTHREAD + 1); ++i) {
#else
  for (i = 0; i < LUA_NUMTAGS; ++i) {
#endif
    if (i != LUA_TLIGHTUSERDATA && strcmp(lua_typename(L, i), name) == 0) {
      found = i;
      break;
    }
  }

  if (found == -1 && strcmp("lightuserdata", name) == 0)
    found = LUA_TLIGHTUSERDATA;
  return found;
}

/*
** Decode the msgpack_object and place its contents onto the Lua stack.
** Returning 1 on success, 0 on failure.
**/
static int mp_decode_to_lua_type (lua_State *L, msgpack_object *obj, lua_Integer flags);

/*
** {==================================================================
** msgpack-c bindings
** ===================================================================
*/
#define mp_rel_index(idx, n) (((idx) < 0) ? ((idx) - (n)) : (idx))

static int mp_table_is_an_array (lua_State *L, int idx, lua_Integer flags, size_t *array_length) {
  lua_Integer n;
  size_t count = 0, max = 0, arraylen = 0;
#if !defined(LUACMSGPACK_COMPAT)
  size_t strlen = 0;
  const char *key = NULL;
#endif

  int stacktop = lua_gettop(L);
  int i_idx = mp_rel_index(idx, 1);

  mp_checkstack(L, 2);
  lua_pushnil(L);
  while (lua_next(L, i_idx)) {  /* [key, value] */
    if (mp_isinteger(L, -2)  /* && within range of size_t */
        && ((n = lua_tointeger(L, -2)) >= 1 && ((size_t)n) <= MAX_SIZE)) {
      count++;  /* Is a valid array index ... */
      max = ((size_t)n) > max ? ((size_t)n) : max;
    }
#if !defined(LUACMSGPACK_COMPAT)
    /* support the common table.unpack / { n = select("#", ...), ... } idiom */
    else if (lua_type(L, -2) == LUA_TSTRING
             && mp_isinteger(L, -1)
             && ((n = lua_tointeger(L, -1)) >= 1 && ((size_t)n) <= MAX_SIZE)
             && (key = lua_tolstring(L, -2, &strlen), strlen == 1)
             && key[0] == 'n') {
      arraylen = (size_t)n;
      max = arraylen > max ? arraylen : max;
    }
#endif
    else {
      lua_settop(L, stacktop);
      return 0;
    }
    lua_pop(L, 1);  /* [key] */
  }
  *array_length = max;

  lua_settop(L, stacktop);
  if (max == count)
    return max > 0 || (flags & MP_EMPTY_AS_ARRAY);
  /* don't create an array with too many holes (inserted nils) */
  else if (flags & MP_ARRAY_WITH_HOLES)
    return ((max < MP_TABLE_CUTOFF) || max <= arraylen || (count >= (max >> 1)));
  return 0;
}

static void mp_encode_lua_table_as_array( lua_State *L, lua_msgpack *ud, int idx, int level, size_t array_length) {
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

static void mp_encode_lua_table_as_map (lua_State *L, lua_msgpack *ud, int idx, int level) {
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

static int mp_decode_to_lua_type (lua_State *L, msgpack_object *obj, lua_Integer flags) {
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
      lua_pushnumber(L, (lua_Number)((float)obj->via.f64));
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
      mp_checkstack(L, 2);
      for (i = 0; i < array.size; ++i) {
#if LUA_VERSION_NUM >= 503
        if (mp_decode_to_lua_type(L, &(array.ptr[i]), flags))
          lua_rawseti(L, -2, (lua_Integer)(i + 1));
#else
        lua_pushinteger(L, (lua_Integer)(i + 1));
        if (mp_decode_to_lua_type(L, &(array.ptr[i]), flags))
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

      /*
      ** Compat: Technically, a map can be an "array_with_holes". Therefore, we
      ** cannot preallocate the number of map records safely. Another workaround
      ** would be to allocate the same number of table & array records, thereby
      ** creating a "mixed" table where "unpack" and "rawlen" still work as
      ** intended
      */
      /* lua_createtable(L, 0, (map.size <= INT_MAX) ? (int)map.size : 0); */

      lua_newtable(L);
      mp_checkstack(L, 5);
      for (i = 0; i < map.size; ++i) {
        if (mp_decode_to_lua_type(L, &(map.ptr[i].key), flags)) {
          /* If the key of the map is null, replace it with the null-sentinel */
          if (flags & MP_USE_SENTINEL)
            mp_replace_null(L);

          if (!lua_isnil(L, -1) && mp_decode_to_lua_type(L, &(map.ptr[i].val), flags))
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
#if LUA_VERSION_NUM >= 503
      lua_rawgeti(L, -1, (lua_Integer)ext.type);
#else
      lua_rawgeti(L, -1, (int)ext.type);
#endif
      if (lua_type(L, -1) == LUA_TTABLE) {
        lua_getfield(L, -1, LUACMSGPACK_META_DECODE);  /* [table, table, decoder] */
        if (lua_isfunction(L, -1)) {
          lua_insert(L, -3); lua_pop(L, 2);  /* [decoder] */
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
  static int popcount(uint32_t x) {
    uint32_t c = 0;
    for (; x > 0; x &= x - 1)
      c++;
    return c;
  }
#endif

/*
** Maximum number of extension type associations, e.g., type(X) is associated to
** type(Y), type(Y) is associated to type(Z), etc.
*/
#define EXT_INDIRECT_MAX 5

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
** If the object at the specified index has a metatable, attempt to use the
** encoders specified.
*/
static inline int mp_encode_ext_metatable (lua_State *L, lua_msgpack *ud, int idx, int8_t ext_id) {
  int metafield; /* Attempt to use packer within the objects metatable */
#if LUA_VERSION_NUM >= 503
  if ((metafield = luaL_getmetafield(L, idx, LUACMSGPACK_META_ENCODE)) == LUA_TNIL) {
#else
  if ((metafield = luaL_getmetafield(L, idx, LUACMSGPACK_META_ENCODE)) == 0) {
#endif
    return 0;
  }
  else if (metafield != LUA_TFUNCTION) {
    lua_pop(L, 1);
    return 0;
  }

  lua_pushvalue(L, mp_rel_index(idx, 1));  /* [encoder, value] */
  lua_pushinteger(L, (lua_Integer)ext_id);  /* [encoder, value, extension type] */
  lua_call(L, 2, 2);  /* [encoded value, optional flag] */
  if (lua_type(L, -2) == LUA_TSTRING) {
    size_t len = 0;
    const char *s = lua_tolstring(L, -2, &len);
    if (lua_toboolean(L, -1))
      lua_mpbuffer_append(LUACMSGPACK_BUFFER(ud), s, len);
    else {
      msgpack_packer *pk = &(ud->u.packed.packer);
      msgpack_pack_ext(pk, len, ext_id);
      msgpack_pack_ext_body(pk, s, len);
    }
    lua_pop(L, 2);  /* metafield & encoded values */
    return 1;
  }
  else {
    lua_pop(L, 2);
    return luaL_error(L, "invalid encoder result from encoder <%d>", (int)ext_id);
  }
}

static int mp_encode_ext_lua_type (lua_State *L, lua_msgpack *ud, int idx, int8_t ext_id) {
  int i;
  lua_checkstack(L, 5);
  /* If the object at the specified index has a metatable, check it for an encoder function */
  if (mp_encode_ext_metatable(L, ud, idx, ext_id))
    return 1;

  /* metatable lookup failed, use extension registry table */
  mp_getregt(L, LUACMSGPACK_REG_EXT);  /* [table] */
  for (i = 0; i < EXT_INDIRECT_MAX; ++i) {
    lua_rawgeti(L, -1, mp_ti(ext_id));

    /* Parse the encoder table */
    if (lua_type(L, -1) == LUA_TTABLE) {  /* [table, table] */
      lua_getfield(L, -1, LUACMSGPACK_META_ENCODE);  /* [table, table, encoder] */
      if (lua_isfunction(L, -1)) {

        lua_insert(L, -3); lua_pop(L, 2);  /* [encoder] */
        lua_pushvalue(L, mp_rel_index(idx, 1));  /* [encoder, value to encode] */
        lua_pushinteger(L, (lua_Integer)ext_id);  /* [encoder, value, type] */
        lua_call(L, 2, 2);  /* [encoded value] */
        if (lua_type(L, -2) == LUA_TSTRING) {
          size_t len = 0;
          const char *s = lua_tolstring(L, -2, &len);
          /* Second, and optional, return value denotes a custom encoding */
          if (lua_toboolean(L, -1))
            lua_mpbuffer_append(LUACMSGPACK_BUFFER(ud), s, len);
          else {
            msgpack_packer *pk = &(ud->u.packed.packer);
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
      return luaL_error(L, "msgpack extension type: invalid encoder");
    }
    /* Lua type has been associated to an extension type */
    else if (lua_type(L, -1) == LUA_TNUMBER) {  /* [table, type] */
      lua_Integer ext = lua_tointeger(L, -1);

      lua_pop(L, 1);  /* [table] */
      if (ext == ext_id)  /* prevent cycles */
        return luaL_error(L, "msgpack extension type: invalid encoder");
      else if (!LUACMSGPACK_EXT_VALID(ext))
        return luaL_error(L, "msgpack extension type: invalid identifier");
      else if (i == (EXT_INDIRECT_MAX - 1))
        return luaL_error(L, "msgpack extension type: invalid identifier associations");

      ext_id = (int8_t)ext;
    }
    /* neither an encoder table or extension type. */
    else {
      lua_pop(L, 1);
      break;
    }
  }
  lua_pop(L, 1);
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
  else if (mode & MP_EXTERNAL) {
#if LUA_VERSION_NUM < 503
    lua_createtable(L, 1, 0);  /* [table] fenv for 5.1, uservalue table for 5.2 */
#endif

    /* Allocate buffer userdata [table, buffer] */
    if ((ud->u.external.buffer = (lua_mpbuffer *)mp_newuserdata(L, sizeof(lua_mpbuffer))) == NULL) {
      luaL_error(L, "Could not allocate buffer UD");
      return NULL;
    }

    luaL_getmetatable(L, LUA_MPBUFFER_USERDATA);  /* [..., buffer, meta] */
    lua_setmetatable(L, -2);  /* [..., buffer] */
#if LUA_VERSION_NUM < 503
    ud->u.external.__ref = luaL_ref(L, -2);  /* [table] */
#endif

#if LUA_VERSION_NUM >= 502
    lua_setuservalue(L, -2);
#elif LUA_VERSION_NUM == 501
    lua_setfenv(L, -2);
#else
  #error unsupported Lua version
#endif
    lua_mpbuffer_init(L, ud->u.external.buffer);
    msgpack_packer_init(&ud->u.external.packer, ud->u.external.buffer, lua_mpbuffer_append);
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
      lua_mpbuffer_free(L, &ud->u.packed.buffer);
    }
    else if (ud->flags & MP_EXTERNAL) {
      if (ud->u.external.buffer) {
        lua_mpbuffer_free(L, ud->u.external.buffer);
      }

      mp_checkstack(L, 3);  /* function can be called from pack/unpack */
#if LUA_VERSION_NUM >= 503
      lua_getuservalue(L, idx);
      if (lua_type(L, -1) == LUA_TUSERDATA) {
        lua_pushnil(L);
        lua_setmetatable(L, -2);
      }
      lua_pop(L, 1);
#else
  #if LUA_VERSION_NUM >= 502
      lua_getuservalue(L, idx);
  #elif LUA_VERSION_NUM == 501
      lua_getfenv(L, idx);
  #else
    #error unsupported Lua version
  #endif
      lua_rawgeti(L, -1, ud->u.external.__ref);
      if (lua_type(L, -1) == LUA_TUSERDATA) {
        lua_pushnil(L);
        lua_setmetatable(L, -2);
      }

      luaL_unref(L, -2, ud->u.external.__ref);
      ud->u.external.__ref = LUA_NOREF;
      lua_pop(L, 2);
#endif
    }
    else if (ud->flags & MP_UNPACKING) {
      msgpack_zone_destroy(&ud->u.unpacked.zone);
    }
    ud->flags = 0;

    lua_pushnil(L);
    lua_setmetatable(L, idx);  /* Remove metatable, memory already managed */
    return 1;
  }
  return 0;
}

LUA_API void lua_msgpack_extension (lua_State *L, lua_Integer type, lua_CFunction encoder, lua_CFunction decoder) {
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
  lua_pack_any(L, ud, idx, level);
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
        if ((done = mp_decode_to_lua_type(L, &obj, ud->flags))) {
          ++object_count;
        }
        else {
          err_res = MSGPACK_UNPACK_PARSE_ERROR;
          err_msg = "could not unpack final type";
        }
        break;
      }
      case MSGPACK_UNPACK_EXTRA_BYTES: {
        if (mp_decode_to_lua_type(L, &obj, ud->flags)) {
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

/* Reference to null */
#if defined(LUACMSGPACK_STATIC_NIL)
static int mp_null_ref = LUA_NOREF;
#endif

LUA_API void mp_replace_null (lua_State *L) {
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
#if defined(LUACMSGPACK_STATIC_NIL)
    lua_rawgeti(L, LUA_REGISTRYINDEX, mp_null_ref);
#else
    lua_rawgeti(L, LUA_REGISTRYINDEX, mp_ti(mp_getregi(L, LUACMSGPACK_REG_NULL, LUA_REFNIL)));
#endif
  }
}

LUA_API int mp_is_null (lua_State *L, int idx) {
  int is = 0;

  mp_checkstack(L, 3);
  lua_pushvalue(L, idx); /* [value] */
#if defined(LUACMSGPACK_STATIC_NIL)
  lua_rawgeti(L, LUA_REGISTRYINDEX, mp_null_ref);
#else
  lua_rawgeti(L, LUA_REGISTRYINDEX, mp_ti(mp_getregi(L, LUACMSGPACK_REG_NULL, LUA_REFNIL)));
#endif
  is = lua_rawequal(L, -1, -2) != 0;
  lua_pop(L, 2);
  return is;
}

/* }================================================================== */

/*
** {==================================================================
** Userdata API
** ===================================================================
*/
#define _opext(ud) ((ud->flags & (MP_OPEN | MP_EXTERNAL)) == (MP_OPEN | MP_EXTERNAL))

#define lua_msgpack_template(INPUT)                                               \
  int i, nargs;                                                                   \
  lua_msgpack *ud = ((lua_msgpack *)luaL_checkudata(L, 1, LUACMSGPACK_USERDATA)); \
  if ((nargs = lua_gettop(L)) < (INPUT))                                          \
    return luaL_argerror(L, 0, "MessagePack pack needs input");                   \
  else if (ud == NULL || !_opext(ud))                                             \
    return luaL_error(L, "invalid packer UD");

#define lua_msgpack_function_op(NAME, PACKER) \
  static int(NAME)(lua_State * L) {           \
    lua_msgpack_template(1);                  \
    PACKER(L, ud);                            \
    lua_pushvalue(L, 1); /* Return "self" */  \
    ((void)(i));                              \
    return 1;                                 \
  }

#define lua_msgpack_function(NAME, PACKER)   \
  static int(NAME)(lua_State * L) {          \
    lua_msgpack_template(2);                 \
    for (i = 2; i <= nargs; i++) {           \
      PACKER(L, ud, i);                      \
    }                                        \
    lua_pushvalue(L, 1); /* Return "self" */ \
    return 1;                                \
  }

#define lua_msgpack_function_level(NAME, PACKER) \
  static int(NAME)(lua_State * L) {              \
    lua_msgpack_template(2);                     \
    for (i = 2; i <= nargs; i++) {               \
      PACKER(L, ud, i, 1);                       \
    }                                            \
    lua_pushvalue(L, 1); /* Return "self" */     \
    return 1;                                    \
  }

lua_msgpack_function(luaL_pack_char, lua_pack_char)

lua_msgpack_function(luaL_pack_signed_char, lua_pack_signed_char)
lua_msgpack_function(luaL_pack_short, lua_pack_short)
lua_msgpack_function(luaL_pack_int, lua_pack_int)
lua_msgpack_function(luaL_pack_long, lua_pack_long)
lua_msgpack_function(luaL_pack_long_long, lua_pack_long_long)
lua_msgpack_function(luaL_pack_unsigned_char, lua_pack_unsigned_char)
lua_msgpack_function(luaL_pack_unsigned_short, lua_pack_unsigned_short)
lua_msgpack_function(luaL_pack_unsigned_int, lua_pack_unsigned_int)
lua_msgpack_function(luaL_pack_unsigned_long, lua_pack_unsigned_long)
lua_msgpack_function(luaL_pack_unsigned_long_long, lua_pack_unsigned_long_long)

lua_msgpack_function(luaL_pack_uint8, lua_pack_uint8);
lua_msgpack_function(luaL_pack_uint16, lua_pack_uint16);
lua_msgpack_function(luaL_pack_uint32, lua_pack_uint32);
lua_msgpack_function(luaL_pack_uint64, lua_pack_uint64);
lua_msgpack_function(luaL_pack_int8, lua_pack_int8);
lua_msgpack_function(luaL_pack_int16, lua_pack_int16);
lua_msgpack_function(luaL_pack_int32, lua_pack_int32);
lua_msgpack_function(luaL_pack_int64, lua_pack_int64);

lua_msgpack_function(luaL_pack_fix_uint8, lua_pack_fix_uint8);
lua_msgpack_function(luaL_pack_fix_uint16, lua_pack_fix_uint16);
lua_msgpack_function(luaL_pack_fix_uint32, lua_pack_fix_uint32);
lua_msgpack_function(luaL_pack_fix_uint64, lua_pack_fix_uint64);
lua_msgpack_function(luaL_pack_fix_int8, lua_pack_fix_int8);
lua_msgpack_function(luaL_pack_fix_int16, lua_pack_fix_int16);
lua_msgpack_function(luaL_pack_fix_int32, lua_pack_fix_int32);
lua_msgpack_function(luaL_pack_fix_int64, lua_pack_fix_int64);

lua_msgpack_function(luaL_pack_signed_int16, lua_pack_signed_int16);
lua_msgpack_function(luaL_pack_signed_int32, lua_pack_signed_int32);
lua_msgpack_function(luaL_pack_signed_int64, lua_pack_signed_int64);

lua_msgpack_function(luaL_pack_float, lua_pack_float)
lua_msgpack_function(luaL_pack_double, lua_pack_double)
lua_msgpack_function(luaL_pack_integer, lua_pack_integer)
lua_msgpack_function(luaL_pack_number, lua_pack_number)

lua_msgpack_function_op(luaL_pack_nil, lua_pack_nil);
lua_msgpack_function_op(luaL_pack_true, lua_pack_true);
lua_msgpack_function_op(luaL_pack_false, lua_pack_false);
lua_msgpack_function(luaL_pack_boolean, lua_pack_boolean);

lua_msgpack_function(luaL_pack_string, lua_pack_string)
lua_msgpack_function(luaL_pack_v4, lua_pack_v4)
lua_msgpack_function(luaL_pack_bin, lua_pack_bin)
lua_msgpack_function(luaL_pack_parse_string, lua_pack_parse_string)

lua_msgpack_function_level(luaL_pack_array, lua_pack_array);
lua_msgpack_function_level(luaL_pack_map, lua_pack_map);
lua_msgpack_function_level(luaL_pack_parsed_table, lua_pack_extended_table);
lua_msgpack_function_level(luaL_pack_unparsed_table, lua_pack_table);
lua_msgpack_function_level(luaL_pack_any, lua_pack_any);

static const luaL_Reg packers[] = {
  { "nil", luaL_pack_nil, }, { "null", luaL_pack_nil, },
  { "boolean", luaL_pack_boolean, },
  { "string_compat", luaL_pack_v4, },
  { "_string", luaL_pack_string, },
  { "string", luaL_pack_parse_string },
  { "binary", luaL_pack_bin, },
  { "map", luaL_pack_map, },
  { "array", luaL_pack_array, },
  { "table", luaL_pack_parsed_table },
  { "_table", luaL_pack_unparsed_table },
  { "unsigned", luaL_pack_unsigned_long_long },
  { "signed", luaL_pack_long_long },
  { "integer", luaL_pack_integer },
  { "float", luaL_pack_float },
  { "double", luaL_pack_double },
  { "number", luaL_pack_number },
  { "any", luaL_pack_any },
  { "true", luaL_pack_true, }, { "t", luaL_pack_true, },
  { "false", luaL_pack_false, }, { "f", luaL_pack_false, },
  { "fix_uint8", luaL_pack_fix_uint8, },
  { "fix_uint16", luaL_pack_fix_uint16, },
  { "fix_uint32", luaL_pack_fix_uint32, },
  { "fix_uint64", luaL_pack_fix_uint64, },
  { "fix_int8", luaL_pack_fix_int8, },
  { "fix_int16", luaL_pack_fix_int16, },
  { "fix_int32", luaL_pack_fix_int32, },
  { "fix_int64", luaL_pack_fix_int64, },
  { "uint8", luaL_pack_uint8, },
  { "uint16", luaL_pack_uint16, },
  { "uint32", luaL_pack_uint32, },
  { "uint64", luaL_pack_uint64, },
  { "int8", luaL_pack_int8, },
  { "int16", luaL_pack_int16, },
  { "int32", luaL_pack_int32, },
  { "int64", luaL_pack_int64, },
  { "char", luaL_pack_char, },
  { "signed_char", luaL_pack_signed_char, },
  { "unsigned_char", luaL_pack_unsigned_char, },
  { "short", luaL_pack_short, },
  { "integer", luaL_pack_int, },
  { "long", luaL_pack_long, },
  { "long_long", luaL_pack_long_long, },
  { "unsigned_short", luaL_pack_unsigned_short, },
  { "unsigned_int", luaL_pack_unsigned_int, },
  { "unsigned_long", luaL_pack_unsigned_long, },
  { "unsigned_long_long", luaL_pack_unsigned_long_long, },
  { "signed_int16", luaL_pack_signed_int16, },
  { "signed_int32", luaL_pack_signed_int32, },
  { "signed_int64", luaL_pack_signed_int64, },
  { NULL, NULL },
};

LUALIB_API int mp_packer_new (lua_State *L) {
  lua_msgpack_create(L, MP_EXTERNAL);
  return 1;
}

static int packed_len (lua_State *L) {
  lua_msgpack *ud = ((lua_msgpack *)luaL_checkudata(L, 1, LUACMSGPACK_USERDATA));
  if (ud && _opext(ud))
    lua_pushinteger(L, (lua_Integer)(LUACMSGPACK_BUFFER(ud))->n);
  else
    lua_pushinteger(L, 0);
  return 1;
}

static int packed_buffer_append (lua_State *L) {
  int i;
  size_t len = 0;
  const char *s = NULL;

  lua_msgpack *ud = ((lua_msgpack *)luaL_checkudata(L, 1, LUACMSGPACK_USERDATA));
  if (ud && _opext(ud)) {
    lua_mpbuffer *buffer = LUACMSGPACK_BUFFER(ud);
    for (i = 2; i <= lua_gettop(L); ++i) {
      if ((s = lua_tolstring(L, i, &len)) != NULL)
        lua_mpbuffer_append(buffer, s, len);
    }
  }
  return 0;
}

static int packed_encode (lua_State *L) {
  int i;
  lua_msgpack *ud = ((lua_msgpack *)luaL_checkudata(L, 1, LUACMSGPACK_USERDATA));
  if (ud && _opext(ud)) {
    for (i = 2; i <= lua_gettop(L); ++i)
      lua_msgpack_encode(L, ud, i, 0);
  }
  return 0;
}

static int packed_tostring (lua_State *L) {
  lua_msgpack *ud = ((lua_msgpack *)luaL_checkudata(L, 1, LUACMSGPACK_USERDATA));
  if (ud && _opext(ud)) {
    lua_mpbuffer *buffer = LUACMSGPACK_BUFFER(ud);
    lua_pushlstring(L, buffer->b, buffer->n);
    return 1;
  }
  return 0;
}

static int packud_gc (lua_State *L) {
  lua_msgpack_destroy(L, 1, NULL);
  return 0;
}

static const luaL_Reg msgpack_metafuncs[] = {
  { "__gc", packud_gc },
  { "__len", packed_len },
  { "__concat", packed_buffer_append },
  { "__call", packed_encode },
  { "__add", packed_encode },
#if LUA_VERSION_NUM >= 503
  { "__shl", packed_encode },  /* lua++ */
#endif
  { "__tostring", packed_tostring },
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
  "small_lua", "full64bits", "long_double", "sentinel", NULL
};

static const int optsnum[] = {
  MP_UNSIGNED_INTEGERS, MP_NUMBER_AS_INTEGER, MP_NUMBER_AS_FLOAT, MP_NUMBER_AS_DOUBLE,
  MP_STRING_COMPAT, MP_STRING_BINARY,
  MP_EMPTY_AS_ARRAY, MP_ARRAY_WITHOUT_HOLES, MP_ARRAY_WITH_HOLES, MP_ARRAY_AS_MAP,
  MP_SMALL_LUA, MP_FULL_64_BITS, MP_LONG_DOUBLE, MP_USE_SENTINEL,
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
  else if (!(ud = lua_msgpack_create(L, MP_PACKING)))
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

static int mp_unpacker (lua_State *L, int compat_api, int include_offset) {
  int top = 0, count = 0, limit = 0;
  size_t len = 0, position = 0, offset = 0, end_position = 0;
  lua_msgpack *ud = NULL;  /* Decoder */

  msgpack_unpack_return err_code = MSGPACK_UNPACK_SUCCESS;
  const char *err_msg = NULL;  /* Error message on decoding failure. */
  const char *s = luaL_checklstring(L, 1, &len);
  if (compat_api) {
    position = 1;
    limit = include_offset ? 1 : 0;
    end_position = 0;
  }
  else {
    position = luaL_optsizet(L, 2, 1);
    limit = (int)luaL_optinteger(L, 3, include_offset ? 1 : 0);
    end_position = luaL_optsizet(L, 4, 0);
  }
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

LUALIB_API int mp_unpack (lua_State *L) { return mp_unpacker(L, 0, 0); }

LUALIB_API int mp_unpack_compat (lua_State *L) { return mp_unpacker(L, 1, 0); }

LUALIB_API int mp_unpack_next (lua_State *L) { return mp_unpacker(L, 0, 1); }

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
  lua_rawgeti(L, -1, mp_ti(type));  /* [ext, value] */

  /* Do: registry.ext[type] = extension_table */
  if (lua_isnil(L, -1)) {
    lua_pushvalue(L, 1);
    lua_rawseti(L, -3, mp_ti(type));  /* Pop: value */
    lua_pop(L, 2);  /* Pop: nil & LUACMSGPACK_REG_EXT */

    lua_pushvalue(L, 1);  /* Return the extension table */
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
#if LUA_VERSION_NUM >= 503
    lua_rawgeti(L, -1, ext);
#else
    lua_pushinteger(L, ext);
    lua_rawget(L, -2);
#endif
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
    lua_Integer ext = lua_tointeger(L, 2);
    if (!LUACMSGPACK_EXT_VALID(ext) || ext == LUACMSGPACK_LUATYPE_EXT(ltype))
      return luaL_error(L, "msgpack extension type: invalid encoder!");

    lua_rawgeti(L, -1, mp_ti(ext));  /* [ext, encoder] */
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
    case MP_USE_SENTINEL:
    case MP_EMPTY_AS_ARRAY:
    case MP_UNSIGNED_INTEGERS:
      luaL_checktype(L, 2, LUA_TBOOLEAN);
      mp_setregi(L, LUACMSGPACK_REG_OPTIONS, lua_toboolean(L, 2) ? (flags | opt) : (flags & ~opt));
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
    case MP_USE_SENTINEL:
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

/* Returns messagepack.null */
static int mp_null (lua_State *L) {
#if defined(LUACMSGPACK_STATIC_NIL)
  lua_rawgeti(L, LUA_REGISTRYINDEX, mp_null_ref);
#else
  lua_rawgeti(L, LUA_REGISTRYINDEX, mp_ti(mp_getregi(L, LUACMSGPACK_REG_NULL, LUA_REFNIL)));
#endif
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
    lua_insert(L, -2);
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
#if defined(LUACMSGPACK_COMPAT)
  { "unpack", mp_unpack_compat },
  { "unpack2", mp_unpack },
#else
  { "unpack", mp_unpack },
#endif
  { "next", mp_unpack_next },
  /* Userdata/Packers API */
  { "new", mp_packer_new },
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
  { "sentinel", mp_null }, { "null", mp_null }, /* compatibility alias */
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
    for (i = 0; i < (sizeof(msgpack_lib) / sizeof(*msgpack_lib) - 1); i++) {
      lua_getfield(L, -1, msgpack_lib[i].name);
      lua_pushcclosure(L, mp_safe, 1);
      lua_setfield(L, -2, msgpack_lib[i].name);
    }
  }
#endif

  /* metatable for (internal) buffer userdata */
  if (luaL_newmetatable(L, LUA_MPBUFFER_USERDATA)) {
#if LUA_VERSION_NUM == 501
    luaL_register(L, NULL, mpbuffer_metafuncs);
#else
    luaL_setfuncs(L, mpbuffer_metafuncs, 0);
#endif
  }
  lua_pop(L, 1);  /* pop metatable */

  /* metatable for packer userdata */
  if (luaL_newmetatable(L, LUACMSGPACK_USERDATA)) {
#if LUA_VERSION_NUM == 501
    luaL_register(L, NULL, msgpack_metafuncs);
#else
    luaL_setfuncs(L, msgpack_metafuncs, 0);
#endif
    /*
    ** __index metamethod contains reference to all generic packers to allow the
    ** userdata to be referenced, e.g., packer:integer(...)
    */
#if LUA_VERSION_NUM == 501
    lua_newtable(L);
    luaL_register(L, NULL, packers);
#else
    luaL_newlib(L, packers);
#endif
    lua_setfield(L, -2, "__index");
  }
  lua_pop(L, 1);  /* pop metatable */

  lua_pushliteral(L, LUACMSGPACK_NAME); lua_setfield(L, -2, "_NAME");
  lua_pushliteral(L, LUACMSGPACK_VERSION); lua_setfield(L, -2, "_VERSION");
  lua_pushliteral(L, LUACMSGPACK_COPYRIGHT); lua_setfield(L, -2, "_COPYRIGHT");
  lua_pushliteral(L, LUACMSGPACK_DESCRIPTION); lua_setfield(L, -2, "_DESCRIPTION");

  /*
  ** Generic packers table.
  **
  ** In Lua-MessagePack each 'packers' function is of the form: "function (buffer, n)"
  ** where "buffer" is a table that gets table.concat'd after a message packing
  ** has been completed. In this implementation "buffer" is the packer userdata.
  */
  lua_newtable(L);
#if LUA_VERSION_NUM == 501
  luaL_register(L, NULL, packers);
#else
  luaL_setfuncs(L, packers, 0);
#endif
  lua_setfield(L, -2, "packers");

  /* Create messagepack.null reference */
  lua_getfield(L, -1, "sentinel");
#if defined(LUACMSGPACK_STATIC_NIL)
  mp_null_ref = luaL_ref(L, LUA_REGISTRYINDEX);
#else
  mp_setregi(L, LUACMSGPACK_REG_NULL, luaL_ref(L, LUA_REGISTRYINDEX));
#endif

  /* Register name globally for 5.1 */
#if LUA_VERSION_NUM == 501
  lua_pushvalue(L, -1);
  lua_setglobal(L, "msgpack");
#endif
  return 1;
}

/* }================================================================== */
