
#include <bootkit/runtime.h>
#include <asm/global_data.h>

DECLARE_GLOBAL_DATA_PTR;

void memory_region_restore(memory_region_t* dest, memory_region_t* src)
{
	dest->base = src->base;
	dest->down = src->down;
	dest->pos = src->pos;
}

void memory_region_save(memory_region_t* src, memory_region_t* dest)
{
	dest->base = src->base;
	dest->down = src->down;
	dest->pos = src->pos;
}

/*
 * memory_reserve
 *
 * Reserve a contigious chunk of memory from 'region' of 'size' and
 * with aligned on an 'align_boundary' boundary.
 */
void* memory_reserve(memory_region_t* region, uint32_t size, uint32_t align_boundary)
{
	uintptr_t start;
	
	if (region->down) {
		region->pos -= size;
		
		if (align_boundary) {
			region->pos = align_down(region->pos, align_boundary);
		}
		
		start = region->pos;
	}
	else {
		if (align_boundary) {
			region->pos = align_up(region->pos, align_boundary);
		}
		
		start = region->pos;
		region->pos += size;
	}
	
	return (void*)start;
}

uint32_t get_memory_base(void)
{
	return 0x20000000;
}

uint32_t total_memory_size(void)
{
	return 0x20000000;
}