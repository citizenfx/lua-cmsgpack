/*
 * packers['unsigned'] == msgpack_pack_real_int64
 * packers['signed'] == msgpack_pack_signed_int64
 *
 * MessagePack packing routine template
 *
 * Copyright (C) 2008-2010 FURUHASHI Sadayuki
 *
 *    Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *    http://www.boost.org/LICENSE_1_0.txt)
 */
#include <msgpack/pack.h>

#if MSGPACK_ENDIAN_LITTLE_BYTE
  #define TAKE8_8(d)  ((uint8_t*)&d)[0]
  #define TAKE8_16(d) ((uint8_t*)&d)[0]
  #define TAKE8_32(d) ((uint8_t*)&d)[0]
  #define TAKE8_64(d) ((uint8_t*)&d)[0]
#elif MSGPACK_ENDIAN_BIG_BYTE
  #define TAKE8_8(d)  ((uint8_t*)&d)[0]
  #define TAKE8_16(d) ((uint8_t*)&d)[1]
  #define TAKE8_32(d) ((uint8_t*)&d)[3]
  #define TAKE8_64(d) ((uint8_t*)&d)[7]
#else
  #error msgpack-c supports only big endian and little endian
#endif

#if defined(_MSC_VER)
#   pragma warning(push)
#   pragma warning(disable : 4204)   /* nonstandard extension used: non-constant aggregate initializer */
#endif

#define msgpack_pack_append_buffer(user, buf, len) \
    return (*(user)->callback)((user)->data, (const char*)buf, len)

#define msgpack_pack_signed_real_int16(x, d) \
do { \
    if(d < -(1<<5)) { \
        if(d < -(1<<7)) { \
            /* signed 16 */ \
            unsigned char buf[3]; \
            buf[0] = 0xd1; _msgpack_store16(&buf[1], (int16_t)d); \
            msgpack_pack_append_buffer(x, buf, 3); \
        } else { \
            /* signed 8 */ \
            unsigned char buf[2] = {0xd0, TAKE8_32(d)}; \
            msgpack_pack_append_buffer(x, buf, 2); \
        } \
    } else if(d < (1<<7)) { \
        /* fixnum */ \
        msgpack_pack_append_buffer(x, &TAKE8_32(d), 1);  \
    } else { \
        /* signed 16 */ \
        unsigned char buf[3]; \
        buf[0] = 0xd1; _msgpack_store16(&buf[1], (int16_t)d); \
        msgpack_pack_append_buffer(x, buf, 3); \
    } \
} while(0)

#define msgpack_pack_signed_real_int32(x, d) \
do { \
    if(d < -(1<<5)) { \
        if(d < -(1<<15)) { \
            /* signed 32 */ \
            unsigned char buf[5]; \
            buf[0] = 0xd2; _msgpack_store32(&buf[1], (int32_t)d); \
            msgpack_pack_append_buffer(x, buf, 5); \
        } else if(d < -(1<<7)) { \
            /* signed 16 */ \
            unsigned char buf[3]; \
            buf[0] = 0xd1; _msgpack_store16(&buf[1], (int16_t)d); \
            msgpack_pack_append_buffer(x, buf, 3); \
        } else { \
            /* signed 8 */ \
            unsigned char buf[2] = {0xd0, TAKE8_32(d)}; \
            msgpack_pack_append_buffer(x, buf, 2); \
        } \
    } else if(d < (1<<7)) { \
        /* fixnum */ \
        msgpack_pack_append_buffer(x, &TAKE8_32(d), 1);  \
    } else { \
        if(d < (1LL<<15)) { \
            /* signed 16 */ \
            unsigned char buf[3]; \
            buf[0] = 0xd1; _msgpack_store16(&buf[1], (int16_t)d); \
            msgpack_pack_append_buffer(x, buf, 3); \
        } else { \
            /* signed 32 */ \
            unsigned char buf[5]; \
            buf[0] = 0xd2; _msgpack_store32(&buf[1], (int32_t)d); \
            msgpack_pack_append_buffer(x, buf, 5); \
        } \
    } \
} while(0)

#define msgpack_pack_signed_real_int64(x, d) \
do { \
    if(d < -(1LL<<5)) { \
        if(d < -(1LL<<15)) { \
            if(d < -(1LL<<31)) { \
                /* signed 64 */ \
                unsigned char buf[9]; \
                buf[0] = 0xd3; _msgpack_store64(&buf[1], d); \
                msgpack_pack_append_buffer(x, buf, 9); \
            } else { \
                /* signed 32 */ \
                unsigned char buf[5]; \
                buf[0] = 0xd2; _msgpack_store32(&buf[1], (int32_t)d); \
                msgpack_pack_append_buffer(x, buf, 5); \
            } \
        } else { \
            if(d < -(1<<7)) { \
                /* signed 16 */ \
                unsigned char buf[3]; \
                buf[0] = 0xd1; _msgpack_store16(&buf[1], (int16_t)d); \
                msgpack_pack_append_buffer(x, buf, 3); \
            } else { \
                /* signed 8 */ \
                unsigned char buf[2] = {0xd0, TAKE8_64(d)}; \
                msgpack_pack_append_buffer(x, buf, 2); \
            } \
        } \
    } else if(d < (1<<7)) { \
        /* fixnum */ \
        msgpack_pack_append_buffer(x, &TAKE8_64(d), 1);  \
    } else { \
        if(d < (1LL<<15)) { \
            /* signed 16 */ \
            unsigned char buf[3]; \
            buf[0] = 0xd1; _msgpack_store16(&buf[1], (int16_t)d); \
            msgpack_pack_append_buffer(x, buf, 3); \
        } else { \
            if(d < (1LL<<31)) { \
                /* signed 32 */ \
                unsigned char buf[5]; \
                buf[0] = 0xd2; _msgpack_store32(&buf[1], (int32_t)d); \
                msgpack_pack_append_buffer(x, buf, 5); \
            } else { \
                /* signed 64 */ \
                unsigned char buf[9]; \
                buf[0] = 0xd3; _msgpack_store64(&buf[1], d); \
                msgpack_pack_append_buffer(x, buf, 9); \
            } \
        } \
    } \
} while(0)

static inline int msgpack_pack_signed_int16(msgpack_packer *x, int32_t d) { msgpack_pack_signed_real_int16(x, d); }
static inline int msgpack_pack_signed_int32(msgpack_packer *x, int32_t d) { msgpack_pack_signed_real_int32(x, d); }
static inline int msgpack_pack_signed_int64(msgpack_packer *x, int64_t d) { msgpack_pack_signed_real_int64(x, d); }

#undef msgpack_pack_append_buffer

#undef TAKE8_8
#undef TAKE8_16
#undef TAKE8_32
#undef TAKE8_64

#undef msgpack_pack_signed_real_int16
#undef msgpack_pack_signed_real_int32
#undef msgpack_pack_signed_real_int64

#if defined(_MSC_VER)
#   pragma warning(pop)
#endif

