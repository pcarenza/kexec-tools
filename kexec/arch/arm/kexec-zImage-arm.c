/*
 * - 08/21/2007 ATAG support added by Uli Luckas <u.luckas@road.de>
 *
 */
#define _GNU_SOURCE
#define _XOPEN_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include <getopt.h>
#include <unistd.h>
#include <libfdt.h>
#include <arch/options.h>
#include "../../kexec.h"
#include "../../kexec-syscall.h"
#include "kexec-arm.h"
#include "../../fs2dt.h"
#include "crashdump-arm.h"

#define BOOT_PARAMS_SIZE 1536

off_t initrd_base = 0, initrd_size = 0;
char *atags_file = NULL;

struct tag_header {
	uint32_t size;
	uint32_t tag;
};

/* The list must start with an ATAG_CORE node */
#define ATAG_CORE       0x54410001

struct tag_core {
	uint32_t flags;	    /* bit 0 = read-only */
	uint32_t pagesize;
	uint32_t rootdev;
};

/* it is allowed to have multiple ATAG_MEM nodes */
#define ATAG_MEM	0x54410002

struct tag_mem32 {
	uint32_t   size;
	uint32_t   start;  /* physical start address */
};

/* describes where the compressed ramdisk image lives (virtual address) */
/*
 * this one accidentally used virtual addresses - as such,
 * it's deprecated.
 */
#define ATAG_INITRD     0x54410005

/* describes where the compressed ramdisk image lives (physical address) */
#define ATAG_INITRD2    0x54420005

struct tag_initrd {
        uint32_t start;    /* physical start address */
        uint32_t size;     /* size of compressed ramdisk image in bytes */
};

/* ETERNITYPROJECT START: Define the DEVTREE tag */

#define ATAG_DEVTREE 0xF100040A
struct tag_devtree {
        uint32_t start;
        uint32_t size;
};


/* command line: \0 terminated string */
#define ATAG_CMDLINE    0x54410009

struct tag_cmdline {
	char    cmdline[1];     /* this is the minimum size */
};

/* The list ends with an ATAG_NONE node. */
#define ATAG_NONE       0x00000000

struct tag {
	struct tag_header hdr;
	union {
		struct tag_core	 core;
		struct tag_mem32	mem;
		struct tag_initrd       initrd;
		struct tag_devtree	devtree;
		struct tag_cmdline      cmdline;
	} u;
};

#define tag_next(t)     ((struct tag *)((uint32_t *)(t) + (t)->hdr.size))
#define byte_size(t)    ((t)->hdr.size << 2)
#define tag_size(type)  ((sizeof(struct tag_header) + sizeof(struct type) + 3) >> 2)

int zImage_arm_probe(const char *UNUSED(buf), off_t UNUSED(len))
{
	/* 
	 * Only zImage loading is supported. Do not check if
	 * the buffer is valid kernel image
	 */	
	return 0;
}

void zImage_arm_usage(void)
{
	printf(	"     --command-line=STRING Set the kernel command line to STRING.\n"
		"     --append=STRING       Set the kernel command line to STRING.\n"
		"     --initrd=FILE         Use FILE as the kernel's initial ramdisk.\n"
		"     --ramdisk=FILE        Use FILE as the kernel's initial ramdisk.\n"
		"     --dtb=FILE            Use FILE as the fdt blob.\n"
		"     --atags=FILE          Use FILE as ATAGs.\n"
		);
}

static
struct tag * atag_read_tags(void)
{
	static unsigned long buf[BOOT_PARAMS_SIZE];
	char def_fn[]= "/atags";
	char *fn = atags_file;
	FILE *fp;

	if (fn == NULL)
		fn = def_fn;

	fp = fopen(fn, "r");
	if (!fp) {
		fprintf(stderr, "Cannot open %s: %s\n", 
			fn, strerror(errno));
		return NULL;
	}

	if (!fread(buf, sizeof(buf[1]), BOOT_PARAMS_SIZE, fp)) {
		fclose(fp);
		return NULL;
	}

	if (ferror(fp)) {
		fprintf(stderr, "Cannot read %s: %s\n",
			fn, strerror(errno));
		fclose(fp);
		return NULL;
	}

	fclose(fp);
	return (struct tag *) buf;
}


static
int atag_arm_load(struct kexec_info *info, unsigned long base,
	const char *command_line, off_t command_line_len,
	const char *initrd, off_t initrd_len, off_t initrd_off,
	const char *devtree, off_t devtree_len, off_t devtree_off)
{
	struct tag *saved_tags = atag_read_tags();
	char *buf;
	off_t len;
	struct tag *params;
	uint32_t *initrd_start = NULL;
	uint32_t *devtree_start = NULL;

	buf = xmalloc(getpagesize());
	if (!buf) {
		fprintf(stderr, "Compiling ATAGs: out of memory\n");
		return -1;
	}

	memset(buf, 0xff, getpagesize());
	params = (struct tag *)buf;

	if (saved_tags) {
		// Copy tags
		saved_tags = (struct tag *) saved_tags;
		while(byte_size(saved_tags)) {
			switch (saved_tags->hdr.tag) {
			case ATAG_INITRD:
			case ATAG_INITRD2:
			case ATAG_DEVTREE:
			case ATAG_CMDLINE:
			case ATAG_NONE:
				// skip these tags
				break;
			default:
				// copy all other tags
				memcpy(params, saved_tags, byte_size(saved_tags));
				params = tag_next(params);
			}
			saved_tags = tag_next(saved_tags);
		}
	} else {
		params->hdr.size = 2;
		params->hdr.tag = ATAG_CORE;
		params = tag_next(params);
	}

	if (devtree) {
		printf("\nAdding Device Tree...\n");
		params->hdr.size = tag_size(tag_devtree);
		params->hdr.tag = ATAG_DEVTREE;
		devtree_start = &params->u.devtree.start;
		params->u.devtree.size = devtree_len;
		params = tag_next(params);
	}

	if (initrd) {
		printf("Adding initrd...\n");
		params->hdr.size = tag_size(tag_initrd);
		params->hdr.tag = ATAG_INITRD2;
		initrd_start = &params->u.initrd.start;
		params->u.initrd.size = initrd_len;
		params = tag_next(params);
	}

	if (command_line) {
		params->hdr.size = (sizeof(struct tag_header) + command_line_len + 3) >> 2;
		params->hdr.tag = ATAG_CMDLINE;
		memcpy(params->u.cmdline.cmdline, command_line,
			command_line_len);
		params->u.cmdline.cmdline[command_line_len - 1] = '\0';
		params = tag_next(params);
	}

	params->hdr.size = 0;
	params->hdr.tag = ATAG_NONE;

	len = ((char *)params - buf) + sizeof(struct tag_header);

	printf("Atags segment info: %lx (%d)\n", base, len);
	add_segment(info, buf, len, base, len);

	if (initrd) {
		*initrd_start = locate_hole(info, initrd_len, getpagesize(),
				initrd_off, ULONG_MAX, INT_MAX);
		if (*initrd_start == ULONG_MAX)
			return -1;
		printf("Initrd segment info: %lx (%d)\n", initrd_start, initrd_len);
		add_segment(info, initrd, initrd_len, *initrd_start, initrd_len);
	}

	if (devtree) {
		*devtree_start = locate_hole(info, devtree_len, getpagesize(),
				 devtree_off, ULONG_MAX, INT_MAX);
		if (*devtree_start == ULONG_MAX)
			return -1;
		printf("Devtree segment info: %lx (%d)\n", devtree_start, devtree_len);
		add_segment(info, devtree, devtree_len, *devtree_start, devtree_len);
	}

	return 0;
}

int zImage_arm_load(int argc, char **argv, const char *buf, off_t len,
	struct kexec_info *info)
{
	unsigned long base;
	unsigned int atag_offset = 0x1000; /* 4k offset from memory start */
	unsigned int offset = 0x8000;      /* 32k offset from memory start */
	const char *command_line;
	char *modified_cmdline = NULL;
	off_t command_line_len;
	const char *ramdisk;
	char *ramdisk_buf;
	int opt;
	char *dtb_buf;
	off_t dtb_length;
	char *dtb_file;
	off_t dtb_offset;

	/* See options.h -- add any more there, too. */
	static const struct option options[] = {
		KEXEC_ARCH_OPTIONS
		{ "command-line",	1, 0, OPT_APPEND },
		{ "append",		1, 0, OPT_APPEND },
		{ "initrd",		1, 0, OPT_RAMDISK },
		{ "ramdisk",	1, 0, OPT_RAMDISK },
		{ "devtree",	1, 0, OPT_DTB },
		{ "atags",		1, 0, OPT_ATAGS },
		{ 0, 			0, 0, 0 },
	};
	static const char short_options[] = KEXEC_ARCH_OPT_STR "a:r:";

	/*
	 * Parse the command line arguments
	 */
	command_line = 0;
	command_line_len = 0;
	ramdisk = 0;
	ramdisk_buf = 0;
	initrd_size = 0;
	dtb_file = NULL;
	while((opt = getopt_long(argc, argv, short_options, options, 0)) != -1) {
		switch(opt) {
		default:
			/* Ignore core options */
			if (opt < OPT_ARCH_MAX) {
				break;
			}
		case '?':
			usage();
			return -1;
		case OPT_APPEND:
			command_line = optarg;
			break;
		case OPT_RAMDISK:
			ramdisk = optarg;
			break;
		case OPT_DTB:
			dtb_file = optarg;
			break;
		case OPT_ATAGS:
			atags_file = optarg;
			break;
		}
	}
	if (command_line) {
		command_line_len = strlen(command_line) + 1;
		if (command_line_len > COMMAND_LINE_SIZE)
			command_line_len = COMMAND_LINE_SIZE;
	}
	if (ramdisk) {
		ramdisk_buf = slurp_file(ramdisk, &initrd_size);
	}

	/*
	 * If we are loading a dump capture kernel, we need to update kernel
	 * command line and also add some additional segments.
	 */
	if (info->kexec_flags & KEXEC_ON_CRASH) {
		uint64_t start, end;

		modified_cmdline = xmalloc(COMMAND_LINE_SIZE);
		if (!modified_cmdline)
			return -1;

		if (command_line) {
			(void) strncpy(modified_cmdline, command_line,
				       COMMAND_LINE_SIZE);
			modified_cmdline[COMMAND_LINE_SIZE - 1] = '\0';
		}

		if (load_crashdump_segments(info, modified_cmdline) < 0) {
			free(modified_cmdline);
			return -1;
		}

		command_line = modified_cmdline;
		command_line_len = strlen(command_line) + 1;

		/*
		 * We put the dump capture kernel at the start of crashkernel
		 * reserved memory.
		 */
		if (parse_iomem_single("Crash kernel\n", &start, &end)) {
			/*
			 * No crash kernel memory reserved. We cannot do more
			 * but just bail out.
			 */
			return -1;
		}
		base = start;
	} else {
		base = locate_hole(info,len+offset,0,0,ULONG_MAX,INT_MAX);
	}

	if (base == ULONG_MAX)
		return -1;

	printf("Kernel segment stats: %lx (%ld)\n", base, len);

	/* assume the maximum kernel compression ratio is 4,
	 * and just to be safe, place ramdisk after that
	 */
	initrd_base = base + (len * 4) + getpagesize();
	initrd_base &= ~(getpagesize() - 1);


	if (dtb_file) {
		dtb_buf = slurp_file(dtb_file, &dtb_length);
		if (fdt_check_header(dtb_buf) != 0) {
			fprintf(stderr, "Invalid FDT buffer.\n");
			return -1;
		}

		/* Stick the dtb at the end of the initrd and page
		 * align it.
		 */
		dtb_offset = initrd_base + initrd_size + getpagesize();
		dtb_offset &= ~(getpagesize() - 1);

		if (atag_arm_load(info, base + atag_offset,
                  command_line, command_line_len,
                  ramdisk_buf, initrd_size, initrd_base,
                  dtb_buf, dtb_length, dtb_offset) == -1)
			return -1;
	}
	else {
		if (ramdisk) {
			printf("Ramdisk segment: %lx (%ld)\n", initrd_base, initrd_size);
			add_segment(info, ramdisk_buf, initrd_size,
				        initrd_base, initrd_size);
		}
	}

	printf("Kernel segment info: %lx (%d)\n", base+offset, len);
	add_segment(info, buf, len, base + offset, len);

	info->entry = (void*)base + offset;

	return 0;
}
