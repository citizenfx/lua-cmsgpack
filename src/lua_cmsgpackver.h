/* Lua version compatibility and other macros. */
#ifndef lua_cmsgpackconf_h
#define lua_cmsgpackconf_h

#if defined(LUA_COMPILED_AS_HPP)
#include <limits>
extern "C++" {
#endif

#include <lua.h>
#include <luaconf.h>
#include <lauxlib.h>

#if defined(LUA_COMPILED_AS_HPP)
}
#endif

#ifdef _MSC_VER
  #define LUACMSGPACK_INLINE __forceinline
#elif defined(__has_attribute)
  #if __has_attribute(__always_inline__)
    #define LUACMSGPACK_INLINE inline __attribute__((__always_inline__))
  #else
    #define LUACMSGPACK_INLINE inline
  #endif
#else
  #define LUACMSGPACK_INLINE inline
#endif

/*
** When compiling for C++ bundles, include macros for common warnings, e.g.,
** -Wold-style-cast.
*/
#if defined(__cplusplus)
  #define mp_cast(t, exp) static_cast<t>(exp)
  #define mp_pcast(t, exp) reinterpret_cast<t>(exp)
#else
  #define mp_cast(t, exp) ((t)(exp))
  #define mp_pcast(t, exp) ((t)(exp))
#endif

/* Macro for geti/seti. Parameters changed from int to lua_Integer in Lua 53 */
#if LUA_VERSION_NUM >= 503
  #define mp_ti(v) v
#else
  #define mp_ti(v) mp_cast(int, v)
#endif

/* Unnecessary & unsafe macro to disable checkstack */
#if defined(LUACMSGPACK_UNSAFE)
  #define mp_checkstack(L, sz) ((void)0)
#else
  #define mp_checkstack(L, sz) luaL_checkstack((L), (sz), "too many (nested) values in encoded msgpack")
#endif

/* lua_absindex introduced in Lua 52 */
#if LUA_VERSION_NUM < 502
  #define lua_absindex(L, i) ((i) > 0 || (i) <= LUA_REGISTRYINDEX ? (i) : lua_gettop(L) + (i) + 1)
#endif

/* Lua 54 changed the definition of lua_newuserdata */
#if LUA_VERSION_NUM >= 504
  #define mp_newuserdata(L, s) lua_newuserdatauv((L), (s), 1)
#elif LUA_VERSION_NUM >= 501 && LUA_VERSION_NUM <= 503
  #define mp_newuserdata(L, s) lua_newuserdata((L), (s))
#else
  #error unsupported Lua version
#endif

/*
** {==================================================================
** std::limits
** ===================================================================
*/

/* Check if float or double can be an integer without loss of precision */
#define IS_INT_TYPE_EQUIVALENT(x, T) (!isinf(x) && mp_cast(lua_Number, mp_cast(t, x)) == (x))
#define IS_INT64_EQUIVALENT(x) IS_INT_TYPE_EQUIVALENT(x, int64_t)
#define IS_INT_EQUIVALENT(x) IS_INT_TYPE_EQUIVALENT(x, int)

/* maximum value for size_t */
#if defined(__cplusplus)
  #define MP_MAX_SIZET std::numeric_limits<size_t>::max()
#else
  #define MP_MAX_SIZET ((size_t)(~(size_t)0))
#endif

/* maximum size visible for Lua (must be representable in a lua_Integer) */
#if LUA_VERSION_NUM == 503 || LUA_VERSION_NUM == 504
  #define MP_MAX_LUAINDEX (sizeof(size_t) < sizeof(lua_Integer) ? MP_MAX_SIZET : mp_cast(size_t, LUA_MAXINTEGER))
#elif LUA_VERSION_NUM == 501 || LUA_VERSION_NUM == 502
  #define MP_MAX_LUAINDEX MP_MAX_SIZET
#else
  #error unsupported Lua version
#endif

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

/* }================================================================== */

#endif
