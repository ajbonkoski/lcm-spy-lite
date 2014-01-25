#include "timeutil.h"

#include <stdlib.h>
#include <sys/time.h>

uint64_t timestamp_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t) tv.tv_sec * 1000000 + tv.tv_usec;
}
