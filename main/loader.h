
#ifndef _LOADER_H
#define _LOADER_H

#define NAME_LEN 64

typedef struct _loaded_driver_image_t {
	memory_range_t range;
	uint32_t info_offset;
	struct _loaded_driver_image_t* next;
	boolean_t has_exec;
	char name[NAME_LEN];
} loaded_driver_image_t;

#define DRIVER_PAD_START 256

/* All of these are physical */
extern loaded_driver_image_t* gLoadedDriverImages;
extern memory_range_t gKernelMemoryRange;
extern memory_range_t gRAMDiskRange;
extern uint32_t gKernelEntryPoint; 
extern uint32_t gKernelMemoryTop;

extern uint32_t gKernelVirtualBase;
extern uint32_t gKernelPhysicalBase;

extern boolean_t gHasDeviceTree;

extern void teardown_loaded_driver_images(void);

#endif