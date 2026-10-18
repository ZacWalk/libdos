#ifndef __LIBDOS_SIGNAL_H__
#define __LIBDOS_SIGNAL_H__

#define SIGINT 2
#define SIGABRT 6
#define SIGTERM 15

#define SIG_DFL 0
#define SIG_IGN 1

typedef void (*sighandler_t)(int);

sighandler_t signal(int number, sighandler_t handler);

#endif
