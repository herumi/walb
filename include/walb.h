/**
 * General definitions for Walb.
 *
 * @author HOSHINO Takashi <hoshino@labs.cybozu.co.jp>
 */
#ifndef _WALB_H
#define _WALB_H

#define WALB_VERSION 1

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/kdev_t.h>
#include "inttypes_kern.h"
#ifdef WALB_DEBUG
#define ASSERT(cond) BUG_ON(!(cond))
#else /* WALB_DEBUG */
#define ASSERT(cond)
#endif /* WALB_DEBUG */
#else /* __KERNEL__ */
#include "userland.h"
#include <assert.h>
#include <string.h>
#define ASSERT(cond) assert(cond)
#endif /* __KERNEL__ */

/**
 * Print macro for debug.
 */
#ifdef __KERNEL__
#include <linux/kernel.h>
#define PRINT(flag, fmt, args...) printk(flag fmt, ##args)
#define PRINT_E(fmt, args...) PRINT(KERN_ERR, fmt, ##args)
#define PRINT_W(fmt, args...) PRINT(KERN_WARNING, fmt, ##args)
#define PRINT_N(fmt, args...) PRINT(KERN_NOTICE, fmt, ##args)
#define PRINT_I(fmt, args...) PRINT(KERN_INFO, fmt, ##args)
#ifdef WALB_DEBUG
#define PRINT_D(fmt, args...) PRINT(KERN_DEBUG, fmt, ##args)
#else
#define PRINT_D(fmt, args...)
#endif
#define PRINTV_E(fmt, args...) PRINT_E("walb(%s) " fmt, __func__, ##args)
#define PRINTV_W(fmt, args...) PRINT_W("walb(%s) " fmt, __func__, ##args)
#define PRINTV_N(fmt, args...) PRINT_N("walb(%s) " fmt, __func__, ##args)
#define PRINTV_I(fmt, args...) PRINT_I("walb(%s) " fmt, __func__, ##args)
#define PRINTV_D(fmt, args...) PRINT_D("walb(%s:%d) " fmt, __func__, __LINE__, ##args)
#else /* __KERNEL__ */
#include <stdio.h>
#ifdef WALB_DEBUG
#define PRINT_D(fmt, args...) printf(fmt, ##args)
#else
#define PRINT_D(fmt, args...)
#endif
#define PRINT_E(fmt, args...) fprintf(stderr, fmt, ##args)
#define PRINT_W PRINT_E
#define PRINT_N PRINT_E
#define PRINT_I PRINT_E
#define PRINT(flag, fmt, args...) printf(fmt, ##args)
#define PRINTV_E(fmt, args...) PRINT_E("ERROR(%s) " fmt, __func__, ##args)
#define PRINTV_W(fmt, args...) PRINT_W("WARNING(%s) " fmt, __func__, ##args)
#define PRINTV_N(fmt, args...) PRINT_N("NOTICE(%s) " fmt, __func__, ##args)
#define PRINTV_I(fmt, args...) PRINT_I("INFO(%s) " fmt, __func__, ##args)
#define PRINTV_D(fmt, args...) PRINT_D("DEBUG(%s:%d) " fmt, __func__, __LINE__, ##args)
#endif /* __KERNEL__ */

/**
 * Memory allocator/deallocator.
 */
#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/slab.h>
#define MALLOC(size, mask) kmalloc(size, mask)
#define ZALLOC(size, mask) kzalloc(size, mask)
#define REALLOC(p, size, mask) krealloc(p, size, mask)
#define FREE(p) kfree(p)
#define AMALLOC(size, align, mask) kmalloc((align > size ? align : size), mask)
#else
#include <stdlib.h>
#define MALLOC(size, mask) malloc(size)
#define ZALLOC(size, mask) calloc(1, size)
#define REALLOC(p, size, mask) realloc(p, size)
static inline void* amalloc(size_t size, size_t align)
{
        void *p; return (posix_memalign(&p, align, size) == 0 ? p : NULL);
}
#define AMALLOC(size, align, mask) amalloc(size, align)
#define FREE(p) free(p)
#endif

/**
 * Disk name length.
 */
#define DISK_NAME_LEN_USER 32
#ifdef __KERNEL__
#include <linux/genhd.h>
#else
#define DISK_NAME_LEN DISK_NAME_LEN_USER
#endif
#define ASSERT_DISK_NAME_LEN() ASSERT(DISK_NAME_LEN == DISK_NAME_LEN_USER)

/**
 * Device name prefix/suffix.
 *
 * walb control: /dev/walb/control
 * walb device: /dev/walb/NAME
 * walblog device: /dev/walb/NAME_log
 */
#define WALB_NAME "walb"
#define WALB_DIR_NAME "walb"
#define WALB_CONTROL_NAME "control"
#define WALBLOG_NAME_SUFFIX "_log"
#define WALB_CONTROL_PATH "/dev/" WALB_DIR_NAME "/" WALB_CONTROL_NAME

/**
 * Maximum length of the device name.
 * This must include WALB_DIR_NAME, "/" and '\0' terminator.
 *
 * walb device file:    ("%s/%s",  WALB_DIR_NAME, name)
 * walblog device file: ("%s/L%s", WALB_DIR_NAME, name)
 */
#define WALB_DEV_NAME_MAX_LEN (DISK_NAME_LEN - sizeof(WALB_DIR_NAME) - 3)

/**
 * Identification to confirm sector type (u16).
 */
#define SECTOR_TYPE_SUPER           0x0001
#define SECTOR_TYPE_SNAPSHOT        0x0002
#define SECTOR_TYPE_LOGPACK         0x0003
#define SECTOR_TYPE_WALBLOG_HEADER  0x0004

/**
 * Constants for lsid.
 */
#define INVALID_LSID ((u64)(-1))
#define MAX_LSID     ((u64)(-2))

/**
 * Calculate checksum incrementally.
 */
static inline u64 checksum_partial(u64 sum, const u8 *data, u32 size)
{
        u32 n = size / sizeof(u32);
        u32 i;

        ASSERT(size % sizeof(u32) == 0);

        for (i = 0; i < n; i ++) {
                sum += *(u32 *)(data + (sizeof(u32) * i));
        }
        return sum;
}

/**
 * Finish checksum.
 */
static inline u32 checksum_finish(u64 sum)
{
        u32 ret;
        
        ret = ~(u32)((sum >> 32) + (sum << 32 >> 32)) + 1;
        return (ret == (u32)(-1) ? 0 : ret);
}

/**
 * Calclate checksum of byte array.
 */
static inline u32 checksum(const u8 *data, u32 size)
{
        return checksum_finish(checksum_partial(0, data, size));
}

/**
 * Sprint uuid.
 *
 * @buf buffer to store result. Its size must be 16 * 2 + 1.
 * @uuid uuid ary. Its size must be 16.
 */
static inline void sprint_uuid(char *buf, const u8 *uuid)
{
        char tmp[3];
        int i;

        buf[0] = '\0';
        for (i = 0; i < 16; i ++) {
                sprintf(tmp, "%02x", uuid[i]);
                strcat(buf, tmp);
        }
}

/**
 * Determine whether a bit is set.
 *
 * @nr bit number to test. 0 <= nr < 63.
 * @bits pointer to a u64 value as a bit array.
 *
 * @return On: non-zero, off: 0.
 */
static inline int test_u64bits(int nr, const u64 *bits)
{
        ASSERT(0 <= nr && nr < 64);
        return (((*bits) & ((u64)(1) << nr)) != 0);
}

/**
 * Set a bit of u64 bits.
 */
static inline void set_u64bits(int nr, u64 *bits)
{
        ASSERT(0 <= nr && nr < 64);
        (*bits) |= ((u64)(1) << nr);
}

/**
 * Clear a bit of u64 bits.
 */
static inline void clear_u64bits(int nr, u64 *bits)
{
        ASSERT(0 <= nr && nr < 64);
        (*bits) &= ~((u64)(1) << nr);
}

#endif /* _WALB_H */
