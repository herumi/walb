/**
 * wrapper_blk_walb_easy.c - WalB block device with Easy Algorithm for test.
 *
 * Copyright(C) 2012, Cybozu Labs, Inc.
 * @author HOSHINO Takashi <hoshino@labs.cybozu.co.jp>
 */
#include <linux/blkdev.h>
#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/spinlock.h>

#include "wrapper_blk.h"
#include "wrapper_blk_walb.h"
#include "sector_io.h"
#include "logpack.h"
#include "walb/walb.h"
#include "walb/block_size.h"

/*******************************************************************************
 * Static data definition.
 *******************************************************************************/

/**
 * Main queue to process requests.
 * This should be prepared per device.
 */
#define WQ_REQ_LIST_NAME "wq_req_list"
struct workqueue_struct *wq_req_list_ = NULL;

/**
 * Queue for flush requests.
 */
#define WQ_REQ_FLUSH_NAME "wq_req_flush"
struct workqueue_struct *wq_req_flush_ = NULL;

/**
 * Flush work.
 *
 * if flush_req is NULL, packs in the list can be executed in parallel,
 * else, run flush_req first, then enqueue packs in the list.
 */
struct flush_work
{
	struct work_struct work;
	struct list_head list; /* list entry */
	struct wrapper_blk_dev *wdev;
	struct request *flush_req; /* flush request if flush */
	bool must_restart_queue; /* If true, the task must restart queue. */

	struct list_head wpack_list; /* list head of writepack. */
	struct list_head rpack_list; /* list head of readpack. */
};
/* kmem_cache for flush_work. */
#define KMEM_CACHE_FLUSH_WORK_NAME "flush_work_cache"
struct kmem_cache *flush_work_cache_ = NULL;

/**
 * Request entry struct.
 */
struct req_entry
{
	struct list_head list; /* list entry */
	struct request *req;
	struct list_head bio_entry_list; /* list head of bio_entry */
	bool is_submitted; /* true after submitted. */
};
/* kmem cache for dbio. */
#define KMEM_CACHE_REQ_ENTRY_NAME "req_entry_cache"
struct kmem_cache *req_entry_cache_ = NULL;

/**
 * A pack.
 * There are no overlapping requests in a pack.
 */
struct pack
{
	struct list_head list; /* list entry. */
	struct list_head req_ent_list; /* list head of req_entry. */
	bool is_write; /* true if write, or read. */

	/* This is for only write pack. */
	struct sector_data *logpack_header_sector;
};
#define KMEM_CACHE_PACK_NAME "pack_cache"
struct kmem_cache *pack_cache_ = NULL;

/* bio as a list entry. */
struct bio_entry
{
	struct list_head list; /* list entry */
	struct bio *bio;
	struct completion done;
	unsigned int bi_size; /* keep bi_size at initialization,
				 because bio->bi_size will be 0 after endio. */
	int error; /* bio error status. */
};
/* kmem cache for dbio. */
#define KMEM_CACHE_BIO_ENTRY_NAME "bio_entry_cache"
struct kmem_cache *bio_entry_cache_ = NULL;

/*******************************************************************************
 * Macros definition.
 *******************************************************************************/

/*******************************************************************************
 * Static functions prototype.
 *******************************************************************************/

/* Print request flags for debug. */
static void print_req_flags(struct request *req);

/* flush_work related. */
static struct flush_work* create_flush_work(
	struct request *flush_req,
	struct wrapper_blk_dev *wdev, gfp_t gfp_mask);
static void destroy_flush_work(struct flush_work *work);

/* req_entry related. */
static struct req_entry* create_req_entry(struct request *req, gfp_t gfp_mask);
static void destroy_req_entry(struct req_entry *reqe);

/* bio_entry related. */
static void bio_entry_end_io(struct bio *bio, int error);
static struct bio_entry* create_bio_entry(
	struct bio *bio, struct block_device *bdev, gfp_t gfp_mask);
static void destroy_bio_entry(struct bio_entry *bioe);

/* pack related. */
static struct pack* create_pack(bool is_write, gfp_t gfp_mask);
static struct pack* create_writepack(
	gfp_t gfp_mask, unsigned int pbs, u64 logpack_lsid);
static struct pack* create_readpack(gfp_t gfp_mask);
static void destroy_pack(struct pack *pack);
static bool is_overlap_pack_reqe(struct pack *pack, struct req_entry *reqe);

/* helper function. */
static bool readpack_add_req(
	struct list_head *rpack_list, struct pack **rpackp, struct request *req,
	gfp_t gfp_mask);
static bool writepack_add_req(
	struct list_head *wpack_list, struct pack **wpackp, struct request *req,
	u64 ring_buffer_size, u64 *latest_lsidp, gfp_t gfp_mask);

/* Request flush_work tasks. */
static void flush_work_task(struct work_struct *work); /* for non-flush, concurrent. */
static void req_flush_task(struct work_struct *work); /* for flush, sequential. */

/* Helper functions. */
static bool create_bio_entry_list(struct req_entry *reqe, struct wrapper_blk_dev *wdev);
static void submit_req_entry(struct req_entry *reqe);
static void wait_for_req_entry(struct req_entry *reqe);
static void enqueue_fwork_list(struct list_head *listh, struct request_queue *q);

/* Validator for debug. */
static bool is_valid_prepared_pack(struct pack *pack);
static bool is_valid_fwork_list(struct list_head *listh);


/*******************************************************************************
 * Static functions definition.
 *******************************************************************************/

/**
 * Print request flags for debug.
 */
UNUSED
static void print_req_flags(struct request *req)
{
	LOGd("REQ_FLAGS: "
		"%s%s%s%s%s"
		"%s%s%s%s%s"
		"%s%s%s%s%s"
		"%s%s%s%s%s"
		"%s%s%s%s%s"
		"%s%s%s%s\n", 
		((req->cmd_flags & REQ_WRITE) ?              "REQ_WRITE" : ""),
		((req->cmd_flags & REQ_FAILFAST_DEV) ?       " REQ_FAILFAST_DEV" : ""),
		((req->cmd_flags & REQ_FAILFAST_TRANSPORT) ? " REQ_FAILFAST_TRANSPORT" : ""),
		((req->cmd_flags & REQ_FAILFAST_DRIVER) ?    " REQ_FAILFAST_DRIVER" : ""),
		((req->cmd_flags & REQ_SYNC) ?               " REQ_SYNC" : ""),
		((req->cmd_flags & REQ_META) ?               " REQ_META" : ""),
		((req->cmd_flags & REQ_PRIO) ?               " REQ_PRIO" : ""),
		((req->cmd_flags & REQ_DISCARD) ?            " REQ_DISCARD" : ""),
		((req->cmd_flags & REQ_NOIDLE) ?             " REQ_NOIDLE" : ""),
		((req->cmd_flags & REQ_RAHEAD) ?             " REQ_RAHEAD" : ""),
		((req->cmd_flags & REQ_THROTTLED) ?          " REQ_THROTTLED" : ""),
		((req->cmd_flags & REQ_SORTED) ?             " REQ_SORTED" : ""),
		((req->cmd_flags & REQ_SOFTBARRIER) ?        " REQ_SOFTBARRIER" : ""),
		((req->cmd_flags & REQ_FUA) ?                " REQ_FUA" : ""),
		((req->cmd_flags & REQ_NOMERGE) ?            " REQ_NOMERGE" : ""),
		((req->cmd_flags & REQ_STARTED) ?            " REQ_STARTED" : ""),
		((req->cmd_flags & REQ_DONTPREP) ?           " REQ_DONTPREP" : ""),
		((req->cmd_flags & REQ_QUEUED) ?             " REQ_QUEUED" : ""),
		((req->cmd_flags & REQ_ELVPRIV) ?            " REQ_ELVPRIV" : ""),
		((req->cmd_flags & REQ_FAILED) ?             " REQ_FAILED" : ""),
		((req->cmd_flags & REQ_QUIET) ?              " REQ_QUIET" : ""),
		((req->cmd_flags & REQ_PREEMPT) ?            " REQ_PREEMPT" : ""),
		((req->cmd_flags & REQ_ALLOCED) ?            " REQ_ALLOCED" : ""),
		((req->cmd_flags & REQ_COPY_USER) ?          " REQ_COPY_USER" : ""),
		((req->cmd_flags & REQ_FLUSH) ?              " REQ_FLUSH" : ""),
		((req->cmd_flags & REQ_FLUSH_SEQ) ?          " REQ_FLUSH_SEQ" : ""),
		((req->cmd_flags & REQ_IO_STAT) ?            " REQ_IO_STAT" : ""),
		((req->cmd_flags & REQ_MIXED_MERGE) ?        " REQ_MIXED_MERGE" : ""),
		((req->cmd_flags & REQ_SECURE) ?             " REQ_SECURE" : ""));
}

/**
 * Create a flush_work.
 *
 * RETURN:
 * NULL if failed.
 * CONTEXT:
 * Any.
 */
static struct flush_work* create_flush_work(
	struct request *flush_req,
	struct wrapper_blk_dev *wdev,
	gfp_t gfp_mask)
{
	struct flush_work *work;

	ASSERT(wdev);
	ASSERT(flush_work_cache_);

	work = kmem_cache_alloc(flush_work_cache_, gfp_mask);
	if (!work) {
		goto error0;
	}
	INIT_LIST_HEAD(&work->list);
	work->wdev = wdev;
	work->flush_req = flush_req;
	work->must_restart_queue = false;
	INIT_LIST_HEAD(&work->wpack_list);
	INIT_LIST_HEAD(&work->rpack_list);
        
	return work;
error0:
	return NULL;
}

/**
 * Destory a flush_work.
 */
static void destroy_flush_work(struct flush_work *work)
{
	struct pack *pack, *next;

	if (!work) { return; }
	
	list_for_each_entry_safe(pack, next, &work->rpack_list, list) {
		list_del(&pack->list);
		destroy_pack(pack);
	}
	list_for_each_entry_safe(pack, next, &work->wpack_list, list) {
		list_del(&pack->list);
		destroy_pack(pack);
	}
#ifdef WALB_DEBUG
	work->flush_req = NULL;
	work->wdev = NULL;
	INIT_LIST_HEAD(&work->rpack_list);
	INIT_LIST_HEAD(&work->wpack_list);
#endif
	kmem_cache_free(flush_work_cache_, work);
}

/**
 * Create req_entry struct.
 */
static struct req_entry* create_req_entry(struct request *req, gfp_t gfp_mask)
{
	struct req_entry *reqe;

	reqe = kmem_cache_alloc(req_entry_cache_, gfp_mask);
	if (!reqe) {
		goto error0;
	}
	ASSERT(req);
	reqe->req = req;
	INIT_LIST_HEAD(&reqe->list);
	INIT_LIST_HEAD(&reqe->bio_entry_list);
	reqe->is_submitted = false;
        
	return reqe;
error0:
	return NULL;
}

/**
 * Destroy a req_entry.
 */
static void destroy_req_entry(struct req_entry *reqe)
{
	struct bio_entry *bioe, *next;

	if (reqe) {
		list_for_each_entry_safe(bioe, next, &reqe->bio_entry_list, list) {
			list_del(&bioe->list);
			destroy_bio_entry(bioe);
		}
#ifdef WALB_DEBUG
		reqe->req = NULL;
		INIT_LIST_HEAD(&reqe->list);
		INIT_LIST_HEAD(&reqe->bio_entry_list);
#endif
		kmem_cache_free(req_entry_cache_, reqe);
	}
}

/**
 * endio callback for bio_entry.
 */
static void bio_entry_end_io(struct bio *bio, int error)
{
	struct bio_entry *bioe = bio->bi_private;
	UNUSED int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	ASSERT(bioe);
	ASSERT(bioe->bio == bio);
	ASSERT(uptodate);
        
	/* LOGd("bio_entry_end_io() begin.\n"); */
	bioe->error = error;
	bio_put(bio);
	bioe->bio = NULL;
	complete(&bioe->done);
	/* LOGd("bio_entry_end_io() end.\n"); */
}

/**
 * Create a bio_entry.
 *
 * @bio original bio.
 * @bdev block device to forward bio.
 */
static struct bio_entry* create_bio_entry(
	struct bio *bio, struct block_device *bdev, gfp_t gfp_mask)
{
	struct bio_entry *bioe;
	struct bio *biotmp;

	/* LOGd("create_bio_entry() begin.\n"); */

	bioe = kmem_cache_alloc(bio_entry_cache_, gfp_mask);
	if (!bioe) {
		LOGd("kmem_cache_alloc() failed.");
		goto error0;
	}
	init_completion(&bioe->done);
	bioe->error = 0;
	bioe->bi_size = bio->bi_size;

	/* clone bio */
	bioe->bio = NULL;
	biotmp = bio_clone(bio, gfp_mask);
	if (!biotmp) {
		LOGe("bio_clone() failed.");
		goto error1;
	}
	biotmp->bi_bdev = bdev;
	biotmp->bi_end_io = bio_entry_end_io;
	biotmp->bi_private = bioe;
	bioe->bio = biotmp;
        
	/* LOGd("create_bio_entry() end.\n"); */
	return bioe;

error1:
	destroy_bio_entry(bioe);
error0:
	LOGe("create_bio_entry() end with error.\n");
	return NULL;
}

/**
 * Destroy a bio_entry.
 */
static void destroy_bio_entry(struct bio_entry *bioe)
{
	/* LOGd("destroy_bio_entry() begin.\n"); */
        
	if (!bioe) {
		return;
	}

	if (bioe->bio) {
		LOGd("bio_put %p\n", bioe->bio);
		bio_put(bioe->bio);
		bioe->bio = NULL;
	}
	kmem_cache_free(bio_entry_cache_, bioe);

	/* LOGd("destroy_bio_entry() end.\n"); */
}

/**
 * Create a pack.
 */
static struct pack* create_pack(bool is_write, gfp_t gfp_mask)
{
	struct pack *pack;

	/* LOGd("create_bio_entry() begin.\n"); */

	pack = kmem_cache_alloc(pack_cache_, gfp_mask);
	if (!pack) {
		LOGd("kmem_cache_alloc() failed.");
		goto error0;
	}
	INIT_LIST_HEAD(&pack->list);
	INIT_LIST_HEAD(&pack->req_ent_list);
	pack->is_write = is_write;
	
	return pack;
#if 0
error1:
	destory_pack(pack);
#endif
error0:
	LOGe("create_pack() end with error.\n");
	return NULL;
}

/**
 * Create a writepack.
 *
 * @gfp_mask allocation mask.
 * @pbs physical block size in bytes.
 * @logpack_lsid logpack lsid.
 *
 * RETURN:
 *   Allocated and initialized writepack in success, or NULL.
 */
static struct pack* create_writepack(
	gfp_t gfp_mask, unsigned int pbs, u64 logpack_lsid)
{
	struct pack *pack;
	struct walb_logpack_header *lhead;

	ASSERT(logpack_lsid != INVALID_LSID);
	pack = create_pack(true, gfp_mask);
	if (!pack) { goto error0; }
	pack->logpack_header_sector = sector_alloc(pbs, gfp_mask | __GFP_ZERO);
	if (!pack->logpack_header_sector) { goto error1; }

	lhead = get_logpack_header(pack->logpack_header_sector);
	lhead->sector_type = SECTOR_TYPE_LOGPACK;
	lhead->logpack_lsid = logpack_lsid;
	/* lhead->total_io_size = 0; */
	/* lhead->n_records = 0; */
	/* lhead->n_padding = 0; */
	
	return pack;
	
error1:
	destroy_pack(pack);
error0:
	return NULL;
}

/**
 * Create a readpack.
 */
static struct pack* create_readpack(gfp_t gfp_mask)
{
	struct pack *pack;
	
	pack = create_pack(false, gfp_mask);
	if (!pack) { goto error0; }
	pack->logpack_header_sector = NULL;
	return pack;
error0:
	return NULL;
}

/**
 * Destory a pack.
 */
static void destroy_pack(struct pack *pack)
{
	struct req_entry *reqe, *next;
	
	if (!pack) { return; }
	
	list_for_each_entry_safe(reqe, next, &pack->req_ent_list, list) {
		list_del(&reqe->list);
		destroy_req_entry(reqe);
	}
	if (pack->logpack_header_sector) {
		sector_free(pack->logpack_header_sector);
		pack->logpack_header_sector = NULL;
	}
#ifdef WALB_DEBUG
	INIT_LIST_HEAD(&work->req_ent_list);
#endif
	kmem_cache_free(pack_cache_, pack);
}

/**
 * Check a request in a pack and a request is overlapping.
 */
static bool is_overlap_pack_reqe(struct pack *pack, struct req_entry *reqe)
{
	struct req_entry *tmp_reqe;

	ASSERT(pack);
	ASSERT(reqe);
	ASSERT(reqe->req);
	
	list_for_each_entry(tmp_reqe, &pack->req_ent_list, list) {
		if (is_overlap_req(tmp_reqe->req, reqe->req)) {
			return true;
		}
	}
	return false;
}

/**
 * Add a request entry to a pack.
 *
 * @pack pack to added.
 * @reqe req_entry to add.
 *
 * If an overlapping request exists, add nothing and return false.
 * Else, add the request and return true.
 */
/* DEPRECATED */
/* static bool pack_add_reqe(struct pack *pack, struct req_entry *reqe) */
/* { */
/* 	struct req_entry *tmp_reqe; */
	
/* 	ASSERT(pack); */
/* 	ASSERT(reqe); */
/* 	ASSERT(pack->is_write == (reqe->req->cmd_flags & REQ_WRITE != 0)); */

/* 	/\* Search overlapping requests. *\/ */
/* 	if (is_overlap_pack_reqe(pack, reqe)) { */
/* 		return false; */
/* 	} else { */
/* 		list_add_tail(&reqe->list, &pack->req_ent_list); */
/* 		return true; */
/* 	} */
/* } */

/**
 * Helper function to add request to a readpack.
 * 
 * @rpack_list readpack list.
 * @rpackp pointer to readpack pointer.
 * @req reqeuest to add.
 *
 * RETURN:
 *   true: succeeded.
 *   false: out of memory.
 *
 * CONTEXT:
 *   IRQ: no, ATOMIC: yes.
 *   queue lock is held.
 */
static bool readpack_add_req(
	struct list_head *rpack_list, struct pack **rpackp,
	struct request *req, gfp_t gfp_mask)
{
	struct req_entry *reqe;
	struct pack *pack;

	ASSERT(rpack_list);
	ASSERT(rpackp);
	ASSERT(*rpackp);
	ASSERT(req);
	ASSERT(!(req->cmd_flags & REQ_WRITE));
	pack = *rpackp;
	ASSERT(!pack->is_write);
	ASSERT(!pack->logpack_header_sector);
	
	reqe = create_req_entry(req, gfp_mask);
	if (!reqe) { goto error0; }

	if (is_overlap_pack_reqe(pack, reqe)) {
		/* overlap found then create a new pack. */
		list_add_tail(&pack->list, rpack_list);
		pack = create_readpack(gfp_mask);
		if (!pack) { goto error1; }
		*rpackp = pack;
	}
	list_add_tail(&reqe->list, &pack->req_ent_list);
	return true;
error1:
	destroy_req_entry(reqe);
error0:
	return false;
}

/**
 * Add a request to a writepack.
 */
static bool writepack_add_req(
	struct list_head *wpack_list, struct pack **wpackp, struct request *req,
	u64 ring_buffer_size, u64 *latest_lsidp, gfp_t gfp_mask)
{
	struct req_entry *reqe;
	struct pack *pack;
	bool ret;
	unsigned int pbs;
	struct walb_logpack_header *lhead;

	ASSERT(wpack_list);
	ASSERT(wpackp);
	ASSERT(*wpackp);
	ASSERT(req);
	ASSERT(!(req->cmd_flags & REQ_WRITE));
	pack = *wpackp;
	ASSERT(pack->is_write);
	ASSERT(pack->logpack_header_sector);
	
	reqe = create_req_entry(req, gfp_mask);
	if (!reqe) { goto error0; }

	pbs = pack->logpack_header_sector->size;
	ASSERT_PBS(pbs);
	lhead = get_logpack_header(pack->logpack_header_sector);
	ASSERT(*latest_lsidp == lhead->logpack_lsid);

	if (is_overlap_pack_reqe(pack, reqe)) {
		/* overlap found so create a new pack. */
		goto newpack;
	}
	if (!walb_logpack_header_add_req(
			get_logpack_header(pack->logpack_header_sector),
			req, pbs, ring_buffer_size)) {
		/* logpack header capacity full so create a new pack. */
		goto newpack;
	}

	/* The request is just added to the pack. */
	list_add_tail(&reqe->list, &pack->req_ent_list);
	return true;

newpack:
	list_add_tail(&pack->list, wpack_list);
	*latest_lsidp = get_next_lsid(lhead);
	pack = create_writepack(gfp_mask, pbs, *latest_lsidp);
	if (!pack) { goto error1; }
	*wpackp = pack;

	ret = walb_logpack_header_add_req(
		get_logpack_header(pack->logpack_header_sector),
		req, pbs, ring_buffer_size);
	ASSERT(ret);

	list_add_tail(&reqe->list, &pack->req_ent_list);
	return true;
error1:
	destroy_req_entry(reqe);
error0:
	return false;
}

/**
 * Create bio_entry list for a request.
 *
 * RETURN:
 *     true if succeed, or false.
 * CONTEXT:
 *     Non-IRQ. Non-atomic.
 */
static bool create_bio_entry_list(struct req_entry *reqe, struct wrapper_blk_dev *wdev)

{
	struct bio_entry *bioe, *next;
	struct bio *bio;
	struct pdata *pdata = wdev->private_data;
	struct block_device *bdev = pdata->ddev;
        
	ASSERT(reqe);
	ASSERT(reqe->req);
	ASSERT(wdev);
	ASSERT(list_empty(&reqe->bio_entry_list));
        
	/* clone all bios. */
	__rq_for_each_bio(bio, reqe->req) {
		/* clone bio */
		bioe = create_bio_entry(bio, bdev, GFP_NOIO);
		if (!bioe) {
			LOGd("create_bio_entry() failed.\n"); 
			goto error1;
		}
		list_add_tail(&bioe->list, &reqe->bio_entry_list);
	}

	return true;
error1:
	list_for_each_entry_safe(bioe, next, &reqe->bio_entry_list, list) {
		list_del(&bioe->list);
		destroy_bio_entry(bioe);
	}
	ASSERT(list_empty(&reqe->bio_entry_list));
	return false;
}

/**
 * Submit all bios in a bio_entry.
 *
 * @reqe target req_entry.
 */
static void submit_req_entry(struct req_entry *reqe)
{
	struct bio_entry *bioe;
	list_for_each_entry(bioe, &reqe->bio_entry_list, list) {
		generic_make_request(bioe->bio);
	}
	reqe->is_submitted = true;
}

/**
 * Wait for completion and end request.
 *
 * @reqe target req_entry.
 */
static void wait_for_req_entry(struct req_entry *reqe)
{
	struct bio_entry *bioe, *next;
	int remaining;

	ASSERT(reqe);
        
	remaining = blk_rq_bytes(reqe->req);
	list_for_each_entry_safe(bioe, next, &reqe->bio_entry_list, list) {
		wait_for_completion(&bioe->done);
		blk_end_request(reqe->req, bioe->error, bioe->bi_size);
		remaining -= bioe->bi_size;
		list_del(&bioe->list);
		destroy_bio_entry(bioe);
	}
	ASSERT(remaining == 0);
}

/**
 * Normal pack list execution task.
 *
 * (1) Clone all bios related each request in the list.
 * (2) Submit them.
 * (3) wait completion of all bios.
 * (4) notify completion to the block layer.
 * (5) free memories.
 *
 * CONTEXT:
 *   Non-IRQ. Non-atomic.
 *   Request queue lock is not held.
 *   Other tasks may be running concurrently.
 */
static void flush_work_task(struct work_struct *work)
{
	/* now editing */

	
	struct flush_work *fwork = container_of(work, struct flush_work, work);
	struct wrapper_blk_dev *wdev = fwork->wdev;
	struct req_entry *reqe, *next;
	struct blk_plug plug;

	/* LOGd("flush_work_task begin.\n"); */
        
	ASSERT(fwork->flush_req == NULL);


	
/* 	/\* prepare and submit *\/ */
/* 	blk_start_plug(&plug); */
/* 	list_for_each_entry(reqe, &fwork->req_entry_list, list) { */
/* 		if (!create_bio_entry_list(reqe, wdev)) { */
/* 			LOGe("create_bio_entry_list failed.\n"); */
/* 			goto error0; */
/* 		} */
/* 		submit_req_entry(reqe); */
/* 	} */
/* 	blk_finish_plug(&plug); */

/* 	/\* wait completion and end requests. *\/ */
/* 	list_for_each_entry_safe(reqe, next, &fwork->req_entry_list, list) { */
/* 		wait_for_req_entry(reqe); */
/* 		list_del(&reqe->list); */
/* 		destroy_req_entry(reqe); */
/* 	} */
/* 	/\* destroy work struct *\/ */
/* 	destroy_flush_work(fwork); */
/* 	/\* LOGd("flush_work_task end.\n"); *\/ */
/* 	return; */

/* error0: */
/* 	list_for_each_entry_safe(reqe, next, &fwork->req_entry_list, list) { */
/* 		if (reqe->is_submitted) { */
/* 			wait_for_req_entry(reqe); */
/* 		} else { */
/* 			blk_end_request_all(reqe->req, -EIO); */
/* 		} */
/* 		list_del(&reqe->list); */
/* 		destroy_req_entry(reqe); */
/* 	} */
/* 	destroy_flush_work(fwork); */
/* 	LOGd("flush_work_task error.\n"); */
}

/**
 * Flush request executing task.
 *
 * CONTEXT:
 *   Non-IRQ. Non-atomic.
 *   Request queue lock is not held.
 *   This task is serialized by the singlethreaded workqueue.
 */
static void req_flush_task(struct work_struct *work)
{
	/* now editing */

	
	struct flush_work *fwork = container_of(work, struct flush_work, work);
	struct request_queue *q = fwork->wdev->queue;
	bool must_restart_queue = fwork->must_restart_queue;
	unsigned long flags;
        
	LOGd("req_flush_task begin.\n");
	ASSERT(fwork->flush_req);


	
	/* /\* Flush previous all requests. *\/ */
	/* flush_workqueue(wq_req_list_); */
	/* blk_end_request_all(fwork->flush_req, 0); */

	/* /\* Restart queue if required. *\/ */
	/* if (must_restart_queue) { */
	/* 	spin_lock_irqsave(q->queue_lock, flags); */
	/* 	ASSERT(blk_queue_stopped(q)); */
	/* 	blk_start_queue(q); */
	/* 	spin_unlock_irqrestore(q->queue_lock, flags); */
	/* } */
        
	/* if (list_empty(&fwork->req_entry_list)) { */
	/* 	destroy_flush_work(fwork); */
	/* } else { */
	/* 	/\* Enqueue the following requests *\/ */
	/* 	fwork->flush_req = NULL; */
	/* 	INIT_WORK(&fwork->work, flush_work_task); */
	/* 	queue_work(wq_req_list_, &fwork->work); */
	/* } */
	/* LOGd("req_flush_task end.\n"); */
}


/**
 * Enqueue all works in a list of flush_work.
 *
 * @listh list of flush_work.
 * @q request queue of a walb block device.
 *
 * CONTEXT:
 *     in_interrupt(): false. is_atomic(): true.
 *     queue lock is held.
 */
static void enqueue_fwork_list(struct list_head *listh, struct request_queue *q)
{
	struct flush_work *fwork;

	ASSERT(listh);
	ASSERT(q);
	
	list_for_each_entry(fwork, listh, list) {
		if (fwork->flush_req) {
			if (list_is_last(&fwork->list, listh)) {
				fwork->must_restart_queue = true;
				blk_stop_queue(q);
			}
			INIT_WORK(&fwork->work, req_flush_task);
			queue_work(wq_req_flush_, &fwork->work);
		} else {
			INIT_WORK(&fwork->work, flush_work_task);
			queue_work(wq_req_list_, &fwork->work);
		}
	}
}

/**
 * Check whether pack is valid.
 *   Assume just created and filled. checksum is not calculated at all.
 *
 * RETURN:
 *   true if valid, or false.
 */
static bool is_valid_prepared_pack(struct pack *pack)
{
	unsigned int idx = 0;
	struct walb_logpack_header *lhead;
	unsigned int pbs;
	struct walb_log_record *lrec;
	unsigned int i;
	struct req_entry *reqe;
	bool is_write;
	u64 total_pb; /* total io size in physical block. */

	CHECK(pack);
	CHECK(pack->lpack_header_sector);
	is_write = pack->is_write;

	lhead = get_logpack_header(pack->lpack_header_sector);
	pbs = pack->lpack_header_sector->size;
	ASSERT_PBS(pbs);
	CHECK(lhead);
	CHECK(is_valid_logpack_header(lhead));

	CHECK(!list_empty(&pack->req_ent_list));
	
	i = 0;
	total_pb = 0;
	list_for_each_entry(reqe, &pack->req_ent_list, list) {

		CHECK(i < lhead->n_records);
		lrec = &lhead->record[i];
		CHECK(lrec);
		CHECK(lrec->is_exist);
		CHECK(!(lrec->is_padding));
		CHECK(reqe->req);

		CHECK(!(reqe->req->cmd_flags & REQ_FLUSH));
		if (is_write) {
			CHECK(reqe->req->cmd_flags & REQ_WRITE);
		} else {
			CHECK(!(reqe->req->cmd_flags & REQ_WRITE));
		}

		CHECK(blk_rq_pos(reqe->req) == (sector_t)lrec->offset);
		CHECK(lhead->logpack_lsid == lrec->lsid - lrec->lsid_local);
		CHECK(blk_rq_sectors(reqe->req) == lrec->io_size);
		total_pb += capacity_pb(pbs, lrec->io_size);
		
		i ++;
		if (lhead->record[i].is_padding) {
			total_pb += capacity_pb(pbs, lrec->io_size);
			i ++;
		}
	}
	CHECK(i == lhead->n_records);
	CHECK(total_pb == lhead->total_io_size);
	return true;
error:
	return false;
}

/**
 * Check whether fwork_list is valid.
 * This is just for debug.
 *
 * @listh list of struct flush_work.
 *
 * RETURN:
 *   true if valid, or false.
 */
static bool is_valid_fwork_list(struct list_head *listh)
{
	struct flush_work *fwork;
	struct pack *pack;

	list_for_each_entry(fwork, listh, list) {

		ASSERT(fwork->wdev);
		if (fwork->flush_req) {
			CHECK(fwork->flush_req->cmd_flags & REQ_FLUSH);
		}
		list_for_each_entry(pack, &fwork->wpack_list, list) {
			CHECK(is_valid_prepared_pack(pack));
		}
		list_for_each_entry(pack, &fwork->rpack_list, list) {
			CHECK(is_valid_prepared_pack(pack));
		}
	}
	return true;

error:
	return false;
}


/*******************************************************************************
 * Global functions definition.
 *******************************************************************************/

/**
 * Make requrest callback.
 *
 * CONTEXT:
 *     IRQ: no. ATOMIC: yes.
 *     queue lock is held.
 */
void wrapper_blk_req_request_fn(struct request_queue *q)
{
	struct wrapper_blk_dev *wdev = wdev_get_from_queue(q);
	struct pdata *pdata = pdata_get_from_wdev(wdev);
	struct request *req;
	struct req_entry *reqe;
	struct flush_work *fwork;
	struct list_head fwork_list;
	struct pack *wpack, *rpack, *pack;
	struct bool ret;
	struct bool is_write;
	u64 latest_lsid, latest_lsid_old;
	
	bool errorOccurd = false;

	/* Load latest_lsid */
	spin_lock(&pdata->lsid_lock);
	latest_lsid = pdata->latest_lsid;
	spin_unlock(&pdata->lsid_lock);
	latest_lsid_old = latest_lsid;

	INIT_LIST_HEAD(&fwork_list);
	fwork = create_flush_work(NULL, wdev, GFP_ATOMIC);
	if (!fwork) { goto error0; }
	wpack = create_writepack(GFP_ATOMIC, pdata->pbs, latest_lsid);
	if (!wpack) { goto error1; }
	rpack = create_readpack(GFP_ATOMIC);
	if (!rpack) { goto error2; }

	/* Fetch requests and create pack list. */
	while ((req = blk_fetch_request(q)) != NULL) {

		/* print_req_flags(req); */
		if (errorOccurd) { goto req_error; }

		if (req->cmd_flags & REQ_FLUSH) {
			LOGd("REQ_FLUSH request with size %u.\n", blk_rq_bytes(req));
			list_add_tail(&fwork->list, &fwork_list);
			fwork = create_flush_work(req, wdev, GFP_ATOMIC);
			if (!fwork) {
				errorOccurd = true;
				goto req_error;
			}
		} else if (req->cmd_flags & REQ_WRITE) {
			ret = writepack_add_req(&fwork->wpack_list, &wpack, req,
						pdata->ring_buffer_size,
						&latest_lsid, GFP_ATOMIC);
			if (!ret) { goto req_error; }
		} else {
			ret = readpack_add_req(&fwork->rpack_list, &rpack, req, GFP_ATOMIC);
			if (!ret) { goto req_error; }
		}
		continue;
	req_error:
		__blk_end_request_all(req, -EIO);
	}
	ASSERT(get_next_lsid(get_logpack_header(wpack->logpack_header_sector)) == latest_lsid)
	list_add_tail(&wpack->list, &fwork->wpack_list);
	list_add_tail(&rpack->list, &fwork->rpack_list);
	list_add_tail(&fwork->list, &fwork_list);

	ASSERT(latest_lsid >= latest_lsid_old);

	/* Currently all requests are packed and lsid of all writepacks is defined. */
	ASSERT(is_valid_fwork_list(&listh));
	enqueue_fwork_list(&listh, q);
	INIT_LIST_HEAD(&listh);

	/* Store latest_lsid */
	spin_lock(&pdata->lsid_lock);
	ASSERT(pdata->latest_lsid == latest_lsid_old);
	pdata->latest_lsid = latest_lsid;
	spin_unlock(&pdata->lsid_lock);
	
	/* LOGd("wrapper_blk_req_request_fn: end.\n"); */
	return;
#if 0
error3:
	destroy_pack(rpack);
#endif
error2:
	destroy_pack(wpack);	
error1:
	destroy_flush_work(fwork);
error0:
	while ((req = blk_fetch_request(q)) != NULL) {
		__blk_end_request_all(req, -EIO);
	}
	/* LOGe("wrapper_blk_req_request_fn: error.\n"); */
}

/* Called before register. */
bool pre_register(void)
{
	LOGd("pre_register called.");

	/* Prepare kmem_cache data. */
	flush_work_cache_ = kmem_cache_create(
		KMEM_CACHE_FLUSH_WORK_NAME,
		sizeof(struct flush_work), 0, 0, NULL);
	if (!flush_work_cache_) {
		LOGe("failed to create a kmem_cache.\n");
		goto error0;
	}
	req_entry_cache_ = kmem_cache_create(
		KMEM_CACHE_REQ_ENTRY_NAME,
		sizeof(struct req_entry), 0, 0, NULL);
	if (!req_entry_cache_) {
		LOGe("failed to create a kmem_cache.\n");
		goto error1;
	}
	bio_entry_cache_ = kmem_cache_create(
		KMEM_CACHE_BIO_ENTRY_NAME,
		sizeof(struct bio_entry), 0, 0, NULL);
	if (!bio_entry_cache_) {
		LOGe("failed to create a kmem_cache.\n");
		goto error2;
	}
	pack_cache_ = kmem_cache_create(
		KMEM_CACHE_PACK_NAME,
		sizeof(struct pack), 0, 0, NULL);
	if (pack_cache_) {
		LOGe("failed to create a kmem_cache.\n");
		goto error3;
	}
	
	/* prepare workqueue data. */
	wq_req_list_ = alloc_workqueue(WQ_REQ_LIST_NAME, WQ_MEM_RECLAIM, 0);
	if (!wq_req_list_) {
		LOGe("failed to allocate a workqueue.");
		goto error4;
	}
	wq_req_flush_ = create_singlethread_workqueue(WQ_REQ_FLUSH_NAME);
	if (!wq_req_flush_) {
		LOGe("failed to allocate a workqueue.");
		goto error5;
	}

	return true;

#if 0
error6:
	destroy_workqueue(wq_req_flush_);
#endif
error5:
	destroy_workqueue(wq_req_list_);
error4:	
	kmem_cache_destroy(pack_cache_);
error3:
	kmem_cache_destroy(bio_entry_cache_);
error2:
	kmem_cache_destroy(req_entry_cache_);
error1:
	kmem_cache_destroy(flush_work_cache_);
error0:
	return false;
}

/* Called after unregister. */
void post_unregister(void)
{
	LOGd("post_unregister called.");

	/* finalize workqueue data. */
	destroy_workqueue(wq_req_flush_);
	wq_req_flush_ = NULL;
	destroy_workqueue(wq_req_list_);
	wq_req_list_ = NULL;

	/* Destory kmem_cache data. */
	kmem_cache_destroy(pack_cache_);
	pack_cache_ = NULL;
	kmem_cache_destroy(bio_entry_cache_);
	bio_entry_cache_ = NULL;
	kmem_cache_destroy(req_entry_cache_);
	req_entry_cache_ = NULL;
	kmem_cache_destroy(flush_work_cache_);
	flush_work_cache_ = NULL;
}

/* end of file. */