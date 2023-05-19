/* Standard / system libraries */
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/functionfs.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

/* libIIO */
#include <iio.h>

/* Local modules */
#include "epoll_loop.h"
#include "thread_read.h"
#include "thread_write.h"
#include "usb_descriptors.h"

/* Macros */
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define DEBUG_PRINT(...) if (debug) printf("Main: "__VA_ARGS__)

/* Constants */
#define SDR_USB_GADGET_COMMAND_START (0x10)
#define SDR_USB_GADGET_COMMAND_STOP (0x11)
#define SDR_USB_GADGET_COMMAND_TARGET_RX (0x00)
#define SDR_USB_GADGET_COMMAND_TARGET_TX (0x01)

/* Type definitions */
#pragma pack(push,1)
typedef struct
{
	/* Bitmask of enabled channels */
	uint32_t enabled_channels;

	/*
	** Buffer size (in samples)
	** Note: This should include space for the 64-bit timestamp.
	** For example with RX0's I and Q channels enabled, each sample will be 2 * 16bit = 32bit
	** therefore a timestamp will occupy 64bit / 32bit = 2 samples. If a timestamp were to be provided
	** at the start of each buffer's worth of samples, an additional two samples would need to be added to
	** the buffer space.
	** Likewise if RX0 and RX1's I and Q channels were enabled, each sample will be 4 * 16bit = 64bit
	** as such only one sample would be required for the timestamp.
	*/
	uint32_t buffer_size;

} cmdStartRequest_t;
#pragma pack(pop)

typedef struct
{
	/* Endpoint file descriptors */
	int ep[3];

	/* Eventfds to signal threads */
	int read_thread_event_fd;
	int write_thread_event_fd;

	/* Thread status */
	bool read_started;
	bool write_started;

	/* Thread arguments */
	THREAD_READ_Args_t read_args;
	THREAD_WRITE_Args_t write_args;

	/* Configuration enabled */
	bool config_enabled;

	/* Threads */
	pthread_t thread_read;
	pthread_t thread_write;

} state_t;

/* Epoll event handler */
typedef int (*epoll_event_handler)(state_t *state);

/* Global variables */
bool debug;

/* Private function */
static int handle_ep0(state_t *state);
static bool start_thread(state_t *state, bool tx);
static bool stop_thread(state_t *state, bool tx);
static bool open_endpoints(state_t *state, const char* path);
static void close_endpoints(state_t *state);
static void signal_handler(int signum);
static void print_usage(const char *program_name, FILE *dest);
static const char* event_to_string(struct usb_functionfs_event *event);

/* Private variables */
static volatile sig_atomic_t keep_running = 1;

/* Public functions */
int main(int argc, char *argv[])
{
	state_t state;

	/* Reset state */
	memset(&state, 0x00, sizeof(state));

	/* Ensure stdout is line buffered */
	setlinebuf(stdout);

	/* Hello world */
	printf("Welcome!\n");
	printf("--------\n");

	/* Basic argument parsing */
	int opt_c;
	bool err = false;
	while ((opt_c = getopt(argc, argv, "dvh")) != -1)
	{
			switch (opt_c)
			{
				case 'd':
				{
					debug = true;
					break;
				}
				case 'v':
				{
					printf("Version %s\n", PROGRAM_VERSION);
					return 0;
				}
				case 'h':
				{
					print_usage(argv[0], stdout);
					return 0;
				}
				case '?':
				{
					err = true;
					break;
				}
			}
	}
	if ((optind+1) > argc)
	{
		/* Missing FFS directory */
		fprintf(stderr, "Error: FFS_DIRECTORY is required\n");
		print_usage(argv[0], stderr);
		return 1;
	}
	else if (err)
	{
		/* Unrecognised argument */
		fprintf(stderr, "Error: Unrecognised argument\n");
		print_usage(argv[0], stderr);
		return 1;
	}

	/* Retrieve FFS directory */
	char *ffs_directory = argv[optind];

	/* Register signal handler */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Open endpoints */
	if (!open_endpoints(&state, ffs_directory))
		return 1;

	/* Prepare eventfds to notify threads to cancel */
	state.read_thread_event_fd = eventfd(0, 0);
	if (state.read_thread_event_fd < 0)
	{
		perror("Failed to open read eventfd");
		return 1;
	}
	else
	{
		DEBUG_PRINT("Opened read eventfd :-)\n");
	}
	state.write_thread_event_fd = eventfd(0, 0);
	if (state.write_thread_event_fd < 0)
	{
		perror("Failed to open write eventfd");
		return 1;
	}
	else
	{
		DEBUG_PRINT("Opened write eventfd :-)\n");
	}

	/* Prepare read args */
	state.read_args.quit_event_fd = state.read_thread_event_fd;
	state.read_args.output_fd = state.ep[1];

	/* Prepare write args */
	state.write_args.quit_event_fd = state.write_thread_event_fd;
	state.write_args.input_fd = state.ep[2];

	/* Create epoll instance */
	int epoll_fd = epoll_create1(0);
	if (epoll_fd < 0)
	{
		perror("Failed to create epoll instance");
		return 1;
	}
	else
	{
		DEBUG_PRINT("Opened epoll :-)\n");
	}

	struct epoll_event epoll_event;

	/* Register ep0 with epoll */
	epoll_event.events = EPOLLIN;
	epoll_event.data.ptr = handle_ep0;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, state.ep[0], &epoll_event) < 0)
	{
		/* Failed to register ep0 with epoll */
		perror("Failed to register ep0 with epoll");
		return 1;
	}
	else
	{
		DEBUG_PRINT("Registered ep0 with epoll :-)\n");
	}

	/* Here we go */
	printf("Ready :-)\n");

	/* Enter main loop */
	DEBUG_PRINT("Enter main loop..\n");
	while (keep_running)
	{
		/* Run epoll until it or one of its handlers fails */
		if (EPOLL_LOOP_Run(epoll_fd, 30000, &state) < 0)
		{
			/* Handler failed...bail */
			break;
		}
	}
	DEBUG_PRINT("Exit main loop :-(\n");

	/* Stop threads */
	stop_thread(&state, false);
	stop_thread(&state, true);

	/* Close files */
	close(epoll_fd);
	close(state.read_thread_event_fd);
	close(state.write_thread_event_fd);
	close_endpoints(&state);

	/* Goodbye */
	printf("Bye!\n");

	return 0;
}

/* Private functions */
static int handle_ep0(state_t *state)
{
	struct usb_functionfs_event event;
	int ret;

	/* Read event from ep0 */
	ret = read(state->ep[0], &event, sizeof(event));
	if (sizeof(event) != ret)
	{
		perror("Failed to read event from ep0");
		return -1;
	}

	/* Print event summary */
	DEBUG_PRINT("Handle ep0 event: %s\n", event_to_string(&event));

	switch (event.type)
	{
		case FUNCTIONFS_SETUP:
		{
			DEBUG_PRINT("Received setup control transfer: bRequestType = %d, bRequest = %d, wValue = %d, wIndex = %d, wLength = %d\n",
						(int)event.u.setup.bRequestType,
						(int)event.u.setup.bRequest,
						(int)event.u.setup.wValue,
						(int)event.u.setup.wIndex,
						(int)event.u.setup.wLength
					   );

			if (event.u.setup.bRequestType & USB_DIR_IN)
			{
				/* Write null response */
				if (write(state->ep[0], NULL, 0) < 0)
				{
					perror("Failed to write packet to host");
					return -1;
				}
			}
			else
			{
				uint8_t control_in_data[64];
				const cmdStartRequest_t *cmd_start_req = (const cmdStartRequest_t*)control_in_data;

				/* Read request */
				ssize_t read_count = read(state->ep[0], control_in_data, sizeof(control_in_data));
				if (read_count < 0)
				{
					perror("Failed to read packet from host");
					return -1;
				}

				/* Act on request */
				switch (event.u.setup.bRequest)
				{
					case SDR_USB_GADGET_COMMAND_START:
					{
						/* Check request size */
						if (read_count != sizeof(*cmd_start_req))
						{
							printf("Bad start request, incorrect data size\n");
							break;
						}

						/* Decide on TX vs RX thread */
						bool tx = (SDR_USB_GADGET_COMMAND_TARGET_TX == event.u.setup.wValue);

						/* Ensure thread stopped */
						stop_thread(state, tx);

						/* Act on direction */
						if (tx)
						{
							/* TX thread, store args */
							state->write_args.iio_channels = cmd_start_req->enabled_channels;
							state->write_args.iio_buffer_size = cmd_start_req->buffer_size;
						}
						else
						{
							/* RX thread, store args */
							state->read_args.iio_channels = cmd_start_req->enabled_channels;
							state->read_args.iio_buffer_size = cmd_start_req->buffer_size;
						}

						/* Start thread */
						start_thread(state, tx);
						break;
					}
					case SDR_USB_GADGET_COMMAND_STOP:
					{
						/* Decide on TX vs RX thread */
						bool tx = (0 != event.u.setup.wValue);

						/* Stop thread */
						stop_thread(state, tx);
						break;
					}
					default:
					{
						/* Ignore unknown requests */
						break;
					}
				}
			}
			break;
		}
		case FUNCTIONFS_DISABLE:
		{
			if (state->config_enabled)
			{
				/* Stop threads */
				if (!(	  stop_thread(state, false)
					   && stop_thread(state, true)
					 )
				   )
				{
					/* Failed to stop a thread */
					return -1;
				}
			}

			/* Flag disabled */
			state->config_enabled = false;
			break;
		}
		case FUNCTIONFS_ENABLE:
		{
			/* Flag enabled */
			state->config_enabled = true;
			break;
		}
		default:
		{
			/* Ignore unknown requests */
			break;
		}
	}

	return 0;
}

static bool start_thread(state_t *state, bool tx)
{
	/* Mask all signals (such that threads will by default not handle them) */
	sigset_t new_mask, old_mask;
	sigfillset(&new_mask);
	if (sigprocmask(SIG_SETMASK, &new_mask, &old_mask) < 0)
	{
		perror("Failed to mask signals");
		return false;
	}

	/* Create appropriate thread */
	if (tx && !state->write_started)
	{
		/* Start thread */
		state->write_started = (0 == pthread_create(&state->thread_write, NULL, &THREAD_WRITE_Entrypoint, &state->write_args));
		if (!state->write_started)
		{
			perror("Failed to start write thread");
			return false;
		}
	}
	else if (!tx && !state->read_started)
	{
		/* Start thread */
		state->read_started = (0 == pthread_create(&state->thread_read, NULL, &THREAD_READ_Entrypoint, &state->read_args));
		if (!state->read_started)
		{
			perror("Failed to start read thread");
			return false;
		}
	}

	/* Return signal mask to old value, such that all signals will be handled by main thread */
	if (sigprocmask(SIG_SETMASK, &old_mask, NULL) < 0)
	{
		perror("Failed to unmask signals");
		return false;
	}

	return true;
}

static bool stop_thread(state_t *state, bool tx)
{
	if (tx && state->write_started)
	{
		/* Write eventfd to signal thread to stop */
		uint64_t eventfd_val = 0x1;
		if (write(state->write_thread_event_fd, &eventfd_val, sizeof(eventfd_val)) < 0)
		{
			perror("Failed to write to write thread eventfd");
			return false;
		}

		/* Join with thread */
		pthread_join(state->thread_write, NULL);

		/* Read eventfd now thread has stopped to reset it */
		if (read(state->write_thread_event_fd, &eventfd_val, sizeof(eventfd_val)) < 0)
		{
			perror("Failed to read from write thread eventfd");
			return false;
		}

		/* Clear running flag */
		state->write_started = false;
	}
	else if (!tx && state->read_started)
	{
		/* Write eventfd to signal thread to stop */
		uint64_t eventfd_val = 0x1;
		if (write(state->read_thread_event_fd, &eventfd_val, sizeof(eventfd_val)) < 0)
		{
			perror("Failed to write to read thread eventfd");
			return false;
		}

		/* Join with thread */
		pthread_join(state->thread_read, NULL);

		/* Read eventfd now thread has stopped to reset it */
		if (read(state->read_thread_event_fd, &eventfd_val, sizeof(eventfd_val)) < 0)
		{
			perror("Failed to read from read thread eventfd");
			return false;
		}

		/* Clear running flag */
		state->read_started = false;
	}

	return true;
}

static bool open_endpoints(state_t *state, const char* path)
{
	/* Prepare buffer for endpoint paths */
	char *ep_path = malloc(strlen(path) + 4 /* "/ep#" */ + 1 /* '\0' */);
	if (!ep_path)
	{
		perror("Failed to allocate endpoint path buffer");
		return false;
	}

	/* Open and prepare EP0 */
	sprintf(ep_path, "%s/ep0", path);
	DEBUG_PRINT("Opening: %s...\n", ep_path);
	state->ep[0] = open(ep_path, O_RDWR);
	if (state->ep[0] < 0)
	{
		perror("Failed to open ep0");
		return false;
	}
	else
	{
		DEBUG_PRINT("Opened ep0 :-)\n");
	}

	/* Provide descriptors and strings to kernel, writing them to ep0 */
	if (!USB_DESCRIPTORS_WriteToEP0(state->ep[0]))
		return false;

	/* Open bulk in/out endpoints */
	sprintf(ep_path, "%s/ep1", path);
	DEBUG_PRINT("Opening: %s...\n", ep_path);
	state->ep[1] = open(ep_path, O_WRONLY);
	if (state->ep[1] < 0)
	{
		perror("Failed to open ep1");
		return false;
	}
	else
	{
		DEBUG_PRINT("Opened ep1 :-)\n");
	}

	sprintf(ep_path, "%s/ep2", path);
	DEBUG_PRINT("Opening: %s...\n", ep_path);
	state->ep[2] = open(ep_path, O_RDONLY);
	if (state->ep[2] < 0)
	{
		perror("Failed to open ep2");
		return false;
	}
	else
	{
		DEBUG_PRINT("Opened ep2 :-)\n");
	}

	/* Free endpoint path buffer */
	free(ep_path);
	ep_path = NULL;

	return true;
}

static void close_endpoints(state_t *state)
{
	/* Close endpoints */
	for (unsigned int i = 0; i < ARRAY_SIZE(state->ep); i++)
	{
		close(state->ep[i]);
	}
}

static void signal_handler(int signum)
{
	(void)signum;

	/* Clear running flag */
	keep_running = 0;
}

static void print_usage(const char *program_name, FILE *dest)
{
	fprintf(dest, "Usage: %s [OPTIONS] FFS_DIRECTORY\n", program_name);
	fprintf(dest, "OPTIONS:\n");
	fprintf(dest, "  -h, --help\tDisplay this help message\n");
	fprintf(dest, "  -v, --version\tDisplay the version of the program\n");
}

static const char* event_to_string(struct usb_functionfs_event *event)
{
	/* Event type names */
	static const char *const names[] =
	{
		[FUNCTIONFS_BIND] = "BIND",
		[FUNCTIONFS_UNBIND] = "UNBIND",
		[FUNCTIONFS_ENABLE] = "ENABLE",
		[FUNCTIONFS_DISABLE] = "DISABLE",
		[FUNCTIONFS_SETUP] = "SETUP",
		[FUNCTIONFS_SUSPEND] = "SUSPEND",
		[FUNCTIONFS_RESUME] = "RESUME",
	};

	/* Lookup event type name */
	switch (event->type)
	{
		case FUNCTIONFS_BIND:
		case FUNCTIONFS_UNBIND:
		case FUNCTIONFS_ENABLE:
		case FUNCTIONFS_DISABLE:
		case FUNCTIONFS_SETUP:
		case FUNCTIONFS_SUSPEND:
		case FUNCTIONFS_RESUME:
			return names[event->type];
		default:
			return "UNKNOWN";
	}
}
