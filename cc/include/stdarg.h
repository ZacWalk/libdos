#ifndef __OVRWIN_STDARG_H__
#define __OVRWIN_STDARG_H__

// ovrwin by Zac Walker
//
// Arguments are pushed right to left and the caller cleans up, so the extra
// ones sit above the last named parameter in the caller's frame. __args is a
// compiler builtin that gives their address.

typedef char *va_list;

#define va_start(ap, last) ((ap) = __args(last))
#define va_arg(ap, type) (*(type *)(((ap) += sizeof(type)) - sizeof(type)))
#define va_end(ap) ((void)0)

#endif
