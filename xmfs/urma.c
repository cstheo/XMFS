// SPDX-License-Identifier: GPL-2.0-only
#include "xmfs_i.h"

#ifdef CONFIG_XMFS_FS_URMA

#include <ub/urma/ubcore_types.h>
#include <ub/urma/ubcore_uapi.h>
#include <ub/urma/ubcore_api.h>
#include <linux/hashtable.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/interrupt.h>
#include <linux/cacheflush.h>

static struct ubcore_device *g_urma_dev[10];
static char g_dev_name[100];

enum xmfs_urma_status {
	URMA_ISSUING = 1,
	URMA_COMPLETE,
	URMA_ERROR,
};

struct wr_status {
	int magic;
	refcount_t refs;
	int ret;
	int ops;
	int slot;
	enum xmfs_urma_status status;
	struct completion done;
};

#define XMFS_ACCESS_LOCAL_ONLY 0x1
#define XMFS_ACCESS_READ (0x1 << 1)
#define XMFS_ACCESS_WRITE (0x1 << 2)
#define XMFS_ACCESS_ATOMIC (0x1 << 3)

#define XMFS_DEPTH 1024
#define XMFS_POST_RETRY_CNT 100

static void xmfs_wrs_get(struct wr_status *wrs)
{
	refcount_inc(&wrs->refs);
}

static void xmfs_wrs_put(struct wr_status **wrs)
{
	if (refcount_dec_and_test(&(*wrs)->refs)) {
		/* xmfs_info(NULL, "qyf FREE 1 wrs=%px", *wrs); */
		kfree(*wrs);
		*wrs = NULL;
	}
}

static int xmfs_add_udma_device(struct ubcore_device *ubc_dev)
{
	xmfs_info(NULL, "add device %p %s %p %p %d", ubc_dev, ubc_dev->dev_name,
		  ubc_dev->ops->register_seg, ubc_dev->ops->unregister_seg,
		  IS_ERR_OR_NULL(ubc_dev->eid_table.eid_entries));
	xmfs_info(NULL, "hooks check %p %p %p", ubc_dev->ops->import_jetty,
		  ubc_dev->ops->import_jetty_ex, ubc_dev->ops->import_seg);
	if (ubc_dev == NULL) {
		xmfs_error(NULL,
			   "Param ubc_dev is null, add udma device fail.");
		return -EINVAL;
	}

	if (strcmp(ubc_dev->dev_name, g_dev_name) == 0) {
		xmfs_info(NULL, "find udma dev %s", ubc_dev->dev_name);
		g_urma_dev[0] = ubc_dev;
		xmfs_error(NULL, "--------->g_urma_dev set");
	}
	return 0;
}

static void xmfs_remove_udma_device(struct ubcore_device *ubc_dev,
				    void *client_ctx)
{
	xmfs_error(NULL, "ubma device removed %s, all in-flight ops will fail",
		   ubc_dev->dev_name);
}

static int xmfs_urma_pool_init(struct xmfs_urma_pool *pool, void *base,
			       size_t total_size, size_t slot_size)
{
	pool->base = base;
	pool->total_size = total_size;
	pool->slot_size = slot_size;

	if (slot_size == PAGE_SIZE)
		pool->nr_slots = total_size / slot_size;
	else
		pool->nr_slots = XMFS_DEPTH;
	pool->free_slots = pool->nr_slots;
	pool->bitmap = bitmap_zalloc(pool->nr_slots, GFP_KERNEL);
	if (!pool->bitmap)
		return -ENOMEM;
	xmfs_info(NULL, "pool init nr %d slot_size %ld total %ld",
		  pool->nr_slots, pool->slot_size, pool->total_size);
	spin_lock_init(&pool->lock);
	init_waitqueue_head(&pool->waitq);

	return 0;
}

static void xmfs_urma_pool_destroy(struct xmfs_urma_pool *pool)
{
	bitmap_free(pool->bitmap);
}

void xmfs_urma_unregister(struct xmfs_sb_info *sbi)
{
	if (!sbi)
		return;
	for (int i = 0; i < 5; i++)
		if (sbi->urma_cfg.jetty[i])
			ubcore_delete_jetty(sbi->urma_cfg.jetty[i]);
	if (sbi->urma_cfg.jfr)
		ubcore_delete_jfr(sbi->urma_cfg.jfr);
	if (sbi->urma_cfg.jfc)
		ubcore_delete_jfc(sbi->urma_cfg.jfc);
	if (sbi->urma_cfg.local_seg)
		ubcore_unregister_seg(sbi->urma_cfg.local_seg);
	if (sbi->urma_cfg.urma_local_va)
		vfree(sbi->urma_cfg.urma_local_va);
	if (sbi->urma_cfg.ubc_dev)
		ubcore_unregister_client(&sbi->urma_cfg.ubc_client);
	xmfs_urma_pool_destroy(&sbi->urma_cfg.pool);
	xmfs_urma_pool_destroy(&sbi->urma_cfg.send_pool);
	if (sbi->urma_cfg.send_jetty)
		ubcore_delete_jetty(sbi->urma_cfg.send_jetty);
	if (sbi->urma_cfg.send_jfr)
		ubcore_delete_jfr(sbi->urma_cfg.send_jfr);
	if (sbi->urma_cfg.send_jfc)
		ubcore_delete_jfc(sbi->urma_cfg.send_jfc);
	if (sbi->urma_cfg.recv_jfc)
		ubcore_delete_jfc(sbi->urma_cfg.recv_jfc);
	if (sbi->urma_cfg.send_seg)
		ubcore_unregister_seg(sbi->urma_cfg.send_seg);
	if (sbi->urma_cfg.recv_seg)
		ubcore_unregister_seg(sbi->urma_cfg.recv_seg);
	if (sbi->urma_cfg.recv_buf)
		vfree(sbi->urma_cfg.recv_buf);
	if (sbi->urma_cfg.send_buf)
		vfree(sbi->urma_cfg.send_buf);
	xa_destroy(&sbi->urma_cfg.grant_waits);
	/* g_urma_client = NULL; */
}

static void *urma_pool_acquire(struct xmfs_urma_pool *pool, u32 *slot)
{
	unsigned long flags;
	u32 idx;
	int ret;

	for (;;) {
		spin_lock_irqsave(&pool->lock, flags);

		idx = find_first_zero_bit(pool->bitmap, pool->nr_slots);

		if (idx < pool->nr_slots) {
			__set_bit(idx, pool->bitmap);
			pool->free_slots--;
			spin_unlock_irqrestore(&pool->lock, flags);

			if (slot)
				*slot = idx;
			return pool->base + idx * pool->slot_size;
		}

		spin_unlock_irqrestore(&pool->lock, flags);

		ret = wait_event_interruptible_timeout(pool->waitq,
						       READ_ONCE(pool->free_slots) >=
							       1,
						       msecs_to_jiffies(25000));
		if (ret <= 0)
			return NULL;
	}

	return NULL;
}

void *urma_pool_acquire_nslots(struct xmfs_urma_pool *pool, int nr, u32 *slots)
{
	unsigned long flags;
	u32 idx, pos;
	int found;
	int ret;

	if (unlikely(nr <= 0 || nr > pool->nr_slots))
		return NULL;

	for (;;) {
		found = 0;
		pos = 0;

		spin_lock_irqsave(&pool->lock, flags);

		/* 空闲slot数量都不够，直接等待 */
		if (pool->free_slots < nr)
			goto retry;

		/* 找到 nr 个空闲slot */
		while (found < nr) {
			idx = find_next_zero_bit(pool->bitmap, pool->nr_slots,
						 pos);

			if (idx >= pool->nr_slots)
				break;

			__set_bit(idx, pool->bitmap);
			slots[found++] = idx;
			pos = idx + 1;
		}

		/* 成功拿到全部slot */
		if (likely(found == nr)) {
			void *addr;

			pool->free_slots -= nr;

			addr = pool->base + slots[0] * pool->slot_size;

			spin_unlock_irqrestore(&pool->lock, flags);
			return addr;
		}

		/*
		 * 理论上不会进入这里：
		 * free_slots >= nr，但扫描过程中没找到足够slot。
		 * 为了健壮性仍然回滚。
		 */
		while (found > 0)
			__clear_bit(slots[--found], pool->bitmap);

retry:
		spin_unlock_irqrestore(&pool->lock, flags);

		ret = wait_event_interruptible_timeout(pool->waitq,
						       READ_ONCE(pool->free_slots) >=
							       nr,
						       msecs_to_jiffies(25000));
		if (ret <= 0)
			return NULL;
	}
}

void urma_pool_release_nslots(struct xmfs_urma_pool *pool, int nr,
			      const u32 *slots)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&pool->lock, flags);

	for (i = 0; i < nr; i++) {
		WARN_ON(!test_bit(slots[i], pool->bitmap));
		__clear_bit(slots[i], pool->bitmap);
	}

	pool->free_slots += nr;

	spin_unlock_irqrestore(&pool->lock, flags);

	wake_up_all(&pool->waitq);
}

void urma_pool_release(struct xmfs_urma_pool *pool, u32 slot)
{
	unsigned long flags;

	if (WARN_ON(slot >= pool->nr_slots || slot < 0))
		return;

	spin_lock_irqsave(&pool->lock, flags);

	if (!test_bit(slot, pool->bitmap))
		xmfs_info(NULL, "err release find bit %d empty", slot);

	__clear_bit(slot, pool->bitmap);
	pool->free_slots++;
	spin_unlock_irqrestore(&pool->lock, flags);

	wake_up(&pool->waitq);
}

static void *xmfs_get_recv_buf(struct xmfs_sb_info *sbi, int idx)
{
	return (char *)sbi->urma_cfg.recv_buf +
	       idx * sizeof(struct xmfs_rw_msg);
}

static int xmfs_post_recv(struct xmfs_sb_info *sbi, int idx)
{
	struct ubcore_jfr_wr *bad_wr = NULL;
	struct ubcore_jfr_wr wr = {};
	struct ubcore_sge sge = {};
	int ret;

	sge.addr = (uint64_t)xmfs_get_recv_buf(sbi, idx);
	sge.len = sizeof(struct xmfs_rw_msg);
	sge.tseg = sbi->urma_cfg.recv_seg;

	wr.src.sge = &sge;
	wr.src.num_sge = 1;
	wr.user_ctx = idx;
	wr.next = NULL;

	ret = ubcore_post_jetty_recv_wr(sbi->urma_cfg.send_jetty, &wr, &bad_wr);
	if (ret)
		xmfs_info(NULL, "err xmfs post recv buffer failed ret %d", ret);

	return ret;
}

static void xmfs_process_recv(struct xmfs_sb_info *sbi, struct ubcore_cr *cr)
{
	struct wr_status *wrs;

	if (cr->status != UBCORE_CR_SUCCESS) {
		xmfs_warning(sbi->sb,
			     "JFC receive completion failed: status %d opcode %d",
			     cr->status, cr->opcode);
		if (cr->status == UBCORE_CR_WR_FLUSH_ERR_DONE)
			return;
	}
	wrs = (struct wr_status *)(uintptr_t)cr->user_ctx;
	if (!virt_addr_valid(wrs)) {
		xmfs_info(NULL, "jfc callback got an invalid wrs %p", wrs);
		return;
	}
	/* xmfs_info(sbi->sb, "qyf COMP user_ctx=%llx", cr->user_ctx); */
	if (!wrs || wrs->magic != XMFS_MAGIC) {
		xmfs_info(NULL,
			  "err cannot get a correct wrs during process recv %p",
			  wrs);
		return;
	}
	if (cr->status == UBCORE_CR_SUCCESS)
		wrs->status = URMA_COMPLETE;
	else {
		wrs->status = URMA_ERROR;
		wrs->ret = cr->status;
	}
	if (wrs->ops == XMFS_URMA_SEND)
		xmfs_info(NULL, "err slave send jfc callback");

	if (wrs->ops == XMFS_URMA_WRITE && wrs->slot != -1) {
		if (wrs->slot >= sbi->urma_cfg.pool.nr_slots || wrs->slot < 0)
			xmfs_info(NULL,
				  "err jfc process write got wrong slot %d %d",
				  wrs->slot, sbi->urma_cfg.pool.nr_slots);
		else
			urma_pool_release(&sbi->urma_cfg.pool, wrs->slot);
	}
	complete(&wrs->done);
	xmfs_wrs_put(&wrs);
}

static void xmfs_jfc_callback(struct ubcore_jfc *jfc)
{
	struct ubcore_cr cr[8];

	if (!jfc)
		return;
	struct xmfs_sb_info *sbi = jfc->jfc_cfg.jfc_context;
	int ret, i;

	if (!jfc || !sbi || sbi->magic != XMFS_MAGIC)
		return;

	if (jfc != sbi->urma_cfg.jfc) {
		xmfs_error(NULL, "err foreign jfc cr, drop it");
		ubcore_rearm_jfc(jfc, false);
		return;
	}

	/* xmfs_info(sbi->sb, "qyf jfc callback"); */

	do {
		ret = ubcore_poll_jfc(jfc, 1, cr);
		if (ret < 0) {
			xmfs_info(NULL, "err jfc poll failed %d", ret);
			break;
		}

		for (i = 0; i < ret; i++)
			xmfs_process_recv(sbi, &cr[i]);
	} while (ret > 0 && jfc && sbi && sbi->magic == XMFS_MAGIC);

	ret = ubcore_rearm_jfc(jfc, false);
	if (ret < 0)
		xmfs_info(NULL, "err jfc rearm failed %d", ret);
}

static int xmfs_host_send(struct xmfs_sb_info *sbi, struct ubcore_cr *cr)
{
	u32 idx;

	idx = (u32)cr->user_ctx;
	if (cr->status != UBCORE_CR_SUCCESS)
		/* if (cr->status != 9 && cr->status != 10) */
		xmfs_warning(sbi->sb,
			     "send completion failed: node %d status %d index %d",
			     sbi->id, cr->status, idx);
	if (cr->status == UBCORE_CR_WR_FLUSH_ERR_DONE)
		return 0;
	/*
	 * if (sbi->id != 0)
	 *	xmfs_info(sbi->sb,
	 *		  "qyf send request callback send pool slot %d release",
	 *		  idx);
	 */
	urma_pool_release(&sbi->urma_cfg.send_pool, idx);
	return 0;
}

static void xmfs_host_reply_worker(struct work_struct *work)
{
	struct xmfs_send_work *sw =
		container_of(work, struct xmfs_send_work, work);
	struct xmfs_rw_msg grant = sw->grant;
	struct xmfs_rw_msg *buf;
	struct xmfs_sb_info *sbi = sw->sbi;
	struct ubcore_jfs_wr *bad_send_wr = NULL;
	struct ubcore_jfs_wr send_wr = {};
	struct ubcore_sge sge = {};
	int slot, ret;

	buf = (struct xmfs_rw_msg *)urma_pool_acquire(&sbi->urma_cfg.send_pool,
						      &slot);
	if (!buf) {
		xmfs_info(NULL, "cannot get the buf during host process recv");
		kfree(sw);
		return;
	}
	memcpy(buf, &grant, sizeof(struct xmfs_rw_msg));

	sge.addr = (uint64_t)buf;
	sge.len = sizeof(grant);
	sge.tseg = sbi->urma_cfg.send_seg;

	send_wr.opcode = UBCORE_OPC_SEND;
	send_wr.tjetty = sbi->urma_cfg.send_tjettys[grant.slave_id];
	send_wr.send.src.sge = &sge;
	send_wr.send.src.num_sge = 1;
	send_wr.user_ctx = slot;
	send_wr.flag.value = 0;
	send_wr.flag.bs.complete_enable = 1;
	send_wr.next = NULL;

	ret = ubcore_post_jetty_send_wr(sbi->urma_cfg.send_jetty, &send_wr,
					&bad_send_wr);
	if (ret) {
		urma_pool_release(&sbi->urma_cfg.send_pool, slot);
		xmfs_info(NULL, "err host send post err %d", ret);
	}
	kfree(sw);
}

static int xmfs_host_recv(struct xmfs_sb_info *sbi, struct ubcore_cr *cr)
{
	struct xmfs_rw_msg *req;
	struct xmfs_rw_msg grant;
	u32 idx;

	if (cr->status != UBCORE_CR_SUCCESS) {
		idx = (u32)cr->user_ctx;
		/* if (cr->status != 9 && cr->status != 10) */
		xmfs_warning(sbi->sb,
			     "receive completion failed: node %d status %d opcode %d index %d",
			     sbi->id, cr->status, cr->opcode, idx);
		xmfs_post_recv(sbi, idx);
		return -EINVAL;
	}

	idx = (u32)cr->user_ctx;

	if (sbi->id == 0) {
		if (idx >= XMFS_DEPTH || idx < 0) {
			xmfs_info(NULL, "err host recv idx %d is not correct",
				  idx);
			return -EINVAL;
		}
		req = (struct xmfs_rw_msg *)xmfs_get_recv_buf(sbi, idx);
		int slave_id = req->slave_id;
		struct xmfs_send_work *sw = kmalloc(sizeof(*sw), GFP_ATOMIC);

		if (!sw) {
			xmfs_post_recv(sbi, idx);
			return 0;
		}
		/*
		 * xmfs_info(sbi->sb,
		 *	     "qyf host recv callback recv pool slot %d req id %d req ops %d",
		 *	     idx, req->request_id, req->ops);
		 */
		xmfs_host_handle_urma_request(sbi, slave_id, req, &sw->grant);
		sw->sbi = sbi;
		xmfs_post_recv(sbi, idx);
		/*
		 * xmfs_info(sbi->sb, "host process req %d %d %d",
		 *	     req->request_id, req->ops, grant.log_slot_start);
		 */
		if (req->ops == XMFS_SETATTR) {
			kfree(sw);
			return 0;
		}

		INIT_WORK(&sw->work, xmfs_host_reply_worker);
		queue_work(sbi->send_wq, &sw->work);
		/*
		 * buf = (struct xmfs_rw_msg *)urma_pool_acquire(
		 *	&sbi->urma_cfg.send_pool, &slot);
		 * if (!buf) {
		 *	xmfs_info(sbi->sb,
		 *		  "cannot get the buf during host process recv");
		 *	xmfs_post_recv(sbi, idx);
		 *	return 0;
		 * }
		 * memcpy(buf, &grant, sizeof(struct xmfs_rw_msg));
		 * xmfs_info(sbi->sb,
		 *	     "qyf host recv callback recv pool slot %d "
		 *	     "req id %d req ops %d send pool slot %d",
		 *	     idx, req->request_id, req->ops, slot);
		 * xmfs_post_recv(sbi, idx);
		 *
		 * sge.addr = (uint64_t)buf;
		 * sge.len = sizeof(grant);
		 * sge.tseg = sbi->urma_cfg.send_seg;
		 *
		 * send_wr.opcode = UBCORE_OPC_SEND;
		 * send_wr.tjetty = sbi->urma_cfg.send_tjettys[slave_id];
		 * send_wr.send.src.sge = &sge;
		 * send_wr.send.src.num_sge = 1;
		 * send_wr.user_ctx = slot;
		 * send_wr.flag.value = 0;
		 * send_wr.flag.bs.complete_enable = 1;
		 * send_wr.next = NULL;
		 *
		 * ret = ubcore_post_jetty_send_wr(sbi->urma_cfg.send_jetty,
		 *				      &send_wr, &bad_send_wr);
		 * if (ret) {
		 *	urma_pool_release(&sbi->urma_cfg.send_pool, slot);
		 *	xmfs_info(sbi->sb, "err host send post err %d", ret);
		 * }
		 */
	} else {
		if (idx >= XMFS_DEPTH || idx < 0) {
			xmfs_info(NULL, "err slave recv idx %d is not correct",
				  idx);
			return -EINVAL;
		}
		req = (struct xmfs_rw_msg *)xmfs_get_recv_buf(sbi, idx);
		/*
		 * xmfs_info(sbi->sb,
		 *	     "qyf slave recv callback recv pool slot %d "
		 *	     "req id %d req ops %d type %d log addr %ld "
		 *	     "data addr %ld %ld data num %d %d version %ld",
		 *	     idx, req->request_id, req->ops, req->type,
		 *	     req->log_trunk_addr, req->data_addr,
		 *	     req->second_data_addr, req->data_page_count,
		 *	     req->second_data_pages, req->version);
		 */
		if (req->ops == XMFS_GC) {
			xmfs_info(NULL, "slave receive gc req %d", req->status);
			if (req->status == 1)
				sbi->gc_ctl.gc_running = true;
			else if (req->status == 0) {
				sbi->gc_ctl.gc_running = false;
				wake_up_all(&sbi->gc_ctl.wait);
			} else if (req->status == 2)
				sbi->gc_ctl.need_replay = true;
			xmfs_post_recv(sbi, idx);
			return 0;
		}

		memcpy(&grant, req, sizeof(struct xmfs_rw_msg));
		xmfs_post_recv(sbi, idx);
		/* xmfs_info(sbi->sb, "qyf slave post recv %d", idx); */
		struct xmfs_grant_wait *gw =
			xa_load(&sbi->urma_cfg.grant_waits, grant.request_id);
		if (gw) {
			memcpy(&gw->grant, &grant, sizeof(struct xmfs_rw_msg));
			complete(&gw->done);
		} else {
			xmfs_error(NULL, "err cannot get the grant wait %ld",
				   grant.request_id);
		}
	}

	return 0;
}

static void xmfs_send_jfc_callback(struct ubcore_jfc *jfc)
{
	struct ubcore_cr cr[8];

	if (!jfc)
		return;
	struct xmfs_sb_info *sbi = jfc->jfc_cfg.jfc_context;
	int ret, i;

	if (!jfc || !sbi || sbi->magic != XMFS_MAGIC)
		return;

	if (jfc != sbi->urma_cfg.recv_jfc && jfc != sbi->urma_cfg.send_jfc) {
		xmfs_error(NULL, "err foreign send jfc cr, drop it");
		ubcore_rearm_jfc(jfc, false);
		return;
	}

	/* xmfs_info(sbi->sb, "send callback %d", sbi->id); */
	do {
		ret = ubcore_poll_jfc(jfc, 1, cr);
		if (ret < 0) {
			xmfs_info(NULL, "err jfc send poll failed %d", ret);
			break;
		}

		for (i = 0; i < ret; i++)
			xmfs_host_send(sbi, &cr[i]);
	} while (ret > 0 && jfc && sbi && sbi->magic == XMFS_MAGIC);

	ret = ubcore_rearm_jfc(jfc, false);
	if (ret < 0)
		xmfs_info(NULL, "err jfc recv rearm failed %d", ret);
}

static void xmfs_recv_jfc_callback(struct ubcore_jfc *jfc)
{
	struct ubcore_cr cr[8];

	if (!jfc)
		return;
	struct xmfs_sb_info *sbi = jfc->jfc_cfg.jfc_context;
	int ret, i;

	if (!jfc || !sbi || sbi->magic != XMFS_MAGIC)
		return;

	if (jfc != sbi->urma_cfg.recv_jfc && jfc != sbi->urma_cfg.send_jfc) {
		xmfs_error(NULL, "err foreign recv jfc cr, drop it");
		ubcore_rearm_jfc(jfc, false);
		return;
	}
	/* xmfs_info(sbi->sb, "recv callback %d", sbi->id); */
	do {
		ret = ubcore_poll_jfc(jfc, 1, cr);
		if (ret < 0) {
			xmfs_info(NULL, "err jfc recv poll failed %d", ret);
			break;
		}

		for (i = 0; i < ret; i++)
			xmfs_host_recv(sbi, &cr[i]);
	} while (ret > 0 && jfc && sbi && sbi->magic == XMFS_MAGIC);

	ret = ubcore_rearm_jfc(jfc, false);
	if (ret < 0)
		xmfs_info(NULL, "err jfc recv rearm failed %d", ret);
}

static int xmfs_register_send_recv(struct xmfs_sb_info *sbi,
				   struct xmfs_fs_context *ctx)
{
	struct ubcore_seg_cfg seg_cfg = { 0 };
	union ubcore_reg_seg_flag seg_flag = { 0 };
	struct ubcore_target_seg *seg;
	struct ubcore_jfc_cfg jfc_cfg = { 0 };
	struct ubcore_jfc *jfc;
	struct ubcore_jfr_cfg jfr_cfg = { 0 };
	struct ubcore_jfr *jfr;
	struct ubcore_jetty *jetty;
	struct ubcore_jetty_cfg jetty_cfg = { 0 };
	struct ubcore_eid_info *eid_info;
	int err, cnt = 0;

	seg_flag.bs.token_policy = 0;
	if (ctx->debug)
		seg_flag.bs.access = XMFS_ACCESS_READ | XMFS_ACCESS_WRITE |
				     XMFS_ACCESS_ATOMIC |
				     XMFS_ACCESS_LOCAL_ONLY;
	else
		seg_flag.bs.access = XMFS_ACCESS_READ | XMFS_ACCESS_WRITE |
				     XMFS_ACCESS_ATOMIC;
	seg_flag.bs.cacheable = UBCORE_NON_CACHEABLE;

	sbi->urma_cfg.recv_buf =
		vzalloc(PAGE_ALIGN(XMFS_DEPTH * sizeof(struct xmfs_rw_msg)));
	if (!sbi->urma_cfg.recv_buf)
		return -ENOMEM;

	seg_cfg.va = (uint64_t)sbi->urma_cfg.recv_buf;
	seg_cfg.len = PAGE_ALIGN(XMFS_DEPTH * sizeof(struct xmfs_rw_msg));
	seg_cfg.flag = seg_flag;

	seg = ubcore_register_seg(sbi->urma_cfg.ubc_dev, &seg_cfg, NULL);
	if (IS_ERR_OR_NULL(seg)) {
		xmfs_error(NULL,
			   "ubcore_register_seg failed during register recv");
		return -EOPNOTSUPP;
	}
	sbi->urma_cfg.recv_seg = seg;

	seg_flag.bs.token_policy = 0;
	seg_flag.bs.access = XMFS_ACCESS_LOCAL_ONLY;
	seg_flag.bs.cacheable = UBCORE_NON_CACHEABLE;

	sbi->urma_cfg.send_buf =
		vzalloc(PAGE_ALIGN(XMFS_DEPTH * sizeof(struct xmfs_rw_msg)));
	if (!sbi->urma_cfg.send_buf)
		return -ENOMEM;

	seg_cfg.va = (uint64_t)sbi->urma_cfg.send_buf;
	seg_cfg.len = PAGE_ALIGN(XMFS_DEPTH * sizeof(struct xmfs_rw_msg));
	seg_cfg.flag = seg_flag;
	xmfs_urma_pool_init(&sbi->urma_cfg.send_pool, (void *)seg_cfg.va,
			    seg_cfg.len, sizeof(struct xmfs_rw_msg));

	seg = ubcore_register_seg(sbi->urma_cfg.ubc_dev, &seg_cfg, NULL);
	if (IS_ERR_OR_NULL(seg)) {
		xmfs_error(NULL,
			   "ubcore_register_seg failed during register send");
		return -EOPNOTSUPP;
	}
	sbi->urma_cfg.send_seg = seg;

	jfc_cfg.depth = XMFS_DEPTH;
	jfc_cfg.jfc_context = sbi;
	jfc = ubcore_create_jfc(sbi->urma_cfg.ubc_dev, &jfc_cfg,
				xmfs_recv_jfc_callback, NULL, NULL);
	if (IS_ERR_OR_NULL(jfc)) {
		xmfs_error(NULL,
			   "ubcore_create_jfc failed during register recv");
		return -EOPNOTSUPP;
	}
	sbi->urma_cfg.recv_jfc = jfc;

	jfc = ubcore_create_jfc(sbi->urma_cfg.ubc_dev, &jfc_cfg,
				xmfs_send_jfc_callback, NULL, NULL);
	if (IS_ERR_OR_NULL(jfc)) {
		xmfs_error(NULL,
			   "ubcore_create_jfc failed during register send");
		return -EOPNOTSUPP;
	}
	sbi->urma_cfg.send_jfc = jfc;

	err = ubcore_rearm_jfc(sbi->urma_cfg.send_jfc, false);
	if (err)
		return -EOPNOTSUPP;
	err = ubcore_rearm_jfc(sbi->urma_cfg.recv_jfc, false);
	if (err)
		return -EOPNOTSUPP;

	jfr_cfg.depth = XMFS_DEPTH;
	jfr_cfg.trans_mode = UBCORE_TP_RM;
	jfr_cfg.min_rnr_timer = (u8)19U;
	jfr_cfg.max_sge = 1;
	jfr_cfg.jfc = sbi->urma_cfg.recv_jfc;
	jfr = ubcore_create_jfr(sbi->urma_cfg.ubc_dev, &jfr_cfg, NULL, NULL);
	if (IS_ERR_OR_NULL(jfr)) {
		xmfs_error(NULL,
			   "ubcore_create_jfr failed during register send");
		return -EOPNOTSUPP;
	}
	sbi->urma_cfg.send_jfr = jfr;

	jetty_cfg.trans_mode = UBCORE_TP_RM;
	jetty_cfg.eid_index = ctx->host ? 0 : 1;
	jetty_cfg.max_send_sge = 1;
	jetty_cfg.max_recv_sge = 1;
	jetty_cfg.rnr_retry = 7;
	jetty_cfg.min_rnr_timer = 20U;
	jetty_cfg.err_timeout = 20;
	jetty_cfg.jfs_depth = XMFS_DEPTH;
	jetty_cfg.jfr_depth = XMFS_DEPTH;
	jetty_cfg.send_jfc = sbi->urma_cfg.send_jfc;
	jetty_cfg.recv_jfc = sbi->urma_cfg.recv_jfc;
	jetty_cfg.jfr = jfr;
	jetty_cfg.flag.bs.share_jfr = 1;
	jetty = ubcore_create_jetty(sbi->urma_cfg.ubc_dev, &jetty_cfg, NULL,
				    NULL);
	if (IS_ERR_OR_NULL(jetty)) {
		xmfs_error(NULL,
			   "ubcore_create_jetty failed during register send");
		return -EOPNOTSUPP;
	}
	sbi->urma_cfg.send_jetty = jetty;

	eid_info = ubcore_get_eid_list(sbi->urma_cfg.ubc_dev, &cnt);
	if (!eid_info) {
		xmfs_error(NULL,
			   "ubcore_get_eid_list failed during register send");
		return -EOPNOTSUPP;
	}

	sbi->urma_cfg.send_info.urma_send_va =
		(u64)(uintptr_t)sbi->urma_cfg.send_buf;
	sbi->urma_cfg.send_info.urma_recv_va =
		(u64)(uintptr_t)sbi->urma_cfg.recv_buf;
	sbi->urma_cfg.send_info.urma_eid_subnet_prefix =
		eid_info->eid.in6.subnet_prefix;
	sbi->urma_cfg.send_info.urma_eid_interface_id =
		eid_info->eid.in6.interface_id;
	sbi->urma_cfg.send_info.urma_jetty_id = jetty->jetty_id.id;
	sbi->urma_cfg.send_info.urma_send_token_id =
		sbi->urma_cfg.send_seg->seg.token_id;
	sbi->urma_cfg.send_info.urma_recv_token_id =
		sbi->urma_cfg.recv_seg->seg.token_id;
	xmfs_info(NULL, "send info %lld %lld %lld %lld %d %d %d",
		  sbi->urma_cfg.send_info.urma_send_va,
		  sbi->urma_cfg.send_info.urma_recv_va,
		  sbi->urma_cfg.send_info.urma_eid_subnet_prefix,
		  sbi->urma_cfg.send_info.urma_eid_interface_id,
		  sbi->urma_cfg.send_info.urma_jetty_id,
		  sbi->urma_cfg.send_info.urma_send_token_id,
		  sbi->urma_cfg.send_info.urma_recv_token_id);

	ubcore_free_eid_list(eid_info);

	for (int i = 0; i < XMFS_DEPTH; i++)
		xmfs_post_recv(sbi, i);

	return 0;
}

int xmfs_urma_register(struct xmfs_sb_info *sbi, struct xmfs_fs_context *ctx)
{
	struct ubcore_seg_cfg seg_cfg = { 0 };
	union ubcore_reg_seg_flag seg_flag = { 0 };
	struct ubcore_target_seg *seg;
	struct ubcore_jfc_cfg jfc_cfg = { 0 };
	struct ubcore_jfc *jfc;
	struct ubcore_jfr_cfg jfr_cfg = { 0 };
	struct ubcore_jfr *jfr;
	struct ubcore_jetty *jetty;
	struct ubcore_jetty_cfg jetty_cfg = { 0 };
	struct ubcore_eid_info *eid_info;
	int err, cnt = 0;

	if (!ctx->host) {
		sbi->urma_cfg.urma_remote_va = ctx->urma_va;
		sbi->urma_cfg.urma_eid_subnet_prefix =
			ctx->urma_eid_subnet_prefix;
		sbi->urma_cfg.urma_eid_interface_id =
			ctx->urma_eid_interface_id;
		sbi->urma_cfg.urma_jetty_id = ctx->urma_jetty_id;
		sbi->urma_cfg.urma_token_id = ctx->urma_token_id;
		sbi->urma_cfg.last_log_start = XMFS_DEFAULT_CHUNK_SIZE * 3;
		sbi->urma_cfg.last_data_start = XMFS_DEFAULT_CHUNK_SIZE * 3;
	}

	xmfs_info(NULL, "ctx name %ld %s", strlen(ctx->dev_name),
		  ctx->dev_name);
	memcpy(g_dev_name, ctx->dev_name, strlen(ctx->dev_name));
	xmfs_info(NULL, "g dev name %s", g_dev_name);

	INIT_LIST_HEAD(&sbi->urma_cfg.ubc_client.list_node);
	sbi->urma_cfg.ubc_client.client_name = "xmfs_urma_client";
	sbi->urma_cfg.ubc_client.add = xmfs_add_udma_device;
	sbi->urma_cfg.ubc_client.remove = xmfs_remove_udma_device;
	xa_init(&sbi->urma_cfg.grant_waits);

	err = ubcore_register_client(&sbi->urma_cfg.ubc_client);
	if (err) {
		xmfs_error(NULL, "ubcore_register_client failed");
		return -EOPNOTSUPP;
	}
	xmfs_info(NULL, "register client done %d %d %d %d", XMFS_ACCESS_READ,
		  XMFS_ACCESS_WRITE, XMFS_ACCESS_ATOMIC,
		  XMFS_ACCESS_LOCAL_ONLY);

	seg_flag.bs.token_policy = 0;
	if (ctx->debug)
		seg_flag.bs.access = XMFS_ACCESS_READ | XMFS_ACCESS_WRITE |
				     XMFS_ACCESS_ATOMIC |
				     XMFS_ACCESS_LOCAL_ONLY;
	else
		seg_flag.bs.access = XMFS_ACCESS_READ | XMFS_ACCESS_WRITE |
				     XMFS_ACCESS_ATOMIC;
	seg_flag.bs.non_pin = 1;

	if (ctx->host) {
		seg_cfg.va = (u64)(uintptr_t)sbi->kaddr;
		seg_cfg.len = sbi->len;
	} else {
		sbi->urma_cfg.urma_local_va = vzalloc(21474836480);
		if (!sbi->urma_cfg.urma_local_va) {
			xmfs_error(NULL, "vzalloc local va failed");
			return -ENOMEM;
		}
		seg_cfg.va = (u64)(uintptr_t)sbi->urma_cfg.urma_local_va;
		seg_cfg.len = 21474836480;
	}
	xmfs_urma_pool_init(&sbi->urma_cfg.pool, (void *)seg_cfg.va,
			    seg_cfg.len, PAGE_SIZE);
	seg_cfg.flag = seg_flag;
	sbi->urma_cfg.ubc_dev = g_urma_dev[0];
	seg = ubcore_register_seg(sbi->urma_cfg.ubc_dev, &seg_cfg, NULL);
	if (IS_ERR_OR_NULL(seg)) {
		xmfs_error(NULL, "ubcore_register_seg failed");
		return -EOPNOTSUPP;
	}
	sbi->urma_cfg.local_seg = seg;

	if (ctx->debug)
		jfc_cfg.depth = 2048;
	else
		jfc_cfg.depth = 524288;
	jfc_cfg.jfc_context = sbi;
	jfc = ubcore_create_jfc(sbi->urma_cfg.ubc_dev, &jfc_cfg,
				xmfs_jfc_callback, NULL, NULL);
	if (IS_ERR_OR_NULL(jfc)) {
		xmfs_error(NULL, "ubcore_create_jfc failed");
		return -EOPNOTSUPP;
	}
	sbi->urma_cfg.jfc = jfc;

	err = ubcore_rearm_jfc(jfc, false);
	if (err) {
		xmfs_error(NULL, "ubcore_rearm_jfc failed");
		return -EOPNOTSUPP;
	}

	if (ctx->debug)
		jfr_cfg.depth = 2048;
	else
		jfr_cfg.depth = 16384;
	jfr_cfg.trans_mode = UBCORE_TP_RM;
	jfr_cfg.min_rnr_timer = (u8)19U;
	jfr_cfg.max_sge = 1;
	jfr_cfg.jfc = jfc;
	jfr = ubcore_create_jfr(sbi->urma_cfg.ubc_dev, &jfr_cfg, NULL, NULL);
	if (IS_ERR_OR_NULL(jfr)) {
		xmfs_error(NULL, "ubcore_create_jfr failed");
		return -EOPNOTSUPP;
	}
	sbi->urma_cfg.jfr = jfr;

	jetty_cfg.trans_mode = UBCORE_TP_RM;
	jetty_cfg.eid_index = ctx->host ? 0 : 1;
	jetty_cfg.max_send_sge = 1;
	jetty_cfg.max_recv_sge = 1;
	jetty_cfg.rnr_retry = 7;
	jetty_cfg.min_rnr_timer = 20U;
	jetty_cfg.err_timeout = 20;
	if (ctx->debug)
		jetty_cfg.jfs_depth = 8192;
	else
		jetty_cfg.jfs_depth = 2048;
	jetty_cfg.jfr_depth = jfr_cfg.depth;
	jetty_cfg.send_jfc = jfc;
	jetty_cfg.recv_jfc = jfc;
	jetty_cfg.jfr = jfr;
	jetty_cfg.flag.bs.share_jfr = 1;
	for (int i = 0; i < 5; i++) {
		jetty = ubcore_create_jetty(sbi->urma_cfg.ubc_dev, &jetty_cfg,
					    NULL, NULL);
		if (IS_ERR_OR_NULL(jetty)) {
			xmfs_error(NULL, "ubcore_create_jetty failed");
			return -EOPNOTSUPP;
		}
		sbi->urma_cfg.jetty[i] = jetty;
	}

	eid_info = ubcore_get_eid_list(sbi->urma_cfg.ubc_dev, &cnt);
	if (!eid_info) {
		xmfs_error(NULL, "ubcore_get_eid_list failed");
		return -EOPNOTSUPP;
	}
	if (ctx->host) {
		sbi->urma_cfg.urma_remote_va = (u64)(uintptr_t)sbi->kaddr;
		sbi->urma_cfg.urma_eid_subnet_prefix =
			eid_info->eid.in6.subnet_prefix;
		sbi->urma_cfg.urma_eid_interface_id =
			eid_info->eid.in6.interface_id;
		sbi->urma_cfg.urma_jetty_id = jetty->jetty_id.id;
		sbi->urma_cfg.urma_token_id = seg->seg.token_id;
	} else {
		sbi->urma_cfg.own_info.urma_remote_va =
			(u64)(uintptr_t)sbi->urma_cfg.urma_local_va;
		sbi->urma_cfg.own_info.urma_eid_subnet_prefix =
			eid_info->eid.in6.subnet_prefix;
		sbi->urma_cfg.own_info.urma_eid_interface_id =
			eid_info->eid.in6.interface_id;
		sbi->urma_cfg.own_info.urma_jetty_id = jetty->jetty_id.id;
		sbi->urma_cfg.own_info.urma_token_id = seg->seg.token_id;
	}

	ubcore_free_eid_list(eid_info);

	xmfs_info(NULL, "register done");
	return xmfs_register_send_recv(sbi, ctx);
}

void xmfs_urma_unimport(struct xmfs_sb_info *sbi)
{
	if (!sbi)
		return;
	if (sbi->urma_cfg.remote_seg)
		ubcore_unimport_seg(sbi->urma_cfg.remote_seg);
	if (sbi->urma_cfg.tjetty) {
		/* ubcore_unbind_jetty(sbi->urma_cfg.jetty); */
		ubcore_unimport_jetty(sbi->urma_cfg.tjetty);
	}
	for (int i = 0; i < 64; i++) {
		if (sbi->urma_cfg.tjettys[i])
			ubcore_unimport_jetty(sbi->urma_cfg.tjettys[i]);
		if (sbi->urma_cfg.send_tjettys[i])
			ubcore_unimport_jetty(sbi->urma_cfg.send_tjettys[i]);
	}
}

int xmfs_import_jetty(struct xmfs_sb_info *sbi, struct urma_info info,
		      int index, bool is_send)
{
	struct ubcore_tjetty_cfg cfg = { 0 };
	struct ubcore_get_tp_cfg tp_flag = { 0 };
	struct ubcore_active_tp_cfg active_tp_cfg = { 0 };
	struct ubcore_tp_info tpid_info = { 0 };
	struct ubcore_tjetty *tjetty;
	struct ubcore_eid_info *eid_info;
	uint32_t tp_cnt = 1;
	int err, cnt = 0;

	eid_info = ubcore_get_eid_list(sbi->urma_cfg.ubc_dev, &cnt);
	if (!eid_info) {
		xmfs_error(NULL, "ubcore_get_eid_list failed");
		return -EOPNOTSUPP;
	}
	xmfs_info(NULL, "get eid list done");
	for (int i = 0; i < cnt; i++) {
		xmfs_info(NULL, "eid get during import %lld %lld",
			  eid_info->eid.in6.subnet_prefix,
			  eid_info->eid.in6.interface_id);
	}
	memcpy(&cfg.id.eid, &eid_info->eid, sizeof(union ubcore_eid));
	cfg.id.eid.in6.subnet_prefix = info.urma_eid_subnet_prefix;
	cfg.id.eid.in6.interface_id = info.urma_eid_interface_id;
	cfg.id.id = info.urma_jetty_id;
	cfg.trans_mode = UBCORE_TP_RM;
	cfg.type = UBCORE_JETTY;
	cfg.eid_index = 1;

	tp_flag.flag.bs.ctp = 1;
	tp_flag.trans_mode = UBCORE_TP_RM;
	memcpy(&tp_flag.peer_eid, &eid_info->eid, sizeof(union ubcore_eid));
	tp_flag.peer_eid.in6.subnet_prefix = info.urma_eid_subnet_prefix;
	tp_flag.peer_eid.in6.interface_id = info.urma_eid_interface_id;
	memcpy(&tp_flag.local_eid, &eid_info->eid, sizeof(union ubcore_eid));
	xmfs_info(NULL, "node %d import jetty %lld %lld %d", sbi->id,
		  info.urma_eid_interface_id, info.urma_eid_subnet_prefix,
		  info.urma_jetty_id);
	err = ubcore_get_tp_list(sbi->urma_cfg.ubc_dev, &tp_flag, &tp_cnt,
				 &tpid_info, NULL);
	if (err) {
		xmfs_error(NULL, "ubcore_get_tp_list failed.");
		return -EOPNOTSUPP;
	}
	xmfs_info(NULL, "get tp list done");
	active_tp_cfg.tp_handle = tpid_info.tp_handle;
	if (sbi->urma_cfg.ubc_dev->ops->import_jetty)
		tjetty = ubcore_import_jetty(sbi->urma_cfg.ubc_dev, &cfg, NULL);
	else
		tjetty = ubcore_import_jetty_ex(sbi->urma_cfg.ubc_dev, &cfg,
						&active_tp_cfg, NULL);
	if (IS_ERR_OR_NULL(tjetty)) {
		xmfs_error(NULL, "ubcore_import_jetty_ex failed.");
		return -EOPNOTSUPP;
	}
	if (!is_send) {
		if (sbi->urma_cfg.tjettys[index]) {
			xmfs_info(NULL, "unimport tjetty %d", index);
			err = ubcore_unimport_jetty(sbi->urma_cfg.tjettys[index]);
			xmfs_info(NULL, "unimport tjetty %d ret %d", index,
				  err);
		}
		sbi->urma_cfg.tjettys[index] = tjetty;
	} else {
		if (sbi->urma_cfg.send_tjettys[index]) {
			xmfs_info(NULL, "unimport send tjetty %d", index);
			err = ubcore_unimport_jetty(sbi->urma_cfg.send_tjettys[index]);
			xmfs_info(NULL, "unimport send tjetty %d ret %d", index,
				  err);
		}
		sbi->urma_cfg.send_tjettys[index] = tjetty;
	}

	ubcore_free_eid_list(eid_info);
	xmfs_info(NULL, "import jetty done");

	return 0;
}

int xmfs_urma_import(struct xmfs_sb_info *sbi)
{
	struct ubcore_tjetty_cfg cfg = { 0 };
	struct ubcore_get_tp_cfg tp_flag = { 0 };
	struct ubcore_active_tp_cfg active_tp_cfg = { 0 };
	struct ubcore_tp_info tpid_info = { 0 };
	struct ubcore_target_seg_cfg seg_cfg = { 0 };
	struct ubcore_tjetty *tjetty;
	struct ubcore_target_seg *tseg;
	struct ubcore_eid_info *eid_info;
	uint32_t tp_cnt = 1;
	int err, cnt = 0;

	xmfs_info(NULL, "start import");
	eid_info = ubcore_get_eid_list(sbi->urma_cfg.ubc_dev, &cnt);
	if (!eid_info) {
		xmfs_error(NULL, "ubcore_get_eid_list failed");
		return -EOPNOTSUPP;
	}
	for (int i = 0; i < cnt; i++) {
		xmfs_info(NULL, "eid get during import %lld %lld",
			  eid_info->eid.in6.subnet_prefix,
			  eid_info->eid.in6.interface_id);
	}
	xmfs_info(NULL, "get eid list done %lld %lld",
		  sbi->urma_cfg.urma_eid_subnet_prefix,
		  sbi->urma_cfg.urma_eid_interface_id);
	memcpy(&cfg.id.eid, &eid_info->eid, sizeof(union ubcore_eid));
	cfg.id.eid.in6.subnet_prefix = sbi->urma_cfg.urma_eid_subnet_prefix;
	cfg.id.eid.in6.interface_id = sbi->urma_cfg.urma_eid_interface_id;
	cfg.id.id = sbi->urma_cfg.urma_jetty_id;
	cfg.trans_mode = UBCORE_TP_RM;
	cfg.type = UBCORE_JETTY;
	cfg.eid_index = 1;

	tp_flag.flag.bs.ctp = 1;
	tp_flag.trans_mode = UBCORE_TP_RM;
	memcpy(&tp_flag.peer_eid, &eid_info->eid, sizeof(union ubcore_eid));
	tp_flag.peer_eid.in6.subnet_prefix =
		sbi->urma_cfg.urma_eid_subnet_prefix;
	tp_flag.peer_eid.in6.interface_id = sbi->urma_cfg.urma_eid_interface_id;
	memcpy(&tp_flag.local_eid, &eid_info->eid, sizeof(union ubcore_eid));

	err = ubcore_get_tp_list(sbi->urma_cfg.ubc_dev, &tp_flag, &tp_cnt,
				 &tpid_info, NULL);
	if (err) {
		xmfs_error(NULL, "ubcore_get_tp_list failed.");
		return -EOPNOTSUPP;
	}
	xmfs_info(NULL, "get tp list done");
	active_tp_cfg.tp_handle = tpid_info.tp_handle;
	if (sbi->urma_cfg.ubc_dev->ops->import_jetty)
		tjetty = ubcore_import_jetty(sbi->urma_cfg.ubc_dev, &cfg, NULL);
	else
		tjetty = ubcore_import_jetty_ex(sbi->urma_cfg.ubc_dev, &cfg,
						&active_tp_cfg, NULL);
	if (IS_ERR_OR_NULL(tjetty)) {
		xmfs_error(NULL, "ubcore_import_jetty_ex failed.");
		return -EOPNOTSUPP;
	}
	sbi->urma_cfg.tjetty = tjetty;
	xmfs_info(NULL, "import jetty done");
	/*
	 * err = ubcore_bind_jetty_ex(sbi->urma_cfg.jetty, tjetty,
	 *				   &active_tp_cfg, NULL);
	 * if (err) {
	 *	xmfs_error(sbi->sb, "ubcore_bind_jetty_ex failed.");
	 *	return -EOPNOTSUPP;
	 * }
	 */

	memcpy(&seg_cfg.seg.ubva.eid, &eid_info->eid, sizeof(union ubcore_eid));
	seg_cfg.seg.ubva.eid.in6.subnet_prefix =
		sbi->urma_cfg.urma_eid_subnet_prefix;
	seg_cfg.seg.ubva.eid.in6.interface_id =
		sbi->urma_cfg.urma_eid_interface_id;
	ubcore_free_eid_list(eid_info);
	seg_cfg.seg.ubva.va = sbi->urma_cfg.urma_remote_va;
	seg_cfg.seg.len = sbi->len;
	seg_cfg.seg.token_id = sbi->urma_cfg.urma_token_id;
	tseg = ubcore_import_seg(sbi->urma_cfg.ubc_dev, &seg_cfg, NULL);
	if (IS_ERR_OR_NULL(tseg)) {
		xmfs_error(NULL, "ubcore_import_seg failed.");
		return -EOPNOTSUPP;
	}
	xmfs_info(NULL, "import seg done");
	sbi->urma_cfg.remote_seg = tseg;

	return 0;
}

static int xmfs_urma_send_and_wait(struct xmfs_sb_info *sbi,
				   struct ubcore_jfs_wr *wr,
				   struct xmfs_urma_msg *msg)
{
	int err = 0;
	struct ubcore_jfs_wr *bad_wr = NULL;
	struct ubcore_jfs_wr *tmp;
	struct ubcore_jfs_wr *retry_wr;
	struct ubcore_jfs_wr *failed_head = NULL;
	int ret = 0;
	int retry;
	int timeout;
	unsigned int delay = 50;
	unsigned int multi = 1;

	if (sbi->debug)
		multi = 1000;

	if (sbi->debug)
		timeout = msecs_to_jiffies(20000);
	else
		timeout = msecs_to_jiffies(20000);

	retry_wr = wr;
	for (retry = 0; retry < XMFS_POST_RETRY_CNT && retry_wr; retry++) {
		bad_wr = NULL;
		err = ubcore_post_jetty_send_wr(sbi->urma_cfg.jetty[jiffies % 5],
						retry_wr, &bad_wr);
		if (!err) {
			retry_wr = NULL;
			break;
		}

		/*
		 * xmfs_error(sbi->sb,
		 *	      "qyf post wr failed ret=%d retry=%d bad_wr=%px",
		 *	      err, retry, bad_wr);
		 */

		if (bad_wr)
			retry_wr = bad_wr;
		usleep_range(delay * multi, delay * multi + 50 * multi);
		delay = min(delay * 2, 5000U);
	}
	failed_head = retry_wr;
	if (failed_head) {
		xmfs_error(NULL, "still have unposted wr after retry=%d",
			   XMFS_POST_RETRY_CNT);

		for (tmp = failed_head; tmp; tmp = tmp->next) {
			struct wr_status *wrs =
				(struct wr_status *)(uintptr_t)tmp->user_ctx;

			kfree(wrs);
			wrs = NULL;
		}

		ret = -EAGAIN;
	}

	for (tmp = wr; tmp != NULL && tmp != failed_head; tmp = tmp->next) {
		struct wr_status *wrs =
			(struct wr_status *)(uintptr_t)tmp->user_ctx;
		err = wait_for_completion_interruptible_timeout(&wrs->done,
								timeout);
		if (err <= 0) {
			xmfs_warning(sbi->sb,
				     "request wait failed: %d, op %d, status %d, offset %lld, length %lld",
				     err, wrs->ops, wrs->ret, msg->offset,
				     msg->len);
			ret = -EAGAIN;
		}

		if (wrs->status == URMA_ERROR) {
			xmfs_error(NULL,
				   "urma send get err %d ret %d ops %d len %d %d",
				   wrs->status, wrs->ret, wrs->ops,
				   wr->rw.src.sge->len, wr->rw.dst.sge->len);
			ret = -EAGAIN;
		}
		xmfs_wrs_put(&wrs);
	}

	return ret;
}

static int xmfs_urma_get_read_wr(struct xmfs_sb_info *sbi,
				 struct ubcore_jfs_wr **wr,
				 struct xmfs_urma_msg *msg)
{
	int trunk_size;

	if (sbi->debug)
		trunk_size = 4096;
	else
		trunk_size = XMFS_URMA_DEFAULT_WR_SIZE;

	uint64_t wr_count = DIV_ROUND_UP(msg->len, trunk_size);
	struct ubcore_jfs_wr *wr_ptr, *wr_pre = NULL;
	*wr = vzalloc(wr_count * sizeof(struct ubcore_jfs_wr));
	if (!*wr)
		return -ENOMEM;

	wr_ptr = *wr;
	for (uint64_t read_count = 0; read_count < msg->len;
	     read_count += trunk_size) {
		struct wr_status *wrs =
			kzalloc(sizeof(struct wr_status), GFP_KERNEL);
		if (!wrs)
			return -ENOMEM;

		wr_ptr->rw.src.sge = vzalloc(sizeof(struct ubcore_sge));
		wr_ptr->rw.dst.sge = vzalloc(sizeof(struct ubcore_sge));
		if (!wr_ptr->rw.src.sge || !wr_ptr->rw.dst.sge)
			return -ENOMEM;

		wrs->status = URMA_ISSUING;
		init_completion(&wrs->done);
		wrs->ret = 1;
		wrs->ops = XMFS_URMA_READ;
		wrs->magic = XMFS_MAGIC;
		refcount_set(&wrs->refs, 1);
		xmfs_wrs_get(wrs);
		uint64_t read_len = read_count + trunk_size < msg->len ?
					    trunk_size :
					    msg->len - read_count;
		if (wr_pre)
			wr_pre->next = wr_ptr;
		wr_ptr->opcode = UBCORE_OPC_READ;
		wr_ptr->flag.bs.complete_enable = 1;
		wr_ptr->tjetty = sbi->urma_cfg.tjetty;
		wr_ptr->rw.src.sge->addr =
			sbi->urma_cfg.urma_remote_va + msg->offset + read_count;
		wr_ptr->rw.src.sge->len = read_len;
		wr_ptr->rw.src.sge->tseg = sbi->urma_cfg.remote_seg;
		wr_ptr->rw.src.num_sge = 1;
		wr_ptr->rw.dst.sge->addr =
			(uint64_t)msg->local_addr + read_count;
		wr_ptr->rw.dst.sge->len = read_len;
		wr_ptr->rw.dst.sge->tseg = sbi->urma_cfg.local_seg;
		wr_ptr->rw.dst.num_sge = 1;
		wr_ptr->user_ctx = (uint64_t)(uintptr_t)wrs;
		wr_ptr->next = NULL;
		wr_pre = wr_ptr++;
	}
	msg->wr_cnt = wr_count;
	return 0;
}

static int xmfs_urma_get_write_wr(struct xmfs_sb_info *sbi,
				  struct ubcore_jfs_wr **wr,
				  struct xmfs_urma_msg *msg)
{
	*wr = vzalloc(sizeof(struct ubcore_jfs_wr));
	if (!*wr)
		return -ENOMEM;

	struct wr_status *wrs = kzalloc(sizeof(struct wr_status), GFP_KERNEL);

	if (!wrs)
		return -ENOMEM;

	(*wr)->rw.src.sge = vzalloc(sizeof(struct ubcore_sge));
	(*wr)->rw.dst.sge = vzalloc(sizeof(struct ubcore_sge));
	if (!(*wr)->rw.src.sge || !(*wr)->rw.dst.sge)
		return -ENOMEM;

	wrs->status = URMA_ISSUING;
	init_completion(&wrs->done);
	wrs->ret = 2;
	wrs->ops = XMFS_URMA_WRITE;
	wrs->magic = XMFS_MAGIC;
	wrs->slot = -1; /* msg.slot */
	refcount_set(&wrs->refs, 1);
	xmfs_wrs_get(wrs);
	memcpy(msg->write_buffer, msg->local_addr, msg->len);
	(*wr)->opcode = UBCORE_OPC_WRITE;
	(*wr)->flag.bs.complete_enable = 1;
	(*wr)->tjetty = sbi->urma_cfg.tjetty;
	(*wr)->rw.src.sge->addr = (uint64_t)msg->write_buffer;
	(*wr)->rw.src.sge->len = msg->len;
	(*wr)->rw.src.sge->tseg = sbi->urma_cfg.local_seg;
	(*wr)->rw.src.num_sge = 1;
	(*wr)->rw.dst.sge->addr = sbi->urma_cfg.urma_remote_va + msg->offset;
	(*wr)->rw.dst.sge->len = msg->len;
	(*wr)->rw.dst.sge->tseg = sbi->urma_cfg.remote_seg;
	(*wr)->rw.dst.num_sge = 1;
	(*wr)->user_ctx = (uint64_t)(uintptr_t)wrs;
	(*wr)->next = NULL;
	msg->wr_cnt = 1;
	return 0;
}

static int xmfs_urma_get_cas_wr(struct xmfs_sb_info *sbi,
				struct ubcore_jfs_wr **wr,
				struct xmfs_urma_msg *msg)
{
	*wr = vzalloc(sizeof(struct ubcore_jfs_wr));
	if (!*wr)
		return -ENOMEM;

	struct wr_status *wrs = kzalloc(sizeof(struct wr_status), GFP_KERNEL);

	if (!wrs)
		return -ENOMEM;

	(*wr)->cas.src = vzalloc(sizeof(struct ubcore_sge));
	(*wr)->cas.dst = vzalloc(sizeof(struct ubcore_sge));
	if (!(*wr)->cas.src || !(*wr)->cas.dst)
		return -ENOMEM;

	wrs->status = URMA_ISSUING;
	init_completion(&wrs->done);
	wrs->ret = 3;
	wrs->ops = XMFS_URMA_CAS;
	wrs->magic = XMFS_MAGIC;
	refcount_set(&wrs->refs, 1);
	xmfs_wrs_get(wrs);
	/* msg->local_addr = sbi->urma_cfg.urma_local_va; */
	(*wr)->opcode = UBCORE_OPC_CAS;
	(*wr)->flag.bs.complete_enable = 1;
	(*wr)->tjetty = sbi->urma_cfg.tjetty;
	(*wr)->cas.src->addr = (uint64_t)msg->local_addr;
	(*wr)->cas.src->len = msg->len;
	(*wr)->cas.src->tseg = sbi->urma_cfg.local_seg;
	(*wr)->cas.dst->addr = sbi->urma_cfg.urma_remote_va + msg->offset;
	(*wr)->cas.dst->len = msg->len;
	(*wr)->cas.dst->tseg = sbi->urma_cfg.remote_seg;
	(*wr)->cas.cmp_data = msg->cas_cmp_data;
	(*wr)->cas.swap_data = msg->cas_swap_data;
	(*wr)->user_ctx = (uint64_t)(uintptr_t)wrs;
	(*wr)->next = NULL;
	msg->wr_cnt = 1;
	return 0;
}

static void xmfs_urma_free_wr(struct xmfs_urma_msg *msg,
			      struct ubcore_jfs_wr *wr, int err)
{
	struct ubcore_jfs_wr *first_wr = wr;
	struct wr_status *wrs;

	if (!wr)
		return;
	while (wr) {
		switch (msg->ops) {
		case XMFS_URMA_READ:
		case XMFS_URMA_WRITE:
			vfree(wr->rw.src.sge);
			vfree(wr->rw.dst.sge);
			break;
		case XMFS_URMA_CAS:
			vfree(wr->cas.src);
			vfree(wr->cas.dst);
			break;
		default:
			break;
		}
		wrs = (struct wr_status *)wr->user_ctx;
		if (wrs != NULL && err != -EAGAIN && err != 0)
			kfree(wrs);

		wr = wr->next;
	}
	vfree(first_wr);
}

int xmfs_pmem_write(struct xmfs_sb_info *sbi, unsigned long offset,
		    const void *buf, unsigned long size)
{
	switch (sbi->mode) {
	case XMFS_SHARED_MEM_MODE:
	case XMFS_HYBRID_MODE:
		memcpy(sbi->kaddr + offset, buf, size);
		return 0;
#ifdef CONFIG_XMFS_FS_URMA
	case XMFS_URMA_MODE: {
		unsigned long written = 0;
		u32 slot;

		while (written < size) {
			unsigned long chunk = size - written;
			struct xmfs_urma_msg msg;
			int err;

			if (chunk > PAGE_SIZE)
				chunk = PAGE_SIZE;
			memset(&msg, 0, sizeof(msg));
			msg.ops = XMFS_URMA_WRITE;
			msg.offset = offset + written;
			msg.len = chunk;
			msg.local_addr = (void *)(buf + written);
			msg.write_buffer =
				urma_pool_acquire(&sbi->urma_cfg.pool, &slot);
			if (!msg.write_buffer) {
				xmfs_info(NULL,
					  "pmem write cannot get a buffer to write");
				return -EINVAL;
			}
			msg.slot = slot;
			err = xmfs_urma_send(sbi, &msg);
			urma_pool_release(&sbi->urma_cfg.pool, slot);
			if (err)
				return err;

			written += chunk;
		}
		return 0;
	}
#endif
	default:
		return -ENODEV;
	}
}
EXPORT_SYMBOL(xmfs_pmem_write);

static int
xmfs_urma_get_read_wr_pages(struct xmfs_sb_info *sbi, struct ubcore_jfs_wr **wr,
			    struct xmfs_urma_msg *msg, unsigned long *offsets,
			    struct page **pages, unsigned long page_num,
			    void **kaddrs, int *cnt)
{
	uint64_t wr_count = DIV_ROUND_UP(msg->len, PAGE_SIZE);
	struct ubcore_jfs_wr *wr_ptr, *wr_pre = NULL;
	*wr = vzalloc(wr_count * sizeof(struct ubcore_jfs_wr));
	if (!*wr)
		return -ENOMEM;

	wr_ptr = *wr;
	for (uint64_t read_count = 0; read_count < page_num; read_count++) {
		struct wr_status *wrs =
			kzalloc(sizeof(struct wr_status), GFP_KERNEL);
		if (!wrs)
			return -ENOMEM;

		wr_ptr->rw.src.sge = vzalloc(sizeof(struct ubcore_sge));
		wr_ptr->rw.dst.sge = vzalloc(sizeof(struct ubcore_sge));
		if (!wr_ptr->rw.src.sge || !wr_ptr->rw.dst.sge)
			return -ENOMEM;

		wrs->status = URMA_ISSUING;
		init_completion(&wrs->done);
		wrs->ret = 4;
		wrs->ops = XMFS_URMA_READ;
		wrs->magic = XMFS_MAGIC;
		refcount_set(&wrs->refs, 1);
		xmfs_wrs_get(wrs);
		uint64_t read_len = PAGE_SIZE;

		if (wr_pre)
			wr_pre->next = wr_ptr;

		kaddrs[read_count] = kmap_local_page(pages[read_count]);
		*cnt = read_count + 1;
		wr_ptr->opcode = UBCORE_OPC_READ;
		wr_ptr->flag.bs.complete_enable = 1;
		wr_ptr->tjetty = sbi->urma_cfg.tjetty;
		wr_ptr->rw.src.sge->addr =
			sbi->urma_cfg.urma_remote_va + offsets[read_count];
		wr_ptr->rw.src.sge->len = read_len;
		wr_ptr->rw.src.sge->tseg = sbi->urma_cfg.remote_seg;
		wr_ptr->rw.src.num_sge = 1;
		wr_ptr->rw.dst.sge->addr = (uint64_t)kaddrs[read_count];
		wr_ptr->rw.dst.sge->len = read_len;
		wr_ptr->rw.dst.sge->tseg = sbi->urma_cfg.local_seg;
		wr_ptr->rw.dst.num_sge = 1;
		wr_ptr->user_ctx = (uint64_t)(uintptr_t)wrs;
		wr_ptr->next = NULL;
		wr_pre = wr_ptr++;
	}
	return 0;
}

static int xmfs_urma_get_write_wr_pages(struct xmfs_sb_info *sbi,
					struct ubcore_jfs_wr **wr,
					struct xmfs_urma_msg *msg, int *slots,
					struct page **pages,
					unsigned long page_num)
{
	uint64_t wr_count = DIV_ROUND_UP(msg->len, PAGE_SIZE);
	struct ubcore_jfs_wr *wr_ptr, *wr_pre = NULL;
	*wr = vzalloc(wr_count * sizeof(struct ubcore_jfs_wr));
	if (!*wr)
		return -ENOMEM;

	wr_ptr = *wr;
	void *write_buffer =
		urma_pool_acquire_nslots(&sbi->urma_cfg.pool, page_num, slots);
	if (!write_buffer) {
		xmfs_info(NULL, "%s get write buffer failed", __func__);
		return -EAGAIN;
	}
	for (uint64_t read_count = 0; read_count < page_num; read_count++) {
		struct wr_status *wrs =
			kzalloc(sizeof(struct wr_status), GFP_KERNEL);
		if (!wrs)
			return -ENOMEM;

		wr_ptr->rw.src.sge = vzalloc(sizeof(struct ubcore_sge));
		wr_ptr->rw.dst.sge = vzalloc(sizeof(struct ubcore_sge));
		if (!wr_ptr->rw.src.sge || !wr_ptr->rw.dst.sge)
			return -ENOMEM;

		wrs->status = URMA_ISSUING;
		init_completion(&wrs->done);
		wrs->ret = 5;
		wrs->ops = XMFS_URMA_WRITE;
		wrs->magic = XMFS_MAGIC;
		refcount_set(&wrs->refs, 1);
		xmfs_wrs_get(wrs);
		wrs->slot = slots[read_count];
		uint64_t read_len = PAGE_SIZE;

		if (wr_pre)
			wr_pre->next = wr_ptr;

		void *kaddr = kmap_local_page(pages[read_count]);

		memcpy(write_buffer + read_count * PAGE_SIZE, kaddr, PAGE_SIZE);
		kunmap_local(kaddr);

		wr_ptr->opcode = UBCORE_OPC_WRITE;
		wr_ptr->flag.bs.complete_enable = 1;
		wr_ptr->tjetty = sbi->urma_cfg.tjetty;
		wr_ptr->rw.dst.sge->addr = sbi->urma_cfg.urma_remote_va +
					   msg->offset + read_count * PAGE_SIZE;
		wr_ptr->rw.dst.sge->len = read_len;
		wr_ptr->rw.dst.sge->tseg = sbi->urma_cfg.remote_seg;
		wr_ptr->rw.dst.num_sge = 1;
		wr_ptr->rw.src.sge->addr =
			(uint64_t)(write_buffer + read_count * PAGE_SIZE);
		wr_ptr->rw.src.sge->len = read_len;
		wr_ptr->rw.src.sge->tseg = sbi->urma_cfg.local_seg;
		wr_ptr->rw.src.num_sge = 1;
		wr_ptr->user_ctx = (uint64_t)(uintptr_t)wrs;
		wr_ptr->next = NULL;
		wr_pre = wr_ptr++;
	}
	return 0;
}

int xmfs_urma_send_pages(struct xmfs_sb_info *sbi, struct xmfs_urma_msg *msg,
			 unsigned long *offsets, struct page **pages,
			 unsigned long page_num)
{
	struct ubcore_jfs_wr *wr = NULL;
	int *slots = vzalloc(sizeof(int) * page_num);

	if (!slots)
		return -ENOMEM;
	void **kaddrs = vzalloc(sizeof(void *) * page_num);

	if (!kaddrs) {
		vfree(slots);
		return -ENOMEM;
	}
	int err = 0;
	int cnt = 0;

	switch (msg->ops) {
	case XMFS_URMA_READ:
		err = xmfs_urma_get_read_wr_pages(sbi, &wr, msg, offsets, pages,
						  page_num, kaddrs, &cnt);
		break;
	case XMFS_URMA_WRITE:
		err = xmfs_urma_get_write_wr_pages(sbi, &wr, msg, slots, pages,
						   page_num);
		break;
	default:
		xmfs_error(NULL, "What exactly are you going to do?");
		err = -EOPNOTSUPP;
	}

	if (err) {
		xmfs_error(NULL, "failed to alloc wr or local buffer %ld %d",
			   page_num, err);
		goto free_ret;
	}

	err = xmfs_urma_send_and_wait(sbi, wr, msg);

free_ret:
	if (msg->ops == XMFS_URMA_READ) {
		for (int i = 0; i < cnt; i++)
			kunmap_local(kaddrs[i]);
	}
	vfree(kaddrs);
	xmfs_urma_free_wr(msg, wr, err);
	vfree(slots);
	return err;
}

int xmfs_pmem_write_multi_pages(struct xmfs_sb_info *sbi, unsigned long offset,
				struct page **pages, unsigned long page_num)
{
	switch (sbi->mode) {
	case XMFS_SHARED_MEM_MODE:
	case XMFS_HYBRID_MODE:
		/* memcpy(sbi->kaddr + offset, buf, size); */
		return 0;
#ifdef CONFIG_XMFS_FS_URMA
	case XMFS_URMA_MODE: {
		if (page_num == 0)
			return 0;
		unsigned long chunk = PAGE_SIZE * page_num;
		struct xmfs_urma_msg msg;
		int err;

		memset(&msg, 0, sizeof(msg));
		msg.ops = XMFS_URMA_WRITE;
		msg.offset = offset;
		msg.len = chunk;
		err = xmfs_urma_send_pages(sbi, &msg, &offset, pages, page_num);
		if (err)
			return err;

		return 0;
	}
#endif
	default:
		return -ENODEV;
	}
}
EXPORT_SYMBOL(xmfs_pmem_write_multi_pages);

int xmfs_pmem_read_multi_pages(struct xmfs_sb_info *sbi, unsigned long *offsets,
			       struct page **pages, unsigned long page_num)
{
	switch (sbi->mode) {
	case XMFS_SHARED_MEM_MODE:
	case XMFS_HYBRID_MODE:
		/* memcpy(sbi->kaddr + offset, buf, size); */
		return 0;
#ifdef CONFIG_XMFS_FS_URMA
	case XMFS_URMA_MODE: {
		if (page_num == 0)
			return 0;
		unsigned long chunk = PAGE_SIZE * page_num;
		struct xmfs_urma_msg msg;
		int err;

		memset(&msg, 0, sizeof(msg));
		msg.ops = XMFS_URMA_READ;
		msg.len = chunk;
		err = xmfs_urma_send_pages(sbi, &msg, offsets, pages, page_num);
		if (err)
			return err;

		return 0;
	}
#endif
	default:
		return -ENODEV;
	}
}
EXPORT_SYMBOL(xmfs_pmem_read_multi_pages);

int xmfs_send_request(struct xmfs_sb_info *sbi, struct xmfs_rw_msg *req,
		      struct xmfs_grant_wait *gw, int id)
{
	struct ubcore_jfs_wr *bad_send_wr = NULL;
	struct ubcore_jfs_wr send_wr = {};
	struct ubcore_sge sge = {};
	int ret, slot;
	struct xmfs_rw_msg *buf;

	int timeout;

	if (sbi->debug)
		timeout = msecs_to_jiffies(20000);
	else
		timeout = msecs_to_jiffies(20000);

	buf = urma_pool_acquire(&sbi->urma_cfg.send_pool, &slot);
	if (!buf) {
		xmfs_info(NULL, "cannot get buf to send request");
		return -ENOMEM;
	}
	/* xmfs_info(sbi->sb, "qyf acquire slot %d", slot); */
	memcpy(buf, req, sizeof(struct xmfs_rw_msg));
	sge.addr = (uint64_t)buf;
	sge.len = sizeof(*req);
	sge.tseg = sbi->urma_cfg.send_seg;

	send_wr.opcode = UBCORE_OPC_SEND;
	send_wr.tjetty = sbi->urma_cfg.send_tjettys[id];
	send_wr.send.src.sge = &sge;
	send_wr.send.src.num_sge = 1;
	send_wr.flag.value = 0;
	send_wr.flag.bs.complete_enable = 1;
	send_wr.user_ctx = slot;
	send_wr.next = NULL;
	/*
	 * xmfs_info(sbi->sb, "qyf send request send pool slot %d id %d",
	 *	     slot, req->request_id);
	 */
	if (req->ops == XMFS_GC)
		xmfs_info(NULL, "gc req send %d", req->status);
	ret = ubcore_post_jetty_send_wr(sbi->urma_cfg.send_jetty, &send_wr,
					&bad_send_wr);
	if (ret) {
		xmfs_info(NULL, "err send request failed %d, release %d", ret,
			  slot);
		urma_pool_release(&sbi->urma_cfg.send_pool, slot);
		if (req->ops != XMFS_SETATTR && req->ops != XMFS_GC)
			xa_erase(&sbi->urma_cfg.grant_waits, req->request_id);
		return ret;
	}

	if (req->ops == XMFS_SETATTR || req->ops == XMFS_GC)
		return 0;

	ret = wait_for_completion_interruptible_timeout(&gw->done, timeout);
	xa_erase(&sbi->urma_cfg.grant_waits, req->request_id);
	/*
	 * xmfs_info(sbi->sb, "qyf send request done send pool slot %d id %d",
	 *	     slot, req->request_id);
	 */
	if (ret <= 0) {
		if (ret == 0)
			ret = -ETIMEDOUT;
		xmfs_warning(sbi->sb,
			     "slave request wait failed: %d, request ID %ld",
			     ret, req->request_id);
		return ret;
	}
	if (gw->grant.type != MSG_RW_GRANT) {
		xmfs_warning(sbi->sb,
			     "unexpected grant: type %d, request ID %ld, op %d",
			     gw->grant.type, gw->grant.request_id,
			     gw->grant.ops);
		return -EINVAL;
	}

	return 0;
}

/*
 * static int xmfs_urma_post_wr(struct xmfs_sb_info *sbi,
 *				    struct ubcore_jfs_wr *wr)
 * {
 *	struct ubcore_jfs_wr *bad_wr = NULL;
 *	int err = ubcore_post_jetty_send_wr(sbi->urma_cfg.jetty, wr,
 *					   &bad_wr);
 *
 *	if (err)
 *		xmfs_error(sbi->sb,
 *			   "ubcore_post_jetty_send_wr failed %d", err);
 *	return err;
 * }
 *
 * static int xmfs_urma_wait_chain(struct ubcore_jfs_wr *wr)
 * {
 *	struct ubcore_jfs_wr *tmp;
 *	int ret = 0;
 *
 *	for (tmp = wr; tmp != NULL; tmp = tmp->next) {
 *		struct wr_status *wrs =
 *			(struct wr_status *)(uintptr_t)tmp->user_ctx;
 *		int err;
 *
 *		if (wrs->status != URMA_ISSUING)
 *			goto skip_wait;
 *		err = wait_for_completion_interruptible_timeout(
 *			&wrs->done, msecs_to_jiffies(20000));
 *		if (err <= 0) {
 *			xmfs_info(NULL,
 *				  "wait for wr timeout or interrupted %d ops %d wrs->ret %d",
 *				  err, wrs->ops, wrs->ret);
 *			ret = -EAGAIN;
 *		}
 * skip_wait:
 *		if (wrs->status == URMA_ERROR) {
 *			xmfs_error(NULL,
 *				   "urma send get err status=%d ret=%d ops=%d len src=%d dst=%d",
 *				   wrs->status, wrs->ret, wrs->ops,
 *				   tmp->rw.src.sge->len,
 *				   tmp->rw.dst.sge->len);
 *			ret = -EAGAIN;
 *		}
 *		xmfs_wrs_put(&wrs);
 *	}
 *	return ret;
 * }
 *
 * static int xmfs_urma_send_and_wait_new(struct xmfs_sb_info *sbi,
 *					  struct ubcore_jfs_wr *wr,
 *					  struct xmfs_urma_msg *msg)
 * {
 *	int err = xmfs_urma_post_wr(sbi, wr);
 *
 *	if (err)
 *		return err;
 *	return xmfs_urma_wait_chain(wr);
 * }
 *
 * struct xmfs_urma_pending {
 *	struct ubcore_jfs_wr *wr;
 *	void *write_buffer;
 *	int *slots;
 *	int nr_slots;
 *	struct xmfs_urma_msg msg;
 * };
 *
 * static void wr_chain_append(struct ubcore_jfs_wr **chain_head,
 *			       struct ubcore_jfs_wr **tail,
 *			       struct ubcore_jfs_wr *wr)
 * {
 *	wr->next = NULL;
 *	if (*chain_head == NULL) {
 *		*chain_head = wr;
 *		*tail = wr;
 *	} else {
 *		(*tail)->next = wr;
 *		*tail = wr;
 *	}
 * }
 */

static int xmfs_urma_get_write_wr_ubuf(struct xmfs_sb_info *sbi,
				       struct ubcore_jfs_wr **wr,
				       struct xmfs_urma_msg *msg)
{
	*wr = vzalloc(sizeof(struct ubcore_jfs_wr));
	if (!*wr)
		return -ENOMEM;

	struct wr_status *wrs = kzalloc(sizeof(struct wr_status), GFP_KERNEL);

	if (!wrs)
		return -ENOMEM;

	(*wr)->rw.src.sge = vzalloc(sizeof(struct ubcore_sge));
	(*wr)->rw.dst.sge = vzalloc(sizeof(struct ubcore_sge));
	if (!(*wr)->rw.src.sge || !(*wr)->rw.dst.sge)
		return -ENOMEM;

	wrs->status = URMA_ISSUING;
	init_completion(&wrs->done);
	wrs->ret = 7;
	wrs->ops = XMFS_URMA_WRITE;
	wrs->magic = XMFS_MAGIC;
	wrs->slot = -1; /* msg.slot */
	refcount_set(&wrs->refs, 1);
	xmfs_wrs_get(wrs);
	(*wr)->opcode = UBCORE_OPC_WRITE;
	(*wr)->flag.bs.complete_enable = 1;
	(*wr)->tjetty = sbi->urma_cfg.tjetty;
	(*wr)->rw.src.sge->addr = (uint64_t)msg->write_buffer;
	(*wr)->rw.src.sge->len = msg->len;
	(*wr)->rw.src.sge->tseg = sbi->urma_cfg.local_seg;
	(*wr)->rw.src.num_sge = 1;
	(*wr)->rw.dst.sge->addr = sbi->urma_cfg.urma_remote_va + msg->offset;
	(*wr)->rw.dst.sge->len = msg->len;
	(*wr)->rw.dst.sge->tseg = sbi->urma_cfg.remote_seg;
	(*wr)->rw.dst.num_sge = 1;
	(*wr)->user_ctx = (uint64_t)(uintptr_t)wrs;
	(*wr)->next = NULL;
	msg->wr_cnt = 1;
	return 0;
}

static int xmfs_urma_get_read_wr_ubuf(struct xmfs_sb_info *sbi,
				      struct ubcore_jfs_wr **wr,
				      struct xmfs_urma_msg *msg)
{
	int trunk_size;

	if (sbi->debug)
		trunk_size = 4096;
	else
		trunk_size = msg->len;

	uint64_t wr_count = DIV_ROUND_UP(msg->len, trunk_size);
	struct ubcore_jfs_wr *wr_ptr, *wr_pre = NULL;
	*wr = vzalloc(wr_count * sizeof(struct ubcore_jfs_wr));
	if (!*wr)
		return -ENOMEM;

	wr_ptr = *wr;
	for (uint64_t read_count = 0; read_count < msg->len;
	     read_count += trunk_size) {
		struct wr_status *wrs =
			kzalloc(sizeof(struct wr_status), GFP_KERNEL);
		if (!wrs)
			return -ENOMEM;

		wr_ptr->rw.src.sge = vzalloc(sizeof(struct ubcore_sge));
		wr_ptr->rw.dst.sge = vzalloc(sizeof(struct ubcore_sge));
		if (!wr_ptr->rw.src.sge || !wr_ptr->rw.dst.sge)
			return -ENOMEM;

		wrs->status = URMA_ISSUING;
		init_completion(&wrs->done);
		wrs->ret = 1;
		wrs->ops = XMFS_URMA_READ;
		wrs->magic = XMFS_MAGIC;
		refcount_set(&wrs->refs, 1);
		xmfs_wrs_get(wrs);
		uint64_t read_len = read_count + trunk_size < msg->len ?
					    trunk_size :
					    msg->len - read_count;
		if (wr_pre)
			wr_pre->next = wr_ptr;
		wr_ptr->opcode = UBCORE_OPC_READ;
		wr_ptr->flag.bs.complete_enable = 1;
		wr_ptr->tjetty = sbi->urma_cfg.tjetty;
		wr_ptr->rw.src.sge->addr =
			sbi->urma_cfg.urma_remote_va + msg->offset + read_count;
		wr_ptr->rw.src.sge->len = read_len;
		wr_ptr->rw.src.sge->tseg = sbi->urma_cfg.remote_seg;
		wr_ptr->rw.src.num_sge = 1;
		wr_ptr->rw.dst.sge->addr =
			(uint64_t)msg->local_addr + read_count;
		wr_ptr->rw.dst.sge->len = read_len;
		wr_ptr->rw.dst.sge->tseg = sbi->urma_cfg.local_seg;
		wr_ptr->rw.dst.num_sge = 1;
		wr_ptr->user_ctx = (uint64_t)(uintptr_t)wrs;
		wr_ptr->next = NULL;
		wr_pre = wr_ptr++;
	}
	msg->wr_cnt = wr_count;

	/*
	 * *wr = (struct ubcore_jfs_wr *)vzalloc(
	 *				      sizeof(struct ubcore_jfs_wr));
	 * if (!*wr)
	 *	return -ENOMEM;
	 * struct wr_status *wrs = vzalloc(sizeof(struct wr_status));
	 * if (!wrs)
	 *	return -ENOMEM;
	 * (*wr)->rw.src.sge = vzalloc(sizeof(struct ubcore_sge));
	 * (*wr)->rw.dst.sge = vzalloc(sizeof(struct ubcore_sge));
	 * if (!(*wr)->rw.src.sge || !(*wr)->rw.dst.sge)
	 *	return -ENOMEM;
	 * wrs->status = URMA_ISSUING;
	 * init_completion(&wrs->done);
	 * wrs->ret = 8;
	 * wrs->ops = XMFS_URMA_READ;
	 * wrs->magic = XMFS_MAGIC;
	 * wrs->slot = -1;
	 * refcount_set(&wrs->refs, 1);
	 * xmfs_wrs_get(wrs);
	 * (*wr)->opcode = UBCORE_OPC_READ;
	 * (*wr)->flag.bs.complete_enable = 1;
	 * (*wr)->tjetty = sbi->urma_cfg.tjetty;
	 * (*wr)->rw.dst.sge->addr = (uint64_t)msg->local_addr;
	 * (*wr)->rw.dst.sge->len = msg->len;
	 * (*wr)->rw.dst.sge->tseg = sbi->urma_cfg.local_seg;
	 * (*wr)->rw.dst.num_sge = 1;
	 * (*wr)->rw.src.sge->addr =
	 *	sbi->urma_cfg.urma_remote_va + msg->offset;
	 * (*wr)->rw.src.sge->len = msg->len;
	 * (*wr)->rw.src.sge->tseg = sbi->urma_cfg.remote_seg;
	 * (*wr)->rw.src.num_sge = 1;
	 * (*wr)->user_ctx = (uint64_t)(uintptr_t)wrs;
	 * (*wr)->next = NULL;
	 * msg->wr_cnt = 1;
	 */
	return 0;
}

static int xmfs_urma_send_ubuf(struct xmfs_sb_info *sbi,
			       struct xmfs_urma_msg *msg)
{
	struct ubcore_jfs_wr *wr = NULL;
	int err = 0;

	switch (msg->ops) {
	case XMFS_URMA_READ:
		err = xmfs_urma_get_read_wr_ubuf(sbi, &wr, msg);
		break;
	case XMFS_URMA_WRITE:
		err = xmfs_urma_get_write_wr_ubuf(sbi, &wr, msg);
		break;
	default:
		xmfs_error(NULL, "What exactly are you going to do?");
		return -EOPNOTSUPP;
	}

	if (err) {
		xmfs_error(NULL, "failed to alloc wr or local buffer");
		goto free_ret;
	}

	err = xmfs_urma_send_and_wait(sbi, wr, msg);

free_ret:
	xmfs_urma_free_wr(msg, wr, err);
	return err;
}

int xmfs_urma_write_ubuf(struct xmfs_sb_info *sbi, void __user *buf,
			 size_t size, unsigned long offset)
{
	int chunk_size;

	if (sbi->debug)
		chunk_size = PAGE_SIZE;
	else
		chunk_size = XMFS_DEFAULT_CHUNK_SIZE - PAGE_SIZE;
	int *slots = vzalloc(sizeof(int) * (size + PAGE_SIZE - 1) / PAGE_SIZE);

	if (!slots)
		return -ENOMEM;
	int loop_cnt = 0;
	int chunk_header_cnt = 0;
	size_t written = 0;
	int err;
	/*
	 * xmfs_info(sbi->sb, "qyf before acquire nslots %d",
	 *	     (size + PAGE_SIZE * 16 - 1) / (PAGE_SIZE * 16));
	 */
	void *write_buffer =
		urma_pool_acquire_nslots(&sbi->urma_cfg.pool,
					 (size + PAGE_SIZE - 1) / PAGE_SIZE,
					 slots);
	if (!write_buffer) {
		vfree(slots);
		xmfs_info(NULL, "write ubuf cannot get a buffer to write");
		return -EINVAL;
	}
	/* xmfs_info(sbi->sb, "qyf before copy from user"); */
	err = copy_from_user(write_buffer, buf, size);
	if (err) {
		xmfs_error(NULL, "write ubuf cannot copy from, size is %ld", size);
		urma_pool_release_nslots(&sbi->urma_cfg.pool,
				 (size + PAGE_SIZE - 1) / PAGE_SIZE, slots);
		vfree(slots);
		return err;
	}
	/* xmfs_info(sbi->sb, "qyf end copy from user"); */
	while (written < size) {
		unsigned long chunk = size - written;
		struct xmfs_urma_msg msg;

		if (chunk > chunk_size)
			chunk = chunk_size;
		msg.ops = XMFS_URMA_WRITE;
		msg.len = chunk_size;
		msg.offset = offset + loop_cnt * chunk_size +
			     chunk_header_cnt * PAGE_SIZE;
		if (msg.offset % XMFS_DEFAULT_CHUNK_SIZE == 0) {
			chunk_header_cnt++;
			msg.offset = offset + loop_cnt * chunk_size +
				     chunk_header_cnt * PAGE_SIZE;
		}
		/*
		 * xmfs_info(sbi->sb,
		 *	     "write ubuf msg len %d loopcnt %d offset %ld header cnt %d",
		 *	     chunk_size, loop_cnt, msg.offset, chunk_header_cnt);
		 */
		msg.write_buffer = write_buffer + written;
		/* TODO */
		err = xmfs_urma_send_ubuf(sbi, &msg);
		if (err)
			break;

		written += chunk;
		loop_cnt++;
		/* xmfs_info(sbi->sb, "qyf end send and wait"); */
	}
	urma_pool_release_nslots(&sbi->urma_cfg.pool,
				 (size + PAGE_SIZE - 1) / PAGE_SIZE, slots);
	/* xmfs_info(sbi->sb, "qyf end release"); */
	vfree(slots);
	return err;
}

int xmfs_urma_read_ubuf(struct xmfs_sb_info *sbi, void __user *buf,
			struct xmfs_read_extent *exts, int ext_cnt, size_t size)
{
	pgoff_t start_index;
	unsigned long start_offset;
	unsigned int nr_pages;
	unsigned long offset = 0;
	struct xmfs_urma_msg msg;
	void *local_buf = vzalloc(size);
	int err;

	msg.ops = XMFS_URMA_READ;

	for (int i = 0; i < ext_cnt; i++) {
		start_index = exts[i].start_index;
		start_offset = exts[i].start_offset;
		nr_pages = exts[i].nr_pages;
		msg.local_addr = local_buf + offset;
		msg.len = min(nr_pages * PAGE_SIZE, size - offset);
		msg.offset = start_offset;
		/*
		 * xmfs_info(sbi->sb,
		 *	     "qyf urma read ubuf from %ld read %ld "
		 *	     "real len %ld offset %ld size %ld",
		 *	     start_offset, nr_pages * PAGE_SIZE, msg.len, offset,
		 *	     size);
		 */
		err = xmfs_urma_send_ubuf(sbi, &msg);
		/* xmfs_info(sbi->sb, "qyf urma send ubuf done %d", err); */
		offset += msg.len;
	}
	err = copy_to_user(buf, local_buf, size);
	if (err)
		xmfs_error(NULL, "read ubuf cannot copy to, size is %ld", size);

	vfree(local_buf);
	return err;
}

int xmfs_urma_fill_write_page(struct xmfs_sb_info *sbi,
			      struct ubcore_jfs_wr *wr,
			      struct xmfs_urma_msg *msg, int slot)
{
	struct wr_status *wrs = kzalloc(sizeof(struct wr_status), GFP_KERNEL);

	if (!wrs)
		return -ENOMEM;
	wr->rw.src.sge = vzalloc(sizeof(struct ubcore_sge));
	wr->rw.dst.sge = vzalloc(sizeof(struct ubcore_sge));
	if (!wr->rw.src.sge || !wr->rw.dst.sge) {
		kfree(wrs);
		if (wr->rw.src.sge)
			vfree(wr->rw.src.sge);
		if (wr->rw.dst.sge)
			vfree(wr->rw.dst.sge);
		return -ENOMEM;
	}

	wrs->status = URMA_ISSUING;
	init_completion(&wrs->done);
	wrs->ret = 6;
	wrs->ops = XMFS_URMA_WRITE;
	wrs->magic = XMFS_MAGIC;
	wrs->slot = -1; /* slot */
	refcount_set(&wrs->refs, 1);
	xmfs_wrs_get(wrs);
	memcpy(msg->write_buffer, msg->local_addr, msg->len);

	wr->opcode = UBCORE_OPC_WRITE;
	wr->flag.bs.complete_enable = 1;
	wr->tjetty = sbi->urma_cfg.tjetty;
	wr->rw.src.sge->addr = (uint64_t)msg->write_buffer;
	wr->rw.src.sge->len = msg->len;
	wr->rw.src.sge->tseg = sbi->urma_cfg.local_seg;
	wr->rw.src.num_sge = 1;
	wr->rw.dst.sge->addr = sbi->urma_cfg.urma_remote_va + msg->offset;
	wr->rw.dst.sge->len = msg->len;
	wr->rw.dst.sge->tseg = sbi->urma_cfg.remote_seg;
	wr->rw.dst.num_sge = 1;
	wr->user_ctx = (uint64_t)(uintptr_t)wrs;
	wr->next = NULL;
	msg->wr_cnt = 1;
	return 0;
}

int xmfs_urma_send_write_pages(struct xmfs_sb_info *sbi,
			       struct ubcore_jfs_wr *wr)
{
	struct ubcore_jfs_wr *cur;
	struct wr_status *wrs;
	int err;

	if (!wr)
		return -EINVAL;
	struct xmfs_urma_msg msg;

	msg.offset = 0;
	msg.len = 0;
	err = xmfs_urma_send_and_wait(sbi, &wr[0], &msg);

	cur = &wr[0];
	while (cur) {
		vfree(cur->rw.src.sge);
		vfree(cur->rw.dst.sge);
		wrs = (struct wr_status *)cur->user_ctx;
		if (wrs != NULL && err != -EAGAIN && err != 0) {
			/* xmfs_info(sbi->sb, "qyf FREE 4 wrs=%px", wrs); */
			kfree(wrs);
		}
		cur = cur->next;
	}

	vfree(wr);

	return err;
}

int xmfs_pmem_read(struct xmfs_sb_info *sbi, unsigned long offset, void *buf,
		   unsigned long size)
{
	switch (sbi->mode) {
	case XMFS_SHARED_MEM_MODE:
	case XMFS_HYBRID_MODE:
		memcpy(buf, sbi->kaddr + offset, size);
		return 0;
#ifdef CONFIG_XMFS_FS_URMA
	case XMFS_URMA_MODE:
		return get_meta_and_copy(sbi, offset, buf, size, NULL, 0);
#endif
	default:
		return -ENODEV;
	}
}
EXPORT_SYMBOL(xmfs_pmem_read);

int xmfs_pmem_cas(struct xmfs_sb_info *sbi, unsigned long offset,
		  uint64_t old_val, uint64_t new_val)
{
	switch (sbi->mode) {
	case XMFS_SHARED_MEM_MODE:
	case XMFS_HYBRID_MODE: {
		uint64_t *p = (uint64_t *)(sbi->kaddr + offset);

		return atomic64_cmpxchg((atomic64_t *)p, old_val, new_val) ==
				       old_val ?
			       0 :
			       -EAGAIN;
	}
#ifdef CONFIG_XMFS_FS_URMA
	case XMFS_URMA_MODE: {
		struct xmfs_urma_msg msg;
		uint64_t result;
		int err;

		memset(&msg, 0, sizeof(msg));
		msg.ops = XMFS_URMA_CAS;
		msg.offset = offset;
		msg.len = 8;
		msg.local_addr = &result;
		msg.cas_cmp_data = old_val;
		msg.cas_swap_data = new_val;
		err = xmfs_urma_send(sbi, &msg);
		if (err)
			return err;
		return result == old_val ? 0 : -EAGAIN;
	}
#endif
	default:
		return -ENODEV;
	}
}
EXPORT_SYMBOL(xmfs_pmem_cas);

int xmfs_urma_send(struct xmfs_sb_info *sbi, struct xmfs_urma_msg *msg)
{
	struct ubcore_jfs_wr *wr = NULL;
	int err = 0;

	if (msg->ops == XMFS_URMA_WRITE && msg->len > PAGE_SIZE) {
		xmfs_error(NULL,
			   "write more than PAGE_SIZE is now unsupported");
		return -EOPNOTSUPP;
	}

	switch (msg->ops) {
	case XMFS_URMA_READ:
		err = xmfs_urma_get_read_wr(sbi, &wr, msg);
		break;
	case XMFS_URMA_WRITE:
		err = xmfs_urma_get_write_wr(sbi, &wr, msg);
		break;
	case XMFS_URMA_CAS:
		err = xmfs_urma_get_cas_wr(sbi, &wr, msg);
		break;
	default:
		xmfs_error(NULL, "What exactly are you going to do?");
		return -EOPNOTSUPP;
	}

	if (err) {
		xmfs_error(NULL, "failed to alloc wr or local buffer");
		goto free_ret;
	}

	err = xmfs_urma_send_and_wait(sbi, wr, msg);

free_ret:
	xmfs_urma_free_wr(msg, wr, err);
	return err;
}

#else

void xmfs_urma_unregister(struct xmfs_sb_info *sbi)
{
}

int xmfs_urma_register(struct xmfs_sb_info *sbi, struct xmfs_fs_context *ctx)
{
	return -EOPNOTSUPP;
}

void xmfs_urma_unimport(struct xmfs_sb_info *sbi)
{
}

int xmfs_urma_import(struct xmfs_sb_info *sbi)
{
	return -EOPNOTSUPP;
}

int xmfs_import_jetty(struct xmfs_sb_info *sbi, struct urma_info info,
		      int index, bool is_send)
{
	return -EOPNOTSUPP;
}

int xmfs_send_request(struct xmfs_sb_info *sbi, struct xmfs_rw_msg *req,
		      struct xmfs_grant_wait *gw, int id)
{
	return -EOPNOTSUPP;
}

int xmfs_urma_send(struct xmfs_sb_info *sbi, struct xmfs_urma_msg *msg)
{
	return -EOPNOTSUPP;
}

int xmfs_pmem_write_multi_pages(struct xmfs_sb_info *sbi, unsigned long offset,
				struct page **pages, unsigned long page_num)
{
	return -EOPNOTSUPP;
}

int xmfs_pmem_read_multi_pages(struct xmfs_sb_info *sbi, unsigned long *offsets,
			       struct page **pages, unsigned long page_num)
{
	return -EOPNOTSUPP;
}

int xmfs_urma_write_ubuf(struct xmfs_sb_info *sbi, void __user *buf,
			 size_t size, unsigned long offset)
{
	return -EOPNOTSUPP;
}

int xmfs_urma_read_ubuf(struct xmfs_sb_info *sbi, void __user *buf,
			struct xmfs_read_extent *exts, int ext_cnt, size_t size)
{
	return -EOPNOTSUPP;
}

int xmfs_pmem_write(struct xmfs_sb_info *sbi, unsigned long offset,
		    const void *buf, unsigned long size)
{
	switch (sbi->mode) {
	case XMFS_SHARED_MEM_MODE:
	case XMFS_HYBRID_MODE:
		memcpy(sbi->kaddr + offset, buf, size);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}
EXPORT_SYMBOL(xmfs_pmem_write);

int xmfs_pmem_read(struct xmfs_sb_info *sbi, unsigned long offset, void *buf,
		   unsigned long size)
{
	switch (sbi->mode) {
	case XMFS_SHARED_MEM_MODE:
	case XMFS_HYBRID_MODE:
		memcpy(buf, sbi->kaddr + offset, size);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}
EXPORT_SYMBOL(xmfs_pmem_read);

int xmfs_pmem_cas(struct xmfs_sb_info *sbi, unsigned long offset,
		  uint64_t old_val, uint64_t new_val)
{
	switch (sbi->mode) {
	case XMFS_SHARED_MEM_MODE:
	case XMFS_HYBRID_MODE: {
		uint64_t *p = (uint64_t *)(sbi->kaddr + offset);

		return atomic64_cmpxchg((atomic64_t *)p, old_val, new_val) ==
				       old_val ?
			       0 :
			       -EAGAIN;
	}
	default:
		return -EOPNOTSUPP;
	}
}
EXPORT_SYMBOL(xmfs_pmem_cas);

#endif
