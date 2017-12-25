/*
 * macho.c
 * Copyright (c) 2012-2013 Kristina Brooks
 *
 * Tools for working with mach-o images.
 */

#include <bootkit/runtime.h>
#include <bootkit/mach-o/macho_loader.h>
#include <bootkit/mach-o/macho.h>


#define vhead(x) ((mach_header_t*)(x->base))
#define fhead(x) ((mach_header_t*)(x->file))

#define check_align(x) ((x & 0xfff) == 0)

#define for_each_lc(lcp, head) \
	for ( \
			uint32_t XXi = 0; XXi < head->ncmds; \
			XXi++, lcp = (struct load_command*)add_ptr2(lcp, lcp->cmdsize) \
		)

#define for_each_section(sect, seg) sect = (struct section*)((uintptr_t)seg + sizeof(struct segment_command)); \
for (uint32_t XXi = 0; XXi < seg->nsects; XXi++, sect++)

/* Binary search */
const struct nlist* binary_search_toc(const char* key,
									  const char stringPool[],
									  const struct nlist symbols[],
									  const struct dylib_table_of_contents toc[],
									  uint32_t symbolCount,
									  uint32_t hintIndex);

const struct nlist* binary_search(const char* key,
								  const char stringPool[],
								  const struct nlist symbols[],
								  uint32_t symbolCount);

/*
 * mach_file_vmsize
 *
 * Get the total VM size of the file to be mapped.
 */
loader_return_t mach_file_vmsize(mach_loader_context_t* file, uint32_t* sz)
{
	mach_header_t* head = fhead(file);
	struct load_command* lcp = (struct load_command*)(head+1);
	uint32_t sc = 0;
	uint32_t seg_count = 0;
	uint32_t has_symtab = 0;
	
	for_each_lc(lcp, head)
	{
		/* Only interested in segments */
		if (lcp->cmd == LC_SEGMENT)
		{
			struct segment_command* cmd = (struct segment_command*)lcp;
			
			if (file->filetype == MH_EXECUTE)
			{
				/* Simple - just add the VM sizes up */
				sc += cmd->vmsize;
			}
			else if (file->filetype == MH_OBJECT)
			{
				struct section* sect;
				
				if (seg_count) {
					/* Object files can only have one segment */
					return LOADER_OBJECT_BADSEGMENT;
				}
				
				/*
				 * This is more complicated. To find out the total VM
				 * size for an object file we need to examine the sections.
				 */
				for_each_section(sect, cmd)
				{
					sc += sect->size;
				}
				
				seg_count++;
			}
			else
			{
				return LOADER_BADFILETYPE;
			}
		}
		else if (lcp->cmd == LC_SYMTAB)
		{
			/* This file has a symtab */
			has_symtab = 1;
		}
	}
	
	if (file->filetype == MH_OBJECT && !has_symtab) {
		/* Object file without symtab has to be malformed! */
		return LOADER_NOSYMTAB;
	}
	
	*sz = sc;
	return LOADER_SUCCESS;
}

loader_return_t mach_objc_metadata(uint32_t objc_size, uint8_t* objc_buf)
{
	
}

static void _map_section(mach_loader_context_t* file, struct section* sect, uint8_t* load_addr)
{
	uintptr_t va = add_ptr2(load_addr, sect->addr);
	
	printf("_map_section: va: 0x%x size: 0x%x\n", sect->addr, sect->size);
	
	if ((sect->flags & SECTION_TYPE) == S_ZEROFILL) {
		/* This is a BSS section */
		bzero((void*)va, sect->size);
	}
	else {
		/* Copy the section from the source file */
		bcopy((const void*)add_ptr2(file->file, sect->offset), (void*)va, sect->size);
	}
}

void _symtab_dump(mach_loader_context_t* file)
{
	for (uint32_t i = 0; i < file->symtab->nsyms; i++)
	{
		struct nlist* sym = &file->symbol_base[i];
		printf("[sym]: 0x%08x\n", sym->n_value);
	}
}

boolean_t mach_file_is_prelinked(mach_loader_context_t* file)
{
	return file->is_prelinked;
}

/*
 * mach_file_map
 *
 * Maps in the contents of a mach-o image into a region
 * denoted by "base".
 */
loader_return_t mach_file_map(mach_loader_context_t* file, uint8_t* load_addr, uint32_t vmsize)
{
	mach_header_t* head = fhead(file);
	struct load_command* lcp = (struct load_command*)(head+1);
	uint32_t seg_count = 0;
	uint32_t vm_bias = file->vm_bias;
	
	for_each_lc(lcp, head)
	{
		/* Only interested in segments */
		if (lcp->cmd == LC_SEGMENT)
		{
			struct segment_command* cmd = (struct segment_command*)lcp;
			
			if (file->filetype == MH_EXECUTE)
			{
				struct section* sect;
				
				/*
				 * For executable images, we need to map entrie
				 * segments and zero out deltas.
				 */
				
				uint32_t delta = cmd->vmsize - cmd->filesize;
				uint32_t actual_vmaddr;
				
				if (vm_bias == 0 && seg_count == 0 && cmd->vmaddr != 0)
				{
					/* How strange ... */
					return LOADER_EXEC_UNSUPPORTED;
				}
				
				actual_vmaddr = cmd->vmaddr - vm_bias;

				if (cmd->filesize) {
					void* src = (void*)add_ptr2(file->file, cmd->fileoff);
					void* dst = (void*)add_ptr2(load_addr, actual_vmaddr);

					/* Copy from source file */
					bcopy(src, dst, cmd->filesize);

					printf("[MAP:%s]: copy 0x%08x =(%u)=> 0x%08x\n",
						cmd->segname,
						src,
						cmd->filesize,
						dst);
				}
				
				if (delta && cmd->vmsize > cmd->filesize) {
					void* dst = (void*)add_ptr3(load_addr, actual_vmaddr, cmd->filesize);

					printf("[MAP:%s]: zero 0x%08x (%u)\n",
						cmd->segname, dst, delta);

					/* Zero out the rest */
					bzero(dst, delta);
				}
				
				if (strncmp(cmd->segname, kPrelinkInfoSegment, 15) == 0) {
					/* If the prelink segment has stuff in it, then this
					 * is a prelinked kernel image */
	
					if (cmd->vmsize != 0) {
						file->is_prelinked = true;
					}
				}
				
				/*
				 * Examine ObjC metadata,
				 */
				for_each_section(sect, cmd)
				{
					
				}
				
				seg_count++;
			}
			else if (file->filetype == MH_OBJECT)
			{
				struct section* sect;
				
				if (seg_count) {
					/* Object files can only have one segment */
					return LOADER_OBJECT_BADSEGMENT;
				}
				
				/* Save the first segment for convinience */
				file->first_segment = cmd;
				
				/*
				 * For object files, we map stuff section by
				 * section. We also take note of the string section
				 * ordinal as we will need it later.
				 */
				
				for_each_section(sect, cmd)
				{
					_map_section(file, sect, load_addr);
				}
				
				seg_count++;
			}
			else
			{
				return LOADER_BADFILETYPE;
			}
		}
		else if (lcp->cmd == LC_UNIXTHREAD)
		{
			thread_command_t* th = (thread_command_t*)lcp;
			file->entry_point = th->state.pc;
		}
		else if (lcp->cmd == LC_DYSYMTAB)
		{
			file->dsymtab = (struct dysymtab_command*)lcp;
		}
		else if (lcp->cmd == LC_DYLD_INFO || lcp->cmd == LC_DYLD_INFO_ONLY)
		{
			file->dyld_info = (struct dyld_info_command*)lcp;
			file->compressed = 1;
			
			return LOADER_EXEC_UNSUPPORTED;
		}
		else if (lcp->cmd == LC_SYMTAB)
		{
			file->symtab = (struct symtab_command*)lcp;
			
			/* Load symtab stuff */
			file->string_base = (char*)add_ptr2(file->file, file->symtab->stroff);
			file->symbol_base = (struct nlist *)add_ptr2(file->file, file->symtab->symoff);
		}
	}
	
	/* Save the loader info */
	file->base = load_addr;
	file->vmsize = vmsize;
	
	return LOADER_SUCCESS;
}

#define MAX_SECT_TAB 10

/*
 * _symtab_find_symbol
 *
 * Looks up a symbol by its address in a symbol table. It's
 * fairly slow at it walks over each symbol.
 */
static const struct nlist *
_symtab_find_symbol(mach_loader_context_t* file, uint32_t entry)
{
	for (uint32_t i = 0; i < file->symtab->nsyms; i++)
	{
		struct nlist* sym = &file->symbol_base[i];
		
		if (sym->n_desc & N_ARM_THUMB_DEF) {
			entry = entry & ~1;
		}
		
		if (sym->n_value == entry && !(sym->n_type & N_STAB)) {
			return sym;
		}
	}
	
	return NULL;
}


/*
 * _sect_by_ordinal
 *
 * Gets a section from a segment with a given ordinal.
 */
static struct section*
_sect_by_ordinal(mach_loader_context_t* file, struct segment_command* seg, uint32_t ordinal)
{
	struct section* sect = (struct section*)(seg+1);
	return &sect[ordinal-1];
}

#define is_bad_addr(file, addr) ((uintptr_t)addr < (uintptr_t)file->base || (uintptr_t)addr > add_ptr2(file->base, file->vmsize))
#define is_bad_file_addr(file, addr) (0)

/*
 * _relocate_sect
 *
 * Relocates a section with a given ordinal with a delta
 * of the current context's 'loader_bias'. This has the main guts
 * of the object section relocator.
 */
static loader_return_t
_relocate_sect(mach_loader_context_t* file, struct segment_command* seg, uint32_t ordinal)
{
	struct section* sect = _sect_by_ordinal(file, seg, ordinal);
	struct relocation_info* rbase = NULL;
	
	/* The relocation info stuff is in the source file */
	rbase = (struct relocation_info *)add_ptr2(file->file, sect->reloff);
	
	printf("_relocate_sect: %d %s\n", ordinal, sect->sectname);
	
	/* Relocation iterator */
	for (uint32_t i = 0; i < sect->nreloc; i++)
	{
		/*
		 * This makes the assumption that sections are contigious
		 * and are mapped in bias+vmaddr. This would be more flexible
		 * if we built a section table with a bias for each section which
		 * would allow scattered sections, but I'm not bothered.
		 */
		
		struct relocation_info* rinfo = &rbase[i]; /* Relocation offset */
		uint32_t* entry; /* Absolute address of the patch point */
		
		/* Relocation sanity */
		if (is_bad_file_addr(file, rinfo)) {
			return LOADER_OUTOFBOUNDS;
		}
		if (rinfo->r_address & R_SCATTERED) {
			/* We don't support scattered relocations */
			return LOADER_BADRELOC;
		}
		if (rinfo->r_length != 2) {
			/* Bad size, probably unsupported file */
			return LOADER_BADRELOC;
		}
		if (rinfo->r_type != GENERIC_RELOC_VANILLA) {
			/*
			 * PC relative relocations do not need to be modified
			 * unless we're scattering sections (which we're not).
			 * If this is a non-PC relative strange relocation, bail out.
			 */
			
			if (rinfo->r_pcrel) {
				continue;
			}
			else {
				return LOADER_BADRELOC;
			}
		}
		
		entry = (uint32_t*)add_ptr3(rinfo->r_address, sect->addr, file->base);
		if (is_bad_addr(file, entry)) {
			/* Wait a second ... */
			return LOADER_OUTOFBOUNDS;
		}
		
		if (rinfo->r_extern) {
			/* External (unresolved) symbol entry */
			
			//uint32_t sn = rinfo->r_symbolnum; /* Symbol index */
			
			return LOADER_BADRELOC;
		}
		else {
			/* Internal symbol */
			
			uint32_t ordinal = rinfo->r_symbolnum; /* Section ordinal */
			
			if (ordinal == R_ABS) {
				/* Absolute relocs not supported */
				return LOADER_BADRELOC;
			}
			
			/*
			 * Entry points to a symbol address that we can slide
			 * by a given bias, therefore relocating the entry. We
			 * could also do a symbol lookup by its address, if we wanted.
			 */
			
			*entry += (uint32_t)file->loader_bias;
		}
	}
	
	return LOADER_SUCCESS;
}

/*
 * mach_file_relocate_object
 *
 * Relocate all symbols in the object file in accordance
 * with the loader bias.
 */
loader_return_t mach_file_relocate_object(mach_loader_context_t* file)
{
	struct segment_command* cmd;
	
	if (file->filetype != MH_OBJECT) {
		/* This only works on objects */
		return LOADER_BADFILETYPE;
	}
	
	cmd = file->first_segment;
	
	for (uint32_t i = 0; i < cmd->nsects; i++) {
		loader_return_t ret = _relocate_sect(file, cmd, i+1);
		
		if (ret != LOADER_SUCCESS) {
			/* We fucked up ... */
			return ret;
		}
	}
	
	return LOADER_SUCCESS;
}

/*
 * mach_file_relocate_object
 *
 * Relocate all symbols in the executable file in accordance
 * with the loader bias.
 */
loader_return_t mach_file_relocate_executable(mach_loader_context_t* file)
{
	if (file->filetype != MH_EXECUTE) {
		/* This only works on execs */
		return LOADER_BADFILETYPE;
	}
	
	if (file->dsymtab == NULL) {
		/* No symtab, wut? */
		return LOADER_EXEC_UNSUPPORTED;
	}
	
	struct relocation_info* rbase;
	
	/* The relocation info stuff is in the source file */
	rbase = (struct relocation_info *)add_ptr2(file->file, file->dsymtab->locreloff);
	
	/* Relocation iterator */
	for (uint32_t i = 0; i < file->dsymtab->nlocrel; i++)
	{
		struct relocation_info* rinfo = &rbase[i]; /* Relocation offset */
		if (is_bad_file_addr(file, rinfo)) {
			return LOADER_OUTOFBOUNDS;
		}
		
		if (rinfo->r_address & R_SCATTERED)
		{
			/* Handle a scattered relocation */
			return LOADER_BADRELOC;
		}
		else
		{
			/* Handle a regular relocation */
			if (rinfo->r_length != 2) {
				/* Bad size, probably unsupported file */
				return LOADER_BADRELOC;
			}
			if (rinfo->r_type != GENERIC_RELOC_VANILLA) {
				/* Unsupported relocation type */
				return LOADER_BADRELOC;
			}
			if (rinfo->r_symbolnum == R_ABS) {
				return LOADER_BADRELOC;
			}
			
			/* Relocate */
			uint32_t* pp = (uint32_t*)add_ptr2(file->base, rinfo->r_address);
			if (is_bad_addr(file, pp)) {
				return LOADER_OUTOFBOUNDS;
			}
			
			*pp = (uint32_t)((loader_bias_t)*pp + file->loader_bias);
		}
	}
	
	return LOADER_SUCCESS;
}

/*
 * mach_file_get_entry_point
 *
 * Gets the entry point (assumes the file is already
 * mapped in)
 */
loader_return_t mach_file_get_entry_point(mach_loader_context_t* file, uint32_t* entry)
{
	if (file->filetype != MH_EXECUTE) {
		/* This only works on execs */
		return LOADER_BADFILETYPE;
	}
	
	*entry = file->entry_point;
	
	return LOADER_SUCCESS;
}

/*
 * mach_file_find_symbol
 *
 * Finds an exported symbol.
 */
loader_return_t mach_file_find_symbol(mach_loader_context_t* file, const char* name, uint32_t* sym)
{
	if (file->filetype != MH_EXECUTE) {
		/* This only works on execs */
		return LOADER_BADFILETYPE;
	}
	if (file->dsymtab == NULL) {
		/* We need a symtab for the lookup. */
		return LOADER_EXEC_UNSUPPORTED;
	}
	if (!file->string_base || !file->symtab) {
		return LOADER_MALFORMED;
	}
	
	const struct nlist* s = NULL;
	
	if (file->dsymtab->tocoff == 0)
	{
		s = binary_search(name,
						  file->string_base,
						  &file->symbol_base[file->dsymtab->iextdefsym],
						  file->dsymtab->nextdefsym);
	}
	else
	{
		/* XXX */
		return LOADER_EXEC_UNSUPPORTED;
	}
	
	if (!s) {
		/* Didn't find the symbol */
		return LOADER_SYMBOL_NOT_FOUND;
	}
	
	/* Rebase */
	*sym = (uint32_t)((loader_bias_t)s->n_value + file->loader_bias);
	
	return LOADER_SUCCESS;
}

/*
 * mach_file_code_data_range
 *
 * Gets the relative range assosciated with the code and data sections
 * of a mach-o executable, without the symtab, headers or any other
 * garbage.
 */
loader_return_t mach_file_code_data_range(mach_loader_context_t* file, uint32_t* start, uint32_t* size)
{
	if (file->filetype != MH_EXECUTE) {
		/* This only works on execs */
		return LOADER_BADFILETYPE;
	}
	
	mach_header_t* head = fhead(file);
	struct load_command* lcp = (struct load_command*)(head+1);
	uint32_t last_vmaddr = 0;
	uint32_t seg_index = 0;
	uint32_t seg_accounted = 0;
	uint32_t code_start = 0;
	uint32_t vmsize = 0;
	
	for_each_lc(lcp, head)
	{
		if (lcp->cmd == LC_SEGMENT)
		{
			struct segment_command* cmd = (struct segment_command*)lcp;
			
			if (cmd->vmaddr != last_vmaddr) {
				return LOADER_EXEC_NONCONTIGIOUS;
			}
			
			if (seg_index == 1) {
				/* should be __TEXT */
				
				if (strcmp(cmd->segname, SEG_TEXT) == 0) {
					/* Figure out where the actual __TEXT starts */
					struct section* sect = _sect_by_ordinal(file, cmd, 1);
					if (!sect) {
						return LOADER_MALFORMED;
					}
					
					code_start = sect->addr;
					vmsize += cmd->vmsize - (sect->addr - cmd->vmaddr);
					seg_accounted++;
				}
				else {
					return LOADER_EXEC_UNEXPECTED_SEG;
				}
			}
			else if (seg_index == 2) {
				/* should be __DATA */
				
				if (strcmp(cmd->segname, SEG_DATA) == 0) {
					vmsize += cmd->vmsize;
					seg_accounted++;
				}
				else {
					return LOADER_EXEC_UNEXPECTED_SEG;
				}
			}
			
			last_vmaddr = cmd->vmaddr + cmd->vmsize;
			seg_index++;
		}
	}
	
	*start = code_start;
	*size = vmsize;
	
	return LOADER_SUCCESS;
}

/*
 * mach_file_init
 *
 * Creates a new mach-o context.
 */
loader_return_t mach_file_init(mach_loader_context_t* file, uint8_t* fbase)
{
	mach_header_t* head = (mach_header_t*)fbase;
	
	/* Check file sanity */
	if (head->magic != MH_MAGIC) {
		return LOADER_BADMAGIC;
	}
	
	/* We can only load executable and object images */
	if (head->filetype != MH_EXECUTE &&
		head->filetype != MH_OBJECT) {
		return LOADER_BADFILETYPE;
	}
	
	file->filetype = head->filetype;
	file->file = fbase;
	file->vm_bias = 0;
	file->is_prelinked = false;
	
	return LOADER_SUCCESS;
}

void mach_file_set_vm_bias(mach_loader_context_t* file, uint32_t loader_bias)
{
	file->vm_bias = loader_bias;
}

void mach_file_set_loader_bias(mach_loader_context_t* file, long int loader_bias)
{
	file->loader_bias = loader_bias;
}
