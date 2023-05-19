#ifndef __EPOLL_LOOP_H__
#define __EPOLL_LOOP_H__

/* Wait for and handle epoll events */
int EPOLL_LOOP_Run(int epoll_fd, int timeout, void *handler_arg);

#endif
