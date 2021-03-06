/**
 * Control walb device.
 *
 * Copyright(C) 2010, Cybozu Labs, Inc.
 * @author HOSHINO Takashi <hoshino@labs.cybozu.co.jp>
 * @license 3-clause BSD, GPL version 2 or later.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#include "linux/walb/walb.h"
#include "linux/walb/logger.h"
#include "linux/walb/log_device.h"
#include "linux/walb/log_record.h"
#include "linux/walb/ioctl.h"

#include "random.h"
#include "util.h"
#include "walb_util.h"
#include "logpack.h"
#include "walb_log.h"
#include "version.h"

/*******************************************************************************
 * Macros.
 *******************************************************************************/

/* Buffer size for ioctl should be page size due to performance. */
#define BUFFER_SIZE 4096

/*******************************************************************************
 * Static data definition.
 *******************************************************************************/

/**
 * For memory allocation failure message.
 */
static const char NOMEM_STR[] = "Memory allocation failed.\n";

/**
 * Command-line configuration.
 */
struct config
{
	char *cmd_str; /* command string */
	char *ldev_name; /* log device name */
	char *ddev_name; /* data device name */

	/* size_t log_sector_size; /\* sector size of log device *\/ */
	/* size_t log_dev_size; /\* size of log device by the sector *\/ */

	/* Discard flags. */
	bool nodiscard;

	char *wdev_name; /* walb device */
	char *wldev_name;  /* walblog device */
	u64 lsid; /* lsid */

	u64 lsid0; /* from lsid */
	u64 lsid1; /* to lsid */

	char *name; /* name of stuff */

	size_t size; /* (size_t)(-1) means undefined. */

	/**
	 * Parameters to create_wdev.
	 */
	struct walb_start_param param;
};

/**
 * For command string to function.
 */
typedef bool (*command_fn)(const struct config *);

/**
 * Helper data for map command string to function.
 */
struct map_str_to_fn
{
	const char *str;
	command_fn fn;
};

/**
 * Options string.
 */
static const char *helpstr_options_ =
	"OPTIONS:\n"
	"  DISCARD: --nodiscard\n"
	"  SIZE:   --size [size of stuff]\n"
	"  LRANGE: --lsid0 [from lsid] --lsid1 [to lsid]\n"
	"  (NYI)TRANGE: --time0 [from time] --time1 [to time]\n"
	"  LSID:   --lsid [lsid]\n"
	"  DDEV:   --ddev [data device path]\n"
	"  LDEV:   --ldev [log device path]\n"
	"  WDEV:   --wdev [walb device path]\n"
	"  WLDEV:  --wldev [walblog device path]\n"
	"  NAME:   --name [name of stuff]\n"
	"  WLOG:   walb log data as stream\n"
	"  MAX_LOGPACK_KB: --max_logpack_kb [size]\n"
	"  MAX_PENDING_MB: --max_pending_mb [size] \n"
	"  MIN_PENDING_MB: --min_pending_mb [size]\n"
	"  QUEUE_STOP_TIMEOUT_MS: --queue_stop_timeout_ms [timeout]\n"
	"  FLUSH_INTERVAL_MB: --flush_interval_mb [size]\n"
	"  FLUSH_INTERVAL_MS: --flush_interval_ms [timeout]\n"
	"  N_PACK_BULK: --n_pack_bulk [size]\n"
	"  N_IO_BULK: --n_io_bulk [size]\n";

/**
 * Helper data structure for help command.
 */
struct cmdhelp
{
	const char *cmdline;
	const char *description;
};

/**
 * Help string.
 */
static struct cmdhelp cmdhelps_[] = {
	{ "format_ldev LDEV DDEV (NAME) (DISCARD)",
	  "Format log device." },
	{ "create_wdev LDEV DDEV (NAME)"
	  " (MAX_LOGPACK_KB) (MAX_PENDING_MB) (MIN_PENDING_MB)\n"
	  "             "
	  " (QUEUE_STOP_TIMEOUT_MS) (FLUSH_INTERVAL_MB) (FLUSH_INTERVAL_MB)\n"
	  "             "
	  " (N_PACK_BULK) (N_IO_BULK)",
	  "Make walb/walblog device." },
	{ "delete_wdev WDEV",
	  "Delete walb/walblog device." },
	{ "set_checkpoint_interval WDEV SIZE",
	  "Set checkpoint interval in [ms]." },
	{ "get_checkpoint_interval WDEV",
	  "Get checkpoint interval in [ms]." },
	{ "cat_wldev WLDEV (LRANGE) > WLOG",
	  "Extract wlog from walblog device." },
	{ "show_wldev WLDEV (LRANGE)",
	  "Show wlog in walblog device." },
	{ "show_wlog (LRANGE) < WLOG",
	  "Show wlog in stdin." },
	{ "redo_wlog DDEV (LRANGE) < WLOG",
	  "Redo wlog to data device." },
	{ "redo LDEV DDEV",
	  "Redo logs and get consistent data device." },
	{ "set_oldest_lsid WDEV LSID",
	  "Delete old logs in the device." },
	{ "get_oldest_lsid WDEV",
	  "Get oldest_lsid in the device." },
	{ "get_written_lsid WDEV",
	  "Get written_lsid in the device." },
	{ "get_permanent_lsid WDEV",
	  "Get permanent_lsid in the device." },
	{ "get_completed_lsid WDEV",
	  "Get completed_lsid in the device." },
	{ "search_valid_lsid WLDEV LSID SIZE",
	  "Search valid lsid which indicates a logpack header block." },
	{ "get_log_usage WDEV",
	  "Get log usage in the log device." },
	{ "get_log_capacity WDEV",
	  "Get log capacity in the log device." },
	{ "is_flush_capable WDEV",
	  "Check the device can accept flush requests." },
	{ "resize WDEV SIZE",
	  "Resize device capacity [logical block] (Only grow is allowed)."
	  " Specify --size 0 to auto-detect the size." },
	{ "reset_wal WDEV",
	  "Reset log device (and detect new log device size) online." },
	{ "is_log_overflow WDEV",
	  "Check log space overflow." },
	{ "freeze WDEV SIZE",
	  "Freeze a device. Specify SIZE for timeout [sec]." },
	{ "melt WDEV",
	  "Melt a frozen device." },
	{ "is_frozen WDEV",
	  "Check the device is frozen or not." },
	{ "get_version",
	  "Get walb driver version."},
	{ "version",
	  "Show walb version."},
};

/**
 * For getopt.
 */
enum
{
	OPT_LDEV = 1,
	OPT_DDEV,
	OPT_NODISCARD,
	OPT_WDEV,
	OPT_WLDEV,
	OPT_LSID,
	OPT_LSID0,
	OPT_LSID1,
	OPT_NAME,
	OPT_SIZE,
	OPT_MAX_LOGPACK_KB,
	OPT_MAX_PENDING_MB,
	OPT_MIN_PENDING_MB,
	OPT_QUEUE_STOP_TIMEOUT_MS,
	OPT_FLUSH_INTERVAL_MB,
	OPT_FLUSH_INTERVAL_MS,
	OPT_N_PACK_BULK,
	OPT_N_IO_BULK,
	OPT_HELP,
};

/*******************************************************************************
 * Prototype of static functions.
 *******************************************************************************/

/* Helper functions. */
static int close_(int fd);
static int fdatasync_(int fd);
static int fdatasync_and_close(int fd);
static void show_help(bool isShort);
static void init_config(struct config* cfg);
static int parse_opt(int argc, char* const argv[], struct config *cfg);
static bool init_walb_metadata(
	int fd, unsigned int lbs, unsigned int pbs,
	u64 ddev_lb, u64 ldev_lb, const char *name);
static bool invoke_ioctl(
	const char *wdev_name, struct walb_ctl *ctl, int open_flag);
static bool ioctl_and_print_bool(const char *wdev_name, int cmd);
static u64 get_ioctl_u64(const char* wdev_name, int command);
static bool dispatch(const struct config *cfg);
static struct walblog_header *create_and_read_wlog_header(int inFd);
static struct walb_super_sector *create_and_read_super_sector(
	struct sector_data **sectdp, int fd, unsigned int pbs);

/* commands. */
static bool do_format_ldev(const struct config *cfg);
static bool do_create_wdev(const struct config *cfg);
static bool do_delete_wdev(const struct config *cfg);
static bool do_take_checkpoint(const struct config *cfg);
static bool do_set_checkpoint_interval(const struct config *cfg);
static bool do_get_checkpoint_interval(const struct config *cfg);
static bool do_cat_wldev(const struct config *cfg);
static bool do_redo_wlog(const struct config *cfg);
static bool do_redo(const struct config *cfg);
static bool do_show_wlog(const struct config *cfg);
static bool do_show_wldev(const struct config *cfg);
static bool do_set_oldest_lsid(const struct config *cfg);
static bool do_get_oldest_lsid(const struct config *cfg);
static bool do_get_written_lsid(const struct config *cfg);
static bool do_get_permanent_lsid(const struct config *cfg);
static bool do_get_completed_lsid(const struct config *cfg);
static bool do_search_valid_lsid(const struct config *cfg);
static bool do_get_log_usage(const struct config *cfg);
static bool do_get_log_capacity(const struct config *cfg);
static bool do_is_flush_capable(const struct config *cfg);
static bool do_resize(const struct config *cfg);
static bool do_reset_wal(const struct config *cfg);
static bool do_is_log_overflow(const struct config *cfg);
static bool do_freeze(const struct config *cfg);
static bool do_melt(const struct config *cfg);
static bool do_is_frozen(const struct config *cfg);
static bool do_get_version(const struct config *cfg);
static bool do_version(const struct config *cfg);
static bool do_help(const struct config *cfg);

/*******************************************************************************
 * Command map.
 *******************************************************************************/

const struct map_str_to_fn cmd_map_[] = {
	{ "format_ldev", do_format_ldev },
	{ "create_wdev", do_create_wdev },
	{ "delete_wdev", do_delete_wdev },
	{ "take_checkpoint", do_take_checkpoint },
	{ "set_checkpoint_interval", do_set_checkpoint_interval },
	{ "get_checkpoint_interval", do_get_checkpoint_interval },
	{ "cat_wldev", do_cat_wldev },
	{ "show_wlog", do_show_wlog },
	{ "show_wldev", do_show_wldev },
	{ "redo_wlog", do_redo_wlog },
	{ "redo", do_redo },
	{ "set_oldest_lsid", do_set_oldest_lsid },
	{ "get_oldest_lsid", do_get_oldest_lsid },
	{ "get_written_lsid", do_get_written_lsid },
	{ "get_permanent_lsid", do_get_permanent_lsid },
	{ "get_completed_lsid", do_get_completed_lsid },
	{ "search_valid_lsid", do_search_valid_lsid },
	{ "get_log_usage", do_get_log_usage },
	{ "get_log_capacity", do_get_log_capacity },
	{ "is_flush_capable", do_is_flush_capable },
	{ "resize", do_resize },
	{ "reset_wal", do_reset_wal },
	{ "is_log_overflow", do_is_log_overflow },
	{ "freeze", do_freeze },
	{ "melt", do_melt },
	{ "is_frozen", do_is_frozen },
	{ "get_version", do_get_version },
	{ "version", do_version },
	{ "help", do_help },
};

/*******************************************************************************
 * Helper functions.
 *******************************************************************************/

/**
 * RETURN:
 *   0 in success.
 */
static int close_(int fd)
{
	int err = close(fd);
	if (err) {
		LOGe("close() failed with error: %s", strerror(errno));
	}
	return err;
}

/**
 * RETURN:
 *   0 in success.
 */
static int fdatasync_(int fd)
{
	int err = fdatasync(fd);
	if (err) {
		LOGe("fdatasync() failed with error: %s", strerror(errno));
	}
	return err;
}

/**
 * RETURN:
 *   0 in success.
 */
static int fdatasync_and_close(int fd)
{
	int err = fdatasync_(fd);
	if (err) {
		close_(fd);
		return err;
	}
	return close_(fd);
}

static void show_help(bool isShort)
{
	int size, i;

	printf("Usage: walbctl COMMAND OPTIONS\n"
		"COMMAND:\n");
	size = sizeof(cmdhelps_) / sizeof(struct cmdhelp);
	for (i = 0; i < size; i++) {
		if (isShort) {
			printf("  %s\n", cmdhelps_[i].cmdline);
		} else {
			printf("  %s\n"
				"      %s\n"
				, cmdhelps_[i].cmdline
				, cmdhelps_[i].description);
		}
	}
	printf("%s"
		"NIY: Not Implemented Yet.\n",
		helpstr_options_);
}


static void init_config(struct config* cfg)
{
	ASSERT(cfg != NULL);

	memset(cfg, 0, sizeof(struct config));

	cfg->nodiscard = false;

	cfg->lsid = (u64)(-1);
	cfg->lsid0 = (u64)(-1);
	cfg->lsid1 = (u64)(-1);

	cfg->size = (size_t)(-1);

	cfg->param.max_logpack_kb = 0;
	cfg->param.max_pending_mb = 32;
	cfg->param.min_pending_mb = 16;
	cfg->param.queue_stop_timeout_ms = 100;
	cfg->param.log_flush_interval_mb = 16;
	cfg->param.log_flush_interval_ms = 100;
	cfg->param.n_pack_bulk = 128;
	cfg->param.n_io_bulk = 1024;
}

/**
 * Parse options.
 */
static int parse_opt(int argc, char* const argv[], struct config *cfg)
{
	while (1) {
		int option_index = 0;
		static const struct option long_options[] = {
			{"ldev", 1, 0, OPT_LDEV}, /* log device */
			{"ddev", 1, 0, OPT_DDEV}, /* data device */
			{"nodiscard", 0, 0, OPT_NODISCARD},
			{"wdev", 1, 0, OPT_WDEV}, /* walb device */
			{"wldev", 1, 0, OPT_WLDEV}, /* walb log device */
			{"lsid", 1, 0, OPT_LSID}, /* lsid */
			{"lsid0", 1, 0, OPT_LSID0}, /* begin */
			{"lsid1", 1, 0, OPT_LSID1}, /* end */
			{"name", 1, 0, OPT_NAME},
			{"size", 1, 0, OPT_SIZE},
			{"max_logpack_kb", 1, 0, OPT_MAX_LOGPACK_KB},
			{"max_pending_mb", 1, 0, OPT_MAX_PENDING_MB},
			{"min_pending_mb", 1, 0, OPT_MIN_PENDING_MB},
			{"queue_stop_timeout_ms", 1, 0, OPT_QUEUE_STOP_TIMEOUT_MS},
			{"flush_interval_mb", 1, 0, OPT_FLUSH_INTERVAL_MB},
			{"flush_interval_ms", 1, 0, OPT_FLUSH_INTERVAL_MS},
			{"n_pack_bulk", 1, 0, OPT_N_PACK_BULK},
			{"n_io_bulk", 1, 0, OPT_N_IO_BULK},
			{"help", 0, 0, OPT_HELP},
			{0, 0, 0, 0}
		};

		int c = getopt_long(argc, argv, "", long_options, &option_index);
		if (c == -1) {
			break;
		}
		switch (c) {
		case OPT_LDEV:
			cfg->ldev_name = optarg;
			LOGd("ldev: %s\n", optarg);
			break;
		case OPT_DDEV:
			cfg->ddev_name = optarg;
			LOGd("ddev: %s\n", optarg);
			break;
		case OPT_NODISCARD:
			cfg->nodiscard = true;
			break;
		case OPT_WDEV:
			cfg->wdev_name = optarg;
			break;
		case OPT_WLDEV:
			cfg->wldev_name = optarg;
			break;
		case OPT_LSID:
			cfg->lsid = atoll(optarg);
			break;
		case OPT_LSID0:
			cfg->lsid0 = atoll(optarg);
			break;
		case OPT_LSID1:
			cfg->lsid1 = atoll(optarg);
			break;
		case OPT_NAME:
			cfg->name = optarg;
			break;
		case OPT_SIZE:
			cfg->size = atoll(optarg);
			break;
		case OPT_MAX_LOGPACK_KB:
			cfg->param.max_logpack_kb = atoi(optarg);
			break;
		case OPT_MAX_PENDING_MB:
			cfg->param.max_pending_mb = atoi(optarg);
			break;
		case OPT_MIN_PENDING_MB:
			cfg->param.min_pending_mb = atoi(optarg);
			break;
		case OPT_QUEUE_STOP_TIMEOUT_MS:
			cfg->param.queue_stop_timeout_ms = atoi(optarg);
			break;
		case OPT_FLUSH_INTERVAL_MB:
			cfg->param.log_flush_interval_mb = atoi(optarg);
			break;
		case OPT_FLUSH_INTERVAL_MS:
			cfg->param.log_flush_interval_ms = atoi(optarg);
			break;
		case OPT_N_PACK_BULK:
			cfg->param.n_pack_bulk = atoi(optarg);
			break;
		case OPT_N_IO_BULK:
			cfg->param.n_io_bulk = atoi(optarg);
			break;
		case OPT_HELP:
			cfg->cmd_str = "help";
			return 0;
		default:
			LOGw("unknown option.\n");
		}
	}

	if (optind < argc) {
		char buf0[1024];
		sprintf(buf0, "command: ");
		int w0 = strlen(buf0);
		while (optind < argc) {
			char buf1[64];
			int w1 = snprintf(buf1, 64, "%s ", argv[optind]);
			if (64 <= w1) {
				show_help(true);
				return -1;
			}
			cfg->cmd_str = argv[optind];
			if (1024 <= w0 + w1) {
				show_help(true);
				return -1;
			}
			strncat(buf0, buf1, 1024 - w0);
			w0 += w1;
			optind++;
		}
		LOGd("%s\n", buf0);
	} else {
		show_help(true);
		return -1;
	}

	return 0;
}

/**
 * Initialize log device.
 *
 * @fd block device file descripter.
 * @lbs logical block size.
 * @pbs physical block size.
 * @ddev_lb device size [logical block].
 * @ldev_lb log device size [logical block]
 * @name name of the walb device, or NULL.
 *
 * RETURN:
 *   true in success, or false.
 */
static bool init_walb_metadata(
	int fd, unsigned int lbs, unsigned int pbs,
	u64 ddev_lb, u64 ldev_lb, const char *name)
{
	struct sector_data *super_sect;

	ASSERT(0 < fd);
	ASSERT(0 < lbs);
	ASSERT(0 < pbs);
	ASSERT(0 < ddev_lb);
	ASSERT(0 < ldev_lb);
	/* name can be null. */

	/* Alloc super sector. */
	super_sect = sector_alloc_zero(pbs);
	if (!super_sect) {
		LOGe("alloc sector failed.\n");
		return false;
	}

	/* Initialize super sector. */
	if (!init_super_sector(
			super_sect, lbs, pbs,
			ddev_lb, ldev_lb, name)) {
		LOGe("init super sector faield.\n");
		goto error1;
	}

	/* Write super sector */
	if (!write_super_sector(fd, super_sect)) {
		LOGe("write super sector failed.\n");
		goto error1;
	}

	/* Write invalid logpack not to run redo. */
	if (!write_invalid_logpack_header(fd, super_sect, 0)) {
		LOGe("write invalid logpack header for lsid 0 failed.\n");
		goto error1;
	}

#if 1
	/* Read super sector and print for debug. */
	sector_zeroclear(super_sect);
	if (!read_super_sector(fd, super_sect)) {
		goto error1;
	}
	print_super_sector(super_sect);
#endif

	if (fdatasync_(fd)) {
		perror("fdatasync failed.\n");
		goto error1;
	}
	sector_free(super_sect);
	return true;

error1:
	sector_free(super_sect);
	return false;
}

/**
 * Invoke ioctl to WALB_IOCTL_WDEV.
 *
 * @wdev_name walb device name.
 * @ctl data for input/output.
 * @open_flag open flag.
 *
 * RETURN:
 *   true in success, or false.
 */
static bool invoke_ioctl(const char *wdev_name, struct walb_ctl *ctl, int open_flag)
{
	int fd, ret;
	struct bdev_info wdev_info;

	if (!open_bdev_and_get_info(wdev_name, &wdev_info, &fd, open_flag)) {
		LOGe("check and open failed: %s.\n", wdev_name);
		return false;
	}

	ret = ioctl(fd, WALB_IOCTL_WDEV, ctl);
	if (ret < 0) {
		LOGe("invoke_ioctl: ioctl failed.\n");
		goto error1;
	}
	return close_(fd) == 0;

error1:
	close_(fd);
	return false;
}

/**
 * Invoke ioctl and print returned boolean value.
 *
 * RETURN:
 *   true in success, or false.
 */
static bool ioctl_and_print_bool(const char *wdev_name, int cmd)
{
	struct walb_ctl ctl = {
		.command = cmd,
		.u2k = { .buf_size = 0 },
		.k2u = { .buf_size = 0 },
	};

	if (!invoke_ioctl(wdev_name, &ctl, O_RDONLY)) {
		return false;
	}
	printf("%d\n", ctl.val_int);
	return true;
}

/**
 * Get u64 value using walb device ioctl.
 *
 * RETURN:
 *   The value in success, or (u64)(-1).
 */
static u64 get_ioctl_u64(const char* wdev_name, int command)
{
	struct walb_ctl ctl = {
		.command = command,
		.u2k = { .buf_size = 0 },
		.k2u = { .buf_size = 0 },
	};

	if (invoke_ioctl(wdev_name, &ctl, O_RDONLY)) {
		return ctl.val_u64;
	} else {
		return (u64)(-1);
	}
}

/**
 * Dispatch command.
 */
static bool dispatch(const struct config *cfg)
{
	int i;
	bool ret = false;
	const int array_size = sizeof(cmd_map_)/sizeof(cmd_map_[0]);

	ASSERT(cfg->cmd_str);

	for (i = 0; i < array_size; i++) {
		if (strcmp(cfg->cmd_str, cmd_map_[i].str) == 0) {
			ret = (*cmd_map_[i].fn)(cfg);
			break;
		}
	}
	return ret;
}

/**
 * Create and read walblog header.
 *
 * @inFd input file descriptor.
 *
 * RETURN:
 *   allocated and read walblog header, or NULL.
 */
static struct walblog_header *create_and_read_wlog_header(int inFd)
{
	struct walblog_header *wh;

	wh = (struct walblog_header *)malloc(WALBLOG_HEADER_SIZE);
	if (!wh) {
		LOGe("%s", NOMEM_STR);
		return NULL;
	}

	/* Read and print wlog header. */
	if (!read_data(inFd, (u8 *)wh, WALBLOG_HEADER_SIZE)) {
		LOGe("read failed.\n");
		goto error1;
	}

	/* Check wlog header. */
	if (!is_valid_wlog_header(wh)) {
		LOGe("wlog header invalid.\n");
		goto error1;
	}
	return wh;

error1:
	free(wh);
	return NULL;
}

/**
 * Create sector data and read from the log device.
 *
 * @sectdp pointer to sector data pointer (will be set).
 * @fd log device file descriptor.
 * @pbs physical block size.
 *
 * RETURN:
 *  super sector pointer in success, or NULL.
 */
static struct walb_super_sector *create_and_read_super_sector(
	struct sector_data **sectdp, int fd, unsigned int pbs)
{
	struct sector_data *sectd;
	u64 off;

	ASSERT(fd > 0);
	ASSERT(is_valid_pbs(pbs));

	sectd = sector_alloc(pbs);
	if (!sectd) {
		LOGe("memory allocation failed.\n");
		return NULL;
	}
	off = get_super_sector0_offset(pbs);
	if (!sector_read(fd, off, sectd)) {
		LOGe("read super sector0 failed.\n");
		goto error1;
	}
	if (!is_valid_super_sector(sectd)) {
		LOGe("read super sector is not valid.\n");
		goto error1;
	}
	*sectdp = sectd;
	return get_super_sector(sectd);

error1:
	sector_free(sectd);
	return NULL;
}

/*******************************************************************************
 * Commands.
 *******************************************************************************/

/**
 * Execute log device format.
 */
static bool do_format_ldev(const struct config *cfg)
{
	bool retb;
	unsigned int lbs, pbs;
	int fd;
	struct bdev_info ldev_info, ddev_info;

	ASSERT(cfg->cmd_str);
	ASSERT(strcmp(cfg->cmd_str, "format_ldev") == 0);

	/* Open the log device. */
	if (!open_bdev_and_get_info(cfg->ldev_name, &ldev_info, &fd, O_RDWR | O_DIRECT)) {
		LOGe("check and open failed: %s.\n", cfg->ldev_name);
		return false;
	}

	/* Get information of the data device. */
	if (!get_bdev_info(cfg->ddev_name, &ddev_info)) {
		LOGe("check and get info failed: %s.\n", cfg->ddev_name);
		goto error1;
	}
	if (!is_block_size_same(&ldev_info, &ddev_info)) {
		goto error1;
	}
	lbs = ldev_info.lbs;
	pbs = ldev_info.pbs;

	/* Check logical/physical block sizes. */
	LOGd("logical_bs: %u\n" "physical_bs: %u\n"
		"ddev_size: %" PRIu64 "\n" "ldev_size: %" PRIu64 "\n"
		, lbs, pbs, ddev_info.size, ldev_info.size);
	if (ldev_info.size % lbs != 0 || ddev_info.size % lbs != 0) {
		LOGe("device size is not multiple of lbs\n");
		goto error1;
	}

	/* Discard if necessary. */
	if (!cfg->nodiscard && is_discard_supported(fd)) {
		LOGn("Try to discard whole area of the log device...");
		if (!discard_whole_area(fd)) {
			LOGe("Discard whole area failed.\n");
			goto error1;
		}
		LOGn("done\n");
	}

	/* Initialize metadata. */
	retb = init_walb_metadata(
		fd, lbs, pbs,
		ddev_info.size / lbs,
		ldev_info.size / lbs,
		cfg->name);
	if (!retb) {
		LOGe("initialize walb log device failed.\n");
		goto error1;
	}
	return close_(fd) == 0;

error1:
	close_(fd);
	return false;
}

/**
 * Create walb device.
 */
static bool do_create_wdev(const struct config *cfg)
{
	struct bdev_info ldev_info, ddev_info;
	int fd, ret;
	struct walb_start_param u2k_param;
	struct walb_start_param k2u_param;
	struct walb_ctl ctl = {
		.command = WALB_IOCTL_START_DEV,
		.u2k = { .wminor = WALB_DYNAMIC_MINOR,
			 .buf_size = sizeof(struct walb_start_param),
			 .buf = (void *)&u2k_param, },
		.k2u = { .buf_size = sizeof(struct walb_start_param),
			 .buf = (void *)&k2u_param, },
	};

	ASSERT(cfg->cmd_str);
	ASSERT(strcmp(cfg->cmd_str, "create_wdev") == 0);

	/* Parameters check. */
	if (!is_walb_start_param_valid(&cfg->param)) {
		LOGe("Some parameters are not valid.\n");
		return false;
	}

	/*
	 * Get information of the block devices.
	 */
	if (!get_bdev_info(cfg->ldev_name, &ldev_info)) {
		LOGe("create_wdev: check log device failed.\n");
		return false;
	}
	if (!get_bdev_info(cfg->ddev_name, &ddev_info)) {
		LOGe("create_wdev: check data device failed.\n");
		return false;
	}

	/*
	 * Open control device.
	 */
	LOGd("control path: %s\n", WALB_CONTROL_PATH);
	fd = open(WALB_CONTROL_PATH, O_RDWR);
	if (fd < 0) {
		perror("open failed.");
		return false;
	}

	/*
	 * Make ioctl data.
	 */
	memcpy(&u2k_param, &cfg->param, sizeof(struct walb_start_param));
	if (cfg->name) {
		snprintf(u2k_param.name, DISK_NAME_LEN, "%s", cfg->name);
	} else {
		u2k_param.name[0] = '\0';
	}
	ctl.u2k.lmajor = MAJOR(ldev_info.devt);
	ctl.u2k.lminor = MINOR(ldev_info.devt);
	ctl.u2k.dmajor = MAJOR(ddev_info.devt);
	ctl.u2k.dminor = MINOR(ddev_info.devt);

	print_walb_ctl(&ctl); /* debug */

	ret = ioctl(fd, WALB_IOCTL_CONTROL, &ctl);
	if (ret < 0) {
		LOGe("create_wdev: ioctl failed with error %d.\n",
			ctl.error);
		goto error1;
	}
	ASSERT(ctl.error == 0);
	ASSERT(strnlen(k2u_param.name, DISK_NAME_LEN) < DISK_NAME_LEN);
	printf("create_wdev is done successfully.\n"
		"name: %s\n"
		"major: %u\n"
		"minor: %u\n",
		k2u_param.name,
		ctl.k2u.wmajor, ctl.k2u.wminor);
	if (close_(fd)) {
		return false;
	}
	print_walb_ctl(&ctl); /* debug */
	return true;

error1:
	close_(fd);
	return false;
}

/**
 * Delete walb device.
 */
static bool do_delete_wdev(const struct config *cfg)
{
	struct bdev_info wdev_info;
	int fd, ret;
	struct walb_ctl ctl = {
		.command = WALB_IOCTL_STOP_DEV,
		.u2k = { .buf_size = 0, },
		.k2u = { .buf_size = 0, },
	};

	ASSERT(cfg->cmd_str);
	ASSERT(strcmp(cfg->cmd_str, "delete_wdev") == 0);

	/* Check devices. */
	if (!get_bdev_info(cfg->wdev_name, &wdev_info)) {
		LOGe("Check target walb device failed.\n");
		return false;
	}

	/* Open control device. */
	fd = open(WALB_CONTROL_PATH, O_RDWR);
	if (fd < 0) {
		perror("open failed.");
		return false;
	}

	/* Make ioctl data. */
	ctl.u2k.wmajor = MAJOR(wdev_info.devt);
	ctl.u2k.wminor = MINOR(wdev_info.devt);

	/* Invoke ioctl. */
	ret = ioctl(fd, WALB_IOCTL_CONTROL, &ctl);
	if (ret < 0) {
		LOGe("delete_wdev: ioctl failed with error %d.\n",
			ctl.error);
		goto error1;
	}
	ASSERT(ctl.error == 0);
	LOGn("delete_wdev is done successfully.\n");
	if (close_(fd)) {
		return false;
	}
	return true;

error1:
	close_(fd);
	return false;
}

/**
 * Make checkpointo immediately.
 */
static bool do_take_checkpoint(const struct config *cfg)
{
	struct walb_ctl ctl = {
		.command = WALB_IOCTL_TAKE_CHECKPOINT,
		.u2k = { .buf_size = 0 },
		.k2u = { .buf_size = 0 },
	};
	if (!invoke_ioctl(cfg->wdev_name, &ctl, O_RDWR)) {
		LOGe("Take snapshot failed\n");
		return false;
	}
	return true;
}

/**
 * Set checkpoint interval.
 */
static bool do_set_checkpoint_interval(const struct config *cfg)
{
	struct walb_ctl ctl = {
		.command = WALB_IOCTL_SET_CHECKPOINT_INTERVAL,
		.val_u32 = (u32)cfg->size,
		.u2k = { .buf_size = 0 },
		.k2u = { .buf_size = 0 },
	};

	ASSERT(strcmp(cfg->cmd_str, "set_checkpoint_interval") == 0);

	if (cfg->size == (size_t)(-1)) {
		LOGe("Specify checkpoint interval.\n");
		return false;
	}
	if (cfg->size > (size_t)UINT32_MAX) {
		LOGe("Given interval is too big.\n");
		return false;
	}

	if (!invoke_ioctl(cfg->wdev_name, &ctl, O_RDWR)) {
		return false;
	}
	LOGn("checkpoint interval is set to %"PRIu32" successfully.\n",
		ctl.val_u32);
	return true;
}

/**
 * Get checkpoint interval.
 */
static bool do_get_checkpoint_interval(const struct config *cfg)
{
	struct walb_ctl ctl = {
		.command = WALB_IOCTL_GET_CHECKPOINT_INTERVAL,
		.u2k = { .buf_size = 0 },
		.k2u = { .buf_size = 0 },
	};

	ASSERT(strcmp(cfg->cmd_str, "get_checkpoint_interval") == 0);
	if (!invoke_ioctl(cfg->wdev_name, &ctl, O_RDWR)) {
		return false;
	}
	printf("checkpoint interval is %"PRIu32".\n", ctl.val_u32);
	return true;
}

/**
 * Cat logpack in specified range.
 */
static bool do_cat_wldev(const struct config *cfg)
{
	bool retb;
	struct bdev_info wldev_info;
	int fd;
	unsigned int pbs;
	struct sector_data *super_sectd;
	struct walb_super_sector *super;
	const size_t bufsize = 1024 * 1024; /* 1MB */
	struct logpack *pack;
	u64 lsid, oldest_lsid, begin_lsid, end_lsid;
	u32 salt;
	u8 buf[WALBLOG_HEADER_SIZE];
	struct walblog_header *wh = (struct walblog_header *)buf;

	ASSERT(cfg->cmd_str);
	ASSERT(strcmp(cfg->cmd_str, "cat_wldev") == 0);

	if (!open_bdev_and_get_info(cfg->wldev_name, &wldev_info, &fd, O_RDONLY | O_DIRECT)) {
		return false;
	}
	pbs = wldev_info.pbs;

	/* Create and read super sector. */
	super = create_and_read_super_sector(&super_sectd, fd, pbs);
	if (!super) {
		goto error1;
	}
	salt = super->log_checksum_salt;

	/* Allocate memory. */
	pack = alloc_logpack(pbs, bufsize / pbs);
	if (!pack) {
		goto error2;
	}

	/* Range check */
	oldest_lsid = super->oldest_lsid;
	LOGd("oldest_lsid: %"PRIu64"\n", oldest_lsid);
	if (cfg->lsid0 == (u64)(-1)) {
		begin_lsid = oldest_lsid;
	} else {
		begin_lsid = cfg->lsid0;
	}
	if (cfg->lsid0 < oldest_lsid) {
		LOGe("given lsid0 %"PRIu64" < oldest_lsid %"PRIu64"\n",
			cfg->lsid0, oldest_lsid);
		goto error3;
	}
	end_lsid = cfg->lsid1;
	if (begin_lsid > end_lsid) {
		LOGe("lsid0 < lsid1 property is required.\n");
		goto error3;
	}

	/* Prepare and write walblog_header. */
	memset(wh, 0, WALBLOG_HEADER_SIZE);
	wh->header_size = WALBLOG_HEADER_SIZE;
	wh->sector_type = SECTOR_TYPE_WALBLOG_HEADER;
	wh->checksum = 0;
	wh->version = WALB_LOG_VERSION;
	wh->log_checksum_salt = salt;
	wh->logical_bs = wldev_info.lbs;
	wh->physical_bs = pbs;
	copy_uuid(wh->uuid, super->uuid);
	wh->begin_lsid = begin_lsid;
	wh->end_lsid = end_lsid;
	/* Checksum */
	wh->checksum = checksum((const u8 *)wh, WALBLOG_HEADER_SIZE, 0);
	/* Write */
	if (!write_data(1, buf, WALBLOG_HEADER_SIZE)) {
		goto error3;
	}
	LOGd("lsid %"PRIu64" to %"PRIu64"\n", begin_lsid, end_lsid);

	/* Write each logpack to stdout. */
	lsid = begin_lsid;
	while (lsid < end_lsid) {
		unsigned int invalid_idx;
		bool should_break = false;
		struct walb_logpack_header *logh = pack->header;

		/* Logpack header */
		retb = read_logpack_header_from_wldev(
			fd, super, lsid, salt, pack->sectd);
		if (!retb) { break; }
		LOGd_("logpack %"PRIu64"\n", logh->logpack_lsid);

		/* Realloc buffer if buffer size is not enough. */
		if (!resize_logpack_if_necessary(
				pack, logh->total_io_size)) {
			goto error3;
		}

		/* Read and write logpack data. */
		invalid_idx = read_logpack_data_from_wldev(
			fd, super, logh, salt, pack->sectd_ary);
		if (invalid_idx == 0) { break; }
		if (invalid_idx < logh->n_records) {
			LOGn("shrinked from %u to %u records.\n"
				, logh->n_records, invalid_idx);
			shrink_logpack_header(
				logh, invalid_idx, pbs, salt);
			should_break = true;
		}

		/* Write logpack header and data. */
		retb = write_data(1, (u8 *)logh, pbs);
		if (!retb) {
			LOGe("write logpack header failed.\n");
			goto error3;
		}

		retb = sector_array_write(
			1, pack->sectd_ary, 0, logh->total_io_size);
		if (!retb) {
			LOGe("write logpack data failed.\n");
			goto error3;
		}

		if (should_break) { break; }
		lsid += logh->total_io_size + 1;
	}

	/* Write termination block. */
	retb = write_end_logpack_header(1, pbs, salt);
	if (!retb) {
		LOGe("write end block failed.\n");
		goto error3;
	}

	free_logpack(pack);
	sector_free(super_sectd);
	return close_(fd) == 0;

error3:
	free_logpack(pack);
error2:
	sector_free(super_sectd);
error1:
	close_(fd);
	return false;
}

/**
 * Redo wlog.
 *
 * wlog is read from stdin.
 * --ddev (required)
 * --lsid0 (optional, default is the first lsid in the wlog.)
 * --lsid1 (optional, default is the last lsid in the wlog.)
 * redo logs of lsid0 <= lsid < lsid1.
 */
static bool do_redo_wlog(const struct config *cfg)
{
	int fd;
	struct walblog_header *wh;
	u32 salt;
	struct bdev_info ddev_info;
	unsigned int lbs, pbs;
	u64 lsid, begin_lsid, end_lsid;
	struct logpack *pack;
	const size_t bufsize = 1024 * 1024; /* 1MB */

	ASSERT(cfg->cmd_str);
	ASSERT(strcmp(cfg->cmd_str, "redo_wlog") == 0);

	/* Check and open the data device. */
	if (!open_bdev_and_get_info(cfg->ddev_name, &ddev_info, &fd, O_RDWR | O_DIRECT)) {
		return false;
	}
	lbs = ddev_info.lbs;
	pbs = ddev_info.pbs;

	/* Read wlog header. */
	wh = create_and_read_wlog_header(0);
	if (!wh) {
		goto error1;
	}
	salt = wh->log_checksum_salt;
	print_wlog_header(wh); /* debug */

	/* Check block sizes of the device. */
	if (lbs != wh->logical_bs || pbs != wh->physical_bs) {
		LOGe("block size check failed: ddev (%u, %u) wh (%u, %u)\n"
			, lbs, pbs, wh->logical_bs, wh->physical_bs);
		goto error2;
	}

	/* Deside begin_lsid and end_lsid. */
	if (cfg->lsid0 == (u64)(-1)) {
		begin_lsid = wh->begin_lsid;
	} else {
		begin_lsid = cfg->lsid0;
	}
	if (cfg->lsid1 == (u64)(-1)) {
		end_lsid = wh->end_lsid;
	} else {
		end_lsid = cfg->lsid1;
	}

	/* Allocate logpack. */
	pack = alloc_logpack(pbs, bufsize / pbs);
	if (!pack) {
		goto error2;
	}

	lsid = begin_lsid;
	while (lsid < end_lsid) {
		struct walb_logpack_header *logh = pack->header;

		/* Read logpack header */
		if (!read_logpack_header(0, pbs, salt, logh)) {
			break;
		}
		if (is_end_logpack_header(logh)) {
			break;
		}

		/* Read logpack data. */
		if (!resize_logpack_if_necessary(
				pack, logh->total_io_size)) {
			goto error3;
		}
		if (!read_logpack_data(
				0, logh, salt, pack->sectd_ary)) {
			LOGe("read logpack data failed.\n");
			goto error3;
		}

		/* Decision of skip and end. */
		lsid = logh->logpack_lsid;
		if (lsid < begin_lsid) { continue; }
		if (end_lsid <= lsid) { break; }
		LOGd_("logpack %"PRIu64"\n", lsid);

		/* Redo */
		if (!redo_logpack(fd, logh, pack->sectd_ary)) {
			LOGe("redo_logpack failed.\n");
			goto error3;
		}
	}
	free_logpack(pack);
	free(wh);
	return fdatasync_and_close(fd) == 0;

error3:
	free_logpack(pack);
error2:
	free(wh);
error1:
	close_(fd);
	return false;
}

/**
 * Redo
 *
 * Redo logs which are still not written to data device
 * and get consistent data device.
 * Please run this command before creating walb device if need.
 *
 * --ldev (required)
 * --ddev (required)
 */
static bool do_redo(const struct config *cfg)
{
	unsigned int pbs;
	struct bdev_info ldev_info, ddev_info;
	int lfd, dfd;
	struct sector_data *super_sectd;
	struct walb_super_sector *super;
	struct logpack *pack;
	u32 salt;
	const size_t bufsize = 1024 * 1024; /* 1MB */
	u64 lsid, begin_lsid, end_lsid;
	int ret0, ret1;

	ASSERT(cfg->cmd_str);
	ASSERT(strcmp(cfg->cmd_str, "redo") == 0);

	if (!open_bdev_and_get_info(cfg->ldev_name, &ldev_info, &lfd, O_RDWR | O_DIRECT)) {
		return false;
	}
	if (!open_bdev_and_get_info(cfg->ddev_name, &ddev_info, &dfd, O_RDWR | O_DIRECT)) {
		goto error1;
	}
	if (!is_block_size_same(&ldev_info, &ddev_info)) {
		goto error1;
	}
	pbs = ldev_info.pbs;

	/* Read super sector. */
	super = create_and_read_super_sector(&super_sectd, lfd, pbs);
	if (!super) {
		goto error2;
	}
	super = get_super_sector(super_sectd);
	salt = super->log_checksum_salt;

	/* Allocate logpack data. */
	pack = alloc_logpack(pbs, bufsize / pbs);
	if (!pack) {
		goto error3;
	}

	lsid = super->written_lsid;
	begin_lsid = lsid;
	/* Read logpack header */
	while (read_logpack_header_from_wldev(
			lfd, super, lsid, salt, pack->sectd)) {
		unsigned int invalid_idx;
		bool should_break = false;
		struct walb_logpack_header *logh = pack->header;

		LOGd_("logpack %"PRIu64"\n", logh->logpack_lsid);

		/* Realloc buf if bufsize is not enough. */
		if (!resize_logpack_if_necessary(pack, logh->total_io_size)) {
			goto error4;
		}

		/* Read logpack data from log device. */
		invalid_idx = read_logpack_data_from_wldev(
			lfd, super, logh, salt, pack->sectd_ary);

		if (invalid_idx == 0) { break; }
		if (invalid_idx < logh->n_records) {
			shrink_logpack_header(logh, invalid_idx, pbs, salt);
			should_break = true;
		}

		/* Write logpack to data device. */
		if (!redo_logpack(dfd, logh, pack->sectd_ary)) {
			LOGe("redo_logpack failed.\n");
			goto error4;
		}

		if (should_break) { break; }
		lsid += logh->total_io_size + 1;
	}

	/* Set new written_lsid and sync down. */
	end_lsid = lsid;
	super->written_lsid = end_lsid;
	if (!write_super_sector(lfd, super_sectd)) {
		LOGe("write super sector failed.\n");
		goto error4;
	}
	LOGn("Redo from lsid %"PRIu64" to %"PRIu64"\n",
		begin_lsid, end_lsid);

	/* Free resources. */
	free_logpack(pack);
	sector_free(super_sectd);

	/* Finalize block devices. */
	ret0 = fdatasync_and_close(dfd);
	ret1 = fdatasync_and_close(lfd);
	return ret0 == 0 && ret1 == 0;

error4:
	free_logpack(pack);
error3:
	sector_free(super_sectd);
error2:
	close_(dfd);
error1:
	close_(lfd);
	return false;
}

/**
 * Show wlog from stdin.
 */
static bool do_show_wlog(const struct config *cfg)
{
	struct walblog_header *wh;
	u32 salt;
	unsigned int pbs;
	struct logpack *pack;
	struct walb_logpack_header *logh;
	const size_t bufsize = 1024 * 1024; /* 1MB */
	u64 begin_lsid, end_lsid, lsid;
	u64 n_packs = 0, total_padding_size = 0;

	ASSERT(cfg->cmd_str);
	ASSERT(strcmp(cfg->cmd_str, "show_wlog") == 0);

	wh = create_and_read_wlog_header(0);
	if (!wh) { return false; }
	pbs = wh->physical_bs;
	salt = wh->log_checksum_salt;
	print_wlog_header(wh);

	pack = alloc_logpack(pbs, bufsize / pbs);
	if (!pack) { goto error1; }
	logh = pack->header;

	/* Range */
	if (cfg->lsid0 == (u64)(-1)) {
		begin_lsid = wh->begin_lsid;
	} else {
		begin_lsid = cfg->lsid0;
	}
	if (cfg->lsid1 == (u64)(-1)) {
		end_lsid = wh->end_lsid;
	} else {
		end_lsid = cfg->lsid1;
	}
	lsid = begin_lsid;

	/* Read, print and check each logpack */
	while (read_logpack_header(0, pbs, salt, logh)) {
		/* End block check. */
		if (is_end_logpack_header(logh)) break;

		/* Check range. */
		lsid = logh->logpack_lsid;
		if (lsid < begin_lsid) { continue; /* skip */ }
		if (end_lsid <= lsid) { break; /* end */ }

		/* Print logpack header. */
		print_logpack_header(logh);

		/* Check sect_ary size and reallocate if necessary. */
		if (!resize_logpack_if_necessary(pack, logh->total_io_size)) {
			goto error2;
		}

		/* Read logpack data. */
		if (!read_logpack_data(0, logh, salt, pack->sectd_ary)) {
			LOGe("read logpack data failed.\n");
			goto error2;
		}

		lsid += 1 + logh->total_io_size;
		total_padding_size += get_padding_size_in_logpack_header(logh, pbs);
		n_packs++;
	}
	/* Print the end lsids */
	printf("end_lsid_really: %" PRIu64 "\n"
		"lacked_log_size: %" PRIu64 "\n"
		"total_padding_size: %" PRIu64 "\n"
		"n_packs: %" PRIu64 "\n",
		lsid, end_lsid - lsid, total_padding_size, n_packs);

	/* Free resources. */
	free_logpack(pack);
	free(wh);
	return true;

error2:
	free_logpack(pack);
error1:
	free(wh);
	return false;
}

/**
 * Show logpack header inside walblog device.
 */
static bool do_show_wldev(const struct config *cfg)
{
	struct bdev_info wldev_info;
	unsigned int pbs;
	int fd;
	struct sector_data *super_sectd;
	struct walb_super_sector *super;
	struct logpack *pack;
	u64 oldest_lsid, lsid, begin_lsid, end_lsid;
	u32 salt;
	u64 total_padding_size = 0, n_packs = 0;

	ASSERT(strcmp(cfg->cmd_str, "show_wldev") == 0);

	if (!open_bdev_and_get_info(cfg->wldev_name, &wldev_info, &fd, O_RDONLY | O_DIRECT)) {
		LOGe("check and open failed %s.\n", cfg->wldev_name);
		return false;
	}
	pbs = wldev_info.pbs;

	/* Allocate memory and read super block */
	super = create_and_read_super_sector(&super_sectd, fd, pbs);
	if (!super) { goto error1; }
	print_super_sector(super_sectd); /* debug */
	oldest_lsid = super->oldest_lsid;
	LOGd("oldest_lsid: %"PRIu64"\n", oldest_lsid);
	salt = super->log_checksum_salt;

	/* Allocate logpack. */
	pack = alloc_logpack(pbs, 1); /* no need for log data. */
	if (!pack) { goto error2; }

	/* Range check */
	if (cfg->lsid0 == (u64)(-1)) {
		begin_lsid = oldest_lsid;
	} else {
		begin_lsid = cfg->lsid0;
	}
	if (cfg->lsid0 < oldest_lsid) {
		LOGe("given lsid0 %"PRIu64" < oldest_lsid %"PRIu64"\n",
			cfg->lsid0, oldest_lsid);
		goto error3;
	}
	end_lsid = cfg->lsid1;
	if (begin_lsid > end_lsid) {
		LOGe("lsid0 < lsid1 property is required.\n");
		goto error3;
	}

	/* Print each logpack header. */
	lsid = begin_lsid;
	while (lsid < end_lsid) {
		bool retb = read_logpack_header_from_wldev(
			fd, super, lsid, salt, pack->sectd);
		if (!retb) { break; }
		print_logpack_header(pack->header);

		lsid += pack->header->total_io_size + 1;
		total_padding_size +=
			get_padding_size_in_logpack_header(pack->header, pbs);
		n_packs++;
	}
	/* Print the end lsids */
	printf("end_lsid_really: %" PRIu64 "\n"
		"lacked_log_size: %" PRIu64 "\n"
		"total_padding_size: %" PRIu64 "\n"
		"n_packs: %" PRIu64 "\n",
		lsid, end_lsid - lsid, total_padding_size, n_packs);

	free_logpack(pack);
	sector_free(super_sectd);
	return close_(fd) == 0;

error3:
	free_logpack(pack);
error2:
	sector_free(super_sectd);
error1:
	close_(fd);
	return false;
}

/**
 * Set oldest_lsid.
 */
static bool do_set_oldest_lsid(const struct config *cfg)
{
	struct walb_ctl ctl = {
		.command = WALB_IOCTL_SET_OLDEST_LSID,
		.val_u64 = cfg->lsid,
		.u2k = { .buf_size = 0 },
		.k2u = { .buf_size = 0 },
	};

	ASSERT(strcmp(cfg->cmd_str, "set_oldest_lsid") == 0);
	if (!invoke_ioctl(cfg->wdev_name, &ctl, O_RDWR)) {
		return false;
	}
	LOGn("oldest_lsid is set to %"PRIu64" successfully.\n", cfg->lsid);
	return true;
}

/**
 * Get oldest_lsid.
 */
static bool do_get_oldest_lsid(const struct config *cfg)
{
	u64 oldest_lsid;

	ASSERT(strcmp(cfg->cmd_str, "get_oldest_lsid") == 0);

	oldest_lsid = get_ioctl_u64(cfg->wdev_name, WALB_IOCTL_GET_OLDEST_LSID);
	if (oldest_lsid == (u64)(-1)) {
		return false;
	}
	printf("%"PRIu64"\n", oldest_lsid);
	return true;
}

/**
 * Get written_lsid.
 */
static bool do_get_written_lsid(const struct config *cfg)
{
	u64 written_lsid;

	ASSERT(strcmp(cfg->cmd_str, "get_written_lsid") == 0);

	written_lsid = get_ioctl_u64(cfg->wdev_name, WALB_IOCTL_GET_WRITTEN_LSID);
	if (written_lsid == (u64)(-1)) {
		return false;
	}
	printf("%"PRIu64"\n", written_lsid);
	return true;
}

/**
 * Get permanent_lsid.
 */
static bool do_get_permanent_lsid(const struct config *cfg)
{
	u64 lsid;

	ASSERT(strcmp(cfg->cmd_str, "get_permanent_lsid") == 0);

	lsid = get_ioctl_u64(cfg->wdev_name, WALB_IOCTL_GET_PERMANENT_LSID);
	if (lsid == (u64)(-1)) {
		return false;
	}
	printf("%"PRIu64"\n", lsid);
	return true;
}

/**
 * Get completed_lsid.
 */
static bool do_get_completed_lsid(const struct config *cfg)
{
	u64 lsid;

	ASSERT(strcmp(cfg->cmd_str, "get_completed_lsid") == 0);

	lsid = get_ioctl_u64(cfg->wdev_name, WALB_IOCTL_GET_COMPLETED_LSID);
	if (lsid == (u64)(-1)) {
		return false;
	}
	printf("%"PRIu64"\n", lsid);
	return true;
}

/**
 * Search valid lsid which indicates a logpack header block.
 */
static bool do_search_valid_lsid(const struct config *cfg)
{
	unsigned int pbs;
	struct bdev_info wldev_info;
	int fd;
	struct sector_data *super_sectd;
	struct walb_super_sector *super;
	const size_t bufsize = 1024 * 1024; /* 1MB */
	struct logpack *pack;
	u64 lsid, begin_lsid, end_lsid;
	u32 salt;
	u64 n_pb;
	bool found;

	ASSERT(cfg->cmd_str);
	ASSERT(strcmp(cfg->cmd_str, "search_valid_lsid") == 0);

	if (cfg->lsid == (u64)(-1)) {
		LOGe("specify valid lsid.\n");
		return false;
	}
	n_pb = cfg->size;
	if (n_pb == (u64)(-1)) { n_pb = 1 << 16; } /* default value */
	if (n_pb == 0 || (1 << 16) < n_pb) {
		LOGe("specify valid size.\n");
		return false;
	}

	if (!open_bdev_and_get_info(cfg->wldev_name, &wldev_info, &fd, O_RDONLY | O_DIRECT)) {
		return false;
	}
	pbs = wldev_info.pbs;

	super = create_and_read_super_sector(&super_sectd, fd, pbs);
	if (!super) { goto error1; }
	salt = super->log_checksum_salt;

	pack = alloc_logpack(pbs, bufsize / pbs);
	if (!pack) { goto error2; }

	begin_lsid = cfg->lsid;
	if (begin_lsid < super->oldest_lsid) {
		LOGe("Specify valid starting lsid (oldest_lsid <= lsid).\n");
		goto error3;
	}
	end_lsid = begin_lsid + n_pb;
	ASSERT(begin_lsid < end_lsid);

	found = false;
	for (lsid = begin_lsid; lsid < end_lsid; lsid++) {
		bool retb = read_logpack_header_from_wldev(
			fd, super, lsid, salt, pack->sectd);
		if (retb) {
			found = true;
			break;
		}
	}
	if (found) {
		printf("%" PRIu64 "\n", lsid);
	} else {
		printf("NOT_FOUND\n");
	}

	free_logpack(pack);
	sector_free(super_sectd);
	return close_(fd) == 0;

error3:
	free_logpack(pack);
error2:
	sector_free(super_sectd);
error1:
	close_(fd);
	return false;
}

/**
 * Get log usage.
 */
static bool do_get_log_usage(const struct config *cfg)
{
	u64 log_usage;

	ASSERT(strcmp(cfg->cmd_str, "get_log_usage") == 0);

	log_usage = get_ioctl_u64(cfg->wdev_name, WALB_IOCTL_GET_LOG_USAGE);
	if (log_usage == (u64)(-1)) {
		LOGe("Getting log usage failed.\n");
		return false;
	}
	printf("%"PRIu64"\n", log_usage);
	return true;
}

/**
 * Get log capacity.
 */
static bool do_get_log_capacity(const struct config *cfg)
{
	u64 log_capacity;

	ASSERT(strcmp(cfg->cmd_str, "get_log_capacity") == 0);

	log_capacity = get_ioctl_u64(cfg->wdev_name, WALB_IOCTL_GET_LOG_CAPACITY);
	if (log_capacity == (u64)(-1)) {
		LOGe("Getting log_capacity failed.\n");
		return false;
	}
	printf("%"PRIu64"\n", log_capacity);
	return true;
}

/**
 * Is flush capable.
 */
static bool do_is_flush_capable(const struct config *cfg)
{
	ASSERT(strcmp(cfg->cmd_str, "is_flush_capable") == 0);
	return ioctl_and_print_bool(
		cfg->wdev_name, WALB_IOCTL_IS_FLUSH_CAPABLE);
}

/**
 * Resize the disk.
 */
static bool do_resize(const struct config *cfg)
{
	struct walb_ctl ctl = {
		.command = WALB_IOCTL_RESIZE,
		.val_u64 = cfg->size,
		.u2k = { .buf_size = 0 },
		.k2u = { .buf_size = 0 },
	};

	ASSERT(strcmp(cfg->cmd_str, "resize") == 0);

	return invoke_ioctl(cfg->wdev_name, &ctl, O_RDWR);
}

/**
 * Reset WAL.
 */
static bool do_reset_wal(const struct config *cfg)
{
	struct walb_ctl ctl = {
		.command = WALB_IOCTL_CLEAR_LOG,
		.u2k = { .buf_size = 0 },
		.k2u = { .buf_size = 0 },
	};

	ASSERT(strcmp(cfg->cmd_str, "reset_wal") == 0);

	return invoke_ioctl(cfg->wdev_name, &ctl, O_RDWR);
}

/**
 * Check log overflow.
 */
static bool do_is_log_overflow(const struct config *cfg)
{
	ASSERT(strcmp(cfg->cmd_str, "is_log_overflow") == 0);
	return ioctl_and_print_bool(
		cfg->wdev_name, WALB_IOCTL_IS_LOG_OVERFLOW);
}

/**
 * Freeze.
 */
static bool do_freeze(const struct config *cfg)
{
	u32 timeout_sec;
	struct walb_ctl ctl = {
		.command = WALB_IOCTL_FREEZE,
		.u2k = { .buf_size = 0 },
		.k2u = { .buf_size = 0 },
	};

	ASSERT(strcmp(cfg->cmd_str, "freeze") == 0);

	if (cfg->size > UINT32_MAX) {
		timeout_sec = 0;
	} else {
		timeout_sec = (u32)cfg->size;
	}
	ctl.val_u32 = timeout_sec;
	return invoke_ioctl(cfg->wdev_name, &ctl, O_RDWR);
}

/**
 * Melt.
 */
static bool do_melt(const struct config *cfg)
{
	struct walb_ctl ctl = {
		.command = WALB_IOCTL_MELT,
		.u2k = { .buf_size = 0 },
		.k2u = { .buf_size = 0 },
	};

	ASSERT(strcmp(cfg->cmd_str, "melt") == 0);

	return invoke_ioctl(cfg->wdev_name, &ctl, O_RDWR);
}

/**
 * Check the device is frozen or not.
 */
static bool do_is_frozen(const struct config *cfg)
{
	ASSERT(strcmp(cfg->cmd_str, "is_frozen") == 0);
	return ioctl_and_print_bool(
		cfg->wdev_name, WALB_IOCTL_IS_FROZEN);
}

/**
 * Get walb driver version.
 */
static bool do_get_version(UNUSED const struct config *cfg)
{
	int fd;
	u32 version;
	int ret;

	ASSERT(strcmp(cfg->cmd_str, "get_version") == 0);

	fd = open(WALB_CONTROL_PATH, O_RDONLY);
	if (fd < 0) {
		LOGe("open failed.");
		return false;
	}

	ret = ioctl(fd, WALB_IOCTL_VERSION, &version);
	if (ret < 0) {
		LOGe("get version failed.\n");
		close_(fd);
		return false;
	}

	printf("%u.%u.%u\n"
		, (version & 0x00ff0000) >> 16
		, (version & 0x0000ff00) >> 8
		, (version & 0x000000ff));
	return close_(fd) == 0;
}

/**
 * Get walb version.
 */
static bool do_version(UNUSED const struct config *cfg)
{
	printf("%s\n", WALB_VERSION_STR);
	return true;
}

/**
 * Show help message.
 */
static bool do_help(UNUSED const struct config *cfg)
{
	show_help(false);
	return true;
}

/*******************************************************************************
 * Functions for main.
 *******************************************************************************/

int main(int argc, char* argv[])
{
	struct config cfgt;

	init_random();
	init_config(&cfgt);

	if (parse_opt(argc, argv, &cfgt) != 0) {
		return 1;
	}

	if (!dispatch(&cfgt)) {
		LOGe("operation failed.\n");
		return 1;
	}
	return 0;
}

/* end of file. */
