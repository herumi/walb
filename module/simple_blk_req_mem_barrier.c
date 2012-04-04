/**
 * simple_blk_req_mem_barrier.c -
 * request_fn which do memory read/write with barriers.
 *
 * Copyright(C) 2012, Cybozu Labs, Inc.
 * @author HOSHINO Takashi <hoshino@labs.cybozu.co.jp>
 */
#include "check_kernel.h"
#include <linux/blkdev.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/list.h>
#include "simple_blk_req.h"
#include "memblk_data.h"

/* #define PERFORMANCE_DEBUG */

/*******************************************************************************
 * Global data definition.
 *******************************************************************************/

/* module parameters. */
extern int sleep_ms_;

/*******************************************************************************
 * Static data definition.
 *******************************************************************************/

/**
 * Reuqest list work struc.t
 *
 * if flush_req is NULL, req_entry_list can be executed in parallel.
 * else, run flush_req, then enqueue req_entry_list.
 */
struct req_list_work
{
        struct work_struct work;
        struct simple_blk_dev *sdev;
	struct list_head list; /* list entry */

        struct request *flush_req; /* might be NULL. */
	bool is_restart_queue; /* If false, the task must restart after flush. */
	
	struct list_head req_entry_list; /* head of req_entry list. */
};

/**
 * Request entry.
 */
struct req_entry
{
	struct request *req;
	struct list_head list; /* list entry */
};

#define REQ_LIST_WORK_CACHE_NAME "req_list_work_cache"
struct kmem_cache *req_list_work_cache_ = NULL;

#define REQ_ENTRY_CACHE_NAME "req_entry_cache"
struct kmem_cache *req_entry_cache_ = NULL;

#define WQ_IO_NAME "simple_blk_req_mem_barrier_io"
struct workqueue_struct *wq_io_ = NULL;

#define WQ_FLUSH_NAME "simple_blk_req_mem_barrier_flush"
struct workqueue_struct *wq_flush_ = NULL;

/*******************************************************************************
 * Static functions definition.
 *******************************************************************************/

static void sleep_if_required(void);
UNUSED static void log_bi_rw_flag(struct bio *bio);

static void mdata_exec_discard(struct memblk_data *mdata, u64 block_id, unsigned int n_blocks);
static bool mdata_exec_req_special(struct memblk_data *mdata, struct request *req);
static void mdata_exec_req(struct memblk_data *mdata, struct request *req);

static struct memblk_data* get_mdata_from_sdev(struct simple_blk_dev *sdev);
UNUSED static struct memblk_data* get_mdata_from_queue(struct request_queue *q);

static struct req_list_work* create_req_list_work(
        struct request *flush_req, struct simple_blk_dev *sdev, gfp_t gfp_mask);
static void destroy_req_list_work(struct req_list_work *work);

static struct req_entry* create_req_entry(struct request *req, gfp_t gfp_mask);
static void destroy_req_entry(struct req_entry *work);

static void normal_io_task(struct work_struct *work);
static void flush_task(struct work_struct *work);

static void enqueue_all_req_list_work(
	struct list_head *rlwork_list, struct request_queue *q);

/*******************************************************************************
 * Static functions definition.
 *******************************************************************************/

static void sleep_if_required(void)
{
        if (unlikely(sleep_ms_ > 0)) {
                msleep_interruptible(sleep_ms_);
        }
}

/**
 * For debug.
 */
static void log_bi_rw_flag(struct bio *bio)
{
        LOGd("bio bi_sector %"PRIu64" %0lx bi_size %u bi_vcnt %hu "
             "bi_rw %0lx [%s][%s][%s][%s][%s][%s].\n",
             (u64)bio->bi_sector, bio->bi_sector, bio->bi_size, bio->bi_vcnt,
             bio->bi_rw, 
             (bio->bi_rw & REQ_WRITE ? "REQ_WRITE" : ""),
             (bio->bi_rw & REQ_RAHEAD? "REQ_RAHEAD" : ""),
             (bio->bi_rw & REQ_FLUSH ? "REQ_FLUSH" : ""),
             (bio->bi_rw & REQ_FUA ? "REQ_FUA" : ""),
             (bio->bi_rw & REQ_DISCARD ? "REQ_DISCARD" : ""),
             (bio->bi_rw & REQ_SECURE ? "REQ_SECURE" : ""));
}

/**
 * Currently discard just fills zero.
 *
 * This does not call blk_end_request.
 */
static void mdata_exec_discard(struct memblk_data *mdata, u64 block_id, unsigned int n_blocks)
{
        unsigned int i;
        for (i = 0; i < n_blocks; i ++) {
                memset(mdata_get_block(mdata, block_id + i), 0, mdata->block_size);
        }
}

/**
 * Get mdata from a sdev.
 */
static struct memblk_data* get_mdata_from_sdev(struct simple_blk_dev *sdev)
{
        ASSERT(sdev);
        return (struct memblk_data *)sdev->private_data;
}

/**
 * Get mdata from a queue.
 */
static struct memblk_data* get_mdata_from_queue(struct request_queue *q)
{
        return get_mdata_from_sdev(sdev_get_from_queue(q));
}

/**
 * Create a req_list_work.
 *
 * @flush_req flush request. can be NULL.
 * @sdev simplb block device data.
 * @gfp_mask gfp mask for memory allocation.
 *
 * RETURN:
 *     NULL if failed.
 * CONTEXT:
 *     Depends on @gfp_mask.
 */
static struct req_list_work* create_req_list_work(
        struct request *flush_req, struct simple_blk_dev *sdev, gfp_t gfp_mask)
{
	struct req_list_work *rlwork;

	ASSERT(sdev);
	
        rlwork = kmem_cache_alloc(req_list_work_cache_, gfp_mask);
        if (!rlwork) {
                goto error0;
        }
	rlwork->sdev = sdev;
	INIT_LIST_HEAD(&rlwork->list);
	rlwork->flush_req = flush_req;
	rlwork->is_restart_queue = false;
	INIT_LIST_HEAD(&rlwork->req_entry_list);
	
	return rlwork;
error0:
	return NULL;
}

static void destroy_req_list_work(struct req_list_work *work)
{
	if (work) {
		kmem_cache_free(req_list_work_cache_, work);
	}
}

static struct req_entry* create_req_entry(struct request *req, gfp_t gfp_mask)
{
	struct req_entry *reqe;

	ASSERT(req);
	
	reqe = kmem_cache_alloc(req_entry_cache_, gfp_mask);
	if (!reqe) {
		goto error0;
	}
	reqe->req = req;
	INIT_LIST_HEAD(&reqe->list);
	
	return reqe;
error0:
	return NULL;
}
	
static void destroy_req_entry(struct req_entry *reqe)
{
	if (reqe) {
		kmem_cache_free(req_entry_cache_, reqe);
	}
}

/**
 * Execute IO requests.
 *
 * CONTEXT:
 *     Non-irq. Non-atomic.
 *     Might be executed in parallel.
 */
static void normal_io_task(struct work_struct *work)
{
        struct req_list_work *rlwork =
		container_of(work, struct req_list_work, work);
        struct memblk_data *mdata = get_mdata_from_sdev(rlwork->sdev);
	struct req_entry *reqe, *next;
	
	ASSERT(!rlwork->flush_req);

	list_for_each_entry_safe(reqe, next, &rlwork->req_entry_list, list) {

		ASSERT(reqe->req);
		ASSERT(!(reqe->req->cmd_flags & REQ_FLUSH));
		mdata_exec_req(mdata, reqe->req);
		list_del(&reqe->list);
		destroy_req_entry(reqe);
	}
	ASSERT(list_empty(&rlwork->req_entry_list));
	destroy_req_list_work(rlwork);
}

/**
 * Flush pending IO requests and enqueue the followings.
 *
 * CONTEXT:
 *     Non-irq. Non-atomic.
 *     Executed in line.
 */
static void flush_task(struct work_struct *work)
{
        struct req_list_work *rlwork =
		container_of(work, struct req_list_work, work);
        struct simple_blk_dev *sdev = rlwork->sdev;
	struct request_queue *q = sdev->queue;
        struct memblk_data *mdata = get_mdata_from_sdev(sdev);
        struct request *flush_req = rlwork->flush_req;
	unsigned long flags;

	/* LOGd("begin.\n"); */
	ASSERT(flush_req);

	flush_workqueue(wq_io_);
	mdata_exec_req(mdata, flush_req);
	
#if 0
        LOGd("REQ %u: %"PRIu64" (%u).\n", req_work->id, blk_rq_pos(req), blk_rq_bytes(req));
#endif
	
	if (rlwork->is_restart_queue) {
		spin_lock_irqsave(q->queue_lock, flags);
		ASSERT(blk_queue_stopped(q));
		blk_start_queue(q);
		spin_unlock_irqrestore(q->queue_lock, flags);
	}

	if (list_empty(&rlwork->req_entry_list)) {
		destroy_req_list_work(rlwork);
	} else {
		/* Enqueue the following request. */
		rlwork->flush_req = NULL;
		INIT_WORK(&rlwork->work, normal_io_task);
		queue_work(wq_io_, &rlwork->work);
	}
	/* LOGd("end.\n"); */
}

/**
 * Execute a special request.
 *
 * This does not call blk_end_request().
 *
 * RETURN:
 *    true when the request is special, or false.
 * CONTEXT:
 *    Non-IRQ. Non-atomic.
 *    Queue lock is not held.
 */
static bool mdata_exec_req_special(struct memblk_data *mdata, struct request *req)
{
        unsigned int io_size = blk_rq_bytes(req);
        u64 block_id = (u64)blk_rq_pos(req);
        
        if (req->cmd_flags & REQ_DISCARD) {
                mdata_exec_discard(mdata, block_id, io_size / mdata->block_size);
                return true;
        }

        if (req->cmd_flags & REQ_FLUSH && io_size == 0) {
                LOGd("REQ_FLUSH\n");
                return true;
        }

        if (req->cmd_flags & REQ_FUA && io_size == 0) {
                LOGd("REQ_FUA\n");
                return true;
        }
	
        return false;
}

/**
 * Execute whole request,
 * sleep if required, and call blk_end_request().
 *
 * CONTEXT:
 *    Non-IRQ. Non-atomic.
 *    Queue lock is not held.
 */
static void mdata_exec_req(struct memblk_data *mdata, struct request *req)
{
        u64 block_id;
        struct bio_vec *bvec;
        unsigned int is_write;
        unsigned int n_blk;
        struct req_iterator iter;
        unsigned long flags;
        u8 *buf;

        ASSERT(req);
        block_id = (u64)blk_rq_pos(req);

        /* log_bi_rw_flag(bio); */

	if (mdata_exec_req_special(mdata, req)) {
		sleep_if_required();
		blk_end_request_all(req, 0);
		return;
	}
	
        is_write = req->cmd_flags & REQ_WRITE;

        rq_for_each_segment(bvec, req, iter) {
                buf = bvec_kmap_irq(bvec, &flags);
#if 0
                LOGd("bvec->bv_len: %u mdata->block_size %u\n",
                     bvec->bv_len, mdata->block_size);
#endif
                ASSERT(bvec->bv_len % mdata->block_size == 0);
                n_blk = bvec->bv_len / mdata->block_size;

                if (is_write) {
                        mdata_write_blocks(mdata, block_id, n_blk, buf);
                } else {
                        mdata_read_blocks(mdata, block_id, n_blk, buf);
                }
                
                block_id += n_blk;
                flush_kernel_dcache_page(bvec->bv_page);
                bvec_kunmap_irq(buf, &flags);
        }
	sleep_if_required();
	blk_end_request_all(req, 0);
}

/**
 * Enqueue all flush_work in a list.
 *
 * CONTEXT:
 *     in_interrupt(): false. is_atomic(): true.
 *     queue lock is held.
 */
static void enqueue_all_req_list_work(
	struct list_head *rlwork_list, struct request_queue *q)
{
	struct req_list_work *work;
	
	list_for_each_entry(work, rlwork_list, list) {
		if (work->flush_req) {
			if (list_is_last(&work->list, rlwork_list)) {
				work->is_restart_queue = true;
				blk_stop_queue(q);
			}
			INIT_WORK(&work->work, flush_task);
			queue_work(wq_flush_, &work->work);
		} else {
			INIT_WORK(&work->work, normal_io_task);
			queue_work(wq_io_, &work->work);
		}
	}
}

/*******************************************************************************
 * Global functions definition.
 *******************************************************************************/

/**
 * With workqueue.
 */ 
void simple_blk_req_request_fn(struct request_queue *q)
{
        struct simple_blk_dev *sdev = sdev_get_from_queue(q);
        struct request *req;
        struct req_list_work *rlwork;
	struct req_entry *reqe;
	struct list_head rlwork_list;
	bool is_error_occurred = false;

	INIT_LIST_HEAD(&rlwork_list);

        /* LOGd("in_interrupt(): %lu in_atomic(): %u\n", in_interrupt(), in_atomic()); */

	rlwork = create_req_list_work(NULL, sdev, GFP_ATOMIC);
	if (!rlwork) {
		goto error0;
	}
        while ((req = blk_fetch_request(q)) != NULL) {
                /* LOGd("REQ: %"PRIu64" (%u)\n", (u64)blk_rq_pos(req), blk_rq_bytes(req)); */

		if (is_error_occurred) {
			__blk_end_request_all(req, 0);
			continue;
		}
		
                if (req->cmd_flags & REQ_FLUSH) {

			list_add_tail(&rlwork->list, &rlwork_list);
			rlwork = create_req_list_work(req, sdev, GFP_ATOMIC);
			if (!rlwork) {
				__blk_end_request_all(req, 0);
				is_error_occurred = true;
				continue;
			}
                } else {
			reqe = create_req_entry(req, GFP_ATOMIC);
			if (!reqe) {
				__blk_end_request_all(req, -EIO);
				continue;
			}
			list_add_tail(&reqe->list, &rlwork->req_entry_list);
                }
        }
	list_add_tail(&rlwork->list, &rlwork_list);
	enqueue_all_req_list_work(&rlwork_list, q);

        /* LOGd("end.\n"); */
	return;

error0:
        while ((req = blk_fetch_request(q)) != NULL) {
		__blk_end_request_all(req, -EIO);
	}
}

/**
 * Create memory data.
 * @sdev a simple block device. must not be NULL.
 * REUTRN:
 * true in success, or false.
 * CONTEXT:
 * Non-IRQ.
 */
bool create_private_data(struct simple_blk_dev *sdev)
{
        struct memblk_data *mdata = NULL;
        u64 capacity;
        unsigned int block_size;

        ASSERT(sdev);
        
        capacity = sdev->capacity;
        block_size = sdev->blksiz.lbs;
        mdata = mdata_create(capacity, block_size, GFP_KERNEL);
        
        if (!mdata) {
                goto error0;
        }
        sdev->private_data = (void *)mdata;
        return true;
#if 0
error1:
        mdata_destroy(mdata);
#endif
error0:
        return false;
}

/**
 * Destory memory data.
 * @sdev a simple block device. must not be NULL.
 * RETURN:
 * true in success, or false.
 * CONTEXT:
 * Non-IRQ.
 */
void destroy_private_data(struct simple_blk_dev *sdev)
{
        ASSERT(sdev);
        mdata_destroy(sdev->private_data);
}

/**
 * Accept REQ_DISCARD, REQ_FLUSH, and REQ_FUA.
 */
void customize_sdev(struct simple_blk_dev *sdev)
{
        struct request_queue *q;
        ASSERT(sdev);
        q = sdev->queue;
        
        /* Accept REQ_DISCARD. */
        q->limits.discard_granularity = PAGE_SIZE;
        q->limits.discard_granularity = sdev->blksiz.lbs;
	q->limits.max_discard_sectors = UINT_MAX;
	q->limits.discard_zeroes_data = 1;
	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, q);
	/* queue_flag_set_unlocked(QUEUE_FLAG_SECDISCARD, q); */

        /* Accept REQ_FLUSH and REQ_FUA. */
        /* blk_queue_flush(q, REQ_FLUSH | REQ_FUA); */
        blk_queue_flush(q, REQ_FLUSH);
}

/**
 * Initialize kmem_cache.
 */
bool pre_register(void)
{
	might_sleep();
	
        req_list_work_cache_ = kmem_cache_create(
		REQ_LIST_WORK_CACHE_NAME, sizeof(struct req_list_work), 0, 0, NULL);
        if (!req_list_work_cache_) {
                LOGe("req_list_work_cache creation failed.\n");
                goto error0;
        }

        req_entry_cache_ = kmem_cache_create(
		REQ_ENTRY_CACHE_NAME, sizeof(struct req_entry), 0, 0, NULL);
        if (!req_entry_cache_) {
                LOGe("req_entry_cache creation failed.\n");
                goto error1;
        }

	wq_io_ = create_wq_io(WQ_IO_NAME, get_workqueue_type());
        if (!wq_io_) {
                LOGe("create io queue failed.\n");
                goto error2;
        }

	wq_flush_ = create_singlethread_workqueue(WQ_FLUSH_NAME);
	if (!wq_flush_) {
		LOGe("create flush queue failed.\n");
		goto error3;
	}
        return true;
	
#if 0
error4:
        destroy_workqueue(wq_flush_);
#endif
error3:
        destroy_workqueue(wq_io_);
error2:
        kmem_cache_destroy(req_entry_cache_);
error1:
        kmem_cache_destroy(req_list_work_cache_);
error0:
        return false;
}

/**
 * Finalize kmem_cache.
 */
void post_unregister(void)
{
	might_sleep();
	
        destroy_workqueue(wq_flush_);
        destroy_workqueue(wq_io_);
        kmem_cache_destroy(req_entry_cache_);
        kmem_cache_destroy(req_list_work_cache_);
}

/* end of file. */
