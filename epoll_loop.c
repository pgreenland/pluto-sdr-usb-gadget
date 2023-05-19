/* Public header */
#include "epoll_loop.h"

/* Standard / system libraries */
#include <stdio.h>
#include <errno.h>
#include <sys/epoll.h>

/* Macros */
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/* Epoll event handler */
typedef int (*epoll_event_handler)(void *arg);

/* Public functions */
int EPOLL_LOOP_Run(int epoll_fd, int timeout, void *handler_arg)
{
	struct epoll_event epoll_events[10];

	/* Wait for events */
	int event_count = epoll_wait(epoll_fd, epoll_events, ARRAY_SIZE(epoll_events), timeout);
	if (event_count < 0)
	{
		/* Check error */
		if (EINTR != errno)
		{
			perror("Epoll failed");
			return -1;
		}

		/* EINTR (interrupted) either due to a signal handler or timeout, convert to success */
		return 0;
	}

	/* Iterate over events */
	for (int i = 0; i < event_count; i++)
	{
		/* Execute event handler */
		epoll_event_handler handler = (epoll_event_handler)epoll_events[i].data.ptr;
		if (handler(handler_arg) < 0)
		{
			fprintf(stderr, "Epoll event handler failed\n");
			return -1;
		}
	}

	return 0;
}
