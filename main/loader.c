/*
 * loader.c
 * Copyright (c) 2013 Kristina Brooks
 *
 * Loads stuff in the right places.
 */

#include <bootkit/runtime.h>

#include <bootkit/xml.h>
#include <bootkit/device_tree.h>
#include <bootkit/mach-o/macho.h>

#include <asm/global_data.h>
#include <command.h>
#include <fs.h>

#include "loader.h"
#include "hfs_header.h"

#include <bootkit/compressed/quicklz.h>

DECLARE_GLOBAL_DATA_PTR;

extern int fs_set_blk_dev(const char *ifname, const char *dev_part_str, int fstype);
extern int fs_read(const char *filename, ulong addr, int offset, int len);

#define kMachOMagic 0xCAFEBABE

typedef struct {
	uint32_t magic;
	uint32_t ncmds;
	/* ... first command ... */
}
table_of_contents_t;

typedef struct {
	uint32_t magic;
	uint32_t size;
}
command_t;

typedef struct {
	uint32_t magic;
	uint32_t size;
	uint32_t decomp_size;
	uint32_t info_offset;
	uint32_t load_address;
	uint32_t flags;

	/*
	 * Colloquial name of the image. This is passed
	 * as the bundle name to the kernel.
	 */
	char name[NAME_LEN];

	/* ... compressed data ... */
}
command_macho_t;

#define kTableOfContentsMagic ((uint32_t)'CfoT')
#define kCommandMachO ((uint32_t)'hcaM')
#define kCommandXMLDeviceTree ((uint32_t)'TD-X')
#define kCommandJSDeviceTree ((uint32_t)'TDSJ')
#define kCommandRamdisk ((uint32_t)'KSDR')
#define kCommandConfiguration ((uint32_t)'FNOC')

#define kMachDriver 0x1
#define kMachKernel 0x2

#define kCommandMachOFlags_CompressedLZSS 0x100
#define kCommandMachOFlags_HasInfoPlist 0x200
#define kCommandMachOFlags_CompressedQLZ 0x400
#define kCommandMachOFlags_NoExec 0x800

extern int decompress_lzss(uint8_t *dst, uint8_t *src, uint32_t srclen);

typedef struct {
	uint32_t load_address; /* in, actual addr */
	uint32_t flags;        /* in */
	uint32_t entry_point;  /* out, entry point */
} macho_info_t;

#define DRIVER_PAD_START 256

/* All of these are physical */
loaded_driver_image_t* gLoadedDriverImages = NULL;
memory_range_t gKernelMemoryRange = {0,0};
memory_range_t gRAMDiskRange = {0, 0};
uint32_t gKernelEntryPoint; 
uint32_t gKernelMemoryTop = 0;
uint32_t gKernelVirtualBase = 0;
uint32_t gKernelPhysicalBase = 0;
boolean_t gHasDeviceTree = FALSE;

#define __LastEnv(x) \
static uint32_t last_##x(void) \
{\
	unsigned long addr;\
	char* en;\
	en = getenv(#x);\
	if (!en) {\
		return 0;\
	}\
	addr = simple_strtoul(en, NULL, 16);\
	return (uint32_t)addr;\
}

__LastEnv(filesize);
__LastEnv(fileaddr);

#define CheckLoaderReturn(x) macho_ldr_return = (x); \
	if (macho_ldr_return != LOADER_SUCCESS) { \
		printf(KERR "'%s' failed with %d\n", #x, macho_ldr_return); \
		return FALSE; \
	}

static boolean_t load_macho(
	uint32_t image_address,
	uint32_t image_size,
	uint32_t load_bias,
	uint32_t load_address,
	uint32_t* out_entry_point,
	uint32_t* out_size)
{
	mach_loader_context_t ctx;
	loader_return_t macho_ldr_return;
	uint32_t full_size = 0;
	
	CheckLoaderReturn(mach_file_init(&ctx, (uint8_t*)image_address))
	CheckLoaderReturn(mach_file_vmsize(&ctx, &full_size));
	
	printf(KPROC(MMAP) "mapping kernel ...\n");
	
	/* kernel is not PIE, so we need to set vm bias */
	mach_file_set_vm_bias(&ctx, load_bias);

	/* map in the kernel */
	CheckLoaderReturn(mach_file_map(&ctx, (uint8_t*)load_address, full_size));
	
	/* and find entry point */
	CheckLoaderReturn(mach_file_get_entry_point(&ctx, out_entry_point));
	
	printf(KINF "vmsize=0x%x paddr=0x%x vaddr=0x%x\n", full_size, load_address, load_bias);

	*out_size = full_size;

	return true;
}

static boolean_t assert_kernel_load(void)
{
	/*
	 * Make sure the kernel was already loaded
	 * or bail out if it's not.
	 */

	if (RANGE_IS_NULL(gKernelMemoryRange)) {
		printf(KWARN "a kernel image has to be loaded first\n");
		return false;
	}
	return true;
}

/*
 * teardown_loaded_driver_images
 *
 * Release loaded driver structs.
 */
void teardown_loaded_driver_images(void)
{
	loaded_driver_image_t* next;

	next = gLoadedDriverImages;
	if (!next) return;

	do {
		loaded_driver_image_t* this = next;
		next = this->next;
		free((void*)this);
	} while(next != NULL);

	gLoadedDriverImages = NULL;
}

/*
 * teardown_old_loader_context
 *
 * Release old loader context and all memory used by it.
 */
static void teardown_old_loader_context(void)
{
	if (gHasDeviceTree) {
		DT__Finalize();
		gHasDeviceTree = FALSE;
	}

	gKernelMemoryTop = 0;
	gKernelPhysicalBase = 0;
	gKernelVirtualBase = 0;

	ZERO_RANGE(gKernelMemoryRange);
	ZERO_RANGE(gRAMDiskRange);

	teardown_loaded_driver_images();
}

static void increment_kernel_memory(uint32_t by)
{
	/*
	 * align everything up to a page. why?
	 *
	 * 1). ramdisks are meant to be page aligned anyway
	 * 2). all other crap should get DMA alignment
	 */
	by = align_up(by, 0x1000);

	gKernelMemoryTop += by;
	setenv_hex("KernelMemoryTop", gKernelMemoryTop);
}

static int load_macho_from_command(command_macho_t* command)
{
	boolean_t is_compressed = FALSE;

	uint32_t image_address;
	uint32_t image_size;

	uint32_t raw_image_dest;
	uint32_t blob_size;

	uint32_t flags;

	/* Image follows the command header */
	image_address = (uint32_t)(command+1);
	blob_size = command->size - sizeof(command_macho_t);

	flags = command->flags;

	if (flags & (kCommandMachOFlags_CompressedLZSS | kCommandMachOFlags_CompressedQLZ))
		is_compressed = TRUE;

	/* Sanitize the filename */
	if (command->name[NAME_LEN-1] != '\0') {
		printf(KWARN "image name not NULL terminated - adding a NULL\n");
		command->name[NAME_LEN-1] = '\0';
	}

	/* Setup kernel memory pointer */
	if (flags & kMachKernel) {
		bd_t	*bd = gd->bd;

		uint32_t slide = (command->load_address & 0xfffff);
		uint32_t dram_start;

		if (!RANGE_IS_NULL(gKernelMemoryRange)) {
			printf(KWARN "a kernel is already loaded - tearing it down\n");
			teardown_old_loader_context();
		}

		if (CONFIG_NR_DRAM_BANKS < 1) {
			printf(KERR "no DRAM banks available, can't allocate memory\n");
			return 1;
		}

		dram_start = bd->bi_dram[0].start;

		/* Physical load address for the kernel */
		gKernelMemoryTop = (dram_start + slide);

		gKernelVirtualBase  = (command->load_address & ~0xfffff);
		gKernelPhysicalBase = (dram_start);
	}
	else {
		if (!assert_kernel_load())
			return 1;
	}

	raw_image_dest = gKernelMemoryTop;

	printf(KINF "macho@%08x: '%s' cmp=%d sz=%08x dst=%08x\n",
		image_address,
		(const char*)&command->name,
		is_compressed,
		command->decomp_size,
		raw_image_dest);

	/* Pad for drivers */
	if (flags & kMachDriver) {
		raw_image_dest += DRIVER_PAD_START;
	}

	/* Decompress first if it was compressed */
	if (is_compressed) {
		uint8_t* decomp_image;

		if (flags & kMachDriver) {
			decomp_image = (uint8_t*)(raw_image_dest);
		}
		else {
			/*
			 * Assume that the loaded Mach-O file will be less than 
			 * 4 times its unloaded form so we can get some memory
			 * for the buffer from which we will then parse the file.
			 */
			decomp_image = (uint8_t*)(raw_image_dest + (command->decomp_size*4));
		}

		if (flags & kCommandMachOFlags_CompressedLZSS)
		{
			/* LZSS.C compression */

			printf(KPROC(LZSS) "0x%08x => 0x%08x ...\n", image_address, decomp_image);

			decompress_lzss(
					decomp_image,
					(uint8_t*)image_address,
					blob_size
				);
		}
		else if (flags & kCommandMachOFlags_CompressedQLZ)
		{
			/* QuickLz compression */

			qlz_state_decompress *state_decompress;
			unsigned int qlz_len;

			qlz_len = qlz_size_decompressed((char*)image_address);

			if (qlz_len != command->decomp_size) {
				printf(KERR "QLZ decomp size mismatch (QLZ:0x%08x IMGX:0x%08x)\n",
					qlz_len,
					command->decomp_size);

				return 1;
			}

			state_decompress =
			(qlz_state_decompress *)malloc(sizeof(qlz_state_decompress));

			if (!state_decompress) {
				printf(KERR "unable to allocate %u bytes for QLZ decompressor\n",
					sizeof(qlz_state_decompress));

				return 1;
			}

			printf(KPROC3(QLZ) "0x%08x => 0x%08x ...\n", image_address, decomp_image);

			/* go go go! */
			qlz_decompress(
				(char*)image_address,
				(char*)decomp_image,
				state_decompress);

			free((void*)state_decompress);
		}
		else
		{
			printf(KERR "Unrecognized compression type\n");
			return 1;
		}

		image_address = (uint32_t)decomp_image;
		image_size = command->decomp_size;	
	}
	else {
		image_size = blob_size;
	}

	if (flags & kMachDriver) {
		/*
		 * Don't need to map drivers, but we must maintain
		 * a list of all loaded driver binaries for later.
		 */
		loaded_driver_image_t* this;

		/* Sanity */
		if (command->info_offset > image_size) {
			printf(KERR "Malformed load command (InfoOffset > ImageSize)");
			return 1;
		}

		this = malloc(sizeof(loaded_driver_image_t));

		assert(this);

		if (!is_compressed) {
			/*
			 * If not compressed, we need to copy the driver into
			 * the kernel memory.
			 */
			bcopy((void*)image_address, (void*)raw_image_dest, image_size);
		}

		this->range.base = gKernelMemoryTop;
		this->range.size = (image_size + DRIVER_PAD_START);

		/* Is it a bare Info.plist */
		if (flags & kCommandMachOFlags_NoExec)
			this->has_exec = FALSE;
		else
			this->has_exec = TRUE;

		/* Copy as bundle name */
		bcopy((void*)&command->name, (void*)&this->name, NAME_LEN);

		if (flags & kCommandMachOFlags_HasInfoPlist) {
			this->info_offset = command->info_offset;
		}
		else {
			if (flags & kCommandMachOFlags_NoExec) {
				printf(KERR "NoExec driver has no info.plist");
				free((void*)this);
				return 1;
			}
			else {
				this->info_offset = 0;
			}
		}

		this->next = gLoadedDriverImages;
		gLoadedDriverImages = this;

		increment_kernel_memory(this->range.size);

		if (flags & kCommandMachOFlags_NoExec) {
			printf(KDONE "loaded pure Info.plist driver '%s'\n", (const char*)&command->name);
		}
		else {
			printf(KDONE "loaded Info.plist/Exec driver '%s'\n", (const char*)&command->name);
		}
	}
	else if (flags & kMachKernel) {
		int ret;
		uint32_t entry_point;
		uint32_t size;

		/* Hand over to the mach-o loader */
		ret = load_macho(
			image_address,
			image_size,
			command->load_address,
			gKernelMemoryTop,
			&entry_point,
			&size
		);

		if (!ret) {
			/* Failed to map */
			printf(KERR "failed to map kernel\n");
			return 1;
		}

		gKernelMemoryRange.base = gKernelMemoryTop;
		gKernelMemoryRange.size = size;
		gKernelEntryPoint = entry_point;

		increment_kernel_memory(size);

		printf(KDONE "loaded kernel '%s' (ep=%08x)\n", (const char*)&command->name, entry_point);
	}
	else {
		printf(KERR "unsupported mach-o type (want driver or kernel)\n");
		return 1;
	}

	return 0;
}

static int load_general_image(uint32_t image_address, uint32_t image_size);

/* DT parsers */
extern boolean_t parse_xml_device_tree(uint32_t base);
extern boolean_t parse_jsdt_device_tree(uint32_t base);

static int parse_xdt_command(command_t* cmd)
{
	uint32_t base = (uint32_t)(cmd+1);
	boolean_t ret;

	if (gHasDeviceTree) {
		/* not fatal */
		printf(KWARN "a device tree is already loaded, skipping\n");
		return 0;
	}

	if (!assert_kernel_load())
		return 1;

	/*
	 * Parse straight away to avoid having to keep the DT blob
	 * in memory and risking having it overwritten.
	 */
	ret = parse_xml_device_tree(base);
	if (!ret) {
		return 1;
	}

	gHasDeviceTree = TRUE;

	return 0;
}

static int parse_jsdt_command(command_t* cmd)
{
	uint32_t base = (uint32_t)(cmd+1);
	boolean_t ret;

	if (gHasDeviceTree) {
		/* not fatal */
		printf(KWARN "a device tree is already loaded, skipping\n");
		return 0;
	}

	if (!assert_kernel_load())
		return 1;

	/*
	 * Parse straight away to avoid having to keep the DT blob
	 * in memory and risking having it overwritten.
	 */
	ret = parse_jsdt_device_tree(base);
	if (!ret) {
		return 1;
	}

	gHasDeviceTree = TRUE;

	return 0;
}

static int parse_table_of_contents(table_of_contents_t* toc)
{
	uint32_t left_cmds = toc->ncmds;
	command_t* cmd = (command_t*)(toc+1);

	printf(KINF "toc@%08x: %u load commands\n", (uint32_t)toc, left_cmds);

	while (left_cmds--) {
		int ret = 0;

		if (cmd->magic == kTableOfContentsMagic) {
			printf(KERR "ToC within a ToC is not allowed\n");
			ret = 1;
		}
		else if (cmd->magic == kCommandMachO) {
			ret = load_macho_from_command((command_macho_t*)cmd);
		}
		else if (cmd->magic == kCommandXMLDeviceTree) {
			ret = parse_xdt_command(cmd);
		}
		else if (cmd->magic == kCommandJSDeviceTree) {
			ret = parse_jsdt_command(cmd);
		}
		else {
			printf(KERR "load command 0x%08x is unknown\n", cmd->magic);
			ret = 1;
		}

		if (ret) {
			return ret;
		}

		cmd = (command_t*)((uint32_t)cmd + (uint32_t)cmd->size);
	}

	return 0;
}

static int load_general_image(uint32_t image_address, uint32_t image_size)
{
	uint32_t* image_magic_ptr = (uint32_t*)image_address;
	uint32_t image_magic = *image_magic_ptr;
	char* cm = (char*)&image_magic;

	printf(
		KINF "image at 0x%08x, magic %c%c%c%c\n",
		image_address,
		cm[0],
		cm[1],
		cm[2],
		cm[3]
	);

	if (image_magic == kCommandMachO) {
		command_macho_t* cmd = (command_macho_t*)image_address;
		return load_macho_from_command(cmd);
	}
	else if (image_magic == kCommandXMLDeviceTree) {
		command_t* cmd = (command_t*)image_address;
		return parse_xdt_command(cmd);
	}
	else if (image_magic == kCommandJSDeviceTree) {
		command_t* cmd = (command_t*)image_address;
		return parse_jsdt_command(cmd);
	}
	else if (image_magic == kTableOfContentsMagic) {
		table_of_contents_t* toc = (table_of_contents_t*)image_address;
		return parse_table_of_contents(toc);
	}
	else {
		printf(KERR "unknown image type (hex: 0x%08X)\n", image_magic);
		return 1;
	}
}

/*---------------------------------------------------------------*/

static int do_imgx(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	unsigned long addr;

	if (argc != 2) {
		printf(KERR "wrong number of arguments (got %d)\n", argc);
		return 1;
	}

	if (strcmp(argv[1], "last") == 0 ||
		strcmp(argv[1], "l") == 0)
	{
		/* last loaded */
		addr = (unsigned long)last_fileaddr();

		if (!addr) {
			printf(KERR "last address is NULL\n");
			return 1;
		}
	}
	else
	{
		/* abs address */
		addr = simple_strtoul(argv[1], NULL, 16);
	}

	return load_general_image((uint32_t)addr, 0);
}

static char imgx_help_text[] =
	"\t  imgx - can load either a TOC, MachO command or a an XML DT command.\n";

U_BOOT_CMD(
	imgx,	CONFIG_SYS_MAXARGS,	1,	do_imgx,
	"load an image", imgx_help_text
);

/*---------------------------------------------------------------*/

static int do_rdx(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	uint32_t addr;
	uint32_t size;
	struct HFSPlusVolumeHeader* hdr;

	addr = last_fileaddr();
	size = last_filesize();

	if (!addr) {
		printf(KERR "last loaded address is NULL\n");
		return 1;
	}
	if (!size) {
		printf(KERR "last loaded filesize is NULL\n");
		return 1;
	}

	/* uhhh, uboot */
	if (addr != gKernelMemoryTop) {
		printf(KERR "ramdisk loaded at the wrong address (use KernelMemoryTop env var)\n");
		printf(KERR "  (0x%08x instead of 0x%08x)\n", addr, gKernelMemoryTop);
		return 1;
	}

	/* validate the ramdisk */
	if (size < (1024 + sizeof(struct HFSPlusVolumeHeader))) {
		printf(KERR "loaded ramdisk too small to be valid HFS+ dmg (0x%08x bytes)\n", size);
		return 1;
	}

	hdr = (struct HFSPlusVolumeHeader*)(addr+1024);

	if (OSSwapInt16(hdr->signature) != kHFSPlusSigWord &&
		OSSwapInt16(hdr->signature) != kHFSSigWord) {
		printf(KERR "bad HFS+ signature (got 0x%08x wanted 'H+' or 'HX')\n", (uint32_t)OSSwapInt16(hdr->signature));
		return 1;
	}

	gRAMDiskRange.base = addr;
	gRAMDiskRange.size = size;

	increment_kernel_memory(size);

	printf(KDONE "loaded dmg [0x%08x-0x%08x, %u files, %u dirs]\n",
		addr,
		addr+size,
		OSSwapInt32(hdr->fileCount),
		OSSwapInt32(hdr->folderCount));

	return 0;
}

static char rdx_help_text[] =
	"\t  rdx - call after ramdisk load to add it to kernel memory\n";

U_BOOT_CMD(
	rdx,	CONFIG_SYS_MAXARGS,	1,	do_rdx,
	"add RDSK to kernel memory", rdx_help_text
);