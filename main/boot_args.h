/*
 * boot_args.h
 * Copyright (c) 2012 Kristina Brooks
 *
 * Boot args structure for the XNU ARM kernel. Passed to
 * the kernel at boot time.
 */

#ifndef _BOOT_ARGS_H
#define _BOOT_ARGS_H

#include <bootkit/runtime.h>

#define BOOT_LINE_LENGTH        256

struct Boot_Video {
	unsigned long	v_baseAddr;	/* Base address of video memory */
	unsigned long	v_display;	/* Display Code (if Applicable */
	unsigned long	v_rowBytes;	/* Number of bytes per pixel row */
	unsigned long	v_width;	/* Width */
	unsigned long	v_height;	/* Height */
	unsigned long	v_depth;	/* Pixel Depth and other parameters */
};

#define kBootVideoDepthMask		(0xFF)
#define kBootVideoDepthDepthShift	(0)
#define kBootVideoDepthRotateShift	(8)
#define kBootVideoDepthScaleShift	(16)

typedef struct Boot_Video Boot_Video;

#define kBootArgsRevision		1

#define kBootArgsVersion1		1
#define kBootArgsVersion2		2
#define kBootArgsVersion3		3

#define kDeviceTreeMagic 0xBABE5A55

typedef struct {
	uint16_t		revision; /* revision */
	uint16_t		version; /* version */
	
	uint32_t		virt_base; /* identity mapping */
	uint32_t		phys_base; /* sdram base */
	uint32_t		mem_size; /* sdram size */
	uint32_t		data_end; /* end of kernel stuff */
	
	Boot_Video		video; /* Video Information */
	uint32_t		machine; /* machine ID */
	
	void			*dt_base; /* base of DT */
	uint32_t		dt_size; /* size of DT */
	
	char			args[BOOT_LINE_LENGTH];	/* command line args */
} boot_args_t;

#endif
