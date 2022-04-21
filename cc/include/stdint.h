#ifndef __OVRWIN_STDINT_H__
#define __OVRWIN_STDINT_H__

// ovrwin by Zac Walker
//
// The fixed-width types as this compiler sees them: int is 16-bit and long is
// 32-bit, so there is no 64-bit type to name.

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef int int16_t;
typedef unsigned int uint16_t;
typedef long int32_t;
typedef unsigned long uint32_t;

typedef int intptr_t;
typedef unsigned int uintptr_t;

typedef signed char int_least8_t;
typedef unsigned char uint_least8_t;
typedef int int_least16_t;
typedef unsigned int uint_least16_t;
typedef long int_least32_t;
typedef unsigned long uint_least32_t;

// The fast types are the ones an 8086 actually likes, which is a word for
// everything narrower than a word.
typedef int int_fast8_t;
typedef unsigned int uint_fast8_t;
typedef int int_fast16_t;
typedef unsigned int uint_fast16_t;
typedef long int_fast32_t;
typedef unsigned long uint_fast32_t;

#define INT8_MIN (-128)
#define INT8_MAX 127
#define UINT8_MAX 255
#define INT16_MIN (-32767 - 1)
#define INT16_MAX 32767
#define UINT16_MAX 65535u
#define INT32_MIN (-2147483647L - 1)
#define INT32_MAX 2147483647L
#define UINT32_MAX 4294967295uL

#define INT8_C(v) v
#define UINT8_C(v) v
#define INT16_C(v) v
#define UINT16_C(v) v##u
#define INT32_C(v) v##L
#define UINT32_C(v) v##uL

#endif
