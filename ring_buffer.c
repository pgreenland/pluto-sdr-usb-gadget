/* Public header */
#include "ring_buffer.h"

/* Standard / system libraries */
#include <string.h>

/* Public functions */
void RING_BUFFER_Init(RING_BUFFER_Ctx_t *ctx, uint32_t capacity)
{
	/* Reset context */
	memset(ctx, 0x00, sizeof(*ctx));

	/* Store capacity */
	ctx->capacity = capacity;
}

uint32_t RING_BUFFER_Put(RING_BUFFER_Ctx_t *ctx)
{
	uint32_t index = RING_BUFFER_NO_INDEX;

	if (ctx->usage < ctx->capacity)
	{
		/* Return head */
		index = ctx->head;

		/* Increment and wrap head */
		ctx->head++;
		if (ctx->head == ctx->capacity) ctx->head = 0;

		/* Increment usage */
		ctx->usage++;
	}

	return index;
}

uint32_t RING_BUFFER_Get(RING_BUFFER_Ctx_t *ctx)
{
	uint32_t index = RING_BUFFER_NO_INDEX;

	if (ctx->usage > 0)
	{
		/* Return tail */
		index = ctx->tail;

		/* Increment and wrap tail */
		ctx->tail++;
		if (ctx->tail == ctx->capacity) ctx->tail = 0;

		/* Decrement usage */
		ctx->usage--;
	}

	return index;
}
