// SPDX-License-Identifier: GPL-2.0-only
/*
 * XMFS: Log Structured Filesystem for UB
 * Copyright (C) 2024. Huawei Technologies Co., Ltd
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

#include "xmfs_i.h"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/swap.h>
#include <linux/uio.h>
#include <linux/file.h>
#include <linux/seq_file.h>
#include <linux/pagemap.h>
#include <linux/fadvise.h>
#include <linux/splice.h>
#include <linux/falloc.h>

#ifdef CONFIG_XMFS_FS_URMA
static ssize_t xmfs_file_write_iter_slave(struct kiocb *iocb,
					  struct iov_iter *from);
static ssize_t xmfs_file_write_ubuf_slave(struct kiocb *iocb,
					  struct iov_iter *from);
#else
static ssize_t xmfs_file_write_iter_slave(struct kiocb *iocb,
					  struct iov_iter *from)
{
	return -EOPNOTSUPP;
}
#endif
static ssize_t xmfs_file_read_ubuf_slave(struct kiocb *iocb,
					 struct iov_iter *to);
static int xmfs_read_page(struct file *file, struct page *page)
{
	struct inode *inode = page->mapping->host;
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
	struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);
	unsigned long mapped_offset =
		(unsigned long)xa_load(&xmfs_inode->shared_pages, page->index);
	void *kaddr = kmap_local_page(page);

	if (mapped_offset) {
		memcpy(kaddr, get_data(sbi, mapped_offset, PAGE_SIZE),
		       PAGE_SIZE);
	}
	kunmap_local(kaddr);

	return 0;
}

#ifdef CONFIG_XMFS_FS_URMA
static void xmfs_write_worker(struct work_struct *work)
{
	struct xmfs_write_work *ww =
		container_of(work, struct xmfs_write_work, work);
	struct inode *inode = ww->inode;
	struct address_space *mapping = inode->i_mapping;
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
	struct page *page;
	struct folio *folio;
	struct xmfs_rw_msg grant = {};
	struct xmfs_rw_msg req = {};
	unsigned long end_index = ww->end_index;
	unsigned long start_index = ww->index;
	unsigned long page_num = end_index - start_index;
	struct ubcore_jfs_wr *wrs;
	int *slots;
	void *write_buffer;
	int wr_cnt = 0;
	unsigned long mapped_offset;
	struct xmfs_urma_msg msg = {};
	char *addr;
	struct ubcore_jfs_wr *wr;
	int err;
	unsigned long log_base;
	/*
	 * xmfs_info(sbi->sb, "qyf write work %d %d %d", start_index,
	 *	     end_index, ww->size);
	 */

	req.ops = XMFS_UPDATE;
	req.i_ino = inode->i_ino;
	req.data_pages = page_num;
	req.log_entries = 1;
	req.size = ww->size;
	req.type = MSG_RW_REQUEST;
	req.slave_id = sbi->id;
	spin_lock(&sbi->local_ring_lock);
	req.request_id = ++sbi->rw_request_id;
	spin_unlock(&sbi->local_ring_lock);

	struct xmfs_grant_wait gw;

	init_completion(&gw.done);
	xa_store(&sbi->urma_cfg.grant_waits, req.request_id, &gw, GFP_KERNEL);
	if (xmfs_quiesce_wait(sbi))
		return;
	err = xmfs_send_request(sbi, &req, &gw, 0);
	if (err) {
		iput(inode);
		return;
	}
	grant = gw.grant;
	log_base =
		grant.log_trunk_addr + offsetof(struct xmfs_log_trunk, log[0]);
	/*
	 * xmfs_info(sbi->sb, "grant %d %d %d %d %d",
	 *	     grant.log_slot_count, grant.data_addr,
	 *	     grant.data_page_count, grant.second_data_addr,
	 *	     grant.log_slot_start);
	 */
	wrs = vzalloc(page_num * sizeof(struct ubcore_jfs_wr));
	if (!wrs) {
		iput(inode);
		return;
	}
	slots = vzalloc(sizeof(int) * page_num);
	if (!slots) {
		vfree(wrs);
		iput(inode);
		return;
	}

	write_buffer =
		urma_pool_acquire_nslots(&sbi->urma_cfg.pool, page_num, slots);
	if (!write_buffer) {
		vfree(wrs);
		vfree(slots);
		iput(inode);
		return;
	}

	unsigned long start_write = grant.data_addr;
	unsigned long count = grant.data_page_count;
	unsigned long second_write = grant.second_data_addr;

	for (int i = start_index; i < end_index; i++) {
		if (count > 0) {
			mapped_offset = start_write;
			count--;
			start_write += PAGE_SIZE;
			if (start_write % XMFS_DEFAULT_CHUNK_SIZE == 0)
				start_write += PAGE_SIZE;
		} else if (second_write != 0) {
			mapped_offset = second_write;
			second_write += PAGE_SIZE;
			if (second_write % XMFS_DEFAULT_CHUNK_SIZE == 0)
				second_write += PAGE_SIZE;
		}

		folio = filemap_get_folio(mapping, i);
		if (IS_ERR(folio)) {
			xmfs_warning(sbi->sb, "write work cannot get folio %d",
				     i);
			continue;
		}
		xa_store(&(XMFS_I(inode)->shared_pages), i,
			 (void *)mapped_offset, GFP_KERNEL);
		page = folio_page(folio, 0);
		addr = kmap_local_page(page);
		msg.ops = XMFS_URMA_WRITE;
		msg.offset = mapped_offset;
		msg.len = PAGE_SIZE;
		msg.write_buffer = write_buffer + wr_cnt * PAGE_SIZE;
		msg.local_addr = addr;
		wr = &wrs[wr_cnt];
		err = xmfs_urma_fill_write_page(sbi, wr, &msg, slots[wr_cnt]);
		if (err) {
			xmfs_info(sbi->sb,
				  "write work fill write page failed %d folio",
				  i);
			kunmap_local(addr);
			folio_put(folio);
			continue;
		}
		if (wr_cnt > 0)
			wrs[wr_cnt - 1].next = wr;
		wr_cnt++;
		kunmap_local(addr);
		folio_put(folio);
	}

	if (wr_cnt > 0) {
		wrs[wr_cnt - 1].next = NULL;
		xmfs_urma_send_write_pages(sbi, wrs);
	}

	urma_pool_release_nslots(&sbi->urma_cfg.pool, page_num, slots);
	vfree(slots);

	for (int s = 0; s < grant.log_slot_count; s++) {
		struct xmfs_log xm_log = {};

		xm_log.ops = XMFS_UPDATE;
		xm_log.i_ino = inode->i_ino;
		xm_log.version = grant.version + s;
		xm_log.writer_id = sbi->id;
		if (s == 0) {
			xm_log.index = start_index;
			xm_log.end_index = start_index + grant.data_page_count;
			xm_log.data_offset = grant.data_addr;
			xm_log.size = grant.size;
		} else {
			xm_log.index = start_index + grant.data_page_count;
			xm_log.end_index = end_index;
			xm_log.data_offset = grant.second_data_addr;
			xm_log.size = grant.size;
		}

		xmfs_data_write(sbi,
				log_base + (grant.log_slot_start + s) *
						   sizeof(struct xmfs_log),
				&xm_log, sizeof(xm_log));
		/*
		 * xmfs_info(sbi->sb, "write work log %ld %ld %ld",
		 *	     log_base + (grant.log_slot_start + s) *
		 *			sizeof(struct xmfs_log),
		 *	     xm_log.index, xm_log.end_index);
		 */
	}
	iput(inode);
	kfree(ww);
}
#endif

void xmfs_prefetch_worker(struct work_struct *work)
{
	struct xmfs_cache_work *cw =
		container_of(work, struct xmfs_cache_work, work);
	struct inode *inode = cw->inode;
	struct address_space *mapping = inode->i_mapping;
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
	struct folio *folio;
	unsigned long *offsets;
	struct page **pages;
	unsigned long mapped_offset;
	int i;
	int count = 0;

	offsets =
		kzalloc((cw->end_index - cw->index + 1) * sizeof(unsigned long),
			GFP_KERNEL);
	if (!offsets)
		return;

	pages = kzalloc((cw->end_index - cw->index + 1) * sizeof(struct page *),
			GFP_KERNEL);
	if (!pages) {
		kfree(offsets);
		return;
	}

	for (i = cw->index; i < cw->end_index; i++) {
		mapped_offset =
			(unsigned long)xa_load(&XMFS_I(inode)->shared_pages, i);
		if (!mapped_offset)
			continue;

		folio = filemap_grab_folio(mapping, i);

		if (IS_ERR(folio))
			break;

		offsets[count] = mapped_offset;
		pages[count] = &folio->page;
		count++;
	}

	if (count > 0) {
		/*
		 * xmfs_info(sbi->sb, "qyf backgrount cp page %ld to %ld",
		 *	     cw->index, cw->end_index);
		 */
		xmfs_pmem_read_multi_pages(sbi, offsets, pages, count);
		for (i = 0; i < count; i++) {
			folio = page_folio(pages[i]);

			folio_mark_uptodate(folio);
			folio_mark_dirty(folio);
			folio_unlock(folio);
			folio_put(folio);
		}
	}
	kfree(offsets);
	kfree(pages);

	kfree(cw);
}

static int xmfs_write_begin(struct file *filp, struct address_space *mapping,
			    loff_t pos, unsigned int len, struct page **pagep,
			    void **fsdata)
{
	struct inode *inode = mapping->host;
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
	int err = simple_write_begin(filp, mapping, pos, len, pagep, fsdata);

	if (err || sbi->id != 0 || sbi->during_replay || PageUptodate(*pagep) ||
	    len == PAGE_SIZE)
		return err;
	return xmfs_read_page(filp, *pagep);
}

unsigned long xmfs_get_data_offset_nolock(struct inode *inode,
					  struct xmfs_sb_info *sbi,
					  struct page *page)
{
	char *kaddr;

	if (XMFS_I(inode)->count > 0) {
		unsigned long ret_addr;

		if (sbi->id == 0) {
			kaddr = kmap_local_page(page);
			xmfs_data_write(sbi, XMFS_I(inode)->start_write, kaddr,
					PAGE_SIZE);
			kunmap_local(kaddr);
		}
		ret_addr = XMFS_I(inode)->start_write;
		XMFS_I(inode)->count--;
		XMFS_I(inode)->start_write += PAGE_SIZE;
		if (XMFS_I(inode)->start_write % XMFS_DEFAULT_CHUNK_SIZE == 0)
			XMFS_I(inode)->start_write += PAGE_SIZE;
		return ret_addr;
	} else if (XMFS_I(inode)->second_write != 0) {
		unsigned long ret_addr;

		if (sbi->id == 0) {
			kaddr = kmap_local_page(page);
			xmfs_data_write(sbi, XMFS_I(inode)->second_write, kaddr,
					PAGE_SIZE);
			kunmap_local(kaddr);
		}
		ret_addr = XMFS_I(inode)->second_write;
		XMFS_I(inode)->second_write += PAGE_SIZE;
		if (XMFS_I(inode)->second_write % XMFS_DEFAULT_CHUNK_SIZE == 0)
			XMFS_I(inode)->second_write += PAGE_SIZE;
		return ret_addr;
	}
	/*
	 * xmfs_error(inode->i_sb,
	 *	      "no reserved space for inode %ld", inode->i_ino);
	 */
	return 0;
}

unsigned long xmfs_get_data_offset_fallocate(struct inode *inode,
					     struct xmfs_sb_info *sbi,
					     struct page *page)
{
	unsigned long index = page->index;
	unsigned long data_start = XMFS_I(inode)->data_start;
	unsigned long len = XMFS_I(inode)->data_size;

	if (index * PAGE_SIZE >= len) {
		xmfs_error(sbi->sb,
			   "Not support write more than fallocate size now");
		return -1;
	}

	char *kaddr = kmap_local_page(page);

	xmfs_data_write(sbi, data_start + index * PAGE_SIZE, kaddr, PAGE_SIZE);
	kunmap_local(kaddr);

	return data_start + index * PAGE_SIZE;
}

static int xmfs_write_end(struct file *file, struct address_space *mapping,
			  loff_t pos, unsigned int len, unsigned int copied,
			  struct page *page, void *fsdata)
{
	struct folio *folio = page_folio(page);
	struct inode *inode = folio->mapping->host;
	loff_t last_pos = pos + copied;
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);

	if (!folio_test_uptodate(folio)) {
		/* zero the stale part of the folio if we did a short copy */
		if (copied < len) {
			size_t from = offset_in_folio(folio, pos);

			folio_zero_range(folio, from + copied, len - copied);
		}
		folio_mark_uptodate(folio);
	}
	folio_mark_dirty(folio);

	if (i_size_read(inode) < last_pos)
		i_size_write(inode, last_pos);

	unsigned long offset;

	if (XMFS_I(inode)->fallocate)
		offset = xmfs_get_data_offset_fallocate(inode, sbi, page);
	else
		offset = xmfs_get_data_offset_nolock(inode, sbi, page);
	if (offset == 0) {
		folio_unlock(folio);
		folio_put(folio);
		return copied;
	} else if (offset < 0) {
		folio_unlock(folio);
		folio_put(folio);
		return offset;
	}
	/*
	 * xmfs_info(sbi->sb, "write end index %d offset %ld",
	 *	     page->index, offset);
	 */
	/*
	 * if (sbi->id != 0) {
	 *	folio_mark_dirty(folio);
	 *	struct xmfs_urma_msg msg = {};
	 *	char *addr = kmap_local_page(page);
	 *	struct ubcore_jfs_wr *wr;
	 *
	 *	msg.ops = XMFS_URMA_WRITE;
	 *	msg.offset = offset;
	 *	msg.len = PAGE_SIZE;
	 *	msg.write_buffer = XMFS_I(inode)->write_buffer +
	 *		XMFS_I(inode)->wr_cnt * PAGE_SIZE;
	 *	msg.local_addr = addr;
	 *	wr = &XMFS_I(inode)->wr[XMFS_I(inode)->wr_cnt];
	 *	xmfs_urma_fill_write_page(
	 *		sbi, wr, &msg,
	 *		XMFS_I(inode)->slots[XMFS_I(inode)->wr_cnt]);
	 *	if (XMFS_I(inode)->wr_cnt > 0)
	 *		XMFS_I(inode)->wr[XMFS_I(inode)->wr_cnt - 1].next = wr;
	 *	XMFS_I(inode)->wr_cnt++;
	 *	kunmap_local(addr);
	 * }
	 */

	void *old_ret = xa_store(&(XMFS_I(inode)->shared_pages), folio->index,
				 (void *)offset, GFP_KERNEL);
	if (xa_err(old_ret)) {
		xmfs_error(sbi->sb, "cannot store shared pages for page %ld",
			   folio->index);
	}
	if (sbi->id == 0) {
		xmfs_mark_page_dead(sbi, (unsigned long)old_ret);
		xmfs_mark_page_live(sbi, offset);
	}

	folio_unlock(folio);
	folio_put(folio);

	return copied;
}

static ssize_t xmfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	ssize_t ret;
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);

	if (sbi->id != 0) {
#ifdef CONFIG_XMFS_FS_URMA
		if (iter_is_ubuf(from))
			return xmfs_file_write_ubuf_slave(iocb, from);
#endif
		return xmfs_file_write_iter_slave(iocb, from);
	}
	inode_lock(inode);

	if (xmfs_quiesce_wait(sbi))
		return -EINTR;
	if (XMFS_I(inode)->die) {
		xmfs_info(sbi->sb,
			  "inode logs have been interrupted before write iter");
		inode_unlock(inode);
		return -EINVAL;
	}

	struct xmfs_log xm_log = {};

	unsigned long start_index = iocb->ki_pos / PAGE_SIZE;
	unsigned long end_index =
		(iov_iter_count(from) + iocb->ki_pos + PAGE_SIZE - 1) /
		PAGE_SIZE;
	unsigned long start_offset = iocb->ki_pos;
	unsigned long xmlt_addr;
	struct xmfs_rw_msg req = {}, grant = {};

	ret = generic_write_checks(iocb, from);
	if (ret > 0) {
		start_index = iocb->ki_pos / PAGE_SIZE;
		end_index =
			(iov_iter_count(from) + iocb->ki_pos + PAGE_SIZE - 1) /
			PAGE_SIZE;
		start_offset = iocb->ki_pos;
		if (XMFS_I(inode)->fallocate &&
		    end_index > XMFS_I(inode)->data_size / PAGE_SIZE) {
			xmfs_error(sbi->sb,
				   "write iter do not support write more than fallocate size");
			inode_unlock(inode);
			return -EOPNOTSUPP;
		}

		ret = file_remove_privs(file);
		if (ret) {
			xmfs_error(sbi->sb,
				   "write iter remove privs failed %ld", ret);
			inode_unlock(inode);
			return ret;
		}

		ret = file_update_time(file);
		if (ret) {
			xmfs_error(sbi->sb, "write iter update time failed %ld",
				   ret);
			inode_unlock(inode);
			return ret;
		}

		if (XMFS_I(inode)->fallocate)
			goto start_write;

		req.ops = XMFS_UPDATE;
		req.i_ino = inode->i_ino;
		req.data_pages = end_index - start_index;
		req.log_entries = 1;
		req.size = iocb->ki_pos + iov_iter_count(from);
		req.type = MSG_RW_REQUEST;
		req.slave_id = sbi->id;
		spin_lock(&sbi->local_ring_lock);
		req.request_id = ++sbi->rw_request_id;
		spin_unlock(&sbi->local_ring_lock);
		xmfs_host_handle_urma_request(sbi, sbi->id, &req, &grant);
		XMFS_I(inode)->start_write = grant.data_addr;
		XMFS_I(inode)->count = grant.data_page_count;
		XMFS_I(inode)->second_write = grant.second_data_addr;
		XMFS_I(inode)->second_write_count = 0;

start_write:
		/*
		 * xmfs_info(sbi->sb, "write start %ld %ld %d %ld",
		 *	     XMFS_I(inode)->start_write,
		 *	     XMFS_I(inode)->second_write, XMFS_I(inode)->count,
		 *	     XMFS_I(inode)->second_write_count);
		 */
		ret = generic_perform_write(iocb, from);
	} else {
		inode_unlock(inode);
		return ret;
	}
	/*
	 * xmfs_info(sbi->sb,
	 *	     "start offset %ld index %ld end index %ld pos %ld %ld",
	 *	     start_offset, start_index, end_index, iocb->ki_pos,
	 *	     (iocb->ki_pos + PAGE_SIZE - 1) / PAGE_SIZE);
	 */
	XMFS_I(inode)->start_write = 0;
	XMFS_I(inode)->second_write = 0;
	XMFS_I(inode)->count = 0;
	XMFS_I(inode)->second_write_count = 0;
	if (ret <= 0) {
		inode_unlock(inode);
		return ret;
	}

	for (int s = 0; s < grant.log_slot_count; s++) {
		xm_log.ops = XMFS_UPDATE;
		xm_log.i_ino = inode->i_ino;
		xm_log.version = grant.version + s;
		xm_log.writer_id = sbi->id;
		if (s == 0) {
			xm_log.index = start_index;
			xm_log.end_index = start_index + grant.data_page_count;
			xm_log.data_offset = grant.data_addr;
			xm_log.size = req.size;
		} else {
			xm_log.index = start_index + grant.data_page_count;
			xm_log.end_index = end_index;
			xm_log.data_offset = grant.second_data_addr;
			xm_log.size = req.size;
		}
		xmlt_addr =
			xmfs_add_log_v3(&xm_log, sbi, 0, inode->i_ino, true);
	}

	inode_unlock(inode);
	return ret;
}

const struct pipe_buf_operations xmfs_pipe_buf_ops = {
	.release	= generic_pipe_buf_release,
	.try_steal	= generic_pipe_buf_try_steal,
	.get		= generic_pipe_buf_get,
};

static size_t splice_folio_into_pipe(struct pipe_inode_info *pipe,
				     struct folio *folio, loff_t fpos,
				     size_t size)
{
	struct page *page;
	size_t spliced = 0, offset = offset_in_folio(folio, fpos);

	page = folio_page(folio, offset / PAGE_SIZE);
	size = min(size, folio_size(folio) - offset);
	offset %= PAGE_SIZE;

	while (spliced < size &&
	       !pipe_full(pipe->head, pipe->tail, pipe->max_usage)) {
		struct pipe_buffer *buf = pipe_head_buf(pipe);
		size_t part = min_t(size_t, PAGE_SIZE - offset, size - spliced);

		*buf = (struct pipe_buffer) {
			.ops	= &xmfs_pipe_buf_ops,
			.page	= page,
			.offset	= offset,
			.len	= part,
		};
		folio_get(folio);
		pipe->head++;
		page++;
		spliced += part;
		offset = 0;
	}

	return spliced;
}

static bool zero_pipe_buf_get(struct pipe_inode_info *pipe,
			      struct pipe_buffer *buf)
{
	return true;
}

static void zero_pipe_buf_release(struct pipe_inode_info *pipe,
				  struct pipe_buffer *buf)
{
}

static bool zero_pipe_buf_try_steal(struct pipe_inode_info *pipe,
				    struct pipe_buffer *buf)
{
	return false;
}

static const struct pipe_buf_operations zero_pipe_buf_ops = {
	.release	= zero_pipe_buf_release,
	.try_steal	= zero_pipe_buf_try_steal,
	.get		= zero_pipe_buf_get,
};

static size_t splice_zeropage_into_pipe(struct pipe_inode_info *pipe,
					loff_t fpos, size_t size)
{
	size_t offset = fpos & ~PAGE_MASK;

	size = min_t(size_t, size, PAGE_SIZE - offset);

	if (!pipe_full(pipe->head, pipe->tail, pipe->max_usage)) {
		struct pipe_buffer *buf = pipe_head_buf(pipe);

		*buf = (struct pipe_buffer) {
			.ops	= &zero_pipe_buf_ops,
			.page	= ZERO_PAGE(0),
			.offset	= offset,
			.len	= size,
		};
		pipe->head++;
	}

	return size;
}

static ssize_t xmfs_file_read_iter_slave(struct kiocb *iocb,
					 struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
	struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);
	pgoff_t index;
	pgoff_t end_index;
	unsigned long offset;
	unsigned long *offsets;
	int error = 0;
	int count = 0;
	ssize_t retval = 0;
	loff_t *ppos = &iocb->ki_pos;
	loff_t i_size = iov_iter_count(to);
	struct page **pages;
	/* xmfs_info(sbi->sb, "read iter slave"); */

	/*
	 * struct xmfs_sb_index *xmsi =
	 *	kmalloc(sizeof(struct xmfs_sb_index), GFP_KERNEL);
	 * int err = get_xmsi_and_copy(sbi, xmsi);
	 *
	 * if (unlikely(err)) {
	 *	xmfs_error(sbi->sb,
	 *		   "hardware memory error when reading superblock during slave lookup");
	 *	kfree(xmsi);
	 *	return err;
	 * }
	 *
	 * if (xmsi->last_update != sbi->last_update) {
	 *	if (!sbi->debug)
	 *		xmfs_replay_new_entries(sbi, NULL, xmsi->last_update);
	 *	else
	 *		xmfs_replay_new_entries(sbi, NULL, xmsi->last_update);
	 * }
	 *
	 * kfree(xmsi);
	 */

	index = *ppos >> PAGE_SHIFT;
	offset = *ppos & ~PAGE_MASK;
	end_index = (i_size + *ppos) / PAGE_SIZE;
	offsets = kzalloc((end_index - index + 1) * sizeof(unsigned long),
			  GFP_KERNEL);
	if (!offsets)
		return -ENOMEM;

	pages = kzalloc((end_index - index + 1) * sizeof(struct page *),
			GFP_KERNEL);
	if (!pages) {
		kfree(offsets);
		return -ENOMEM;
	}
	for (;;) {
		struct folio *folio = NULL;
		unsigned long nr, ret;

		if (index > end_index)
			break;
		if (index == end_index) {
			nr = (i_size + *ppos) & ~PAGE_MASK;
			if (nr <= offset)
				break;
		} else {
			nr = PAGE_SIZE;
		}
		nr -= offset;

		folio = filemap_get_folio(inode->i_mapping, index);
		if (IS_ERR(folio))
			folio = NULL;

		if (folio) {
			folio_put(folio);
		} else {
			/*
			 * Copy to user tends to be so well optimized, but
			 * clear_user() not so much, that it is noticeably
			 * faster to copy the zero page instead of clearing.
			 */
			/* xmfs_error(sbi->sb, "qyf bad things happened"); */
			unsigned long mapped_offset =
				(unsigned long)xa_load(&xmfs_inode->shared_pages,
						       index);
			if (mapped_offset) {
				folio = __filemap_get_folio(inode->i_mapping, index,
							    FGP_LOCK | FGP_WRITE |
								    FGP_CREAT,
							    inode->i_mapping->gfp_mask);
				if (IS_ERR(folio)) {
					xmfs_info(sbi->sb,
						  "splice cannot get the folio");
					error = PTR_ERR(folio);
					folio = NULL;
					break;
				}
				offsets[count] = mapped_offset;
				pages[count] = &folio->page;
				count++;
			} else {
				xmfs_info(sbi->sb, "no offset for %ld", index);
			}
		}
		ret = nr;
		offset += ret;
		index += offset >> PAGE_SHIFT;
		offset &= ~PAGE_MASK;
	}

	if (count > 0) {
		int idx = 0;

		while (count > 0) {
			int num = min(512, count);

			count -= num;
			xmfs_pmem_read_multi_pages(sbi, offsets + idx,
						   pages + idx, num);
			idx += num;
		}
		for (int i = 0; i < idx; i++) {
			struct folio *folio = page_folio(pages[i]);

			folio_mark_uptodate(folio);
			folio_unlock(folio);
			folio_put(folio);
		}
	}
	kfree(offsets);
	kfree(pages);
	index = *ppos >> PAGE_SHIFT;
	offset = *ppos & ~PAGE_MASK;
	i_size = i_size_read(inode);
	end_index = i_size >> PAGE_SHIFT;

	for (;;) {
		struct folio *folio = NULL;
		struct page *page = NULL;
		unsigned long nr, ret;

		if (index > end_index)
			break;
		if (index == end_index) {
			nr = i_size & ~PAGE_MASK;
			if (nr <= offset)
				break;
		} else {
			nr = PAGE_SIZE;
		}
		nr -= offset;

		folio = filemap_get_folio(inode->i_mapping, index);
		if (!IS_ERR(folio)) {
			bool uptodate;

			folio_lock(folio);
			uptodate = folio_test_uptodate(folio);
			folio_unlock(folio);
			if (!uptodate) {
				folio_put(folio);
				folio = NULL;
			}
		} else {
			folio = NULL;
		}
		if (IS_ERR(folio))
			folio = NULL;

		if (folio) {
			page = folio_page(folio, 0);
			ret = copy_page_to_iter(page, offset, nr, to);
			folio_put(folio);
		} else {
			ret = copy_page_to_iter(ZERO_PAGE(0), offset, nr, to);
		}

		retval += ret;
		offset += ret;
		index += offset >> PAGE_SHIFT;
		offset &= ~PAGE_MASK;

		if (!iov_iter_count(to))
			break;
		if (ret < nr) {
			error = -EFAULT;
			break;
		}
	}

	*ppos = ((loff_t)index << PAGE_SHIFT) + offset;
	file_accessed(file);
	return retval ? retval : error;
}

int xmfs_collect_read_extents(struct xmfs_inode_info *inode, pgoff_t start,
			      pgoff_t end, struct xmfs_read_extent *exts,
			      unsigned int max_exts)
{
	XA_STATE(xas, &inode->shared_pages, start);

	void *entry;
	unsigned long offset;
	pgoff_t index;
	unsigned int nr_exts = 0;
	bool in_extent = false;

	pgoff_t cur_index = 0;
	unsigned long cur_offset = 0;
	unsigned int cur_pages = 0;

	xas_lock(&xas);

	xas_for_each(&xas, entry, end - 1) {
		offset = (unsigned long)entry;

		index = xas.xa_index;

		/*
		 * xa_load() 返回 NULL 时不会进入这里，
		 * xas_for_each() 已经跳过空洞。
		 */

		if (!in_extent) {
			cur_index = index;
			cur_offset = offset;
			cur_pages = 1;
			in_extent = true;
			continue;
		}

		/*
		 * 是否仍然连续。
		 */
		if (index == cur_index + cur_pages &&
		    offset == cur_offset + ((unsigned long)cur_pages
					    << PAGE_SHIFT)) {
			cur_pages++;
			continue;
		}

		/*
		 * 保存上一段 extent。
		 */
		if (nr_exts >= max_exts) {
			xas_unlock(&xas);
			return -ENOSPC;
		}
		exts[nr_exts].start_index = cur_index;
		exts[nr_exts].start_offset = cur_offset;
		exts[nr_exts].nr_pages = cur_pages;
		nr_exts++;
		/*
		 * 新 extent。
		 */
		cur_index = index;
		cur_offset = offset;
		cur_pages = 1;
	}

	xas_unlock(&xas);

	/*
	 * Flush 最后一个 extent。
	 */
	if (in_extent) {
		if (nr_exts >= max_exts)
			return -ENOSPC;
		exts[nr_exts].start_index = cur_index;
		exts[nr_exts].start_offset = cur_offset;
		exts[nr_exts].nr_pages = cur_pages;
		nr_exts++;
	}

	return nr_exts;
}

static ssize_t xmfs_file_read_ubuf_slave(struct kiocb *iocb,
					 struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
	struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);
	pgoff_t index;
	pgoff_t end_index;
	unsigned long offset;
	int err = 0;
	int count = 0;
	ssize_t retval = 0;
	loff_t *ppos = &iocb->ki_pos;
	loff_t i_size = iov_iter_count(to);
	loff_t end_pos = min(i_size + iocb->ki_pos, inode->i_size);
	struct xmfs_read_extent *exts;

	index = *ppos >> PAGE_SHIFT;
	offset = *ppos & ~PAGE_MASK;
	end_index = (end_pos + PAGE_SIZE - 1) / PAGE_SIZE;

	count = end_index - index + 1;

	exts = vzalloc(count * sizeof(struct xmfs_read_extent));
	/*
	 * xmfs_info(sbi->sb, "qyf before count is %d end is %ld %ld %ld",
	 *	     count, inode->i_size, *ppos, i_size);
	 */
	count = xmfs_collect_read_extents(xmfs_inode, index, end_index, exts,
					  count);
	/* xmfs_info(sbi->sb, "qyf count is %d %ld", count, i_size); */
	for (int i = 0; i < count; i++)
		retval += exts[i].nr_pages * PAGE_SIZE;

	retval = min(retval, end_pos - *ppos);
	if (count != 0 && retval != 0) {
		exts[0].start_offset += offset;
		err = xmfs_urma_read_ubuf(sbi, to->ubuf, exts, count, retval);
	}
	vfree(exts);
	iocb->ki_pos += retval;
	file_accessed(file);
	/* xmfs_info(sbi->sb, "qyf read return %ld %ld", err, retval); */
	return err ? err : retval;
}

static ssize_t xmfs_file_splice_read(struct file *in, loff_t *ppos,
				     struct pipe_inode_info *pipe, size_t len,
				     unsigned int flags)
{
	struct inode *inode = file_inode(in);
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
	struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);
	struct address_space *mapping = inode->i_mapping;
	struct folio *folio = NULL;
	size_t total_spliced = 0, used, npages, n, part;
	loff_t isize;
	int error = 0;

	struct xmfs_sb_index *xmsi =
		kmalloc(sizeof(struct xmfs_sb_index), GFP_KERNEL);
	int err = get_xmsi_and_copy(sbi, xmsi);

	if (unlikely(err)) {
		xmfs_error(sbi->sb,
			   "hardware memory error when reading superblock during splice read");
		kfree(xmsi);
		return err;
	}

	if (xmsi->last_update != sbi->last_update) {
		if (!sbi->debug)
			xmfs_replay_new_entries(sbi, NULL, xmsi->last_update);
		else
			xmfs_replay_new_entries(sbi, NULL, xmsi->last_update);
	}
	kfree(xmsi);

	used = pipe_occupancy(pipe->head, pipe->tail);
	npages = max_t(ssize_t, pipe->max_usage - used, 0);
	len = min_t(size_t, len, (npages * PAGE_SIZE));

	do {
		if (*ppos >= i_size_read(inode))
			break;

		folio = filemap_get_folio(inode->i_mapping, *ppos / PAGE_SIZE);
		if (!IS_ERR(folio)) {
			bool uptodate;

			folio_lock(folio);
			uptodate = folio_test_uptodate(folio);
			folio_unlock(folio);
			if (!uptodate) {
				folio_put(folio);
				folio = NULL;
			}
		} else {
			folio = NULL;
		}

		/*
		 * i_size must be checked after we know the pages are Uptodate.
		 *
		 * Checking i_size after the check allows us to calculate
		 * the correct value for "nr", which means the zero-filled
		 * part of the page is not copied back to userspace (unless
		 * another truncate extends the file - this is desired though).
		 */
		isize = i_size_read(inode);
		if (unlikely(*ppos >= isize))
			break;
		part = min_t(loff_t, isize - *ppos, len);

		if (folio) {
			/*
			 * If users can be writing to this page using arbitrary
			 * virtual addresses, take care about potential aliasing
			 * before reading the page on the kernel side.
			 */
			if (mapping_writably_mapped(mapping))
				flush_dcache_folio(folio);
			folio_mark_accessed(folio);
			/*
			 * Ok, we have the page, and it's up-to-date, so we can
			 * now splice it into the pipe.
			 */
			n = splice_folio_into_pipe(pipe, folio, *ppos, part);
			folio_put(folio);
			folio = NULL;
		} else {
			unsigned long mapped_offset =
				(unsigned long)xa_load(&xmfs_inode->shared_pages,
						       *ppos / PAGE_SIZE);
			if (!mapped_offset) {
				n = splice_zeropage_into_pipe(pipe, *ppos,
							      part);
			} else {
				folio = __filemap_get_folio(inode->i_mapping,
							    *ppos / PAGE_SIZE,
							    FGP_LOCK | FGP_WRITE |
								    FGP_CREAT,
							    inode->i_mapping->gfp_mask);
				if (IS_ERR(folio)) {
					xmfs_info(sbi->sb,
						  "splice cannot get the folio");
					error = PTR_ERR(folio);
					folio = NULL;
					break;
				}
				void *kaddr =
					kmap_local_page(folio_file_page(folio,
									*ppos / PAGE_SIZE));
				error = get_data_and_copy(sbi, mapped_offset,
							  kaddr, PAGE_SIZE,
							  NULL, 0);
				if (unlikely(error)) {
					xmfs_error(sbi->sb,
						   "hardware memory error when reading data during splice read");
					folio_unlock(folio);
					folio_put(folio);
					folio = NULL;
					break;
				}
				kunmap_local(kaddr);
				folio_mark_uptodate(folio);
				n = splice_folio_into_pipe(pipe, folio, *ppos,
							   part);
				folio_unlock(folio);
				folio_put(folio);
				folio = NULL;
			}
		}

		if (!n)
			break;
		len -= n;
		total_spliced += n;
		*ppos += n;
		in->f_ra.prev_pos = *ppos;
		if (pipe_full(pipe->head, pipe->tail, pipe->max_usage))
			break;

	} while (len);

	if (folio)
		folio_put(folio);

	file_accessed(in);
	return total_spliced ? total_spliced : error;
}

static ssize_t xmfs_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);

	if (sbi->id != 0) {
		if (iter_is_ubuf(to))
			return xmfs_file_read_ubuf_slave(iocb, to);
		return xmfs_file_read_iter_slave(iocb, to);
	}

	struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);
	pgoff_t index;
	unsigned long offset;
	int error = 0;
	ssize_t retval = 0;
	loff_t *ppos = &iocb->ki_pos;

	struct xmfs_sb_index *xmsi =
		kmalloc(sizeof(struct xmfs_sb_index), GFP_KERNEL);
	int err = get_xmsi_and_copy(sbi, xmsi);

	if (unlikely(err)) {
		xmfs_error(sbi->sb,
			   "hardware memory error when reading superblock during read iter");
		kfree(xmsi);
		return err;
	}

	/*
	 * if (xmsi->last_update != sbi->last_update) {
	 *	if (!sbi->debug)
	 *		xmfs_replay_new_entries(sbi, NULL, xmsi->last_update);
	 *	else
	 *		xmfs_replay_new_entries(sbi, NULL, xmsi->last_update);
	 * }
	 */

	kfree(xmsi);

	index = *ppos >> PAGE_SHIFT;
	offset = *ppos & ~PAGE_MASK;

	for (;;) {
		struct folio *folio = NULL;
		struct page *page = NULL;
		pgoff_t end_index;
		unsigned long nr, ret;
		loff_t i_size = i_size_read(inode);

		end_index = i_size >> PAGE_SHIFT;
		if (index > end_index)
			break;
		if (index == end_index) {
			nr = i_size & ~PAGE_MASK;
			if (nr <= offset)
				break;
		} else {
			nr = PAGE_SIZE;
		}
		nr -= offset;

		folio = filemap_get_folio(inode->i_mapping, index);
		if (!IS_ERR(folio)) {
			bool uptodate;

			folio_lock(folio);
			uptodate = folio_test_uptodate(folio);
			folio_unlock(folio);
			if (!uptodate) {
				folio_put(folio);
				folio = NULL;
			}
		} else {
			folio = NULL;
		}
		if (IS_ERR(folio))
			folio = NULL;

		if (folio) {
			page = folio_page(folio, 0);
			ret = copy_page_to_iter(page, offset, nr, to);
			folio_put(folio);
		} else {
			/*
			 * Copy to user tends to be so well optimized, but
			 * clear_user() not so much, that it is noticeably
			 * faster to copy the zero page instead of clearing.
			 */
			/* xmfs_error(sbi->sb, "qyf bad things happened"); */
			unsigned long mapped_offset =
				(unsigned long)xa_load(&xmfs_inode->shared_pages,
						       index);
			if (!mapped_offset) {
				ret = copy_page_to_iter(ZERO_PAGE(0), offset,
							nr, to);
			} else {
				void *data;

				data = get_data(sbi, mapped_offset + offset,
						nr);
				if (IS_ERR(data)) {
					error = PTR_ERR(data);
					break;
				}
				ret = _copy_to_iter(data, nr, to);
				put_data(sbi, data);
			}
		}

		retval += ret;
		offset += ret;
		index += offset >> PAGE_SHIFT;
		offset &= ~PAGE_MASK;

		if (!iov_iter_count(to))
			break;
		if (ret < nr) {
			error = -EFAULT;
			break;
		}
	}

	*ppos = ((loff_t)index << PAGE_SHIFT) + offset;
	file_accessed(file);
	return retval ? retval : error;
}

#ifdef CONFIG_XMFS_FS_URMA
static ssize_t xmfs_file_write_iter_slave(struct kiocb *iocb,
					  struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
	unsigned long start_index = iocb->ki_pos / PAGE_SIZE;
	unsigned long end_index;
	unsigned long page_num;
	ssize_t ret;

	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		return ret;

	end_index = (iov_iter_count(from) + iocb->ki_pos + PAGE_SIZE - 1) /
		    PAGE_SIZE;
	page_num = end_index - start_index;

	/*
	 * req.ops = XMFS_UPDATE;
	 * req.i_ino = inode->i_ino;
	 * req.data_pages = page_num;
	 * req.log_entries = 1;
	 * req.size = iocb->ki_pos + iov_iter_count(from);
	 * req.type = MSG_RW_REQUEST;
	 * req.slave_id = sbi->id;
	 * spin_lock(&sbi->local_ring_lock);
	 * req.request_id = ++sbi->rw_request_id;
	 * spin_unlock(&sbi->local_ring_lock);
	 *
	 * struct xmfs_grant_wait gw;
	 * init_completion(&gw.done);
	 * xa_store(&sbi->urma_cfg.grant_waits, req.request_id, &gw,
	 *	    GFP_KERNEL);
	 * err = xmfs_send_request(sbi, &req, &gw, 0);
	 * if (err)
	 *	return err;
	 * grant = gw.grant;
	 * log_base = grant.log_trunk_addr +
	 *	offsetof(struct xmfs_log_trunk, log[0]);
	 */

	inode_lock(inode);
	/*
	 * xmfs_inode->start_write = grant.data_addr;
	 * xmfs_inode->count = grant.data_page_count;
	 * xmfs_inode->second_write = grant.second_data_addr;
	 * xmfs_inode->second_write_count = 0;
	 */

	/*
	 * xmfs_inode->wr = vzalloc(page_num * sizeof(struct ubcore_jfs_wr));
	 * xmfs_inode->slots = vzalloc(sizeof(int) * page_num);
	 * xmfs_inode->write_buffer = urma_pool_acquire_nslots(
	 *	&sbi->urma_cfg.pool, page_num, xmfs_inode->slots);
	 * xmfs_inode->wr_cnt = 0;
	 */

	ret = generic_perform_write(iocb, from);

	struct xmfs_write_work *ww;

	ww = kmalloc(sizeof(*ww), GFP_KERNEL);
	if (!ww) {
		inode_unlock(inode);
		return ret;
	}

	ww->inode = inode;
	ww->index = start_index;
	ww->end_index = end_index;
	ww->size = inode->i_size;
	ihold(inode);
	INIT_WORK(&ww->work, xmfs_write_worker);
	if (xmfs_inode->write_wq)
		queue_work(xmfs_inode->write_wq, &ww->work);
	else
		queue_work(sbi->prefetch_wq, &ww->work);

	/*
	 * for (s = 0; s < grant.log_slot_count; s++) {
	 *	struct xmfs_log xm_log = {};
	 *
	 *	xm_log.ops = XMFS_UPDATE;
	 *	xm_log.i_ino = inode->i_ino;
	 *	xm_log.version = grant.version + s;
	 *	xm_log.writer_id = sbi->id;
	 *	if (s == 0) {
	 *		xm_log.index = start_index;
	 *		xm_log.end_index =
	 *			start_index + grant.data_page_count;
	 *		xm_log.data_offset = grant.data_addr;
	 *		xm_log.size = req.size;
	 *	} else {
	 *		xm_log.index = start_index + grant.data_page_count;
	 *		xm_log.end_index = end_index;
	 *		xm_log.data_offset = grant.second_data_addr;
	 *		xm_log.size = req.size;
	 *	}
	 *	xmfs_data_write(
	 *		sbi, log_base + (grant.log_slot_start + s) *
	 *				 sizeof(struct xmfs_log),
	 *		&xm_log, sizeof(xm_log));
	 *
	 *	pages = kzalloc((xm_log.end_index - xm_log.index) *
	 *			sizeof(struct page *), GFP_KERNEL);
	 *	for (int i = xm_log.index; i < xm_log.end_index; i++) {
	 *		struct folio *folio =
	 *			filemap_get_folio(inode->i_mapping, i);
	 *
	 *		if (IS_ERR(folio)) {
	 *			xm_log.end_index = i;
	 *			break;
	 *		}
	 *		pages[i - xm_log.index] = &folio->page;
	 *	}
	 *	xmfs_pmem_write_multi_pages(
	 *		sbi, xm_log.data_offset, pages,
	 *		xm_log.end_index - xm_log.index);
	 *	for (int i = xm_log.index; i < xm_log.end_index; i++) {
	 *		struct folio *folio =
	 *			page_folio(pages[i - xm_log.index]);
	 *
	 *		folio_unlock(folio);
	 *		folio_put(folio);
	 *	}
	 *	kfree(pages);
	 * }
	 */
	/*
	 * xmfs_urma_send_write_pages(sbi, xmfs_inode->wr);
	 *
	 * urma_pool_release_nslots(&sbi->urma_cfg.pool, page_num,
	 *			  xmfs_inode->slots);
	 * vfree(xmfs_inode->slots);
	 * xmfs_inode->start_write = 0;
	 * xmfs_inode->second_write = 0;
	 * xmfs_inode->count = 0;
	 * xmfs_inode->second_write_count = 0;
	 */
	inode_unlock(inode);

	return ret;
}

static ssize_t xmfs_file_write_ubuf_slave(struct kiocb *iocb,
					  struct iov_iter *from)
{
	/* xmfs_info(sbi->sb, "qyf start write ubuf"); */
	struct inode *inode = file_inode(iocb->ki_filp);
	struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
	struct xmfs_rw_msg req = {}, grant = {};
	unsigned long start_index = iocb->ki_pos / PAGE_SIZE;
	unsigned long end_index;
	unsigned long page_num;
	unsigned long log_base;
	int err, s;
	ssize_t ret;

	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		return ret;

	end_index = (iov_iter_count(from) + iocb->ki_pos + PAGE_SIZE - 1) /
		    PAGE_SIZE;
	page_num = end_index - start_index;

	req.ops = XMFS_UPDATE;
	req.i_ino = inode->i_ino;
	req.data_pages = page_num;
	req.log_entries = 1;
	req.size = iocb->ki_pos + iov_iter_count(from);
	req.type = MSG_RW_REQUEST;
	req.slave_id = sbi->id;
	spin_lock(&sbi->local_ring_lock);
	req.request_id = ++sbi->rw_request_id;
	spin_unlock(&sbi->local_ring_lock);

	struct xmfs_grant_wait gw;

	init_completion(&gw.done);
	xa_store(&sbi->urma_cfg.grant_waits, req.request_id, &gw, GFP_KERNEL);
	err = xmfs_send_request(sbi, &req, &gw, 0);
	if (err)
		return err;
	grant = gw.grant;
	log_base =
		grant.log_trunk_addr + offsetof(struct xmfs_log_trunk, log[0]);

	inode_lock(inode);
	xmfs_inode->start_write = grant.data_addr;
	xmfs_inode->count = grant.data_page_count;
	xmfs_inode->second_write = grant.second_data_addr;
	xmfs_inode->second_write_count = 0;
	/* xmfs_info(sbi->sb, "qyf start write ubuf data"); */
	xmfs_urma_write_ubuf(sbi, from->ubuf, grant.data_page_count * PAGE_SIZE,
			     grant.data_addr);
	if (grant.log_slot_count > 1 && grant.second_data_addr != 0 &&
	    grant.second_data_pages != 0) {
		xmfs_urma_write_ubuf(sbi,
				     from->ubuf +
					     grant.data_page_count * PAGE_SIZE,
				     iov_iter_count(from) -
					     grant.data_page_count * PAGE_SIZE,
				     grant.second_data_addr);
	}
	/* xmfs_info(sbi->sb, "qyf start end ubuf data"); */
	for (s = 0; s < grant.log_slot_count; s++) {
		struct xmfs_log xm_log = {};

		xm_log.ops = XMFS_UPDATE;
		xm_log.i_ino = inode->i_ino;
		xm_log.version = grant.version + s;
		xm_log.writer_id = sbi->id;
		if (s == 0) {
			xm_log.index = start_index;
			xm_log.end_index = start_index + grant.data_page_count;
			xm_log.data_offset = grant.data_addr;
			xm_log.size = req.size;
			int chunk_header_cnt = 0;

			for (int i = 0; i < grant.data_page_count; i++) {
				unsigned long offset =
					grant.data_addr + i * PAGE_SIZE +
					chunk_header_cnt * PAGE_SIZE;
				if (offset % XMFS_DEFAULT_CHUNK_SIZE == 0) {
					chunk_header_cnt++;
					offset = grant.data_addr +
						 i * PAGE_SIZE +
						 chunk_header_cnt * PAGE_SIZE;
				}
				xa_store(&(XMFS_I(inode)->shared_pages),
					 i + start_index, (void *)offset,
					 GFP_KERNEL);
			}
		} else {
			xm_log.index = start_index + grant.data_page_count;
			xm_log.end_index = end_index;
			xm_log.data_offset = grant.second_data_addr;
			xm_log.size = req.size;
			int chunk_header_cnt = 0;

			for (int i = 0; i < grant.second_data_pages; i++) {
				unsigned long offset =
					grant.second_data_addr + i * PAGE_SIZE +
					chunk_header_cnt * PAGE_SIZE;
				if (offset % XMFS_DEFAULT_CHUNK_SIZE == 0) {
					chunk_header_cnt++;
					offset = grant.second_data_addr +
						 i * PAGE_SIZE +
						 chunk_header_cnt * PAGE_SIZE;
				}
				xa_store(&(XMFS_I(inode)->shared_pages),
					 i + start_index +
						 grant.data_page_count,
					 (void *)offset, GFP_KERNEL);
			}
		}
		xmfs_data_write(sbi,
				log_base + (grant.log_slot_start + s) *
						   sizeof(struct xmfs_log),
				&xm_log, sizeof(xm_log));
	}
	/* xmfs_info(sbi->sb, "qyf start write ubuf log"); */
	iocb->ki_pos += iov_iter_count(from);
	if (i_size_read(inode) < iocb->ki_pos)
		i_size_write(inode, iocb->ki_pos);
	inode_unlock(inode);

	/*
	 * struct xmfs_cache_work *cw;
	 *
	 * cw = kmalloc(sizeof(*cw), GFP_KERNEL);
	 * if (!cw)
	 *	return ret;
	 *
	 * cw->inode = inode;
	 * cw->index = start_index;
	 * cw->end_index = end_index;
	 * INIT_WORK(&cw->work, xmfs_prefetch_worker);
	 * queue_work(sbi->prefetch_wq, &cw->work);
	 */
	/* xmfs_info(sbi->sb, "qyf end write ubuf"); */
	return ret;
}
#endif

static loff_t xmfs_seek_block(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;
	struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);
	loff_t maxbytes = inode->i_sb->s_maxbytes;
	pgoff_t pgofs, preofs;
	loff_t data_ofs = offset;
	loff_t isize;

	inode_lock_shared(inode);

	isize = i_size_read(inode);

	if (offset >= isize) {
		inode_unlock_shared(inode);
		return -ENXIO;
	}

	pgofs = (pgoff_t)(offset >> PAGE_SHIFT);
	preofs = pgofs;
	unsigned long i;
	void *ptr;

	if (whence == SEEK_DATA) {
		bool found = false;

		xa_for_each_start(&xmfs_inode->shared_pages, i, ptr, pgofs) {
			if (i == pgofs) {
				/*
				 * 场景 A：offset 刚好落在一个包含数据的页内，
				 * 直接返回原 offset。
				 */
				data_ofs = offset;
			} else {
				/*
				 * 场景 B：offset 落在空洞内，下一个数据页在 i，
				 * 返回新数据页的起始地址。
				 */
				data_ofs = (loff_t)i << PAGE_SHIFT;
			}
			found = true;
			/* 找到第一个数据块即可，退出循环。 */
			break;
		}
		if (!found) {
			/* 之后没有数据了。 */
			inode_unlock_shared(inode);
			return -ENXIO;
		}
	} else if (whence == SEEK_HOLE) {
		unsigned long expected = pgofs;
		bool in_hole = true;

		xa_for_each_start(&xmfs_inode->shared_pages, i, ptr, pgofs) {
			if (i > expected) {
				/*
				 * 发现断层：本来期望页号是 expected，但拿到的 i
				 * 却比它大，说明 expected 处存在一个洞，
				 * 跳出循环。
				 */
				break;
			}
			/*
			 * 如果走到这里，说明 i == expected，该页有数据。
			 * 既然第一页有数据，说明 offset 不在洞里。
			 */
			in_hole = false;
			/* 期望下一页。 */
			expected++;
		}
		if (in_hole) {
			/*
			 * 场景 A：offset 所在的起始页没有数据（或者没有后续
			 * 数据），所以 offset 本身就在空洞里，
			 * 直接返回原 offset。
			 */
			data_ofs = offset;
		} else {
			/*
			 * 场景 B：遇到连续的数据页，
			 * 在期望页 expected 发生断层，
			 * 返回断层起始地址。
			 */
			data_ofs = (loff_t)expected << PAGE_SHIFT;
		}
		/*
		 * 场景 C：隐式文件尾空洞。如果算出的洞偏移超过文件大小，
		 * 则取文件大小。
		 */
		if (data_ofs > isize)
			data_ofs = isize;
	} else {
		inode_unlock_shared(inode);
		return -EINVAL;
	}
	inode_unlock_shared(inode);
	return vfs_setpos(file, data_ofs, maxbytes);
}

static loff_t xmfs_file_llseek(struct file *file, loff_t offset, int whence)
{
	loff_t size = i_size_read(file->f_mapping->host);

	switch (whence) {
	case SEEK_END:
	case SEEK_CUR:
	case SEEK_SET:
		return generic_file_llseek(file, offset, whence);
	case SEEK_DATA:
	case SEEK_HOLE:
		if (offset < 0 || offset >= size)
			return -ENXIO;
		return xmfs_seek_block(file, offset, whence);
	}

	return -EINVAL;
}

static vm_fault_t xmfs_fault(struct vm_fault *vmf)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
	vm_fault_t ret = 0;
	unsigned long offset = vmf->pgoff << PAGE_SHIFT;
	pgoff_t index = vmf->pgoff;
	unsigned long mapped_offset;
	int err;

	if (offset >= i_size_read(inode))
		return VM_FAULT_SIGBUS;

	filemap_invalidate_lock_shared(inode->i_mapping);

	struct folio *folio = __filemap_get_folio(inode->i_mapping, index,
						  FGP_CREAT | FGP_FOR_MMAP,
						  vmf->gfp_mask);
	if (IS_ERR(folio)) {
		filemap_invalidate_unlock_shared(inode->i_mapping);
		return vmf_error(PTR_ERR(folio));
	}

	folio_lock(folio);

	void *kaddr = kmap_local_page(folio_file_page(folio, index));

	mapped_offset =
		(unsigned long)xa_load(&xmfs_inode->shared_pages, index);
	if (mapped_offset) {
		err = get_data_and_copy(sbi, mapped_offset, kaddr, PAGE_SIZE, NULL,
					0);
		if (unlikely(err)) {
			kunmap_local(kaddr);
			folio_unlock(folio);
			folio_put(folio);
			filemap_invalidate_unlock_shared(inode->i_mapping);
			return VM_FAULT_SIGBUS;
		}
	} else {
		memset(kaddr, 0, PAGE_SIZE);
	}
	kunmap_local(kaddr);
	folio_mark_uptodate(folio);

	vmf->page = folio_file_page(folio, index);
	filemap_invalidate_unlock_shared(inode->i_mapping);
	ret |= VM_FAULT_LOCKED;
	return ret;
}

static vm_fault_t xmfs_slave_fault(struct vm_fault *vmf)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
	vm_fault_t ret = 0;
	unsigned long offset = vmf->pgoff << PAGE_SHIFT;
	pgoff_t index = vmf->pgoff;
	struct page_freq *page_freq;

	if (offset >= i_size_read(inode)) {
		xmfs_info(sbi->sb, "offset is larger than size");
		return VM_FAULT_SIGBUS;
	}
	if (!sbi->always_hot) {
		page_freq = (struct page_freq *)xa_load(&xmfs_inode->page_freq,
							index);
		if (!page_freq) {
			page_freq =
				kzalloc(sizeof(struct page_freq), GFP_KERNEL);
			if (!page_freq) {
				xmfs_info(sbi->sb, "fault page freq no");
				return VM_FAULT_SIGBUS;
			}
			void *store_ret = xa_store(&xmfs_inode->page_freq,
						   index, (void *)page_freq,
						   GFP_KERNEL);
			if (xa_err(store_ret)) {
				xmfs_error(sbi->sb,
					   "cannot store shared pages for page %lu",
					   index);
			}
		}
		page_freq->in_cache = true;
		page_freq->never_in_cache = false;
	}
	filemap_invalidate_lock_shared(inode->i_mapping);
	struct folio *folio = __filemap_get_folio(inode->i_mapping, index,
						  FGP_CREAT | FGP_FOR_MMAP,
						  vmf->gfp_mask);
	if (IS_ERR(folio)) {
		filemap_invalidate_unlock_shared(inode->i_mapping);
		xmfs_info(sbi->sb, "fault oom %ld %d", PTR_ERR(folio),
			  vmf_error(PTR_ERR(folio)));
		return vmf_error(PTR_ERR(folio));
	}
	folio_lock(folio);

	void *kaddr = kmap_local_page(folio_file_page(folio, index));
	unsigned long mapped_offset =
		(unsigned long)xa_load(&xmfs_inode->shared_pages, index);
	int err = get_data_and_copy(sbi, mapped_offset, kaddr, PAGE_SIZE, NULL,
				    0);
	if (unlikely(err))
		xmfs_error(sbi->sb,
			   "hardware memory error when reading data during slave page fault");
	kunmap_local(kaddr);
	folio_mark_uptodate(folio);

	vmf->page = folio_file_page(folio, index);
	filemap_invalidate_unlock_shared(inode->i_mapping);
	if (!sbi->always_hot)
		page_freq->already_in_cache = true;

	ret |= VM_FAULT_LOCKED;
	return ret;
}

static vm_fault_t xmfs_map_pages(struct vm_fault *vmf, pgoff_t start_pgoff,
				 pgoff_t end_pgoff)
{
	vm_fault_t ret = filemap_map_pages(vmf, start_pgoff, end_pgoff);
	return ret;
}

static vm_fault_t xmfs_mkwrite(struct vm_fault *vmf)
{
	vm_fault_t ret = filemap_page_mkwrite(vmf);
	return ret;
}

static const struct vm_operations_struct xmfs_file_vm_ops = {
	.fault		= xmfs_fault,
	.map_pages	= xmfs_map_pages,
	.page_mkwrite	= xmfs_mkwrite,
};

static const struct vm_operations_struct xmfs_slave_file_vm_ops = {
	.fault		= xmfs_slave_fault,
	.map_pages	= xmfs_map_pages,
	.page_mkwrite	= xmfs_mkwrite,
};

static int xmfs_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct xmfs_sb_info *sbi = XMFS_SB(file_inode(file)->i_sb);

	file_accessed(file);
	if (sbi->id)
		vma->vm_ops = &xmfs_slave_file_vm_ops;
	else
		vma->vm_ops = &xmfs_file_vm_ops;
	return 0;
}

static int xmfs_slave_writepage(struct page *page,
				struct writeback_control *wbc)
{
	struct address_space *mapping = page->mapping;
	struct inode *inode = mapping->host;
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
	struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);
	unsigned long mapped_offset;

	if (!(inode->i_sb->s_flags & SB_ACTIVE)) {
		unlock_page(page);
		return 0;
	}

	mapped_offset =
		(unsigned long)xa_load(&xmfs_inode->shared_pages, page->index);
	if (mapped_offset) {
		unlock_page(page);
		return 0;
	}
	{
		struct xmfs_rw_msg req = {}, grant = {};
		unsigned long log_off;
		int err;

		req.ops = XMFS_UPDATE;
		req.i_ino = inode->i_ino;
		req.data_pages = 1;
		req.log_entries = 1;
		req.size = i_size_read(inode);
		req.type = MSG_RW_REQUEST;
		req.slave_id = sbi->id;
		spin_lock(&sbi->local_ring_lock);
		req.request_id = ++sbi->rw_request_id;
		spin_unlock(&sbi->local_ring_lock);
		struct xmfs_grant_wait gw;

		init_completion(&gw.done);
		xa_store(&sbi->urma_cfg.grant_waits, req.request_id, &gw,
			 GFP_KERNEL);
		err = xmfs_send_request(sbi, &req, &gw, 0);
		if (err) {
			if (!(inode->i_sb->s_flags & SB_ACTIVE)) {
				unlock_page(page);
				return 0;
			}
			redirty_page_for_writepage(wbc, page);
			unlock_page(page);
			return err;
		}
		grant = gw.grant;
		if (grant.type == MSG_RW_ERROR) {
			if (!(inode->i_sb->s_flags & SB_ACTIVE)) {
				unlock_page(page);
				return 0;
			}
			redirty_page_for_writepage(wbc, page);
			unlock_page(page);
			return -ENOSPC;
		}
		{
			char *kaddr = kmap_local_page(page);

			xmfs_data_write(sbi, grant.data_addr, kaddr, PAGE_SIZE);
			kunmap_local(kaddr);
		}
		log_off = grant.log_trunk_addr +
			  offsetof(struct xmfs_log_trunk, log[0]) +
			  grant.log_slot_start * sizeof(struct xmfs_log);
		{
			struct xmfs_log xm_log = {};

			xm_log.index = page->index;
			xm_log.end_index = page->index + 1;
			xm_log.ops = XMFS_UPDATE;
			xm_log.size = i_size_read(inode);
			xm_log.data_offset = grant.data_addr;
			xm_log.version = grant.version;
			xm_log.writer_id = sbi->id;
			xmfs_data_write(sbi, log_off, &xm_log, sizeof(xm_log));
		}
		/* xmfs_mark_page_live(sbi, grant.data_addr); */
		{
			void *old_ret =
				xa_store(&xmfs_inode->shared_pages, page->index,
					 (void *)grant.data_addr, GFP_KERNEL);
			/* xmfs_mark_page_dead(sbi, (unsigned long)old_ret); */
			if (xa_err(old_ret))
				xmfs_error(sbi->sb,
					   "slave writepage cannot store shared pages for page %ld",
					   page->index);
		}
		unlock_page(page);
		return 0;
	}
}

static int xmfs_writepage(struct page *page, struct writeback_control *wbc)
{
	struct address_space *mapping = page->mapping;
	struct inode *inode = mapping->host;
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);

	if (sbi->id != 0)
		return xmfs_slave_writepage(page, wbc);

	if (sbi->mode == XMFS_URMA_MODE) {
		redirty_page_for_writepage(wbc, page);
		unlock_page(page);
		return 0;
	}

	if (inode_is_locked(inode)) {
		redirty_page_for_writepage(wbc, page);
		unlock_page(page);
		return 0;
	}
	if (!inode || !XMFS_I(inode)) {
		xmfs_info(sbi->sb, "no inode now for write page");
		unlock_page(page);
		return -EINVAL;
	}
	struct xmfs_log xm_log;

	if (sbi == NULL || sbi->kaddr == NULL) {
		unlock_page(page);
		return 0;
	}
	unsigned long xmlt_addr;

	if (!spin_trylock(&sbi->space_lock)) {
		redirty_page_for_writepage(wbc, page);
		unlock_page(page);
		return -EAGAIN;
	}
	if (XMFS_I(inode)->die) {
		xmfs_info(sbi->sb,
			  "inode logs have been interrupted before writepage");
		spin_unlock(&sbi->space_lock);
		unlock_page(page);
		return -EINVAL;
	}

	if (XMFS_I(inode)->fallocate &&
	    page->index > XMFS_I(inode)->data_size / PAGE_SIZE) {
		xmfs_error(sbi->sb,
			   "writepage do not support write more than fallocate size");
		spin_unlock(&sbi->space_lock);
		unlock_page(page);
		return -EOPNOTSUPP;
	}

	struct xmfs_sb_index *xmsi = get_xmsi(sbi);

	if (!xmsi) {
		spin_unlock(&sbi->space_lock);
		unlock_page(page);
		return -EINVAL;
	}
	unsigned long data_current = xmsi->data_current;
	unsigned long data_count = xmsi->data_count;
	unsigned long data_offset;

	int logcount = 1;
	int need_data_trunk = (data_count == 511 || data_current == 0) ? 1 : 0;

	if (logcount + xmsi->used_trunk_count + need_data_trunk >
	    sbi->max_chunk) {
		sbi->full = true;
		spin_unlock(&sbi->space_lock);
		put_xmsi(sbi, xmsi);
		unlock_page(page);
		return -ENOSPC;
	}
	if (data_current == 0 || data_count == 511) {
		long slot = xmfs_alloc_and_mark_slot(xmsi, sbi, 'd');

		if (slot < 0) {
			spin_unlock(&sbi->space_lock);
			put_xmsi(sbi, xmsi);
			unlock_page(page);
			return -ENOSPC;
		}
		data_offset =
			DATA_AREA_OFFSET + slot * sbi->trunk_size + PAGE_SIZE;
		xmsi->data_current = data_offset;
		xmsi->data_count = 0;
	} else {
		data_offset = data_current + PAGE_SIZE;
		xmsi->data_current = data_offset;
		xmsi->data_count = data_count + 1;
	}
	spin_unlock(&sbi->space_lock);
	put_xmsi(sbi, xmsi);

	loff_t pos = (xm_log.index + 1) * PAGE_SIZE;

	xm_log.index = page->index;
	xm_log.end_index = xm_log.index + 1;
	xm_log.ops = XMFS_UPDATE;
	xm_log.p_ino = 0;
	xm_log.size = min(i_size_read(inode), pos);
	xm_log.offset = xm_log.index * PAGE_SIZE;
	xm_log.writer_id = sbi->id;
	if (XMFS_I(inode)->fallocate) {
		xm_log.data_offset =
			xmfs_get_data_offset_fallocate(inode, sbi, page);
	} else {
		char *kaddr = kmap_local_page(page);

		xmfs_data_write(sbi, data_offset, kaddr, PAGE_SIZE);
		arch_invalidate_pmem(sbi->kaddr + data_offset, PAGE_SIZE);
		kunmap_local(kaddr);
		xm_log.data_offset = data_offset;
	}

	void *old_ret = xa_store(&(XMFS_I(inode)->shared_pages), page->index,
				 (void *)xm_log.data_offset, GFP_KERNEL);
	if (xa_err(old_ret)) {
		xmfs_error(sbi->sb, "wb cannot store shared pages for page %ld",
			   page->index);
	}
	xmfs_mark_page_dead(sbi, (unsigned long)old_ret);
	xmfs_mark_page_live(sbi, xm_log.data_offset);

	xmlt_addr = xmfs_add_log_v3(&xm_log, sbi, 0, inode->i_ino, true);
	if (xmlt_addr < 0) {
		xmfs_info(sbi->sb, "writepage wrong");
		XMFS_I(inode)->die = true;
		unlock_page(page);
		return -EINVAL;
	}
	unlock_page(page);

	return 0;
}

static int xmfs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	return write_inode_now(file->f_mapping->host, 1);
}

static int xmfs_release(struct inode *inode, struct file *file)
{
	if (XMFS_I(inode)->tmp) {
		struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);

		struct xmfs_log xm_log = { 0 };
		unsigned long xmlt_addr;

		if (inode->__i_nlink > 1)
			xm_log.index = -1;
		else
			xm_log.index = 0;
		xm_log.end_index = -1;
		xm_log.ops = XMFS_DELETE;
		xm_log.data_offset = 0;
		memcpy(xm_log.name, file->f_path.dentry->d_name.name,
		       min(strlen(file->f_path.dentry->d_name.name),
			   255UL));
		xm_log.p_ino = parent_ino(file->f_path.dentry);

		struct xmfs_sb_index *xmsi;

		spin_lock(&sbi->space_lock);
		xmsi = get_xmsi(sbi);
		if (!xmsi) {
			spin_unlock(&sbi->space_lock);
			return -EINVAL;
		}
		int logcount = 1;

		if (logcount + xmsi->used_trunk_count > sbi->max_chunk) {
			sbi->full = true;
			spin_unlock(&sbi->space_lock);
			put_xmsi(sbi, xmsi);
			return -ENOSPC;
		}
		if (XMFS_I(inode)->die) {
			xmfs_info(sbi->sb,
				  "inode logs have been interrupted before close tmpfile");
			spin_unlock(&sbi->space_lock);
			put_xmsi(sbi, xmsi);
			return -EINVAL;
		}
		spin_unlock(&sbi->space_lock);
		put_xmsi(sbi, xmsi);
		xmlt_addr = xmfs_add_log_v3(&xm_log, sbi,
					    min(strlen(file->f_path.dentry->d_name.name),
						255UL),
					    inode->i_ino, true);
		if (xmlt_addr < 0) {
			XMFS_I(inode)->die = true;
			return -EINVAL;
		}
	}
	return 0;
}

long xmfs_common_fallocate(struct inode *inode, int mode, loff_t offset,
			   loff_t len)
{
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);

	if (xmfs_quiesce_wait(sbi))
		return -EINTR;
	/* per-FS log: data_size/data_start now in inode_info */
	while (XMFS_I(inode)->fallocate && XMFS_I(inode)->data_size != 0) {
		struct xmfs_log_trunk *xmlt_local =
			kmalloc(sizeof(struct xmfs_log_trunk), GFP_KERNEL);
		int err = get_log_trunk_and_copy(sbi, XMFS_I(inode)->log_start,
						 xmlt_local, false);
		if (unlikely(err)) {
			xmfs_error(sbi->sb,
				   "hardware memory error when reading xm log trunk during fallocate.");
			kfree(xmlt_local);
			break;
		}
		XMFS_I(inode)->fallocate = true;
		/* data_start from inode_info */
		/* data_size from inode_info */
		kfree(xmlt_local);
		return 0;
	}
	struct xmfs_sb_index *xmsi = get_xmsi(sbi);

	if (!xmsi)
		return 0;
	long first_slot = -1, slot;
	int ti;
	unsigned long num_trunks =
		(len + sbi->trunk_size - 1) / sbi->trunk_size;
	if (xmsi->used_trunk_count + num_trunks > sbi->max_chunk) {
		xmfs_info(sbi->sb, "fallocate more space %lu %lu than total",
			  (unsigned long)len, num_trunks);
		put_xmsi(sbi, xmsi);
		return -ENOSPC;
	}
	XMFS_I(inode)->fallocate = true;
	for (ti = 0; ti < (int)num_trunks; ti++) {
		slot = xmfs_alloc_and_mark_slot(xmsi, sbi, 'd');
		if (slot < 0) {
			put_xmsi(sbi, xmsi);
			return -ENOSPC;
		}
		if (first_slot < 0)
			first_slot = slot;
	}
	XMFS_I(inode)->data_start =
		DATA_AREA_OFFSET + first_slot * sbi->trunk_size;
	XMFS_I(inode)->data_size = len;
	XMFS_I(inode)->data_start = XMFS_I(inode)->data_start;
	XMFS_I(inode)->data_size = XMFS_I(inode)->data_size;
	xmsi->data_current = DATA_AREA_OFFSET + first_slot * sbi->trunk_size +
			     num_trunks * sbi->trunk_size;
	xmsi->data_count = 0;

	put_xmsi(sbi, xmsi);

	return 0;
}

static long xmfs_file_fallocate(struct file *file, int mode, loff_t offset,
				loff_t len)
{
	if ((mode & FALLOC_FL_KEEP_SIZE) == 0 || offset != 0)
		return -EOPNOTSUPP;

	struct inode *inode = file_inode(file);

	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);

	if (xmfs_quiesce_wait(sbi))
		return -EINTR;
	if (!sbi->debug)
		return -EOPNOTSUPP;

	spin_lock(&sbi->space_lock);
	long err = xmfs_common_fallocate(inode, mode, offset, len);

	spin_unlock(&sbi->space_lock);
	return err;
}

static const struct address_space_operations xmfs_aops = {
	.writepage	= xmfs_writepage,
	.write_begin	= xmfs_write_begin,
	.write_end	= xmfs_write_end,
	.dirty_folio	= filemap_dirty_folio,
	/* .free_folio		= xmfs_free_folio, */
};

static const struct file_operations xmfs_file_operations = {
	.release	= xmfs_release,
	.read_iter	= xmfs_file_read_iter,
	.write_iter	= xmfs_file_write_iter,
	.splice_read	= xmfs_file_splice_read,
	.splice_write	= iter_file_splice_write,
	.mmap		= xmfs_file_mmap,
	.fsync		= xmfs_fsync,
	.llseek		= xmfs_file_llseek,
	.fallocate	= xmfs_file_fallocate,
};

static const struct inode_operations xmfs_file_inode_operations = {
	.getattr	= xmfs_getattr,
	.setattr	= xmfs_setattr,
	.listxattr	= xmfs_listxattr,
	.update_time	= xmfs_update_time,
};

void xmfs_init_file_mapping(struct inode *inode)
{
	inode->i_mapping->a_ops = &xmfs_aops;
}

void xmfs_init_file_inode(struct inode *inode)
{
	inode->i_op = &xmfs_file_inode_operations;
	xmfs_init_file_mapping(inode);
	inode->i_fop = &xmfs_file_operations;
}
