#ifndef TIMEUTIL_H
#define TIMEUTIL_H

#include <stdint.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint64_t tic_time;
}ss_timer_t;


uint64_t timestamp_now(void);

void   tmr_tic(ss_timer_t*);
uint64_t tmr_toc(ss_timer_t*);
uint64_t tmr_toc_tic(ss_timer_t*);

#ifdef __cplusplus
}
#endif

#endif  /* TIMEUTIL_H */
