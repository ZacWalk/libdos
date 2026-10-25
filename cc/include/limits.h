#ifndef __LIBDOS_LIMITS_H__
#define __LIBDOS_LIMITS_H__

#define CHAR_BIT 8
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX

#define SHRT_MIN (-32767 - 1)
#define SHRT_MAX 32767
#define USHRT_MAX 65535u

#define INT_MIN (-32767 - 1)
#define INT_MAX 32767
#define UINT_MAX 65535u

#define LONG_MIN (-2147483647L - 1)
#define LONG_MAX 2147483647L
#define ULONG_MAX 4294967295uL

#endif
