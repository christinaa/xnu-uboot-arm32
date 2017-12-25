/*
 * mach_boot.c
 * Copyright (c) 2013 Kristina Brooks
 *
 * This code is responsible for initializing the kernel
 * after all of the bits have been loaded.
 */

#include <bootkit/runtime.h>

#include <bootkit/xml.h>
#include <bootkit/device_tree.h>
#include <bootkit/mach-o/macho.h>

#include <command.h>
#include <video_fb.h>

#include "boot_args.h"

#include "loader.h"

/* Driver info for IOKit */
struct DriverInfo {
	char *plistAddr;
	long plistLength;
	void *executableAddr;
	long executableLength;
	void *bundlePathAddr;
	long bundlePathLength;
};

/* Tracking kernel memory */
static memory_region_t __kernel_mem_store;
static memory_region_t* kernel_mem = &__kernel_mem_store;

/* 
 * 'memory-map' range types
 *
 * kBootDriverTypeKEXT are for in-memory kexts that are
 * picked up by kxld once the kernel is up and running.
 *
 * If a range named 'RAMDisk' is present, the BSD layer will
 * root from that ramdisk instead of finding a root by matching
 * IOKit stuff.
 */
enum {
	kBootDriverTypeInvalid = 0,
	kBootDriverTypeKEXT = 1,
	kBootDriverTypeMKEXT = 2
};

/*
 * Apple's device tree is weird. Objects parents are referenced
 * by the 'AAPL,phandle' property. 
 */

/*--------------------------------------------------------------------*/

#define KERNEL_PHYS (gKernelPhysicalBase)
#define KERNEL_VMADDR (gKernelVirtualBase)

/* Phys to kernel virt */
#define ptokv(addr) (((uint32_t)addr - (uint32_t)KERNEL_PHYS) + KERNEL_VMADDR)
#define kvtop(addr) (((uint32_t)addr - KERNEL_VMADDR) + (uint32_t)KERNEL_PHYS)

/*--------------------------------------------------------------------*/
/* device tree stuff */

void flatten_device_tree(memory_range_t* range)
{
	uint8_t* dt_base;
	uint32_t len;
	
	/* First, find out the length */
	DT__FlattenDeviceTree(NULL, &len);
	
	len += 4; /* magic */

	/* Allocate kernel memory for DT */
	dt_base = (uint8_t*)memory_reserve(kernel_mem, len, 0);
	
	/* write DT magic for debugging */
	*((uint32_t*)dt_base) = kDeviceTreeMagic;
	dt_base += 4;

	printf(KPROC(DTRE) "flattening (0x%08x) ...\n", dt_base);

	/**/
	DT__FlattenDeviceTree((void**)&dt_base, &len);
	
	range->base = (uint32_t)dt_base;
	range->size = len;
}

/*--------------------------------------------------------------------*/
/* boot args */

boot_args_t* allocate_boot_args(memory_range_t* range)
{
	boot_args_t* args;
	
	/* Allocate memory for boot args */
	args = (boot_args_t*)
	memory_reserve(kernel_mem, sizeof(boot_args_t), 0);
	
	args->revision = kBootArgsRevision;
	args->version = kBootArgsVersion3;
	
	range->base = (uintptr_t)args;
	range->size = sizeof(boot_args_t);
	
	return args;
}

/*--------------------------------------------------------------------*/
/* memory map ranges */

boolean_t allocate_memory_range(Node* memory_map, char * rangeName, long start, long length, long type)
{
    char *nameBuf;
    uint32_t *buffer;
    
    nameBuf = malloc(strlen(rangeName) + 1);
    if (nameBuf == 0) return false;
    strcpy(nameBuf, rangeName);
    
    buffer = malloc(2 * sizeof(uint32_t));
    if (buffer == 0) return false;
    
    buffer[0] = start;
    buffer[1] = length;
    
    DT__AddProperty(memory_map, nameBuf, 2 * sizeof(uint32_t), (char *)buffer);
    
    return true;
}

boolean_t enter_memory_range(Node* memory_map, char* prefix, uint32_t id, uint32_t type, memory_range_t* range)
{
	char* range_name;
	if (id != 0) {
		char range_buf[32];
		sprintf(range_buf, "%s-%x", prefix, id);
		range_name = (char*)&range_buf;
	}
	else {
		range_name = prefix;
	}
	
	/* Allocate and enter the range */
	return allocate_memory_range(memory_map, range_name, range->base, range->size, type);
}

/*--------------------------------------------------------------------*/
/* memory map */

Node* create_memory_map(void)
{
	Node* root = DT__RootNode();
	Node* chosen;
	Node* memory_map;
	
	/* /chosen/memory-map */
	chosen = DT__AddChild(root, "chosen");
	memory_map = DT__AddChild(chosen, "memory-map");
	
	return memory_map;
}

boolean_t map_add_ramdisk(Node* memory_map, memory_range_t* ramdisk_range)
{
	printf(KINF "adding ramdisk [0x%08x, sz=0x%08x] to mem map\n",
			ramdisk_range->base,
			ramdisk_range->size);

	return
	enter_memory_range(memory_map,
					 "RAMDisk",
					 0,
					 kBootDriverTypeInvalid,
					 ramdisk_range);
}

static boolean_t map_booter_extension(Node* memory_map, loaded_driver_image_t* image)
{
	/*
	 * The good, old booter extension mechanism which allows 
	 * driver loading by passing them in the device tree.
	 *
	 * This is based on drivers.c from the x86 boot friends.
	 */

	struct DriverInfo* driver;
	char* bundle_name;

	uint32_t actual_base;
	uint32_t actual_size;

	/*
	 * We have a pad region in front of every driver image, but
	 * let's check that we have enough, because of paranoia.
	 */
	if ((sizeof(*driver)+NAME_LEN) > DRIVER_PAD_START) {
		printf(KERR "DRIVER_PAD_START is too small. please fix this.\n");
		return false;
	}

	/*
	 * Drivers MUST have an Info.plist
	 */
	if (!image->info_offset && image->has_exec) {
		printf(KERR "driver %s has no Info.plist\n", (const char*)(&image->name));
		return false;
	}

	/* Pointers to the padded region */

	driver = (struct DriverInfo*)(image->range.base);
	bundle_name = (char*)(driver+1);

	/* Skip the padding at the front of the image */

	actual_base = image->range.base + DRIVER_PAD_START;
	actual_size = image->range.size - DRIVER_PAD_START;

	/* Driver info passed to XNU */

	if (image->has_exec) {
		driver->executableAddr = (char*)(actual_base);
		driver->executableLength = image->info_offset;
	}
	else {
		/*
		 * Some drivers do not have an executable, for 
		 * example System.kext. (uh, wtf)
		 */
		driver->executableAddr = (char*)(0);
		driver->executableLength = 0;
	}
	
	driver->plistAddr = (char*)(actual_base + image->info_offset);
	driver->plistLength = actual_size - image->info_offset;

	/* Bundle name should have been sanitized before */

	bcopy((void*)&image->name, (void*)bundle_name, NAME_LEN);

	driver->bundlePathAddr = (void*)(bundle_name);
	driver->bundlePathLength = strlen(bundle_name);

	if (strncmp(driver->plistAddr, "<?xml", 5) != 0) {
		printf(KWARN "%s has a strange info.plist (starts with %.*s)\n",
			bundle_name,
			5,
			driver->plistAddr);
	}

	printf(KPROC(KEXT) "%s E[0x%08x 0x%x] I[0x%08x 0x%x]\n",
			bundle_name,
			driver->executableAddr,
			driver->executableLength,
			driver->plistAddr,
			driver->plistLength
		);

	/* Enter into the memory map */
	enter_memory_range(
			memory_map,
			"Driver",
			(uint32_t)driver,
			kBootDriverTypeKEXT,
			&image->range
		);

	/* Done */
	return true;
}

boolean_t map_add_drivers(Node* memory_map)
{

	loaded_driver_image_t* next;
	unsigned int driver_cnt = 0;

	next = gLoadedDriverImages;
	if (!next) {
		printf(KINF "no kexts are loaded\n");
		return true;
	}

	do {
		loaded_driver_image_t* this = next;

		if (!map_booter_extension(memory_map, this)) {
			return false;
		}

		driver_cnt += 1;
		next = this->next;
	} while(next != NULL);

	printf(KINF "%u kext(s) loaded\n", driver_cnt);

	/* Free the loaded driver list */
	teardown_loaded_driver_images();

	return true;
}

boolean_t map_add_info(Node* memory_map,
							memory_range_t* boot_args_range,
							memory_range_t* kernel_range)
{
	memory_range_t iBoot_range;
	
	boolean_t ret;
	
	ret = 
	enter_memory_range(memory_map,
					 "iBoot",
					 0,
					 kBootDriverTypeInvalid,
					 &iBoot_range);
	
	if (!ret) { return false; }
	
	/* Enter boot args range */
	ret = 
	enter_memory_range(memory_map,
					 "BootArgs",
					 0,
					 kBootDriverTypeInvalid,
					 boot_args_range);
	
	if (!ret) { return false; }
	
	/* Add a kernel range, just for the sake of it */
	ret = 
	enter_memory_range(memory_map,
					 "Kernel",
					 0,
					 kBootDriverTypeInvalid,
					 kernel_range);
	
	if (!ret) { return false; }
	
	return true;
}

/*--------------------------------------------------------------------*/

static void call_kernel(uint32_t entry_point, uint32_t boot_args_ptr)
{
	__asm__ __volatile__(
		"mov r5, %0\n"
		"mov r0, %1\n"
		"blx r5\n"
		:: "r" (entry_point), "r" (boot_args_ptr)
	);
}

static void exit_boot_services(void)
{
	disable_interrupts();

#ifdef CONFIG_NETCONSOLE
	eth_halt();
#endif

#if defined(CONFIG_CMD_USB)
	usb_stop();
#endif

	arch_preboot_os();
}

/*--------------------------------------------------------------------*/
/* actual loader */

static int mach_boot(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	boolean_t ret;

	boot_args_t* args;
	Node* memory_map;

	memory_range_t r_boot_args;
	memory_range_t r_device_tree;
	uint32_t vm_boot_args, kernel_entry_point;

	uint32_t kernel_image_slide;
	uint32_t virtual_base;

	/*---------------------------------------------------------------*/

	if (!gHasDeviceTree) {
		printf(KERR "device tree is not loaded - load one before starting the kernel\n");
		return 1;
	}

	/*---------------------------------------------------------------*/

	printf(KINF "kmem start=0x%08x size=0x%08x\n",
		gKernelMemoryRange.base,
		gKernelMemoryTop - gKernelMemoryRange.base);

	kernel_mem->pos = gKernelMemoryTop;
	kernel_mem->base = gKernelPhysicalBase;
	kernel_mem->down = false;

	/*---------------------------------------------------------------*/
	/**** boot args ****/
	printf(KPROC(BOOT) "allocating boot args ...\n");
	args = allocate_boot_args(&r_boot_args);
	
	/*---------------------------------------------------------------*/
	/**** MemoryMap ****/
	printf(KPROC(BOOT) "init memory map ...\n");

	memory_map = create_memory_map();
	assert(memory_map);
	
	ret =
	map_add_info(memory_map,
				 &r_boot_args,
				 &gKernelMemoryRange);

	if (!ret) {
		printf(KERR "map_add_info\n");
		goto out_err;
	}

	ret =
	map_add_drivers(memory_map);

	if (!ret) {
		printf(KERR "map_add_drivers\n");
		goto out_err;
	}

	if (!RANGE_IS_NULL(gRAMDiskRange))
	{
		ret =
		map_add_ramdisk(memory_map, &gRAMDiskRange);

		if (!ret) {
			printf(KERR "map_add_ramdisk\n");
			goto out_err;
		}
	}
	
	/*---------------------------------------------------------------*/
	/**** flatten DT ****/
	
	flatten_device_tree(&r_device_tree);

	printf(KPROC(DTRE) "Final DT [%08x-%08x]\n",
		r_device_tree.base,
		r_device_tree.size+r_device_tree.base);
	
	/*---------------------------------------------------------------*/
	/**** pad kernel memory for initial L1s ****/
	kernel_mem->pos = align_up(kernel_mem->pos, 0x100000);

	/*---------------------------------------------------------------*/
	/**** Populate the boot_args structure ****/
	printf(KPROC(BOOT) "populating boot args ...\n");

	printf(KINF "phys_base=0x%08x virt_base=0x%08x\n",
		KERNEL_PHYS,
		KERNEL_VMADDR);

	args->phys_base = KERNEL_PHYS;
	args->virt_base = KERNEL_VMADDR;
	
	args->dt_base = (void*)ptokv(r_device_tree.base);
	args->dt_size = r_device_tree.size;
	
	args->mem_size = (total_memory_size());
	args->data_end = (kernel_mem->pos);

	/*---------------------------------------------------------------*/
	/*
	 * These two we need for the actual kernel call.
	 */
	kernel_entry_point = kvtop(gKernelEntryPoint);
	vm_boot_args = ptokv(r_boot_args.base);
	
out_err:
	printf(KPROC(BOOT) "DT__Finalize\n");
	DT__Finalize();

	if (!ret) {
		return 1;
	}

	printf(KDONE "starting kernel at 0x%08x ...\n", kernel_entry_point);

	exit_boot_services();
	call_kernel(kernel_entry_point, (uint32_t)vm_boot_args);

	return 0;
}

static char darwin_help_text[] =
	"\t  mach_boot - Takes no arguments. Memory ranges have to be populated prior.\n";

U_BOOT_CMD(
	mach_boot,	CONFIG_SYS_MAXARGS,	1,	mach_boot,
	"boot previously loaded mach kernel", darwin_help_text
);