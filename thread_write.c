/* Use non portable functions */
#define _GNU_SOURCE

/* Public header */
#include "thread_write.h"

/* Standard / system libraries */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* libIIO */
#include <iio.h>

/* AsyncIO library */
#include "libaio.h"

/* Local modules */
#include "usb_buff.h"
#include "epoll_loop.h"
#include "utils.h"

/* Set the following to periodically report statistics */
#ifndef GENERATE_STATS
#define GENERATE_STATS (0)
#endif

/* Set stats period */
#ifndef STATS_PERIOD_SECS
#define STATS_PERIOD_SECS (5)
#endif

/* Define number of read buffers to queue up */
#define NUM_BUFS (16)

/* Macros */
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define DEBUG_PRINT(...) if (debug) printf("Write: "__VA_ARGS__)

/* Type definitions */
typedef struct
{
	/* Thread args */
	THREAD_WRITE_Args_t *thread_args;

	/* Keep running */
	bool keep_running;

	/* IIO sample buffer */
	struct iio_buffer *iio_tx_buffer;

	/* Size of USB buffer (bytes) */
	size_t usb_buffer_size;

	/* AIO context */
	io_context_t io_ctx;

	/* AIO completion eventfd */
	int aio_eventfd;

	/* List of buffers */
	usb_buf_t* buffers[NUM_BUFS];

	#if GENERATE_STATS
	/* Stats reporting timer */
	int stats_timerfd;

	/* Overflow count */
	uint32_t overflows;

	/* Write period timer */
	UTILS_TimeStats_t write_period;

	/* Write duration timer */
	UTILS_TimeStats_t write_dur;
	#endif

} state_t;

/* Epoll event handler */
typedef int (*epoll_event_handler)(state_t *state);

/* Global variables */
extern bool debug;

/* Private functions */
static int handle_eventfd_thread(state_t *state);
static int handle_eventfd_aio(state_t *state);
#if GENERATE_STATS
static int handle_stats_timer(state_t *state);
#endif
static usb_buf_t *alloc_usb_buffer(size_t size, int usb_fd, int event_fd);

/* Public functions */
void *THREAD_WRITE_Entrypoint(void *args)
{
	THREAD_WRITE_Args_t *thread_args = (THREAD_WRITE_Args_t*)args;

	/* Enter */
	DEBUG_PRINT("Write thread enter\n");

	/* Set name, priority and CPU affinity */
	pthread_setname_np(pthread_self(), "USB_SDR_GAD_WR");
	UTILS_SetThreadRealtimePriority();
	UTILS_SetThreadAffinity(1);

	/* Reset state */
	state_t state;
	memset(&state, 0x00, sizeof(state));

	/* Store args */
	state.thread_args = thread_args;

	/* Create epoll instance */
	int epoll_fd = epoll_create1(0);
	if (epoll_fd < 0)
	{
		perror("Failed to create epoll instance");
		return NULL;
	}
	else
	{
		DEBUG_PRINT("Opened epoll :-)\n");
	}

	struct epoll_event epoll_event;

	/* Register thread quit eventfd with epoll */
	epoll_event.events = EPOLLIN;
	epoll_event.data.ptr = handle_eventfd_thread;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, thread_args->quit_event_fd, &epoll_event) < 0)
	{
		perror("Failed to register thread quit eventfd with epoll");
		return NULL;
	}
	else
	{
		DEBUG_PRINT("Registered thread quit eventfd with with epoll :-)\n");
	}

	/* Create IIO context */
	struct iio_context *iio_ctx = iio_create_local_context();
	if (!iio_ctx)
	{
		fprintf(stderr, "Failed to open iio\n");
		return NULL;
	}

	/* Retrieve TX streaming device */
	struct iio_device *iio_dev_tx = iio_context_find_device(iio_ctx, "cf-ad9361-dds-core-lpc");
	if (!iio_dev_tx)
	{
		fprintf(stderr, "Failed to open iio tx dev\n");
		return NULL;
	}

	/* Disable all channels */
	unsigned int nb_channels = iio_device_get_channels_count(iio_dev_tx);
	for (unsigned int i = 0; i < nb_channels; i++)
	{
		iio_channel_disable(iio_device_get_channel(iio_dev_tx, i));
	}

	/* Enable required channels */
	for (unsigned int i = 0; i < 32; i++)
	{
		/* Enable channel if required */
		if (thread_args->iio_channels & (1U << i))
		{
			/* Retrieve channel */
			struct iio_channel *channel = iio_device_get_channel(iio_dev_tx, i);
			if (!channel)
			{
				fprintf(stderr, "Failed to find iio rx chan %u\n", i);
				return NULL;
			}

			/* Enable channels */
			iio_channel_enable(channel);
		}
	}

	/* Create non-cyclic buffer */
	state.iio_tx_buffer = iio_device_create_buffer(iio_dev_tx, thread_args->iio_buffer_size, false);
	if (!state.iio_tx_buffer)
	{
		fprintf(stderr, "Failed to create tx buffer for %zu samples\n", thread_args->iio_buffer_size);
		return NULL;
	}

	/* Retrieve number of bytes between two samples of the same channel (aka size of one sample of all enabled channels) */
	size_t sample_size = iio_buffer_step(state.iio_tx_buffer);

	/* Calculate USB buffer size */
	state.usb_buffer_size = sample_size * thread_args->iio_buffer_size;

	/* Summarize info */
	DEBUG_PRINT("TX sample count: %zu, iio sample size: %zu, usb buffer size: %zu\n",
				thread_args->iio_buffer_size,
				sample_size,
				state.usb_buffer_size);

	/* Reset AIO context */
	memset(&state.io_ctx, 0x00, sizeof(state.io_ctx));

	/* Setup AIO context */
	if (io_setup(ARRAY_SIZE(state.buffers), &state.io_ctx) < 0)
	{
		perror("Failed to setup AIO");
		return NULL;
	}
	else
	{
		DEBUG_PRINT("Setup AIO :-)\n");
	}

	/* Prepare eventfd to notify of completed AIO transfers */
	state.aio_eventfd = eventfd(0, 0);
	if (state.aio_eventfd < 0)
	{
		perror("Failed to open eventfd");
		return NULL;
	}
	else
	{
		DEBUG_PRINT("Opened eventfd :-)\n");
	}

	/* Register aio eventfd with epoll */
	epoll_event.events = EPOLLIN;
	epoll_event.data.ptr = handle_eventfd_aio;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, state.aio_eventfd, &epoll_event) < 0)
	{
		/* Failed to register aio completion eventfd with epoll */
		perror("Failed to register aio completion eventfd with epoll");
		return NULL;
	}
	else
	{
		DEBUG_PRINT("Registered aio completion eventfd with with epoll :-)\n");
	}

	/* Allocate buffers */
	struct iocb* bufs[ARRAY_SIZE(state.buffers)];
	for (unsigned int i = 0; i < ARRAY_SIZE(state.buffers); i++)
	{
		/* Allocate buffer */
		usb_buf_t *buf = alloc_usb_buffer(state.usb_buffer_size, thread_args->input_fd, state.aio_eventfd);
		if (!buf)
		{
			return NULL;
		}

		/* Store buffer */
		state.buffers[i] = buf;

		/* Mark buffer as in use */
		buf->in_use = true;

		/* Add buffer to transfer list */
		bufs[i] = &buf->iocb;
	}

	#if GENERATE_STATS
	/* Create stats reporting timer */
	state.stats_timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
	if (state.stats_timerfd < 0)
	{
		perror("Failed to open timerfd");
		return NULL;
	}
	else
	{
		DEBUG_PRINT("Opened timerfd :-)\n");
	}
	struct itimerspec timer_period =
	{
		.it_value = { .tv_sec = STATS_PERIOD_SECS, .tv_nsec = 0 },
		.it_interval = { .tv_sec = STATS_PERIOD_SECS, .tv_nsec = 0 }
	};
	if (timerfd_settime(state.stats_timerfd, 0, &timer_period, NULL) < 0)
	{
		perror("Failed to set timerfd");
		return NULL;
	}
	else
	{
		DEBUG_PRINT("Set timerfd :-)\n");
	}

	/* Register timer with epoll */
	epoll_event.events = EPOLLIN;
	epoll_event.data.ptr = handle_stats_timer;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, state.stats_timerfd, &epoll_event) < 0)
	{
		/* Failed to register timer with epoll */
		perror("Failed to register timer eventfd with epoll");
		return NULL;
	}
	else
	{
		DEBUG_PRINT("Registered timer with with epoll :-)\n");
	}

	/* Init timers */
	UTILS_ResetTimeStats(&state.write_period);
	UTILS_ResetTimeStats(&state.write_dur);
	#endif

	/* Submit all buffers for reading */
	int res = io_submit(state.io_ctx, ARRAY_SIZE(bufs), bufs);
	if (ARRAY_SIZE(bufs) != res)
	{
		fprintf(stderr, "Failed to submit all USB read buffers, req: %zu, act: %d\n", ARRAY_SIZE(bufs), res);
		return NULL;
	}

	/* Enter main loop */
	DEBUG_PRINT("Enter write loop..\n");
	state.keep_running = true;
	while (state.keep_running)
	{
		if (EPOLL_LOOP_Run(epoll_fd, 30000, &state) < 0)
		{
			/* Epoll failed...bail */
			break;
		}
	}
	DEBUG_PRINT("Exit write loop..\n");

	/* Destroy IO context (cancelling any pending transfers) */
	io_destroy(state.io_ctx);

	/* Free buffers after destroying context now kernel won't be using them */
	for (unsigned int i = 0; i < ARRAY_SIZE(state.buffers); i++)
	{
		/* Free buffer */
		free(state.buffers[i]);
		state.buffers[i] = NULL;
	}

	/* Close / destroy everything */
	#if GENERATE_STATS
	close(state.stats_timerfd);
	#endif
	close(state.aio_eventfd);
	iio_buffer_destroy(state.iio_tx_buffer);
	iio_context_destroy(iio_ctx);
	close(epoll_fd);

	/* Exit */
	DEBUG_PRINT("Write thread exit\n");

	return NULL;
}

/* Private functions */
static int handle_eventfd_thread(state_t *state)
{
	/* Quit having detected write on eventfd */
	DEBUG_PRINT("Stop request received\n");
	state->keep_running = false;

	return 0;
}

static int handle_eventfd_aio(state_t *state)
{
	struct io_event events[ARRAY_SIZE(state->buffers)];

	/* Read eventfd to reset it */
	uint64_t dummy;
	if (read(state->aio_eventfd, &dummy, sizeof(dummy)) < 0)
	{
		perror("Failed to read aio completion eventfd");
		return -1;
	}

	/* Read at least one event (having been signalled by eventfd, there should be one pending) but do not block */
	struct timespec timeout = {0, 0};
	int ret = io_getevents(state->io_ctx, 1, ARRAY_SIZE(events), events, &timeout);
	if (ret < 0)
	{
		perror("Failed to read completed io events");
		return -1;
	}

	/* Iterate over events */
	for (int i = 0; i < ret; i++)
	{
		/* Shorthand ptr */
		struct io_event *event = &events[i];

		/* Retrieve buffer */
		usb_buf_t *buf = (usb_buf_t*)event->data;

		/* Check for success */
		if (state->usb_buffer_size == (size_t)event->res)
		{
			/* Copy data into buffer */
			memcpy(iio_buffer_start(state->iio_tx_buffer), buf->data, state->usb_buffer_size);

			#if GENERATE_STATS
			/* Capture write period */
			UTILS_UpdateTimeStats(&state->write_period);

			/* Record write start time */
			UTILS_StartTimeStats(&state->write_dur);
			#endif

			/* Perform blocking write */
			ssize_t nbytes = iio_buffer_push(state->iio_tx_buffer);
			if (nbytes != (ssize_t)state->usb_buffer_size)
			{
				#if GENERATE_STATS
				/* Count overflow */
				state->overflows++;
				#endif
			}

			#if GENERATE_STATS
			/* Capture write end time */
			UTILS_UpdateTimeStats(&state->write_dur);

			/* Record period start time (to subtract write time above) */
			UTILS_StartTimeStats(&state->write_period);
			#endif
		}
		else if (-ESHUTDOWN != (long)event->res)
		{
			/* Not all data was read, or read failed. But error wasn't due to configuration being disabled */
			fprintf(stderr, "USB read completed with error, res: %ld, res2: %ld\n", event->res, event->res2);
		}

		/* Re-submit buffer */
		struct iocb *iocb = &buf->iocb;
		int res = io_submit(state->io_ctx, 1, &iocb);
		if (1 != res)
		{
			/* Failed to submit context */
			perror("Failed to submit usb read");
			buf->in_use = false;
			return -1;
		}
	}

	return 0;
}

#if GENERATE_STATS
static int handle_stats_timer(state_t *state)
{
	/* Read timer to acknowledge it */
	uint64_t timerfd_val;
	if (read(state->stats_timerfd, &timerfd_val, sizeof(timerfd_val)) < 0)
	{
		perror("Failed to read timerfd");
		return 1;
	}

	/* Report min/max/average write period */
	printf("Write period: min: %"PRIu64", max: %"PRIu64", avg: %"PRIu64" (uS)\n",
		   state->write_period.min,
		   state->write_period.max,
		   UTILS_CalcAverageTimeStats(&state->write_period)
	);

	/* Report min/max/average write duration */
	printf("Write dur: min: %"PRIu64", max: %"PRIu64", avg: %"PRIu64" (uS)\n",
		   state->write_dur.min,
		   state->write_dur.max,
		   UTILS_CalcAverageTimeStats(&state->write_dur)
	);

	/* Check for overflows */
	if (state->overflows > 0)
	{
		printf("Read overflows: %u in last 5s period\n", state->overflows);
	}

	/* Reset stats */
	UTILS_ResetTimeStats(&state->write_period);
	UTILS_ResetTimeStats(&state->write_dur);
	state->overflows = 0;

	return 0;
}
#endif

static usb_buf_t *alloc_usb_buffer(size_t size, int usb_fd, int event_fd)
{
	usb_buf_t *buf;

	/* Allocate struct + data data */
	buf = malloc(sizeof(usb_buf_t) + size);
	if (!buf)
	{
		perror("alloc_buffer failed");
		return NULL;
	}

	/* Reset in-use flag */
	buf->in_use = false;

	/* Prepare request */
	io_prep_pread(&buf->iocb, usb_fd, buf->data, size, 0);

	/* Set data to point at buffer such that we can find the buffer on io completion */
	buf->iocb.data = buf;

	/* Enable eventfd notification of completion */
	io_set_eventfd(&buf->iocb, event_fd);

	return buf;
}
