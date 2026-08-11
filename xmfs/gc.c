// SPDX-License-Identifier: GPL-2.0-only
#include "xmfs_i.h"
#include <linux/dcache.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/cacheflush.h>

void xmfs_mark_page_dead(struct xmfs_sb_info *sbi, unsigned long data_offset)
{
	unsigned long trunk_offset;

	if (data_offset == 0 || data_offset < DATA_AREA_OFFSET)
		return;

	trunk_offset = data_offset & ~(sbi->trunk_size - 1);
	if (trunk_offset < DATA_AREA_OFFSET)
		return;

	if (sbi->mode != XMFS_URMA_MODE) {
		struct xmfs_data_trunk_header *header;

		header = (struct xmfs_data_trunk_header *)(sbi->kaddr +
							   trunk_offset);
		if (header->magic != XMFS_DATA_TRUNK_MAGIC)
			return;
		if (header->live_page_count == 0)
			return;
		header->live_page_count--;
	} else {
		unsigned long magic_off = trunk_offset;
		unsigned long magic_val;
		unsigned long count_off =
			trunk_offset + offsetof(struct xmfs_data_trunk_header,
						live_page_count);
		uint64_t old_count, new_count;

		xmfs_pmem_read(sbi, magic_off, &magic_val, sizeof(magic_val));
		if (magic_val != XMFS_DATA_TRUNK_MAGIC)
			return;

		do {
			xmfs_pmem_read(sbi, count_off, &old_count,
				       sizeof(old_count));
			new_count = old_count - 1;
		} while (xmfs_pmem_cas(sbi, count_off, old_count, new_count) !=
			 0);
	}
}
EXPORT_SYMBOL(xmfs_mark_page_dead);

void xmfs_mark_page_live(struct xmfs_sb_info *sbi, unsigned long data_offset)
{
	unsigned long trunk_offset;

	if (data_offset == 0 || data_offset < DATA_AREA_OFFSET)
		return;

	trunk_offset = data_offset & ~(sbi->trunk_size - 1);
	if (trunk_offset < DATA_AREA_OFFSET)
		return;

	if (sbi->mode != XMFS_URMA_MODE) {
		struct xmfs_data_trunk_header *header;

		header = (void *)(sbi->kaddr + trunk_offset);
		if (header->magic != XMFS_DATA_TRUNK_MAGIC)
			return;
		header->live_page_count++;
		/* cmpxchg(&header->last_modify_version, (unsigned long)-1, 0); */
	} else {
		unsigned long magic_off = trunk_offset;
		unsigned long magic_val;
		/*
		 * unsigned long lmv_off = trunk_offset +
		 *	offsetof(struct xmfs_data_trunk_header,
		 *		 last_modify_version);
		 */
		unsigned long count_off =
			trunk_offset + offsetof(struct xmfs_data_trunk_header,
						live_page_count);
		uint64_t old_count, new_count;

		xmfs_pmem_read(sbi, magic_off, &magic_val, sizeof(magic_val));
		if (magic_val != XMFS_DATA_TRUNK_MAGIC)
			return;

		do {
			xmfs_pmem_read(sbi, count_off, &old_count,
				       sizeof(old_count));
			new_count = old_count + 1;
		} while (xmfs_pmem_cas(sbi, count_off, old_count, new_count) !=
			 0);

		/* xmfs_pmem_cas(sbi, lmv_off, (uint64_t)-1, 0); */
	}
}
EXPORT_SYMBOL(xmfs_mark_page_live);

int xmfs_quiesce_wait(struct xmfs_sb_info *sbi)
{
	int ret;

	ret = wait_event_interruptible(sbi->gc_ctl.wait,
				       !READ_ONCE(sbi->gc_ctl.gc_running));

	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL(xmfs_quiesce_wait);

long xmfs_find_free_slot(struct xmfs_sb_index *xmsi, struct xmfs_sb_info *sbi)
{
	unsigned long i;

	if (xmsi->count < sbi->max_chunk)
		return xmsi->count;

	for (i = 0; i < xmsi->count && i < XMFS_BITMAP_CAPACITY; i++) {
		if (xmsi->bitmap[i] == 'u')
			return i;
	}

	return -1;
}

long xmfs_alloc_and_mark_slot(struct xmfs_sb_index *xmsi,
			      struct xmfs_sb_info *sbi, char type)
{
	long slot = xmfs_find_free_slot(xmsi, sbi);

	if (slot < 0)
		return -1;
	if (slot < (long)xmsi->count)
		memset(sbi->kaddr + DATA_AREA_OFFSET + slot * sbi->trunk_size,
		       0, sbi->trunk_size);
	xmsi->bitmap[slot] = type;
	if (type == 'd') {
		struct xmfs_data_trunk_header *hdr;

		hdr = (void *)(sbi->kaddr + DATA_AREA_OFFSET +
			       slot * sbi->trunk_size);
		hdr->magic = XMFS_DATA_TRUNK_MAGIC;
		hdr->last_modify_version = 0;
		hdr->live_page_count = 0;
	}
	if (slot == (long)xmsi->count)
		xmsi->count = slot + 1;
	xmsi->used_trunk_count++;
	return slot;
}

static void write_log_entry_fields(struct xmfs_log_trunk *xmlt,
				   struct xmfs_log *xm_log, unsigned long ino,
				   unsigned long version, int slot,
				   size_t namelen)
{
	xmlt->log[slot].index = xm_log->index;
	xmlt->log[slot].end_index = xm_log->end_index;
	xmlt->log[slot].ops = xm_log->ops;
	xmlt->log[slot].data_offset = xm_log->data_offset;
	xmlt->log[slot].mode = xm_log->mode;
	xmlt->log[slot].size = xm_log->size;
	xmlt->log[slot].i_ino = ino;
	xmlt->log[slot].offset = xm_log->offset;
	xmlt->log[slot].version = version;
	xmlt->log[slot].writer_id = xm_log->writer_id;

	if (xm_log->ops != XMFS_UPDATE) {
		memcpy(xmlt->log[slot].name, xm_log->name, namelen);
		xmlt->log[slot].name[namelen] = '\0';
		xmlt->log[slot].p_ino = xm_log->p_ino;
	}
}

unsigned long xmfs_add_log_v3(struct xmfs_log *xm_log, struct xmfs_sb_info *sbi,
			      size_t namelen, unsigned long ino,
			      bool update_version)
{
	unsigned long version;
	struct xmfs_sb_index *xmsi;
	struct xmfs_log_trunk *xmlt;
	unsigned long addr;
	long slot;

	down_read(&sbi->chain_rwsem);

	spin_lock(&sbi->version_lock);
	sbi->last_update++;
	version = sbi->last_update;
	spin_unlock(&sbi->version_lock);

	spin_lock(&sbi->space_lock);

	xmsi = get_xmsi(sbi);
	if (!xmsi) {
		spin_unlock(&sbi->space_lock);
		up_read(&sbi->chain_rwsem);
		return (unsigned long)-1;
	}

	xm_log->writer_id = 0;

	if (xmsi->log_current == 0 || xmsi->count == 0) {
		slot = xmfs_alloc_and_mark_slot(xmsi, sbi, 'l');
		if (slot < 0) {
			put_xmsi(sbi, xmsi);
			spin_unlock(&sbi->space_lock);
			up_read(&sbi->chain_rwsem);
			return (unsigned long)-1;
		}
		addr = DATA_AREA_OFFSET + slot * sbi->trunk_size;
		xmlt = get_log_trunk(sbi, addr);
		xmlt->used = 0;
		xmlt->next_trunk = 0;
		xmlt->version_base = version;
		xmlt->version_max = 0;

		xmsi->log_start = addr;
		xmsi->log_current = addr;
	} else {
		xmlt = get_log_trunk(sbi, xmsi->log_current);
	}

	if (xmlt->used >= MAX_LOG_PER_TRUNK) {
		xmlt->version_max = version - 1;

		slot = xmfs_alloc_and_mark_slot(xmsi, sbi, 'l');
		if (slot < 0) {
			put_xmsi(sbi, xmsi);
			spin_unlock(&sbi->space_lock);
			up_read(&sbi->chain_rwsem);
			return (unsigned long)-1;
		}
		addr = DATA_AREA_OFFSET + slot * sbi->trunk_size;
		xmlt->next_trunk = addr;
		put_log_trunk(sbi, xmlt);
		xmlt = get_log_trunk(sbi, addr);
		xmlt->used = 0;
		xmlt->next_trunk = 0;
		xmlt->version_base = version;
		xmlt->version_max = 0;
		xmsi->log_current = addr;
	}

	write_log_entry_fields(xmlt, xm_log, ino, version, xmlt->used, namelen);
	xmlt->used++;

	addr = xmsi->log_current;

	if (xm_log->ops == XMFS_UPDATE) {
		unsigned long trunk_off = xm_log->data_offset &
					  ~(sbi->trunk_size - 1);
		struct xmfs_data_trunk_header *hdr =
			(void *)(sbi->kaddr + trunk_off);
		if (hdr->magic == XMFS_DATA_TRUNK_MAGIC &&
		    version > hdr->last_modify_version)
			hdr->last_modify_version = version;
	} else if (xm_log->ops == XMFS_XATTR &&
		   xm_log->data_offset >= DATA_AREA_OFFSET) {
		unsigned long trunk_off = xm_log->data_offset &
					  ~(sbi->trunk_size - 1);
		struct xmfs_data_trunk_header *hdr =
			(void *)(sbi->kaddr + trunk_off);
		if (hdr->magic == XMFS_DATA_TRUNK_MAGIC &&
		    version > hdr->last_modify_version)
			hdr->last_modify_version = version;
	} else if (xm_log->ops == XMFS_SYMLINK &&
		   xm_log->end_index >= DATA_AREA_OFFSET) {
		unsigned long trunk_off = xm_log->end_index &
					  ~(sbi->trunk_size - 1);
		struct xmfs_data_trunk_header *hdr =
			(void *)(sbi->kaddr + trunk_off);
		if (hdr->magic == XMFS_DATA_TRUNK_MAGIC &&
		    version > hdr->last_modify_version)
			hdr->last_modify_version = version;
	}

	if (update_version) {
		spin_lock(&sbi->version_lock);
		if (version > xmsi->last_update)
			xmsi->last_update = version;
		spin_unlock(&sbi->version_lock);
	}

	put_log_trunk(sbi, xmlt);
	put_xmsi(sbi, xmsi);
	spin_unlock(&sbi->space_lock);
	up_read(&sbi->chain_rwsem);

	return addr + offsetof(struct xmfs_log_trunk, log[0]) +
	       (xmlt->used - 1) * sizeof(struct xmfs_log);
}

static void gc_tail_compact(struct xmfs_sb_index *xmsi)
{
	while (xmsi->count > 0 && xmsi->count <= XMFS_BITMAP_CAPACITY &&
	       xmsi->bitmap[xmsi->count - 1] == 'u')
		xmsi->count--;
}

static unsigned long gc_reclaim_log_trunks(struct xmfs_sb_info *sbi,
					   unsigned long safe_version)
{
	struct xmfs_sb_index *xmsi;
	struct xmfs_log_trunk *xmlt;
	unsigned long trunk_offset;
	unsigned long next_trunk_offset;
	unsigned long max_ver_reclaimed = 0;
	unsigned long reclaimed = 0;
	unsigned long first_surviving_trunk = 0;

	xmsi = get_xmsi(sbi);
	if (!xmsi)
		return 0;
	if (safe_version == 0) {
		put_xmsi(sbi, xmsi);
		return 0;
	}

	trunk_offset = xmsi->log_start;

	while (trunk_offset != 0) {
		xmlt = get_log_trunk(sbi, trunk_offset);
		if (!xmlt)
			break;

		if (xmlt->version_max == 0) {
			if (first_surviving_trunk == 0)
				first_surviving_trunk = trunk_offset;
			put_log_trunk(sbi, xmlt);
			break;
		}

		if (xmlt->version_max > safe_version) {
			if (first_surviving_trunk == 0)
				first_surviving_trunk = trunk_offset;
			put_log_trunk(sbi, xmlt);
			break;
		}

		if (xmlt->version_max > max_ver_reclaimed)
			max_ver_reclaimed = xmlt->version_max;

		next_trunk_offset = xmlt->next_trunk;
		put_log_trunk(sbi, xmlt);

		spin_lock(&sbi->space_lock);
		{
			unsigned long idx = (trunk_offset - DATA_AREA_OFFSET) /
					    sbi->trunk_size;
			memset(sbi->kaddr + trunk_offset, 0, sbi->trunk_size);
			if (idx < XMFS_BITMAP_CAPACITY)
				xmsi->bitmap[idx] = 'u';
			if (xmsi->used_trunk_count > 0)
				xmsi->used_trunk_count--;
			sbi->full = false;
		}
		spin_unlock(&sbi->space_lock);
		reclaimed++;

		trunk_offset = next_trunk_offset;
	}

	if (first_surviving_trunk != 0)
		xmsi->log_start = first_surviving_trunk;
	xmfs_info(sbi->sb, "gc log ends, gc to %ld, release %ld",
		  first_surviving_trunk, reclaimed);
	spin_lock(&sbi->space_lock);
	while (xmsi->count > 0 && xmsi->count <= XMFS_BITMAP_CAPACITY &&
	       xmsi->bitmap[xmsi->count - 1] == 'u')
		xmsi->count--;
	spin_unlock(&sbi->space_lock);

	put_xmsi(sbi, xmsi);
	return max_ver_reclaimed;
}

static void gc_scan_data_trunks(struct xmfs_sb_info *sbi,
				unsigned long safe_version)
{
	struct xmfs_sb_index *xmsi;
	struct xmfs_data_trunk_header *header;
	unsigned long trunk_offset;
	unsigned long trunk_index;
	unsigned long batch_count = 0;

	xmsi = get_xmsi(sbi);
	if (!xmsi)
		return;

	for (trunk_index = sbi->gc_ctl.gc_cursor;
	     trunk_index <
	     sbi->max_chunk /*&& batch_count < sbi->gc_ctl.gc_batch*/;
	     trunk_index++) {
		if (trunk_index < XMFS_BITMAP_CAPACITY &&
		    xmsi->bitmap[trunk_index] != 'd')
			continue;

		trunk_offset = DATA_AREA_OFFSET + trunk_index * sbi->trunk_size;
		header = (struct xmfs_data_trunk_header *)(sbi->kaddr +
							   trunk_offset);
		if (header->magic != XMFS_DATA_TRUNK_MAGIC)
			continue;

		if (header->live_page_count > 0)
			continue;

		if (header->last_modify_version > safe_version)
			continue;

		xmfs_info(sbi->sb,
			  "gc: reclaiming data trunk[%lu] live_pages=%lu ver=%lu",
			  trunk_index, header->live_page_count,
			  header->last_modify_version);
		spin_lock(&sbi->space_lock);
		memset(sbi->kaddr + trunk_offset, 0, sbi->trunk_size);
		if (trunk_index < XMFS_BITMAP_CAPACITY)
			xmsi->bitmap[trunk_index] = 'u';
		if (xmsi->used_trunk_count > 0)
			xmsi->used_trunk_count--;
		sbi->full = false;
		batch_count++;
		spin_unlock(&sbi->space_lock);
	}

	spin_lock(&sbi->space_lock);
	sbi->gc_ctl.gc_cursor = trunk_index;
	if (sbi->gc_ctl.gc_cursor >= sbi->max_chunk)
		sbi->gc_ctl.gc_cursor = 0;
	gc_tail_compact(xmsi);
	spin_unlock(&sbi->space_lock);

	put_xmsi(sbi, xmsi);
}

static unsigned long gc_compute_safe_version(struct xmfs_sb_info *sbi)
{
	struct mount_msg_info *mm_info;
	unsigned long safe_version;
	int i;

	safe_version = sbi->last_update;

	if (sbi->mode == XMFS_SHARED_MEM_MODE ||
	    sbi->mode == XMFS_HYBRID_MODE) {
		mm_info = sbi->kaddr + MOUNT_MSG_OFFSET;
	} else {
		mm_info = get_mnt_msg(sbi);
		if (!mm_info)
			return safe_version;
	}

	for (i = 1; i <= sbi->max_slave_id && i < 64; i++) {
		if (mm_info->msg[i] == 'D') {
			unsigned long sv = mm_info->slave_versions[i];

			if (sv < safe_version)
				safe_version = sv;
		}
	}

	if (sbi->mode == XMFS_URMA_MODE)
		put_mnt_msg(sbi, mm_info);

	xmfs_info(sbi->sb,
		  "gc: safe_version=%lu (last_update=%lu, max_slave=%d)",
		  safe_version, sbi->last_update, sbi->max_slave_id);
	return safe_version;
}

static void xmfs_gc_run(struct xmfs_sb_info *sbi)
{
	unsigned long safe_version;
	struct mount_msg_info *mm_info;
#ifdef CONFIG_XMFS_FS_URMA
	struct xmfs_rw_msg req;
#endif
	bool all_caught_up;
	int i;

	if (sbi->id != 0)
		return;

	xmfs_replay_new_entries(sbi, NULL, -1);

	spin_lock(&sbi->gc_ctl.gc_lock);
	if (sbi->gc_ctl.gc_running) {
		spin_unlock(&sbi->gc_ctl.gc_lock);
		return;
	}
	WRITE_ONCE(sbi->gc_ctl.gc_running, true);
	spin_unlock(&sbi->gc_ctl.gc_lock);

	safe_version = gc_compute_safe_version(sbi);

	mm_info = get_mnt_msg(sbi);

	if (mm_info) {
#ifdef CONFIG_XMFS_FS_URMA
		for (i = 1; i < 16; i++) {
			if (sbi->urma_cfg.send_tjettys[i] == NULL)
				continue;
			req.ops = XMFS_GC;
			req.status = 2;
			xmfs_send_request(sbi, &req, NULL, i);
		}
#endif
		all_caught_up = true;
		for (i = 1; i <= sbi->max_slave_id && i < 64; i++) {
			if (mm_info->msg[i] == 'D' &&
			    mm_info->slave_versions[i] < safe_version) {
				all_caught_up = false;
				break;
			}
		}

		if (!all_caught_up) {
			xmfs_info(sbi->sb,
				  "gc: waiting for slaves to catch up to version %lu",
				  safe_version);
			schedule_timeout_interruptible(10 * HZ);
			mm_info = get_mnt_msg(sbi);
			if (mm_info) {
				all_caught_up = true;
				for (i = 1; i <= sbi->max_slave_id && i < 64;
				     i++) {
					if (mm_info->msg[i] == 'D' &&
					    mm_info->slave_versions[i] <
						    safe_version) {
						all_caught_up = false;
						break;
					}
				}
				put_mnt_msg(sbi, mm_info);
			}
		}
	}

	down_write(&sbi->chain_rwsem);
	if (all_caught_up) {
#ifdef CONFIG_XMFS_FS_URMA
		for (i = 1; i < 16; i++) {
			if (sbi->urma_cfg.send_tjettys[i] == NULL)
				continue;
			req.ops = XMFS_GC;
			req.status = 1;
			xmfs_send_request(sbi, &req, NULL, i);
		}
#endif
		gc_reclaim_log_trunks(sbi, safe_version);
#ifdef CONFIG_XMFS_FS_URMA
		for (i = 1; i < 16; i++) {
			if (sbi->urma_cfg.send_tjettys[i] == NULL)
				continue;
			req.ops = XMFS_GC;
			req.status = 0;
			xmfs_send_request(sbi, &req, NULL, i);
		}
#endif
	}
	gc_scan_data_trunks(sbi, safe_version);
	up_write(&sbi->chain_rwsem);

	spin_lock(&sbi->gc_ctl.gc_lock);
	WRITE_ONCE(sbi->gc_ctl.gc_running, false);
	wake_up_all(&sbi->gc_ctl.wait);
	spin_unlock(&sbi->gc_ctl.gc_lock);
}

int xmfs_gc_main(struct xmfs_sb_info *sbi)
{
	xmfs_gc_run(sbi);
	return 0;
}

int xmfs_gc_thread_fn(void *data)
{
	struct xmfs_sb_info *sbi = data;
	struct xmfs_sb_index *xmsi;
	unsigned long interval;

	interval = sbi->gc_interval ? sbi->gc_interval :
				      XMFS_GC_INTERVAL_DEFAULT;

	while (!kthread_should_stop()) {
		xmsi = get_xmsi(sbi);
		/*
		 * xmfs_info(sbi->sb, "gc %d %d %d",
		 *	     xmsi->used_trunk_count,
		 *	     sbi->max_chunk * 80 / 100, sbi->max_chunk);
		 */
		if (xmsi &&
		    (xmsi->used_trunk_count >= sbi->max_chunk * 80 / 100 ||
		     sbi->full))
			xmfs_gc_run(sbi);
		if (xmsi)
			put_xmsi(sbi, xmsi);
		schedule_timeout_interruptible(interval * HZ);
	}

	return 0;
}

int xmfs_gc_thread_slave_fn(void *data)
{
	struct xmfs_sb_info *sbi = data;
	unsigned long interval;

	interval = sbi->gc_interval ? sbi->gc_interval :
				      XMFS_GC_INTERVAL_DEFAULT;

	while (!kthread_should_stop()) {
		if (sbi->gc_ctl.need_replay) {
			xmfs_info(sbi->sb, "gc need to replay");
			xmfs_replay_new_entries(sbi, NULL, -1);
			xmfs_info(sbi->sb, "gc need to replay done");
			sbi->gc_ctl.need_replay = false;
		}
		schedule_timeout_interruptible(interval * HZ);
	}

	return 0;
}

static int xmfs_host_reserve_data_space(struct xmfs_sb_info *sbi,
					unsigned long page_count,
					struct xmfs_data_reserve_result *result)
{
	struct xmfs_sb_index *xmsi = get_xmsi(sbi);
	unsigned long data_current, data_count;
	long slot;
	struct xmfs_data_trunk_header *hdr;
	unsigned long remaining;
	int trunk_idx;
	bool first_partial = false;

	if (!xmsi)
		return -ENOSPC;

	memset(result, 0, sizeof(*result));
	trunk_idx = 0;

	data_current = xmsi->data_current;
	data_count = xmsi->data_count;
	remaining = 511 - data_count;
	if (remaining + 511 * (sbi->max_chunk - xmsi->count) < page_count) {
		xmfs_info(sbi->sb,
			  "try to allocate more than usable space %lu > %lu",
			  page_count,
			  remaining + 511 * (sbi->max_chunk - xmsi->count));
		return -ENOSPC;
	}

	if (data_current != 0 && remaining > 0) {
		unsigned long use = min(page_count, remaining);

		hdr = (struct xmfs_data_trunk_header *)(sbi->kaddr +
							(data_current &
							 ~(sbi->trunk_size -
							   1)));
		result->data_addr = data_current;
		result->data_page_count = use;
		result->trunk_addrs[trunk_idx] = data_current &
						 ~(sbi->trunk_size - 1);
		trunk_idx++;

		xmsi->data_current = data_current + use * PAGE_SIZE;
		xmsi->data_count = data_count + use;
		hdr->total_page_count = data_count + use;
		/* hdr->live_page_count += use; */

		page_count -= use;
		if (page_count == 0) {
			put_xmsi(sbi, xmsi);
			result->trunk_count = trunk_idx;
			return 0;
		}
		first_partial = true;
	}

	while (page_count > 0) {
		unsigned long use;

		slot = xmfs_alloc_and_mark_slot(xmsi, sbi, 'd');
		if (slot < 0) {
			put_xmsi(sbi, xmsi);
			return -ENOSPC;
		}
		data_current = DATA_AREA_OFFSET + slot * sbi->trunk_size;
		hdr = (struct xmfs_data_trunk_header *)(sbi->kaddr +
							data_current);
		memset(hdr, 0, PAGE_SIZE);
		hdr->magic = XMFS_DATA_TRUNK_MAGIC;
		hdr->last_modify_version = 0;

		use = min_t(unsigned long, page_count, 511);

		if (!first_partial && trunk_idx == 0) {
			result->data_addr = data_current + PAGE_SIZE;
			result->data_page_count = use;
		} else if (result->second_data_addr == 0) {
			result->second_data_addr = data_current + PAGE_SIZE;
			result->second_data_pages = use;
		} else {
			result->second_data_pages += use;
		}
		result->trunk_addrs[trunk_idx] = data_current;
		trunk_idx++;

		hdr->total_page_count = use;
		/* hdr->live_page_count = use; */

		if (use < 511) {
			xmsi->data_current =
				data_current + PAGE_SIZE + use * PAGE_SIZE;
			xmsi->data_count = use;
		} else {
			xmsi->data_current = 0;
			xmsi->data_count = 511;
		}

		page_count -= use;
	}

	result->trunk_count = trunk_idx;
	put_xmsi(sbi, xmsi);
	return 0;
}

static unsigned long xmfs_host_reserve_log_space(struct xmfs_sb_info *sbi,
						 unsigned long ino,
						 int entry_count)
{
	struct xmfs_sb_index *xmsi = get_xmsi(sbi);
	struct xmfs_log_trunk *xmlt;
	unsigned long addr;
	long slot;

	if (!xmsi)
		return 0;

	xmlt = get_log_trunk(sbi, xmsi->log_current);
	if (!xmlt || xmlt->used + entry_count > MAX_LOG_PER_TRUNK) {
		if (xmlt) {
			xmlt->version_max = sbi->last_update;
			put_log_trunk(sbi, xmlt);
		}
		slot = xmfs_alloc_and_mark_slot(xmsi, sbi, 'l');
		if (slot < 0 || slot >= sbi->max_chunk) {
			xmfs_info(sbi->sb,
				  "cannot get a correct slot %ld, maybe space is full",
				  slot);
			put_xmsi(sbi, xmsi);
			return 0;
		}
		addr = DATA_AREA_OFFSET + slot * sbi->trunk_size;
		xmlt = get_log_trunk(sbi, addr);
		xmlt->used = 0;
		xmlt->next_trunk = 0;
		xmlt->version_base = sbi->last_update + 1;
		xmlt->version_max = 0;
		put_log_trunk(sbi, xmlt);
		{
			struct xmfs_log_trunk *old =
				get_log_trunk(sbi, xmsi->log_current);
			if (old) {
				old->next_trunk = addr;
				put_log_trunk(sbi, old);
			}
		}
		xmsi->log_current = addr;
		sbi->log_current =
			addr + offsetof(struct xmfs_log_trunk, log[0]);
		xmlt = get_log_trunk(sbi, addr);
	}

	addr = xmsi->log_current;
	put_log_trunk(sbi, xmlt);
	put_xmsi(sbi, xmsi);
	return addr;
}

void xmfs_host_handle_urma_request(struct xmfs_sb_info *sbi, int slave_i,
				   struct xmfs_rw_msg *req,
				   struct xmfs_rw_msg *grant)
{
	struct xmfs_log_trunk *xmlt;
	struct xmfs_data_reserve_result dr;
	unsigned long log_addr;
	int log_needed;
	int data_err;

	if (xmfs_quiesce_wait(sbi))
		return;

	if (req->ops == XMFS_SETATTR) {
		spin_lock(&sbi->space_lock);
		struct xmfs_inode_table *tbl =
			(void *)(sbi->kaddr + INODE_TABLE_OFFSET);
		unsigned long ci;
		struct iattr *attr = (struct iattr *)req->name;
		time64_t ts_sec = ktime_get_real_seconds();

		for (ci = 0; ci < tbl->count; ci++) {
			if (tbl->entries[ci].i_ino == req->i_ino) {
				if (attr->ia_valid & ATTR_SIZE)
					tbl->entries[ci].size = attr->ia_size;
				tbl->entries[ci].ctime.tv_sec = ts_sec;
				tbl->entries[ci].mtime.tv_sec = ts_sec;
				tbl->entries[ci].atime.tv_sec = ts_sec;
				break;
			}
		}
		spin_unlock(&sbi->space_lock);
		return;
	}

	grant->type = MSG_RW_GRANT;
	grant->request_id = req->request_id;
	grant->slave_id = req->slave_id;
	grant->ops = req->ops;
	spin_lock(&sbi->space_lock);
	if (req->data_pages > 0) {
		data_err =
			xmfs_host_reserve_data_space(sbi, req->data_pages, &dr);
		if (data_err) {
			spin_unlock(&sbi->space_lock);
			grant->type = MSG_RW_ERROR;
			return;
		}
		grant->data_addr = dr.data_addr;
		grant->data_page_count = dr.data_page_count;
		grant->second_data_addr = dr.second_data_addr;
		grant->second_data_pages = dr.second_data_pages;
	} else {
		data_err = 0;
		grant->data_addr = 0;
		grant->data_page_count = 0;
	}

	log_needed = req->log_entries;
	if (req->ops == XMFS_UPDATE && grant->second_data_addr != 0)
		log_needed = max(log_needed, 2);
	if (req->ops == XMFS_RENAME)
		log_needed = max(log_needed, 2);
	if (req->ops == XMFS_SYMLINK)
		log_needed = max(log_needed, 2);

	if (log_needed > 0 && slave_i != 0) {
		log_addr = xmfs_host_reserve_log_space(sbi, req->i_ino,
						       log_needed);
		if (log_addr == 0) {
			spin_unlock(&sbi->space_lock);
			grant->type = MSG_RW_ERROR;
			return;
		}

		xmlt = get_log_trunk(sbi, log_addr);
		if (xmlt) {
			grant->log_trunk_addr = log_addr;
			grant->log_slot_start = xmlt->used;
			grant->log_slot_count = log_needed;
			xmlt->used += log_needed;
			put_log_trunk(sbi, xmlt);
		}
	} else {
		grant->log_trunk_addr = 0;
		grant->log_slot_start = 0;
		grant->log_slot_count = log_needed;
	}

	if (req->i_ino == 0)
		grant->assigned_ino = get_next_ino();
	else
		grant->assigned_ino = req->i_ino;

	{
		struct xmfs_sb_index *xmsi = get_xmsi(sbi);

		if (xmsi) {
			grant->version = xmsi->last_update + 1;
			xmsi->last_update += max(log_needed, 1);
			put_xmsi(sbi, xmsi);
		} else {
			grant->version = 1;
		}
	}

	if (req->ops == XMFS_CREATE || req->ops == XMFS_MKDIR ||
	    req->ops == XMFS_SYMLINK || req->ops == XMFS_LINK) {
		struct xmfs_inode_table *tbl =
			(void *)(sbi->kaddr + INODE_TABLE_OFFSET);
		struct timespec64 ts;
		unsigned int nlink;
		{
			struct xmfs_inode_entry *e = &tbl->entries[tbl->count];

			memset(e, 0, sizeof(*e));
			e->i_ino = grant->assigned_ino;
			e->p_ino = req->p_ino;
			e->mode = req->mode;
			if (req->ops != XMFS_LINK)
				e->nlink = 1;
			memcpy(e->name, req->name, strnlen(req->name, 255));
			e->name[strnlen(req->name, 255)] = '\0';
			e->size = req->size;
			ktime_get_real_ts64(&ts);
			e->ctime = ts;
			e->mtime = ts;
			e->atime = ts;
		}
		tbl->count++;
		if (req->ops == XMFS_LINK) {
			nlink = xmfs_inode_table_sync_nlink(tbl, req->i_ino);
			if (nlink == 1)
				xmfs_warning(sbi->sb,
					     "LINK inode %lu not found in table",
					     req->i_ino);
		}
	}

	if (req->ops == XMFS_DELETE) {
		struct xmfs_inode_table *tbl =
			(void *)(sbi->kaddr + INODE_TABLE_OFFSET);

		xmfs_inode_table_remove(tbl, req->i_ino, req->p_ino,
					req->name);
		xmfs_inode_table_sync_nlink(tbl, req->i_ino);
	}

	if (req->ops == XMFS_UPDATE) {
		struct xmfs_inode_table *ctbl =
			(void *)(sbi->kaddr + INODE_TABLE_OFFSET);
		unsigned long ci;
		/* xmfs_info(sbi->sb, "write update size %ld", req->size); */
		for (ci = 0; ci < ctbl->count; ci++) {
			if (ctbl->entries[ci].i_ino == req->i_ino) {
				/*
				 * xmfs_info(sbi->sb,
				 *	     "write find inode to update size %ld",
				 *	     req->size);
				 */
				ctbl->entries[ci].size = req->size;
				break;
			}
		}
	}

	spin_unlock(&sbi->space_lock);
}
