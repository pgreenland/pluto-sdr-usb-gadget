#ifndef __THREAD_READ_H__
#define __THREAD_READ_H__

/* Standard libraries */
#include <stdint.h>
#include <stddef.h>

/* Type definitions - thread args */
typedef struct
{
	/* Eventfd used to signal thread to quit */
	int quit_event_fd;

	/* USB endpoint to write to */
	int output_fd;

	/* Enabled channels */
	uint32_t iio_channels;

	/* Sample buffer size (in samples) */
	size_t iio_buffer_size;

} THREAD_READ_Args_t;

/* Public functions - Thread entrypoint */
void *THREAD_READ_Entrypoint(void *args);

#endif
