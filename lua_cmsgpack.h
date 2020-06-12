/*
** https://github.com/msgpack/msgpack/blob/master/spec.md
** https://github.com/msgpack/msgpack-c
**
** See LICENSE.
*/
#ifndef lua_cmsgpack_h
#define lua_cmsgpack_h

#include <string.h>

#include <lua.h>
#include <lauxlib.h>

#include "msgpack/pack.h"
#include "msgpack/unpack.h"

/* @TODO: Support 16-bit Lua */
#if !defined(LUACMSGPACK_BIT32) && UINTPTR_MAX == UINT_MAX
  #define LUACMSGPACK_BIT32
#endif

#ifndef LUACMSGPACK_MAX_NESTING
  #define LUACMSGPACK_MAX_NESTING 16  /* Max tables nesting. */
#endif

/* Unnecessary & unsafe macro to disable checkstack */
#if defined(LUACMSGPACK_UNSAFE)
  #define mp_checkstack(L, sz) ((void)0)
#else
  #define mp_checkstack(L, sz) luaL_checkstack((L), (sz), "too many (nested) values in encoded msgpack")
#endif

#if LUA_VERSION_NUM >= 503
  #define mp_isinteger(L, idx) lua_isinteger((L), (idx))
#else
static inline int mp_isinteger(lua_State *L, int idx) {
  if (LUA_TNUMBER == lua_type(L, idx)) {
    lua_Number n = lua_tonumber(L, idx);
    return IS_INT_TYPE_EQUIVALENT(n, lua_Integer);
  }
  return 0;
}
#endif

/*
** A luaL_Buffer with its own memory-management.
*/
typedef struct lua_mpbuffer {
  char *b;  /* buffer address */
  size_t size;  /* buffer size */
  size_t n;  /* number of characters in buffer */
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
#define LUACMSGPACK_USERDATA "LUACMSGPACK"

#define MP_OPEN                0x01  /* Userdata data resources are alive. */
#define MP_PACKING             0x02  /* Deallocate packing structures */
#define MP_UNPACKING           0x04  /* Deallocate unpacking structures */

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

#define MP_MODE (MP_PACKING | MP_UNPACKING)
#define MP_MASK_RUNTIME (MP_OPEN | MP_MODE)  /* flags that can't be setoption'd */
#define MP_MASK_ARRAY (MP_ARRAY_AS_MAP | MP_ARRAY_WITH_HOLES | MP_ARRAY_WITHOUT_HOLES)
#define MP_MASK_STRING (MP_STRING_COMPAT | MP_STRING_BINARY)
#define MP_MASK_NUMBER (MP_NUMBER_AS_INTEGER | MP_NUMBER_AS_FLOAT | MP_NUMBER_AS_DOUBLE)

#if defined(LUACMSGPACK_BIT32)
  #define MP_DEFAULT (MP_EMPTY_AS_ARRAY | MP_ARRAY_WITHOUT_HOLES | MP_NUMBER_AS_FLOAT | MP_STRING_COMPAT)
#else
  #define MP_DEFAULT (MP_EMPTY_AS_ARRAY | MP_ARRAY_WITHOUT_HOLES | MP_NUMBER_AS_DOUBLE)
#endif

/* Userdata structure for managing/cleaning message packing */
typedef struct lua_msgpack {
  lua_Integer flags;
  union {
    struct {
      msgpack_packer packer;
      lua_mpbuffer buffer;
    } packed;
    msgpack_unpacked unpacked;
  } u;
} lua_msgpack;


/* Creates a new lua_msgpack userdata and pushes it onto the stack. */
LUA_API lua_msgpack *lua_msgpack_create (lua_State *L, lua_Integer flags);

/*
** Destroy a msgpack userdata and free any additional resources/memory allocated
** by it. If 'ud' is null, this function attempts to check for the userdata at
** the given index.
*/
LUA_API int lua_msgpack_destroy (lua_State *L, int idx, lua_msgpack *ud);

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
LUA_API void lua_msgpack_encode (lua_State *L, lua_msgpack *ud, int level);

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
                    size_t len, size_t *offset, int limit, const char **error);

/* }================================================================== */

/*
** {==================================================================
** msgpack string buffer
** ===================================================================
*/

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

static inline void *lua_mpbuffer_realloc (lua_State *L, void *target,
                                                   size_t osize, size_t nsize) {
  void *ud;
  lua_Alloc local_realloc = lua_getallocf(L, &ud);
  return local_realloc(ud, target, osize, nsize);
}

/* Returns a pointer to a free area with at least 'sz' bytes in buffer 'B'. */
static inline char *lua_mpbuffer_prepare (lua_mpbuffer *B, size_t sz) {
  if ((B->size - B->n) < sz) {  /* reallocate buffer to accommodate 'len' bytes */
    size_t newsize = B->size * 2;  /* double buffer size */
    if (MAX_SIZET - sz < B->n) { /* overflow? */
      luaL_error(B->L, "buffer too large");
      return NULL;
    }
    if (newsize < B->n + sz)  /* double is not big enough? */
      newsize = B->n + sz;

    B->b = (char *)lua_mpbuffer_realloc(B->L, B->b, B->size, newsize);
    B->size = newsize;
  }
  return B->b + B->n;
}

static inline void lua_mpbuffer_init (lua_State *L, lua_mpbuffer *B) {
  B->L = L;
  B->b = NULL;
  B->n = B->size = 0;
}

static inline char *lua_mpbuffer_initsize (lua_State *L, lua_mpbuffer *B, size_t sz) {
  lua_mpbuffer_init(L, B);
  return lua_mpbuffer_prepare(B, sz);
}

static inline void lua_mpbuffer_free (lua_mpbuffer *B) {
  if (B->b != NULL) {
    lua_mpbuffer_realloc(B->L, B->b, B->size, 0);  /* realloc to 0 = free */
    B->b = NULL;
    B->n = B->size = 0;
  }
  B->L = NULL;
}

static inline int lua_mpbuffer_append (void *data, const char *s, size_t len) {
  lua_mpbuffer *B = (lua_mpbuffer *)data;
  memcpy(lua_mpbuffer_prepare(B, len), s, len);  /* copy content */
  B->n += len;
  return 0;  /* LUA_OK */
}

/* }================================================================== */

#endif
