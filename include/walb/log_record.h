/**
 * Definitions for Walb log record.
 *
 * @author HOSHINO Takashi <hoshino@labs.cybozu.co.jp>
 */
#ifndef WALB_LOG_RECORD_H
#define WALB_LOG_RECORD_H

#include "walb.h"
#include "util.h"
#include "checksum.h"

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * Definition of structures.
 *******************************************************************************/

enum {
	LOG_RECORD_EXIST = 0,
	LOG_RECORD_PADDING, /* Non-zero if this is padding log */
	LOG_RECORD_DISCARD, /* Discard IO */
};

/**
 * Log record.
 */
struct walb_log_record {

	/* (4 + 4) + 8 + (2 + 2 + 4) + 8 = 32 bytes */

	/* Just sum of the array assuming data contents
	   as an array of u32 integer.
	   If is_padding non-zero, checksum is not calcurated.
	   You must use the salt that is unique for each device. */
	u32 checksum;

	/* Flags with LOG_RECORD_XXX indicators. */
	u32 flags;

	/* IO offset [logical sector]. */
	u64 offset;

	/* IO size [logical sector].
	   512B * (65K - 1) = (32M-512)B is the maximum request size. */
	u16 io_size;

	/* Local sequence id as the data offset in the log record.
	   lsid - lsid_local is logpack lsid. */
	u16 lsid_local;

	u32 reserved1;

	/* Log sequence id of the record. */
	u64 lsid;

} __attribute__((packed));

/**
 * Logpack header data inside sector.
 *
 * sizeof(struct walb_logpack_header) <= walb_super_sector.sector_size.
 */
struct walb_logpack_header {

	/* Checksum of the logpack header.
	   You must use the salt that is unique for each device. */
	u32 checksum;

	/* Type identifier */
	u16 sector_type;

	/* Total io size in the log pack [physical sector].
	   Log pack size is total_io_size + 1.
	   Discard request's size is not included. */
	u16 total_io_size;

	/* logpack lsid [physical sector]. */
	u64 logpack_lsid;

	/* Number of log records in the log pack.
	   This includes padding records also. */
	u16 n_records;

	/* Number of padding record. 0 or 1. */
	u16 n_padding;

	u32 reserved1;

	struct walb_log_record record[0];
	/* continuous records */

} __attribute__((packed));


/*******************************************************************************
 * Macros.
 *******************************************************************************/

#define MAX_TOTAL_IO_SIZE_IN_LOGPACK_HEADER ((1U << 16) - 1)

#define ASSERT_LOG_RECORD(rec) ASSERT(is_valid_log_record(rec))

/**
 * NOT TESTED YET.
 *
 * for each macro for records in a logpack.
 *
 * int i;
 * struct walb_log_record *lrec;
 * struct walb_logpack_header *lhead;
 */
#define for_each_logpack_record(i, lrec, lhead)				\
	for (i = 0; i < lhead->n_records && ({lrec = &lhead->record[i]; 1;}); i++)

/*******************************************************************************
 * Prototype of static inline functions.
 *******************************************************************************/

static inline unsigned int max_n_log_record_in_sector(unsigned int pbs);
static inline void log_record_init(struct walb_log_record *rec);
static inline int is_valid_log_record(struct walb_log_record *rec);
static inline int is_valid_logpack_header(const struct walb_logpack_header *lhead);
static inline u64 get_next_lsid(const struct walb_logpack_header *lhead);

/*******************************************************************************
 * Definition of static inline functions.
 *******************************************************************************/

/**
 * Get number of log records that a log pack can store.
 * @pbs physical block size.
 */
static inline unsigned int max_n_log_record_in_sector(unsigned int pbs)
{
	ASSERT(pbs > sizeof(struct walb_logpack_header));
	return (pbs - sizeof(struct walb_logpack_header)) /
		sizeof(struct walb_log_record);
}

/**
 * Initialize a log record.
 */
static inline void log_record_init(struct walb_log_record *rec)
{
	ASSERT(rec);
	memset(rec, 0, sizeof(*rec));
}

/**
 * This is for validation of log record.
 *
 * @return Non-zero if valid, or 0.
 */
static inline int is_valid_log_record(struct walb_log_record *rec)
{
	CHECK(rec);
	CHECK(test_bit_u32(LOG_RECORD_EXIST, &rec->flags));

	CHECK(rec->io_size > 0);
	CHECK(rec->lsid_local > 0);
	CHECK(rec->lsid <= MAX_LSID);

	return 1; /* valid */
error:
	return 0; /* invalid */
}

/**
 * Check validness of a logpack header.
 * This does not validate checksum.
 *
 * @logpack logpack to be checked.
 *
 * @return Non-zero in success, or 0.
 */
static inline int is_valid_logpack_header(
	const struct walb_logpack_header *lhead)
{

	CHECK(lhead);
	CHECK(lhead->sector_type == SECTOR_TYPE_LOGPACK);
	if (lhead->n_records == 0) {
		CHECK(lhead->total_io_size == 0);
		CHECK(lhead->n_padding == 0);
	} else {
#if 0
		/* If All records are DISCARD, then total_io_size will be 0. */
		CHECK(lhead->total_io_size > 0);
#endif
		CHECK(lhead->n_padding < lhead->n_records);
	}
	return 1;
error:
	LOGe("log pack header is invalid "
		"(n_records: %u total_io_size %u sector_type %u).\n",
		lhead->n_records, lhead->total_io_size,
		lhead->sector_type);
	return 0;
}

/**
 * Check validness of a logpack header.
 *
 * @logpack logpack to be checked.
 * @pbs physical block size.
 *   (This is logpack header size.)
 *
 * @return Non-zero in success, or 0.
 */
static inline int is_valid_logpack_header_with_checksum(
	const struct walb_logpack_header* lhead, unsigned int pbs, u32 salt)
{
	CHECKL(error0, is_valid_logpack_header(lhead));
	if (lhead->n_records > 0) {
		CHECKL(error1, checksum((const u8 *)lhead, pbs, salt) == 0);
	}
	return 1;
error0:
	return 0;
error1:
	LOGe("logpack header checksum is invalid (lsid %"PRIu64").\n",
		lhead->logpack_lsid);
	return 0;
}

/**
 * Get next lsid of a logpack header.
 * This does not validate the logpack header.
 */
static inline u64 get_next_lsid_unsafe(const struct walb_logpack_header *lhead)
{
	if (lhead->total_io_size == 0 && lhead->n_records == 0) {
		return lhead->logpack_lsid;
	} else {
		return lhead->logpack_lsid + 1 + lhead->total_io_size;
	}
}

/**
 * Get next lsid of a logpack header.
 */
static inline u64 get_next_lsid(const struct walb_logpack_header *lhead)
{
	ASSERT(is_valid_logpack_header(lhead));
	return get_next_lsid_unsafe(lhead);
}

#ifdef __cplusplus
}
#endif

#endif /* WALB_LOG_RECORD_H */
