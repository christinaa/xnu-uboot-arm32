/*
 * macho_util.c
 * Copyright (c) 2012 Kristina Brooks
 *
 * Misc mach-o utilities.
 */

#include <bootkit/runtime.h>
#include <bootkit/mach-o/macho.h>

const struct nlist* binary_search_toc(const char* key,
									  const char stringPool[],
									  const struct nlist symbols[],
									  const struct dylib_table_of_contents toc[],
									  uint32_t symbolCount,
									  uint32_t hintIndex)
{
	int32_t high = symbolCount-1;
	int32_t mid = hintIndex;
	
	if ( mid >= (int32_t)symbolCount )
		mid = symbolCount/2;
	
	for (int32_t low = 0; low <= high; mid = (low+high)/2) {
		const uint32_t index = toc[mid].symbol_index;
		const struct nlist* pivot = &symbols[index];
		const char* pivotStr = &stringPool[pivot->n_un.n_strx];
		
		int cmp = strcmp(key, pivotStr);
		if ( cmp == 0 )
			return pivot;
		if ( cmp > 0 ) {
			// key > pivot
			low = mid + 1;
		}
		else {
			// key < pivot
			high = mid - 1;
		}
	}
	return NULL;
}

const struct nlist* binary_search(const char* key,
								  const char stringPool[],
								  const struct nlist symbols[],
								  uint32_t symbolCount)
{
	const struct nlist* base = symbols;
	for (uint32_t n = symbolCount; n > 0; n /= 2) {
		const struct nlist* pivot = &base[n/2];
		const char* pivotStr = &stringPool[pivot->n_un.n_strx];
		
		int cmp = strcmp(key, pivotStr);
		if ( cmp == 0 )
			return pivot;
		if ( cmp > 0 ) {
			// key > pivot
			// move base to symbol after pivot
			base = &pivot[1];
			--n;
		}
		else {
			// key < pivot
			// keep same base
		}
	}
	return NULL;
}