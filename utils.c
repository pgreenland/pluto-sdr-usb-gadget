/* Use non portable functions */
#define _GNU_SOURCE

/* Public header file */
#include "utils.h"

/* Standard libraries */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Constants */
#define US_PER_SEC (1000000)
#define NS_PER_US (1000)

/* Private functions */
static uint64_t GetMonotonicMicros(void);

/* Public functions */
void UTILS_ResetTimeStats(UTILS_TimeStats_t *ctx)
{
    /* Zero structure */
    memset(ctx, 0x00, sizeof(*ctx));

    /* Pre-set min */
    ctx->min = UINT64_MAX;
}

void UTILS_StartTimeStats(UTILS_TimeStats_t *ctx)
{
    /* Set last timestamp and flag initialized */
    ctx->last_time = GetMonotonicMicros();
    ctx->initialized = true;
}

void UTILS_UpdateTimeStats(UTILS_TimeStats_t *ctx)
{
    uint64_t curr_time = GetMonotonicMicros();

    if (ctx->initialized)
    {
        /* Calculate time difference */
        uint64_t diff = curr_time - ctx->last_time;

        /* Update stats */
        ctx->total += diff;
        ctx->count++;
        if (diff < ctx->min) ctx->min = diff;
        if (diff > ctx->max) ctx->max = diff;
    }

    /* Set last timestamp and flag initialized */
    ctx->last_time = curr_time;
    ctx->initialized = true;
}

uint64_t UTILS_CalcAverageTimeStats(UTILS_TimeStats_t *ctx)
{
    return ctx->total / ctx->count;
}

int UTILS_SetThreadRealtimePriority(void)
{
    int rc;

	int max_prio = sched_get_priority_max(SCHED_RR);
	if (max_prio >= 0) {
		struct sched_param sch;
		sch.sched_priority = max_prio;
		rc = pthread_setschedparam(pthread_self(), SCHED_RR, &sch);
		if (rc) {
			errno = rc;
			perror("Failed to set priority");
		}
	} else {
		perror("Failed to query thread schedular priorities");
	}

    return rc;
}

int UTILS_SetThreadAffinity(int cpu_id)
{
    int rc;

    /* Clear the cpuset */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    /* Set the CPU affinity for the thread */
    CPU_SET(cpu_id, &cpuset);

    /* Set affinity */
    rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc) {
        errno = rc;
        perror("Failed to set affinity");
    }

    return rc;
}

/* Private functions */
static uint64_t GetMonotonicMicros(void)
{
    struct timespec tmp_time;

    if (0 != clock_gettime(CLOCK_MONOTONIC_RAW, &tmp_time))
    {
        /* Failed to query clock */
        return 0;
    }

    /* Convert seconds + nanoseconds to us */
    return (((uint64_t)tmp_time.tv_sec * US_PER_SEC) + ((uint64_t)tmp_time.tv_nsec / NS_PER_US));
}
