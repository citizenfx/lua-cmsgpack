/*
** See LICENSE.
*/
#include <stdint.h>
#include <math.h>
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

/* Lua 5.4 changed the definition of lua_newuserdata; no uservalues required */
#if LUA_VERSION_NUM >= 504
  #define mp_newuserdata(L, s) lua_newuserdatauv((L), (s), 0)
#elif LUA_VERSION_NUM >= 501 && LUA_VERSION_NUM <= 503
  #define mp_newuserdata(L, s) lua_newuserdata((L), (s))
#else
  #error unsupported Lua version
#endif

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

/*
** Return true if the table at the specified stack index can be encoded as an
** array, i.e., a table whose keys are (1) integers; (2) begin at one; (3)
** strictly positive; and (4) form a contiguous sequence.
**
** However, with the flag "MP_ARRAY_WITH_HOLES" set, condition (4) is alleviated
** and msgpack can encode "null" in the nil array indices.
*/
static int mp_table_is_an_array (lua_State *L, size_t *array_length) {
  lua_Integer n;
  size_t count = 0, max = 0;
  int stacktop = lua_gettop(L);

  mp_checkstack(L, 2);
  lua_pushnil(L);
  while (lua_next(L, -2)) {  /* [key, value] */
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
                                               int level, size_t array_length) {
  size_t j;

  msgpack_pack_array(&ud->u.packed.packer, array_length);
  mp_checkstack(L, 1);
  for (j = 1; j <= array_length; j++) {
#if LUA_VERSION_NUM >= 503
    lua_rawgeti(L, -1, (lua_Integer)j);
#else
    lua_pushinteger(L, (lua_Integer)j);
    lua_rawget(L, -2);
#endif
    lua_msgpack_encode(L, ud, level + 1);
  }
}

/*
** Encode the table at the specified stack index as a <key, value> array.
*/
static void mp_encode_lua_table_as_map (lua_State *L, lua_msgpack *ud, int level) {
  size_t len = 0;

  /*
  ** Count the number of <key, value> pairs in the table. It's impossible to
  ** single-pass this logic given the limitations in the Lua API and inability
  ** to reserve bytes and retroactively encode the table length.
  */
  mp_checkstack(L, 3);
  lua_pushnil(L);
  while (lua_next(L, -2)) {
    len++;
    lua_pop(L, 1);  /* remove value, keep key for next iteration. */
  }

  msgpack_pack_map(&ud->u.packed.packer, len);
  lua_pushnil(L);
  while (lua_next(L, -2)) {
    lua_pushvalue(L, -2);
    lua_msgpack_encode(L, ud, level + 1);  /* Encode Key */
    lua_msgpack_encode(L, ud, level + 1);  /* Encode Value */
  }
}

/*
** Earlier versions of Lua have no explicit integer types, therefore detect if
** floating types can be faithfully casted to an int.
*/
static inline void mp_encode_lua_number (lua_State *L, lua_msgpack *ud) {
  lua_Number n = lua_tonumber(L, -1);
  msgpack_packer *pk = &(ud->u.packed.packer);
#if defined(LUACMSGPACK_BIT32)
  if (IS_INT_EQUIVALENT(n))
    msgpack_pack_int32(pk, (int32_t)n);
  else
    msgpack_pack_double(pk, (double)n);
#else
  if (IS_INT64_EQUIVALENT(n))
    msgpack_pack_int64(pk, (int64_t)n);
  else
    msgpack_pack_double(pk, (double)n);
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
    /*
    ** Temporarily treat all extended types as a string: at least until an API
    ** is developed for handling extended types.
    */
    case MSGPACK_OBJECT_EXT: {
      msgpack_object_ext ext = obj->via.ext;
      lua_pushlstring(L, (const char *)ext.ptr, ext.size);
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

LUA_API lua_msgpack *lua_msgpack_create (lua_State *L, int flags) {
  lua_msgpack *ud = NULL;

  int mode = flags & MP_MODE;
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
    msgpack_unpacked_init(&(ud->u.unpacked));
  }
  else {
    luaL_error(L, "invalid msgpack mode");
    return NULL;
  }

  ud->flags = MP_OPEN | mode;
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
      msgpack_unpacked_destroy(&(ud->u.unpacked));
    }
    ud->flags = 0;

    mp_checkstack(L, 1);  /* function can be called from pack/unpack */
    lua_pushnil(L);
    lua_setmetatable(L, idx);  /* Remove metatable, memory already managed */
    return 1;
  }
  return 0;
}

LUA_API void lua_msgpack_encode (lua_State *L, lua_msgpack *ud, int level) {
  int t = lua_type(L, -1);
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
      if (lua_toboolean(L, -1))
        msgpack_pack_true(pk);
      else
        msgpack_pack_false(pk);
      break;
    }
    case LUA_TNUMBER: {
#if LUA_VERSION_NUM < 503
      mp_encode_lua_number(L, ud);
#else
  #if defined(LUACMSGPACK_BIT32)
      if (lua_isinteger(L, -1))
        msgpack_pack_int32(pk, (int32_t)lua_tointeger(L, -1));
      else  /* still attempt to pack numeric types as integers when possible. */
        mp_encode_lua_number(L, ud);
  #else
      if (lua_isinteger(L, -1))
        msgpack_pack_int64(pk, (int64_t)lua_tointeger(L, -1));
      else
        mp_encode_lua_number(L, ud);
  #endif
#endif
      break;
    }
    case LUA_TSTRING: {
      size_t len;
      const char *s = lua_tolstring(L, -1, &len);
      msgpack_pack_str(pk, len);
      msgpack_pack_str_body(pk, s, len);
      break;
    }
    case LUA_TTABLE: {
      size_t array_length = 0;
      if (mp_table_is_an_array(L, &array_length))
        mp_encode_lua_table_as_array(L, ud, level, array_length);
      else
        mp_encode_lua_table_as_map(L, ud, level);
      break;
    }
    /*
    ** @TODO: Associate a msgpack extended type identifier with functions and
    ** userdata types. For the time, push temporary data to ensure all structure
    ** data is still maintained.
    */
    case LUA_TLIGHTUSERDATA:
    case LUA_TUSERDATA:
#if defined(LUACMSGPACK_BIT32)
      msgpack_pack_uint32(pk, (uint32_t)lua_touserdata(L, -1));
#else
      msgpack_pack_uint64(pk, (uint64_t)lua_touserdata(L, -1));
#endif
      break;
    case LUA_TTHREAD:
    case LUA_TFUNCTION:
    default: {
      luaL_error(L, "type <%s> cannot be msgpack'd", lua_typename(L, t));
      break;
    }
  }
  lua_pop(L, 1);
}

LUA_API int lua_msgpack_decode (lua_State *L, lua_msgpack *ud, const char *s,
                    size_t len, size_t *offset, int limit, const char **error) {
  int done = 0;
  int object_count = 0;
  const char *err_msg = NULL;  /* luaL_error message on failure */
  while (err_msg == NULL && !done) {
    switch (msgpack_unpack_next(&(ud->u.unpacked), s, len, offset)) {
      case MSGPACK_UNPACK_SUCCESS: {
        if (mp_decode_to_lua_type(L, &(ud->u.unpacked.data))) {
          done = (++object_count >= limit && limit > 0) || *offset == len;
        }
        else {
          err_msg = "could not unpack final type";
        }
        break;
      }
      case MSGPACK_UNPACK_EXTRA_BYTES: {
        if (mp_decode_to_lua_type(L, &(ud->u.unpacked.data))) {
          done = (++object_count >= limit && limit > 0);
        }
        else {
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
        err_msg = "msgpack unknown decoder result";
        break;
    }
  }

  *error = err_msg;
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
    lua_pushvalue(L, i);
    lua_msgpack_encode(L, ud, 0);  /* PopValue */
  }

  lua_pushlstring(L, ud->u.packed.buffer.b, ud->u.packed.buffer.n);
  lua_msgpack_destroy(L, top, ud);  /* memory already managed */
  /* lua_remove(L, top);  let moveresults remove lua_msgpack */
  return 1;
}

LUALIB_API int mp_unpack (lua_State *L) {
  int top = 0, count = 0, limit = 0;
  size_t len = 0, offset = 0, sub_len = 0;
  lua_msgpack *ud = NULL;  /* Decoder */

  const char *err_msg = NULL;  /* Error message on decoding failure. */
  const char *s = luaL_checklstring(L, 1, &len);
  offset = luaL_optsizet(L, 2, 0);
  limit = (int)luaL_optinteger(L, 3, 0);
  sub_len = luaL_optsizet(L, 4, 0);

  /* @TODO: lua_pushfstring doesn't support %zu's for formatting errors... */
  if (len == 0)  /* edge-case sanitation */
    return 0;
  else if (limit < 0)
    return luaL_error(L, "invalid limit");
  else if (offset > len)
    return luaL_error(L, "start offset greater than input length");
  else if (sub_len > 0 && sub_len > (len - offset))
    return luaL_error(L, "ending offset greater than input sub-length");

  /* Allocate packer userdata and initialize metatables */
  if (!(ud = lua_msgpack_create(L, MP_UNPACKING)))
    return luaL_error(L, "could not allocate packer UD");

  top = lua_gettop(L);
  len = (sub_len == 0) ? len : (offset + sub_len);
  if ((count = lua_msgpack_decode(L, ud, s, len, &offset, limit, &err_msg)) == 0) {
    msgpack_unpacked_destroy(&(ud->u.unpacked)); ud->flags = 0;
    return luaL_error(L, err_msg);
  }

  lua_msgpack_destroy(L, top, ud);  /* memory already managed */
  /* lua_remove(L, top);  let moveresults remove lua_msgpack */
  return count;
}

static const luaL_Reg msgpack_lib[] = {
  { "pack", mp_pack },
  { "unpack", mp_unpack },
  { NULL, NULL }
};

LUAMOD_API int luaopen_cmsgpack (lua_State *L) {
#if LUA_VERSION_NUM == 501
  luaL_register(L, LUACMSGPACK_NAME, msgpack_lib);
#else
  luaL_newlib(L, msgpack_lib);
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
