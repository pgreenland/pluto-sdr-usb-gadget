#ifndef __UTILS_H__
#define __UTILS_H__

/* Standard libraries */
#include <stdint.h>
#include <stdbool.h>

/* Stats */
typedef struct
{
    /* First call has been made */
    bool initialized;

    /* Last timestamp */
    uint64_t last_time;

    /* Total time and sample count */
    uint64_t total;
    uint32_t count;

    /* Min / max time */
    uint64_t min;
    uint64_t max;

} UTILS_TimeStats_t;

/* Init time stats */
void UTILS_ResetTimeStats(UTILS_TimeStats_t *ctx);

/* Start timer */
void UTILS_StartTimeStats(UTILS_TimeStats_t *ctx);

/* Update stats / last time */
void UTILS_UpdateTimeStats(UTILS_TimeStats_t *ctx);

/* Calculate average time */
uint64_t UTILS_CalcAverageTimeStats(UTILS_TimeStats_t *ctx);

/* Set thread priority to realtime */
int UTILS_SetThreadRealtimePriority(void);

/* Set CPU affinity to single CPU */
int UTILS_SetThreadAffinity(int cpu_id);

#endif
