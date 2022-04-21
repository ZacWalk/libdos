#ifndef __OVRWIN_TIME_H__
#define __OVRWIN_TIME_H__

typedef long time_t;
typedef long clock_t;

#define CLOCKS_PER_SEC 18

time_t time(time_t *into);
clock_t clock(void);

#endif
