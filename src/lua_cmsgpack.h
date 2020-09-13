/*
** https://github.com/msgpack/msgpack/blob/master/spec.md
** https://github.com/msgpack/msgpack-c
**
** See LICENSE.
*/
#ifndef lua_cmsgpack_h
#define lua_cmsgpack_h

#include <string.h>
#include <stdint.h>

#include <lua.h>
#include <luaconf.h>
#include <lauxlib.h>

#include "msgpack/pack.h"
#include "msgpack/unpack.h"
#include "msgpack/zone.h"
#include "lua_pack_template.h"

/* @TODO: Support 16-bit Lua */
#if !defined(LUACMSGPACK_BIT32) && UINTPTR_MAX == UINT_MAX
  #define LUACMSGPACK_BIT32
#endif

#ifndef LUACMSGPACK_MAX_NESTING
  #define LUACMSGPACK_MAX_NESTING 16  /* Max tables nesting. */
#endif

#if !defined(LUACMSGPACK_INLINE)
  #if defined(__GNUC__) || defined(__CLANG__)
    #define LUACMSGPACK_INLINE inline __attribute__((__always_inline__))
  #elif defined(LUA_USE_WINDOWS)
    #define LUACMSGPACK_INLINE __forceinline
  #else
    #define LUACMSGPACK_INLINE inline
  #endif
#endif

/* Unnecessary & unsafe macro to disable checkstack */
#if defined(LUACMSGPACK_UNSAFE)
  #define mp_checkstack(L, sz) ((void)0)
#else
  #define mp_checkstack(L, sz) luaL_checkstack((L), (sz), "too many (nested) values in encoded msgpack")
#endif

/* Check if float or double can be an integer without loss of precision */
#define IS_INT_TYPE_EQUIVALENT(x, T) (!isinf(x) && ((lua_Number)((T)(x))) == (x))
#define IS_INT64_EQUIVALENT(x) IS_INT_TYPE_EQUIVALENT(x, int64_t)
#define IS_INT_EQUIVALENT(x) IS_INT_TYPE_EQUIVALENT(x, int)

#if LUA_VERSION_NUM >= 503
  #define mp_isinteger(L, idx) lua_isinteger((L), (idx))
#else
static LUACMSGPACK_INLINE int mp_isinteger(lua_State *L, int idx) {
  if (LUA_TNUMBER == lua_type(L, idx)) {
    lua_Number n = lua_tonumber(L, idx);
    return IS_INT_TYPE_EQUIVALENT(n, lua_Integer);
  }
  return 0;
}
#endif

/* Macro for geti/seti. Parameters changed from int to lua_Integer in 53 */
#if LUA_VERSION_NUM >= 503
  #define mp_ti(v) v
#else
  #define mp_ti(v) (int)(v)
#endif

/*
** A luaL_Buffer with its own memory-management.
*/
typedef struct lua_mpbuffer {
  char *b;  /* buffer address */
  size_t size;  /* buffer size */
  size_t n;  /* number of characters in buffer */
  /*
  ** msgpack-c's buffering interface requires a reference to the active lua
  ** State, or at the very least the lua_State's Alloc function. The buffer
  ** caches the state, but does not guarantee to always use this state.
  */
  lua_State *L;
} lua_mpbuffer;

/*
** {==================================================================
** External API
** ===================================================================
*/

#if !defined(LUAMOD_API)  /* LUA_VERSION_NUM == 501 */
  #define LUAMOD_API LUALIB_API
#endif

#define LUACMSGPACK_REG "lua_cmsgpack"
#define LUACMSGPACK_REG_OPTIONS "lua_cmsgpack_flags"
#define LUACMSGPACK_REG_EXT "lua_cmsgpack_meta"
#define LUACMSGPACK_REG_NULL "lua_rapidjson_nullref"

#define LUACMSGPACK_META_MTYPE "__ext"
#define LUACMSGPACK_META_ENCODE "__pack"
#define LUACMSGPACK_META_DECODE "__unpack"

#define LUACMSGPACK_USERDATA "LUACMSGPACK"

#define MP_OPEN                0x01  /* Userdata data resources are alive. */
#define MP_PACKING             0x02  /* Deallocate packing structures */
#define MP_UNPACKING           0x04  /* Deallocate unpacking structures */
#define MP_EXTERNAL            0x08  /* Userdata */

#define MP_UNSIGNED_INTEGERS   0x10  /* Encode integers as signed/unsigned values */
#define MP_NUMBER_AS_INTEGER   0x20  /* */
#define MP_NUMBER_AS_FLOAT     0x40  /* Encode lua_Numbers as floats (regardless of type) */
#define MP_NUMBER_AS_DOUBLE    0x80  /* Reserved for inverse-toggling NUMBER_AS_FLOAT */
#define MP_STRING_COMPAT       0x100  /* Use raw (V4) for encoding strings */
#define MP_STRING_BINARY       0x200
#define MP_EMPTY_AS_ARRAY      0x400 /* Empty table encoded as an array. */
#define MP_ARRAY_AS_MAP        0x800  /* Encode table as <key,value> pairs */
#define MP_ARRAY_WITH_HOLES    0x1000  /* Encode all tables with positive integer keys as arrays. */
#define MP_ARRAY_WITHOUT_HOLES 0x2000  /* Only encode contiguous arrays as an array */

#define MP_SMALL_LUA           0x4000 /* Compatibility only */
#define MP_FULL_64_BITS        0x8000 /* Compatibility only */
#define MP_LONG_DOUBLE         0x10000 /* */
#define MP_USE_SENTINEL        0x20000 /* Replacement for nil table keys */

#define MP_MODE (MP_PACKING | MP_UNPACKING | MP_EXTERNAL)
#define MP_MASK_RUNTIME (MP_OPEN | MP_MODE)  /* flags that can't be setoption'd */
#define MP_MASK_ARRAY (MP_ARRAY_AS_MAP | MP_ARRAY_WITH_HOLES | MP_ARRAY_WITHOUT_HOLES)
#define MP_MASK_STRING (MP_STRING_COMPAT | MP_STRING_BINARY)
#define MP_MASK_NUMBER (MP_NUMBER_AS_INTEGER | MP_NUMBER_AS_FLOAT | MP_NUMBER_AS_DOUBLE)

#if defined(LUACMSGPACK_BIT32)
  #define MP_DEFAULT (MP_EMPTY_AS_ARRAY | MP_UNSIGNED_INTEGERS | MP_ARRAY_WITHOUT_HOLES | MP_NUMBER_AS_FLOAT | MP_STRING_COMPAT)
#else
  #define MP_DEFAULT (MP_EMPTY_AS_ARRAY | MP_UNSIGNED_INTEGERS | MP_ARRAY_WITHOUT_HOLES | MP_NUMBER_AS_DOUBLE)
#endif

/* True if a lua_Integer is within the extension type range. */
#define LUACMSGPACK_EXT_VALID(i) (INT8_MIN <= (i) && (i) <= INT8_MAX)

/* True if a lua_Integer is within the user-extension range. */
#define LUACMSGPACK_EXT_USER_VALID(i) (0x0 <= (i) && (i) <= INT8_MAX)

/* Potentially reserve types... */
#define LUACMSGPACK_EXT_RESERVED(i) 0

/*
** msgpack reserves -1 for timestamps. Therefore, LUA_TNIL would be mapped to
** -2, LUA_TFUNCTION -8, etc.
*/
#define LUACMSGPACK_LUATYPE_EXT(x) (-(x + 2))

/* A value not within LUACMSGPACK_EXT_VALID */
#define EXT_INVALID -1024

/* Userdata structure for managing/cleaning message packing */
typedef struct lua_msgpack {
  lua_Integer flags;
  union {
    struct {
      msgpack_packer packer;
      lua_mpbuffer buffer;
    } packed;
    struct {
      msgpack_zone zone;
    } unpacked;
    /* External packing API */
    struct {
      msgpack_packer packer;
      lua_mpbuffer *buffer;
#if LUA_VERSION_NUM < 503
      int __ref;  /* Reference to buffer userdata */
#endif
    } external;
  } u;
} lua_msgpack;

/* The lua_mpbuffer instance associated with the active packer state */
#define LUACMSGPACK_BUFFER(P) \
  ((P)->flags & MP_EXTERNAL) ? (P)->u.external.buffer : &((P)->u.packed.buffer)

/* Creates a new lua_msgpack userdata and pushes it onto the stack. */
LUA_API lua_msgpack *lua_msgpack_create (lua_State *L, lua_Integer flags);

/*
** Destroy a msgpack userdata and free any additional resources/memory allocated
** by it. If 'ud' is null, this function attempts to check for the userdata at
** the given index.
*/
LUA_API int lua_msgpack_destroy (lua_State *L, int idx, lua_msgpack *ud);

/*
** Associate two C closures with a msgpack extension-type identifier.
**
** @SEE: mp_set_extension: this function implicitly creates the underlying
**    table definition and 'mp_get_extension' can be used to retrieve that
**    metatable
*/
LUA_API void lua_msgpack_extension (lua_State *L, lua_Integer type, lua_CFunction encoder, lua_CFunction decoder);

/*
** MessagePack the value at the specified stack index.
**
** A "level" is used to monitor the current recursive depth. Once a limit
** (-DLUACMSGPACK_ERROR_NESTING) is reached, the encoder has the option to
** cancel the recursion by replacing whatever value with nil, or throw an error:
** -DLUACMSGPACK_ERROR_NESTING.
**
** Ideally, a supplemental table would be used to track cycles. However, the
** general performance cost likely isn't worth it. @TODO: Experiment.
*/
LUA_API void lua_msgpack_encode (lua_State *L, lua_msgpack *ud, int idx, int level);

/*
** MessageUnpack a specified number of elements at a specific offset within a
** MessagePack encoded string, placing those decoded elements on top of the Lua
** stack and returning the number of decoded values.
**
** PARAMETERS:
**   L - Lua State.
**   ud - Active & initialized MessagePack decoder.
**   s - MessagePack encoded string.
**   len - length of encoded string.
**   offset - Offset within the string to begin/continue decoding.
**   limit - number of elements to decode (0 for all).
**   error - Returned error message on failure.
**
** RETURN:
**    The number of decoded values placed onto the Lua stack; with zero denoting
**    and error, with its message stored in "error".
*/
LUA_API int lua_msgpack_decode (lua_State *L, lua_msgpack *ud, const char *s,
                                          size_t len, size_t *offset, int limit,
                           const char **error, msgpack_unpack_return *err_code);

/* If the element on top of the stack is nil, replace it with its reference */
LUA_API void mp_replace_null (lua_State *L);

/*
** Return true if the object at the specific stack index is, or a reference to,
** the msgpack null sentinel value.
*/
LUA_API int mp_is_null (lua_State *L, int idx);

/* }================================================================== */

/*
** {==================================================================
** msgpack string buffer
** ===================================================================
*/
#define LUA_MPBUFFER_USERDATA "LUAMPBUFFER"
#if !defined(LUA_MPBUFFER_INITSIZE)
  #define LUA_MPBUFFER_INITSIZE 32
#endif

#if !defined(MAX_SIZET)  /* llimits.h */
  #define MAX_SIZET ((size_t)(~(size_t)0))  /* maximum value for size_t */
#endif

/* maximum size visible for Lua (must be representable in a lua_Integer) */
#if !defined(MAX_SIZE)
  #if LUA_VERSION_NUM == 503 || LUA_VERSION_NUM == 504
    #define MAX_SIZE (sizeof(size_t) < sizeof(lua_Integer) ? MAX_SIZET : (size_t)(LUA_MAXINTEGER))
  #elif LUA_VERSION_NUM == 501 || LUA_VERSION_NUM == 502
    #define MAX_SIZE MAX_SIZET
  #else
    #error unsupported Lua version
  #endif
#endif

static LUACMSGPACK_INLINE void *lua_mpbuffer_realloc (lua_State *L, void *target, size_t osize, size_t nsize) {
  void *ud;
  lua_Alloc local_realloc = lua_getallocf(L, &ud);
  return local_realloc(ud, target, osize, nsize);
}

/* Returns a pointer to a free area with at least 'sz' bytes in buffer 'B'. */
static LUACMSGPACK_INLINE char *lua_mpbuffer_prepare (lua_State *L, lua_mpbuffer *B, size_t sz) {
  if ((B->size - B->n) < sz) {  /* reallocate buffer to accommodate 'len' bytes */
    size_t newsize = B->size * 2;  /* double buffer size */
    if (MAX_SIZET - sz < B->n) {  /* overflow? */
      luaL_error(L, "buffer too large");
      return NULL;
    }
    if (newsize < B->n + sz)  /* double is not big enough? */
      newsize = B->n + sz;

    B->b = (char *)lua_mpbuffer_realloc(L, B->b, B->size, newsize);
    B->size = newsize;
  }
  return B->b + B->n;
}

static LUACMSGPACK_INLINE char *lua_mpbuffer_initsize (lua_State *L, lua_mpbuffer *B, size_t sz) {
  B->L = L;
  B->b = NULL;
  B->n = B->size = 0;
  return lua_mpbuffer_prepare(L, B, sz);
}

static LUACMSGPACK_INLINE void lua_mpbuffer_init (lua_State *L, lua_mpbuffer *B) {
  lua_mpbuffer_initsize(L, B, LUA_MPBUFFER_INITSIZE);
}

static LUACMSGPACK_INLINE void lua_mpbuffer_free (lua_State *L, lua_mpbuffer *B) {
  if (B->b != NULL) {
    lua_mpbuffer_realloc(L, B->b, B->size, 0);  /* realloc to 0 = free */
    B->b = NULL;
    B->n = B->size = 0;
  }
  B->L = NULL; /* Invalidate the cache */
}

/*
** Interface function required by msgpack-c, forced into using a cached
** lua_State pointer.
*/
static LUACMSGPACK_INLINE int lua_mpbuffer_append (void *data, const char *s, size_t len) {
  lua_mpbuffer *B = (lua_mpbuffer *)data;
  memcpy(lua_mpbuffer_prepare(B->L, B, len), s, len);  /* copy content */
  B->n += len;
  return 0;  /* LUA_OK */
}

static int lua_mpbuffer_gc (lua_State *L) {
  lua_mpbuffer *B = ((lua_mpbuffer *)luaL_checkudata(L, 1, LUA_MPBUFFER_USERDATA));
  if (B != NULL)
    lua_mpbuffer_free(L, B);
  return 0;
}

static const luaL_Reg mpbuffer_metafuncs[] = {
  { "__gc", lua_mpbuffer_gc },
#if LUA_VERSION_NUM >= 504
  { "__close", lua_mpbuffer_gc },
#endif
  { NULL, NULL }
};

/* }================================================================== */

/*
** {==================================================================
** msgpack packers
** ===================================================================
*/

/*
** Return the extension type, if one exists, associated to the object value at
** the specified stack index.
*/
static lua_Integer mp_ext_type (lua_State *L, int idx);

/*
** Attempt to pack the data at the specified stack index, using the provided
** extension-type identifier (ext_id). Returning non-zero on success; zero on
** failure (e.g., extension type not registered).
**
** @TODO: Missing level. With a poorly defined extension encoder, cycles can
**  exist and the encoder level isn't propagated.
*/
static int mp_encode_ext_lua_type (lua_State *L, lua_msgpack *ud, int idx, int8_t ext_id);

/*
** Return true if the table at the specified stack index can be encoded as an
** array, i.e., a table whose keys are (1) integers; (2) begin at one; (3)
** strictly positive; and (4) form a contiguous sequence.
**
** However, with the flag "MP_ARRAY_WITH_HOLES" set, condition (4) is alleviated
** and msgpack can encode "null" in the nil array indices.
*/
static int mp_table_is_an_array (lua_State *L, int idx, lua_Integer flags, size_t *array_length);

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
static void mp_encode_lua_table_as_array (lua_State *L, lua_msgpack *ud, int idx, int level, size_t array_length);

/*
** Encode the table at the specified stack index as a <key, value> array.
*/
static void mp_encode_lua_table_as_map (lua_State *L, lua_msgpack *ud, int idx, int level);

#define lua_msgpack_op(NAME, PACKER)                          \
  static LUACMSGPACK_INLINE void(NAME)(lua_State * L, lua_msgpack * ud) { \
    ((void)(L));                                              \
    PACKER(&(ud->u.packed.packer));                           \
  }

#define lua_msgpack_number_func(NAME, PACKER, TYPE)                  \
  static LUACMSGPACK_INLINE void(NAME)(lua_State * L, lua_msgpack * ud, int i) { \
    PACKER(&(ud->u.packed.packer), (TYPE)lua_tonumber(L, i));        \
  }

#define lua_msgpack_int_func(NAME, PACKER, TYPE)                     \
  static LUACMSGPACK_INLINE void(NAME)(lua_State * L, lua_msgpack * ud, int i) { \
    PACKER(&(ud->u.packed.packer), (TYPE)lua_tointeger(L, i));       \
  }

#define lua_msgpack_str_func(NAME, LEN, BODY)                        \
  static LUACMSGPACK_INLINE void(NAME)(lua_State * L, lua_msgpack * ud, int i) { \
    size_t len = 0;                                                  \
    const char *s = lua_tolstring(L, i, &len);                       \
    LEN(&(ud->u.packed.packer), len);                                \
    BODY(&(ud->u.packed.packer), s, len);                            \
  }

lua_msgpack_int_func(lua_pack_char, msgpack_pack_char, char)

lua_msgpack_int_func(lua_pack_signed_char, msgpack_pack_signed_char, signed char)
lua_msgpack_int_func(lua_pack_short, msgpack_pack_short, short)
lua_msgpack_int_func(lua_pack_int, msgpack_pack_int, int)
lua_msgpack_int_func(lua_pack_long, msgpack_pack_long, long)
lua_msgpack_int_func(lua_pack_long_long, msgpack_pack_long_long, long long)
lua_msgpack_int_func(lua_pack_unsigned_char, msgpack_pack_unsigned_char, unsigned char)
lua_msgpack_int_func(lua_pack_unsigned_short, msgpack_pack_unsigned_short, unsigned short)
lua_msgpack_int_func(lua_pack_unsigned_int, msgpack_pack_unsigned_int, unsigned int)
lua_msgpack_int_func(lua_pack_unsigned_long, msgpack_pack_unsigned_long, unsigned long)
lua_msgpack_int_func(lua_pack_unsigned_long_long, msgpack_pack_unsigned_long_long, unsigned long long)

lua_msgpack_int_func(lua_pack_uint8, msgpack_pack_uint8, uint8_t)
lua_msgpack_int_func(lua_pack_uint16, msgpack_pack_uint16, uint16_t)
lua_msgpack_int_func(lua_pack_uint32, msgpack_pack_uint32, uint32_t)
lua_msgpack_int_func(lua_pack_uint64, msgpack_pack_uint64, uint64_t)
lua_msgpack_int_func(lua_pack_int8, msgpack_pack_int8, int8_t)
lua_msgpack_int_func(lua_pack_int16, msgpack_pack_int16, int16_t)
lua_msgpack_int_func(lua_pack_int32, msgpack_pack_int32, int32_t)
lua_msgpack_int_func(lua_pack_int64, msgpack_pack_int64, int64_t)

lua_msgpack_int_func(lua_pack_fix_uint8, msgpack_pack_fix_uint8, uint8_t)
lua_msgpack_int_func(lua_pack_fix_uint16, msgpack_pack_fix_uint16, uint16_t)
lua_msgpack_int_func(lua_pack_fix_uint32, msgpack_pack_fix_uint32, uint32_t)
lua_msgpack_int_func(lua_pack_fix_uint64, msgpack_pack_fix_uint64, uint64_t)
lua_msgpack_int_func(lua_pack_fix_int8, msgpack_pack_fix_int8, int8_t)
lua_msgpack_int_func(lua_pack_fix_int16, msgpack_pack_fix_int16, int16_t)
lua_msgpack_int_func(lua_pack_fix_int32, msgpack_pack_fix_int32, int32_t)
lua_msgpack_int_func(lua_pack_fix_int64, msgpack_pack_fix_int64, int64_t)

lua_msgpack_int_func(lua_pack_signed_int16, msgpack_pack_signed_int16, int16_t)
lua_msgpack_int_func(lua_pack_signed_int32, msgpack_pack_signed_int32, int32_t)
lua_msgpack_int_func(lua_pack_signed_int64, msgpack_pack_signed_int64, int64_t)

lua_msgpack_number_func(lua_pack_float, msgpack_pack_float, float)
lua_msgpack_number_func(lua_pack_double, msgpack_pack_double, double)

lua_msgpack_op(lua_pack_nil, msgpack_pack_nil)
lua_msgpack_op(lua_pack_true, msgpack_pack_true)
lua_msgpack_op(lua_pack_false, msgpack_pack_false)

lua_msgpack_str_func(lua_pack_string, msgpack_pack_str, msgpack_pack_str_body)
lua_msgpack_str_func(lua_pack_v4, msgpack_pack_v4raw, msgpack_pack_v4raw_body)
lua_msgpack_str_func(lua_pack_bin, msgpack_pack_bin, msgpack_pack_bin_body)

static LUACMSGPACK_INLINE void lua_pack_array (lua_State *L, lua_msgpack *ud, int idx, int level) {
  ((void)level);
  mp_encode_lua_table_as_array(L, ud, idx, level,
#if LUA_VERSION_NUM < 502
  (size_t)lua_objlen(L, -1)
#else
  (size_t)lua_rawlen(L, -1)
#endif
  );
}

static LUACMSGPACK_INLINE void lua_pack_map (lua_State *L, lua_msgpack *ud, int idx, int level) {
  mp_encode_lua_table_as_map(L, ud, idx, level);
}

static LUACMSGPACK_INLINE void lua_pack_boolean (lua_State *L, lua_msgpack *ud, int idx) {
  if (lua_toboolean(L, idx))
    msgpack_pack_true(&(ud->u.packed.packer));
  else
    msgpack_pack_false(&(ud->u.packed.packer));
}

static LUACMSGPACK_INLINE void lua_pack_integer (lua_State *L, lua_msgpack *ud, int idx) {
  msgpack_packer *pk = &(ud->u.packed.packer);
#if LUA_VERSION_NUM >= 503
  #if defined(LUACMSGPACK_BIT32)
  if (ud->flags & MP_UNSIGNED_INTEGERS)
    msgpack_pack_int32(pk, (int32_t)lua_tointeger(L, idx));
  else
    msgpack_pack_signed_int32(pk, (int32_t)lua_tointeger(L, idx));
  #else
  if (ud->flags & MP_UNSIGNED_INTEGERS)
    msgpack_pack_int64(pk, (int64_t)lua_tointeger(L, idx));
  else
    msgpack_pack_signed_int64(pk, (int64_t)lua_tointeger(L, idx));
  #endif
#else
  #if defined(LUACMSGPACK_BIT32)
  if (ud->flags & MP_UNSIGNED_INTEGERS)
    msgpack_pack_int32(pk, (int32_t)lua_tonumber(L, idx));
  else
    msgpack_pack_signed_int32(pk, (int32_t)lua_tonumber(L, idx));
  #else
  if (ud->flags & MP_UNSIGNED_INTEGERS)
    msgpack_pack_int64(pk, (int64_t)lua_tonumber(L, idx));
  else
    msgpack_pack_signed_int64(pk, (int64_t)lua_tonumber(L, idx));
  #endif
#endif
}

static LUACMSGPACK_INLINE void lua_pack_number (lua_State *L, lua_msgpack *ud, int idx) {
#if LUA_VERSION_NUM >= 503
  if (lua_isinteger(L, idx))
    lua_pack_integer(L, ud, idx);
  else {
    if (ud->flags & MP_NUMBER_AS_FLOAT)
      msgpack_pack_float(&(ud->u.packed.packer), (float)lua_tonumber(L, idx));
    else
      msgpack_pack_double(&(ud->u.packed.packer), (double)lua_tonumber(L, idx));
  }
#else /* LUA_VERSION_NUM < 503 */
  /*
  ** Earlier versions of Lua have no explicit integer types, therefore detect if
  ** the floating type can be faithfully casted to an int.
  */
  lua_Number n = lua_tonumber(L, idx);
#if defined(LUACMSGPACK_BIT32)
  if (IS_INT_EQUIVALENT(n) || (ud->flags & MP_NUMBER_AS_INTEGER)) {
#else
  if (IS_INT64_EQUIVALENT(n) || (ud->flags & MP_NUMBER_AS_INTEGER)) {
#endif
    lua_pack_integer(L, ud, idx);
  }
  else {
    if (ud->flags & MP_NUMBER_AS_FLOAT)
      msgpack_pack_float(&(ud->u.packed.packer), (float)n);
    else
      msgpack_pack_double(&(ud->u.packed.packer), (double)n);
  }
#endif
}

static LUACMSGPACK_INLINE void lua_pack_table (lua_State *L, lua_msgpack *ud, int idx, int level) {
  size_t array_length = 0;
  if ((ud->flags & MP_ARRAY_AS_MAP) == MP_ARRAY_AS_MAP)
    mp_encode_lua_table_as_map(L, ud, idx, level);
  else if (mp_table_is_an_array(L, idx, ud->flags, &array_length))
    mp_encode_lua_table_as_array(L, ud, idx, level, array_length);
  else
    mp_encode_lua_table_as_map(L, ud, idx, level);
}

static LUACMSGPACK_INLINE void lua_pack_extended_table (lua_State *L, lua_msgpack *ud, int idx, int level) {
  lua_Integer type = 0;
  if ((type = mp_ext_type(L, idx)) != EXT_INVALID) {
    if (!mp_encode_ext_lua_type(L, ud, idx, (int8_t)type)) {
      luaL_error(L, "msgpack extension type: not registered!");
      return;
    }
  }
  else if (mp_encode_ext_lua_type(L, ud, idx, (int8_t)LUACMSGPACK_LUATYPE_EXT(LUA_TTABLE))) {
    /* do nothing; table has been packed with a custom extension */
  }
  else {
    lua_pack_table(L, ud, idx, level);
  }
}

static LUACMSGPACK_INLINE void lua_pack_parse_string (lua_State *L, lua_msgpack *ud, int idx) {
  msgpack_packer *pk = &(ud->u.packed.packer);

  /*
  ** Fallback to empty string if the value at the specified index is not a
  ** string or its object can be converted into a string.
  */
  size_t len = 0;
  const char *s = NULL;
  if ((s = lua_tolstring(L, idx, &len)) == NULL) {
    len = 0;
    s = "";
  }

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
}

static LUACMSGPACK_INLINE void lua_pack_type_extended (lua_State *L, lua_msgpack *ud, int idx) {
  int t = lua_type(L, idx);
  lua_Integer type = 0;
  if ((type = mp_ext_type(L, idx)) != EXT_INVALID) {
    if (!mp_encode_ext_lua_type(L, ud, idx, (int8_t)type)) {
      luaL_error(L, "msgpack extension type: not registered!");
      return;
    }
  }
  else if (mp_encode_ext_lua_type(L, ud, idx, (int8_t)LUACMSGPACK_LUATYPE_EXT(t))) {
    /* do nothing */
  }
  else {
    luaL_error(L, "type <%s> cannot be msgpack'd", lua_typename(L, t));
  }
}

static LUACMSGPACK_INLINE void lua_pack_any (lua_State *L, lua_msgpack *ud, int idx, int level) {
  int t = lua_type(L, idx);
#if defined(LUACMSGPACK_ERROR_NESTING)
  if (t == LUA_TTABLE && level >= LUACMSGPACK_MAX_NESTING) {
    luaL_error(L, "maximum table nesting depth exceeded");
    return;
  }
#else
  if (t == LUA_TTABLE && level == LUACMSGPACK_MAX_NESTING)
    t = LUA_TNIL;
#endif

  switch (t) {
    case LUA_TNIL: msgpack_pack_nil(&(ud->u.packed.packer)); break;
    case LUA_TBOOLEAN: lua_pack_boolean(L, ud, idx); break;
    case LUA_TNUMBER: lua_pack_number(L, ud, idx); break;
    case LUA_TSTRING: lua_pack_parse_string(L, ud, idx); break;
    case LUA_TTABLE: lua_pack_extended_table(L, ud, idx, level); break;
    case LUA_TUSERDATA:
    case LUA_TTHREAD:
    case LUA_TFUNCTION:
      if (t == LUA_TFUNCTION && mp_is_null(L, idx)) {
        msgpack_pack_nil(&(ud->u.packed.packer));
        break;
      }
      lua_pack_type_extended(L, ud, idx);
      break;
    case LUA_TLIGHTUSERDATA: {
      /*
      ** TODO: Improve how light userdata is managed. Ideally, there will be
      **       API function lua_msgpack_type_extension( ..., lua_CFunction,
      **       lua_CFunction) that handles the serialization of C pointers.
      */
      if (!mp_encode_ext_lua_type(L, ud, idx, (int8_t)LUACMSGPACK_LUATYPE_EXT(t))) {
        msgpack_packer *pk = &(ud->u.packed.packer);
#if defined(LUACMSGPACK_BIT32)
        msgpack_pack_uint32(pk, (uint32_t)((uintptr_t)lua_touserdata(L, idx)));
#else
        msgpack_pack_uint64(pk, (uint64_t)((uintptr_t)lua_touserdata(L, idx)));
#endif
      }
      break;
    }
    default:
      luaL_error(L, "type <%s> cannot be msgpack'd", lua_typename(L, t));
      break;
  }
}

/* }================================================================== */

#endif
