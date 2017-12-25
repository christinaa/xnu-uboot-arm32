/*
 * image3.c
 * Copyright (c) 2012 Kristina Brooks
 *
 * Support for IMAGE3 format.
 */

#include <bootkit/runtime.h>
#include <bootkit/image3.h>

#define image3_sz(ctx) (ctx->head->full_size)
#define image3_off(ctx, off) ((uint32_t)((uintptr_t)off - (uintptr_t)ctx->head))

void* image3_get_buffer(img3_context_t* ctx)
{
	return (void*)(ctx->head);
}

size_t image3_get_size(img3_context_t* ctx)
{
	return (size_t)(ctx->head->full_size);
}

void image3_destroy(img3_context_t* ctx)
{
	if (ctx->dynamic) {
		free(ctx->head);
		free(ctx);
	}
}

void* image3_reserve_tag(img3_context_t* ctx, uint32_t type, uint32_t data_length)
{
	uint32_t size;
	uint32_t len;
	
	img3_tag_t* tag;
	
	if (!ctx->dynamic) {
		return NULL;
	}
	
	len = data_length + sizeof(img3_tag_t);
	size = image3_sz(ctx) + len;
	
	/* Reallocate */
	ctx->head = realloc(ctx->head, size);
	if (!ctx->head) {
		/* Welp */
		abort();
	}
	
	tag = (img3_tag_t*)add_ptr2(ctx->head, image3_sz(ctx));
	
	/* Populate tag */
	tag->data_length = data_length;
	tag->total_length = len;
	tag->type = type;
	
	/* Writeback to the img3 head */
	ctx->head->full_size = size;
	ctx->head->unpacked_size += len;
	
	/* Return pointer to data */
	return (void*)(tag + 1);
}

img3_context_t* image3_new(uint32_t type)
{
	img3_header_t* head;
	img3_context_t* ctx;
	
	head = malloc(sizeof(img3_header_t));
	if (!head) {
		return NULL;
	}
	
	ctx = malloc(sizeof(ctx));
	if (!ctx) {
		free(head);
		return NULL;
	}
	
	head->full_size = sizeof(img3_header_t);
	head->magic = IMG3_MAGIC;
	head->ident = type;
	head->sig_area = 0;
	head->unpacked_size = 0;
	
	ctx->dynamic = true;
	ctx->head = head;
	
	return ctx;
}

img3_tag_t* image3_find_tag(img3_context_t* ctx, uint32_t type)
{
	img3_tag_t* tag = (img3_tag_t*)(ctx->head + 1);
	
	while (true)
	{
		if (tag->type == type) {
			return tag;
		}
		
		/* Next tag */
		tag = (img3_tag_t*)add_ptr2(tag, tag->total_length);
		
		if (image3_off(ctx, tag) > ctx->head->full_size)
		{
			/* Reached the end */
			break;
		}
	}
	
	return NULL;
}

boolean_t image3_tag_present(img3_context_t* ctx, uint32_t type)
{
	img3_tag_t* tag = image3_find_tag(ctx, type);
	
	if (tag) {
		return true;
	}
	
	return false;
}

boolean_t image3_get_tag_data(img3_context_t* ctx, uint32_t type, void** data, uint32_t* size)
{
	img3_tag_t* tag = image3_find_tag(ctx, type);
	
	if (tag) {
		/* Pointer to the data */
		*size = tag->data_length;
		*data = (void*)(tag + 1);
		
		return true;
	}
	
	return false;
}

boolean_t image3_from_buffer(void* buffer, img3_context_t* ctx)
{
	img3_header_t* head = (img3_header_t*)buffer;
	if (head->magic != IMG3_MAGIC) {
		return false;
	}
	
	ctx->head = head;
	
	return true;
}

boolean_t image3_fast_get_type(void* buffer, uint32_t* type)
{
	img3_header_t* head = (img3_header_t*)buffer;
	if (head->magic != IMG3_MAGIC) {
		printf("image3_fast_get_type: invalid magic %x\n", head->magic);
		return false;
	}
	
	*type = head->ident;
	
	return true;
}