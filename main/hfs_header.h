#ifndef _H_HFS_HEADER
#define _H_HFS_HEADER

/* Signatures used to differentiate between HFS and HFS Plus volumes */
enum {
	kHFSSigWord		= 0x4244,	/* 'BD' in ASCII */
	kHFSPlusSigWord		= 0x482B,	/* 'H+' in ASCII */
	kHFSXSigWord		= 0x4858,	/* 'HX' in ASCII */

	kHFSPlusVersion		= 0x0004,	/* 'H+' volumes are version 4 only */
	kHFSXVersion		= 0x0005,	/* 'HX' volumes start with version 5 */

	kHFSPlusMountVersion	= 0x31302E30,	/* '10.0' for Mac OS X */
	kHFSJMountVersion	= 0x4846534a,	/* 'HFSJ' for journaled HFS+ on OS X */
	kFSKMountVersion	= 0x46534b21	/* 'FSK!' for failed journal replay */
};

struct HFSPlusVolumeHeader {
	uint16_t 	signature;		/* == kHFSPlusSigWord */
	uint16_t 	version;		/* == kHFSPlusVersion */
	uint32_t 	attributes;		/* volume attributes */
	uint32_t 	lastMountedVersion;	/* implementation version which last mounted volume */
	uint32_t 	journalInfoBlock;	/* block addr of journal info (if volume is journaled, zero otherwise) */

	uint32_t 	createDate;		/* date and time of volume creation */
	uint32_t 	modifyDate;		/* date and time of last modification */
	uint32_t 	backupDate;		/* date and time of last backup */
	uint32_t 	checkedDate;		/* date and time of last disk check */

	uint32_t 	fileCount;		/* number of files in volume */
	uint32_t 	folderCount;		/* number of directories in volume */

	uint32_t 	blockSize;		/* size (in bytes) of allocation blocks */
	uint32_t 	totalBlocks;		/* number of allocation blocks in volume (includes this header and VBM*/
	uint32_t 	freeBlocks;		/* number of unused allocation blocks */

	uint32_t 	nextAllocation;		/* start of next allocation search */
	uint32_t 	rsrcClumpSize;		/* default resource fork clump size */
	uint32_t 	dataClumpSize;		/* default data fork clump size */
	uint32_t 	nextCatalogID;		/* next unused catalog node ID */

	uint32_t 	writeCount;		/* volume write count */
	uint64_t 	encodingsBitmap;	/* which encodings have been use  on this volume */

	uint8_t 	finderInfo[32];		/* information used by the Finder */
};

#endif