#ifndef __RING_BUFFER_H__
#define __RING_BUFFER_H__

/* Standard libraries */
#include <stdbool.h>
#include <stdint.h>

/* Defines */
#define RING_BUFFER_NO_INDEX (UINT32_MAX)

/* Type definitions - Buffer context */
typedef struct
{
	/* Capacity / usage */
	uint32_t capacity;
	uint32_t usage;

	/* Head / tail indexes */
	uint32_t head;
	uint32_t tail;

} RING_BUFFER_Ctx_t;

/* Public functions - init buffer */
void RING_BUFFER_Init(RING_BUFFER_Ctx_t *ctx, uint32_t capacity);

/* Add entry to buffer. Index at which to store item will be returned, if no space available return value will be RING_BUFFER_NO_INDEX */
uint32_t RING_BUFFER_Put(RING_BUFFER_Ctx_t *ctx);

/* Fetch entry from buffer. Index at which to retrieve item will be returned, if no items are available return value will be RING_BUFFER_NO_INDEX */
uint32_t RING_BUFFER_Get(RING_BUFFER_Ctx_t *ctx);

#endif