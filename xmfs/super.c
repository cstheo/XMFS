// SPDX-License-Identifier: GPL-2.0-only
/*
 * XMFS: Log Structured Filesystem for UB
 * Copyright (C) 2026. Huawei Technologies Co., Ltd
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

#include "xmfs_i.h"

#include <linux/blkdev.h>
#include <linux/dax.h>
#include <linux/file.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
/* open CONFIG_ZONE_DEVICE */
#include <linux/memremap.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/ratelimit.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/swap.h>
#include <linux/uio.h>

#include "../../drivers/dax/dax-private.h"
#include "../../drivers/nvdimm/pmem.h"

#include <linux/cacheflush.h>
#include <asm/set_memory.h>
#include <asm/tlbflush.h>

static struct kmem_cache *xmfs_inode_cachep;

#define XMFS_RATELIMIT_INTERVAL (5 * HZ)
#define XMFS_RATELIMIT_BURST 10

void xmfs_init_log_ratelimits(struct xmfs_sb_info *sbi)
{
	ratelimit_state_init(&sbi->s_error_ratelimit_state,
			     XMFS_RATELIMIT_INTERVAL, XMFS_RATELIMIT_BURST);
	ratelimit_state_init(&sbi->s_warning_ratelimit_state,
			     XMFS_RATELIMIT_INTERVAL, XMFS_RATELIMIT_BURST);
	ratelimit_state_init(&sbi->s_msg_ratelimit_state,
			     XMFS_RATELIMIT_INTERVAL, XMFS_RATELIMIT_BURST);
	atomic_set(&sbi->s_error_count, 0);
	atomic_set(&sbi->s_warning_count, 0);
	atomic_set(&sbi->s_msg_count, 0);
}

static bool xmfs_ratelimit(struct super_block *sb,
			   struct ratelimit_state *state, atomic_t *count,
			   const char *name)
{
	if (!sb || !XMFS_SB(sb))
		return true;

	atomic_inc(count);
	return ___ratelimit(state, name);
}

void __xmfs_msg(struct super_block *sb, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;
	int level;

	if (sb && XMFS_SB(sb) &&
	    !xmfs_ratelimit(sb, &XMFS_SB(sb)->s_msg_ratelimit_state,
			    &XMFS_SB(sb)->s_msg_count, "XMFS-fs"))
		return;

	level = printk_get_level(fmt);

	va_start(args, fmt);
	vaf.fmt = printk_skip_level(fmt);
	vaf.va = &args;
	if (sb)
		printk("%c%cXMFS-fs (%s): %pV\n", KERN_SOH_ASCII, level, sb->s_id, &vaf);
	else
		printk("%c%cXMFS-fs: %pV\n", KERN_SOH_ASCII, level, &vaf);
	va_end(args);
}

void __xmfs_error(struct super_block *sb, const char *function,
		  unsigned int line, int error, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	if (sb && XMFS_SB(sb) &&
	    !xmfs_ratelimit(sb, &XMFS_SB(sb)->s_error_ratelimit_state,
			    &XMFS_SB(sb)->s_error_count, "XMFS-fs error"))
		return;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (sb) {
		if (error)
			printk(KERN_ERR "XMFS-fs error (%s): %s:%u: comm %s: error %d: %pV\n",
			       sb->s_id, function, line, current->comm, error, &vaf);
		else
			printk(KERN_ERR "XMFS-fs error (%s): %s:%u: comm %s: %pV\n",
			       sb->s_id, function, line, current->comm, &vaf);
	} else if (error) {
		printk(KERN_ERR "XMFS-fs error: %s:%u: comm %s: error %d: %pV\n",
		       function, line, current->comm, error, &vaf);
	} else {
		printk(KERN_ERR "XMFS-fs error: %s:%u: comm %s: %pV\n",
		       function, line, current->comm, &vaf);
	}
	va_end(args);
}

void __xmfs_error_inode(struct inode *inode, const char *function,
			unsigned int line, int error, const char *fmt, ...)
{
	struct super_block *sb = inode->i_sb;
	struct va_format vaf;
	va_list args;

	if (XMFS_SB(sb) &&
	    !xmfs_ratelimit(sb, &XMFS_SB(sb)->s_error_ratelimit_state,
			    &XMFS_SB(sb)->s_error_count, "XMFS-fs error"))
		return;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (error)
		printk(KERN_ERR "XMFS-fs error (%s): %s:%u: inode #%lu: comm %s: error %d: %pV\n",
		       sb->s_id, function, line, inode->i_ino, current->comm, error, &vaf);
	else
		printk(KERN_ERR "XMFS-fs error (%s): %s:%u: inode #%lu: comm %s: %pV\n",
		       sb->s_id, function, line, inode->i_ino, current->comm, &vaf);
	va_end(args);
}

void __xmfs_error_file(struct file *file, const char *function,
		       unsigned int line, int error, const char *fmt, ...)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	char pathname[80];
	struct va_format vaf;
	const char *path;
	va_list args;

	if (XMFS_SB(sb) &&
	    !xmfs_ratelimit(sb, &XMFS_SB(sb)->s_error_ratelimit_state,
			    &XMFS_SB(sb)->s_error_count, "XMFS-fs error"))
		return;

	path = file_path(file, pathname, sizeof(pathname));
	if (IS_ERR(path))
		path = "(unknown)";

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (error)
		printk(KERN_ERR "XMFS-fs error (%s): %s:%u: inode #%lu: comm %s: path %s: error %d: %pV\n",
		       sb->s_id, function, line, inode->i_ino, current->comm, path,
		       error, &vaf);
	else
		printk(KERN_ERR "XMFS-fs error (%s): %s:%u: inode #%lu: comm %s: path %s: %pV\n",
		       sb->s_id, function, line, inode->i_ino, current->comm, path, &vaf);
	va_end(args);
}

void __xmfs_warning(struct super_block *sb, const char *function,
		    unsigned int line, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	if (sb && XMFS_SB(sb) &&
	    !xmfs_ratelimit(sb, &XMFS_SB(sb)->s_warning_ratelimit_state,
			    &XMFS_SB(sb)->s_warning_count, "XMFS-fs warning"))
		return;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (sb)
		printk(KERN_WARNING "XMFS-fs warning (%s): %s:%u: %pV\n",
		       sb->s_id, function, line, &vaf);
	else
		printk(KERN_WARNING "XMFS-fs warning: %s:%u: %pV\n",
		       function, line, &vaf);
	va_end(args);
}

void __xmfs_warning_inode(const struct inode *inode, const char *function,
			  unsigned int line, const char *fmt, ...)
{
	struct super_block *sb = inode->i_sb;
	struct va_format vaf;
	va_list args;

	if (XMFS_SB(sb) &&
	    !xmfs_ratelimit(sb, &XMFS_SB(sb)->s_warning_ratelimit_state,
			    &XMFS_SB(sb)->s_warning_count, "XMFS-fs warning"))
		return;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	printk(KERN_WARNING "XMFS-fs warning (%s): %s:%u: inode #%lu: comm %s: %pV\n",
	       sb->s_id, function, line, inode->i_ino, current->comm, &vaf);
	va_end(args);
}

#if defined(__arm__) || defined(__aarch64__)
static int change_memory_cache(pte_t *ptep, unsigned long addr, void *data)
{
	pte_t pte = READ_ONCE(*ptep);
	pte_t newpte;
	bool cacheable = *(bool *)data;

	if (cacheable)
		pte_val(newpte) = (pte_val(pte) & ~PTE_ATTRINDX_MASK) |
				  PTE_ATTRINDX(MT_NORMAL);
	else
		pte_val(newpte) = (pte_val(pte) & ~PTE_ATTRINDX_MASK) |
				  PTE_ATTRINDX(MT_NORMAL_NC);

	if (pte_val(pte) == pte_val(newpte))
		return 0;

	pte_clear(current->mm, addr, ptep);
	flush_tlb_kernel_range(addr, addr + 4096);
	set_pte(ptep, newpte);

	return 0;
}

static void set_memory_noncacheable(unsigned long addr, size_t size)
{
	bool cache = false;
	int ret = apply_to_page_range(current->mm, addr, size,
				      change_memory_cache, &cache);
	if (ret)
		xmfs_error(NULL, "apply noncache for write err %d", ret);
}
#endif

static struct inode *xmfs_alloc_inode(struct super_block *sb)
{
	struct xmfs_inode_info *p;

	p = alloc_inode_sb(sb, xmfs_inode_cachep, GFP_KERNEL);
	if (unlikely(!p))
		return NULL;
	return &p->vfs_inode;
}

static void xmfs_destroy_inode(struct inode *inode)
{
	struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);

	if (xmfs_inode == NULL)
		return;

	xmfs_simple_xattrs_free(&xmfs_inode->xattrs, NULL);

	if (!(xmfs_inode->leaf))
		return;

	sbi->used_ino--;
	if (xmfs_inode->write_wq) {
		flush_workqueue(xmfs_inode->write_wq);
		destroy_workqueue(xmfs_inode->write_wq);
		xmfs_inode->write_wq = NULL;
	}
	/*
	 * if (sbi->id == 0) {
	 *	unsigned long _idx;
	 *	void *_entry;
	 *
	 *	xa_for_each(&xmfs_inode->shared_pages, _idx, _entry) {
	 *		xmfs_info(sbi->sb, "destroy inode mark dead %d", _idx);
	 *		xmfs_mark_page_dead(sbi, (unsigned long)_entry);
	 *	}
	 * }
	 */
	xa_destroy(&xmfs_inode->shared_pages);

	if (sbi->always_hot || sbi->freq == 0)
		return;

	unsigned long i;
	void *ptr;
	struct page_freq *page_freq;

	xa_for_each(&xmfs_inode->page_freq, i, ptr) {
		page_freq = (struct page_freq *)ptr;
		xa_erase(&xmfs_inode->page_freq, i);
		kfree(page_freq);
	}
	xa_destroy(&xmfs_inode->page_freq);
}

static void xmfs_free_inode(struct inode *inode)
{
	kmem_cache_free(xmfs_inode_cachep, XMFS_I(inode));
}

static int xmfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct xmfs_sb_info *sbi = XMFS_SB(dentry->d_sb);
	struct xmfs_sb_index *xmsi = get_xmsi(sbi);

	if (!xmsi) {
		xmfs_error(NULL,
			   "hardware memory error when reading superblock during statfs.");
		return 0;
	}
	sbi->stop_cnt++;
	if (xmsi->last_update != sbi->last_update) {
		if (!sbi->debug)
			xmfs_replay_new_entries(sbi, NULL, xmsi->last_update);
		else
			xmfs_replay_new_entries(sbi, NULL, xmsi->last_update);
	}
	sbi->stop_cnt--;

	buf->f_type = XMFS_MAGIC;
	buf->f_bsize = sbi->trunk_size;
	buf->f_namelen = NAME_MAX;
	buf->f_blocks = sbi->max_chunk + DATA_AREA_OFFSET / sbi->trunk_size + 1;
	buf->f_bfree = sbi->max_chunk - xmsi->used_trunk_count;
	buf->f_bavail = sbi->max_chunk - xmsi->used_trunk_count;
	if (xmsi->used_trunk_count > sbi->max_chunk) {
		buf->f_bfree = 0;
		buf->f_bavail = 0;
	}
	buf->f_files = sbi->max_chunk * (sbi->trunk_size / sbi->log_size);
	buf->f_ffree = sbi->max_chunk * (sbi->trunk_size / sbi->log_size) -
		       sbi->used_ino;
	if (sbi->id == 0)
		buf->f_fsid = uuid_to_fsid(dentry->d_sb->s_uuid.b);
	else
		buf->f_fsid = xmsi->fsid;
	put_xmsi(sbi, xmsi);
	return 0;
}

static int xmfs_show_options(struct seq_file *seq, struct dentry *root)
{
	struct xmfs_sb_info *sbi = XMFS_SB(root->d_sb);

#ifdef CONFIG_XMFS_FS_URMA
	if (!sbi->id)
		seq_printf(seq,
			   ",id=%d,log_size=%d,size=%ld,addr=%lld,mode=%d,urma_dev_name=%s,urma_va=%llu,urma_eid_subnet_prefix=%llu, urma_eid_interface_id=%llu,urma_token_id=%u,urma_jetty_id=%u",
			   sbi->id, sbi->log_size, sbi->len, sbi->start,
			   sbi->mode, sbi->dev_name,
			   sbi->urma_cfg.urma_remote_va,
			   sbi->urma_cfg.urma_eid_subnet_prefix,
			   sbi->urma_cfg.urma_eid_interface_id,
			   sbi->urma_cfg.urma_token_id,
			   sbi->urma_cfg.urma_jetty_id);
	else
		seq_printf(seq,
			   ",id=%d,log_size=%d,size=%ld,addr=%lld,mode=%d,urma_dev_name=%s,urma_va=%llu,urma_eid_subnet_prefix=%llu, urma_eid_interface_id=%llu,urma_token_id=%u,urma_jetty_id=%u",
			   sbi->id, sbi->log_size, sbi->len, sbi->start,
			   sbi->mode, sbi->dev_name,
			   sbi->urma_cfg.own_info.urma_remote_va,
			   sbi->urma_cfg.own_info.urma_eid_subnet_prefix,
			   sbi->urma_cfg.own_info.urma_eid_interface_id,
			   sbi->urma_cfg.own_info.urma_token_id,
			   sbi->urma_cfg.own_info.urma_jetty_id);
#else
	if (!sbi->id)
		seq_printf(seq, ",id=%d,log_size=%d,size=%ld,addr=%lld,mode=%d",
			   sbi->id, sbi->log_size, sbi->len, sbi->start,
			   sbi->mode);
	else
		seq_printf(seq, ",id=%d,size=%ld,addr=%lld,mode=%d", sbi->id,
			   sbi->len, sbi->start, sbi->mode);
#endif
	return 0;
}

static int xmfs_syncfs(struct super_block *sb, int wait)
{
	return 0;
}

static int xmfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	return 0;
}

static void xmfs_evict_inode(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

static const struct super_operations xmfs_ops = {
	.alloc_inode	= xmfs_alloc_inode,
	.free_inode	= xmfs_free_inode,
	.destroy_inode	= xmfs_destroy_inode,
	.statfs		= xmfs_statfs,
	.show_options	= xmfs_show_options,
	.sync_fs	= xmfs_syncfs,
	.write_inode	= xmfs_write_inode,
	.drop_inode	= generic_drop_inode,
	.evict_inode	= xmfs_evict_inode,
};

static int xmfs_find_index(struct xmfs_sb_info *sbi,
			   struct mount_msg_info *mm_info, bool mount_fn)
{
	int i;

	for (i = 1; i < 256; i++) {
		if (mount_fn && mm_info->msg[i] == 'T') {
			return i;
		} else if (!mount_fn && mm_info->msg[i] != 'D' &&
			   mm_info->msg[i] != 'T') {
			/* mm_info_real->msg[i] = 'T'; */
			xmfs_info(NULL,
				  "------> %s write index %d", __func__, i);
			mnt_msg_write(sbi, i, 'T');
			return i;
		}
	}
	/*
	 * if (mount_fn && mm_info->msg[i - 1] != 'D')
	 *	return -2;
	 */
	return -1;
}

static int xmfs_host_mount_fn(void *data)
{
	struct xmfs_sb_info *sbi = data;
	struct mount_msg_info *mm_info =
		kmalloc(sizeof(struct mount_msg_info), GFP_KERNEL);
	struct mount_msg_info *mm_info_real = get_mnt_msg(sbi);

	int id = 0;
	int new, i, index;
	int err;

	atomic_set(&mm_info_real->id, 0);
	while (!kthread_should_stop()) {
		err = copy_mc_to_kernel(mm_info, get_mnt_msg(sbi),
					sizeof(struct mount_msg_info));
		if (unlikely(err)) {
			xmfs_error(NULL,
				   "hardware memory error when host checking mount msgs");
			kfree(mm_info);
			return err;
		}
		if (mm_info_real->msg[0] == 'T')
			mm_info_real->msg[0] = 'D';

		new = atomic_read(&mm_info->id);
		if (new == id) {
			schedule_timeout_interruptible(msecs_to_jiffies(100));
			continue;
		}
		xmfs_info(NULL,
			  "-------------------->got work to do %c %c %c %d %d",
			  mm_info_real->msg[1], mm_info_real->msg[2],
			  mm_info_real->msg[3], id, new);
		/*
		 * print_hex_dump(KERN_DEBUG, "", DUMP_PREFIX_ADDRESS, 16, 1,
		 *		mm_info_real, sizeof(struct mount_msg_info), true);
		 */
		for (i = id + 1; i <= new; i++) {
			index = xmfs_find_index(sbi, mm_info, true);
			xmfs_info(NULL, "-------------------->index :%d",
				  index);
			if (index == -1) {
				xmfs_info(NULL,
					  "There is a race, which means there alreayd have 255 read nodes or there are more than one write nodes mounted parallely.");
				break;
			} else if (index == -2) {
				id++;
				break;
			}
			mm_info_real->msg[index] = 'D';
			id++;
			break;
		}

		while (index > 0 &&
		       (mm_info_real->urma_infos[index].urma_jetty_id == 0 ||
			mm_info_real->urma_infos[index].urma_remote_va == 0 ||
			mm_info_real->urma_infos[index].urma_token_id == 0 ||
			mm_info_real->send_infos[index].urma_jetty_id == 0 ||
			mm_info_real->send_infos[index].urma_send_token_id ==
				0 ||
			mm_info_real->send_infos[index].urma_recv_token_id ==
				0)) {
			schedule_timeout_interruptible(msecs_to_jiffies(10));
		}
		if (index > 0) {
			sbi->mount_slave++;
			xmfs_import_jetty(sbi, mm_info_real->urma_infos[index],
					  index, false);
			xmfs_import_jetty(sbi, mm_info_real->send_infos[index],
					  index, true);
		}
	}
	kfree(mm_info);

	return 0;
}

static int xmfs_slave_monitor_fn(void *data)
{
	struct xmfs_sb_info *sbi = data;
	int threshold = sbi->freq;

	xmfs_info(NULL, "threshold is %d", threshold);
	struct inode *inode;
	unsigned long index;
	struct page_freq *page_freq;
	int cnt;

	while (!kthread_should_stop()) {
		spin_lock(&sbi->sb->s_inode_list_lock);
		list_for_each_entry(inode, &sbi->sb->s_inodes, i_sb_list) {
			if (S_ISDIR(inode->i_mode))
				continue;

			xa_for_each(&(XMFS_I(inode)->page_freq), index,
				    page_freq) {
				if (page_freq->in_cache ||
				    page_freq->never_in_cache ||
				    page_freq->already_in_cache) {
					continue;
				}
				cnt = atomic_read(&page_freq->read_count);
				if (cnt >= threshold)
					page_freq->in_cache = true;
			}
		}
		spin_unlock(&sbi->sb->s_inode_list_lock);

		schedule_timeout_interruptible(msecs_to_jiffies(10000));
	}

	return 0;
}

static int xmfs_init_msg_queue(struct xmfs_sb_info *sbi)
{
	struct task_struct *thread;

	if (sbi->id == 0)
		thread = kthread_run(xmfs_host_mount_fn, sbi, "msg");
	else if (!sbi->always_hot && sbi->freq)
		thread = kthread_run(xmfs_slave_monitor_fn, sbi,
				     "slave_monitor");
	else
		return 0;

	if (IS_ERR(thread))
		return PTR_ERR(thread);
	sbi->thread = thread;

	return 0;
}

static int xmfs_blk_dax_notify_failure(struct dax_device *dax_devp, u64 offset,
				       u64 len, int mf_flags)
{
	xmfs_error(NULL, "%s: dax_devp %llx offset %llx len %lld mf_flags %x",
		   __func__, (u64)dax_devp, (u64)offset, (u64)len, mf_flags);
	return -EOPNOTSUPP;
}

static const struct dax_holder_operations xmfs_blk_dax_holder_ops = {
	.notify_failure		= xmfs_blk_dax_notify_failure,
};

static void xmfs_stop_threads(struct xmfs_sb_info *sbi)
{
	if (sbi->thread) {
		kthread_stop(sbi->thread);
		sbi->thread = NULL;
	}
	if (sbi->host_worker) {
		kthread_stop(sbi->host_worker);
		sbi->host_worker = NULL;
	}
	if (sbi->gc_thread) {
		kthread_stop(sbi->gc_thread);
		sbi->gc_thread = NULL;
	}
}

static void xmfs_destroy_workqueue(struct workqueue_struct **workqueue)
{
	if (!*workqueue)
		return;

	flush_workqueue(*workqueue);
	destroy_workqueue(*workqueue);
	*workqueue = NULL;
}

static void xmfs_release_backing_resources(struct xmfs_sb_info *sbi)
{
	if (sbi->dax_filp) {
		filp_close(sbi->dax_filp, NULL);
		sbi->dax_filp = NULL;
	}
	if (sbi->dax_dev) {
		fs_put_dax(sbi->dax_dev, sbi);
		sbi->dax_dev = NULL;
	}
	if (sbi->handlep) {
		bdev_release(sbi->handlep);
		sbi->handlep = NULL;
	}
}

static void xmfs_restore_anon_dev(struct super_block *sb,
				  struct xmfs_sb_info *sbi)
{
	if (sbi->dev)
		sb->s_dev = sbi->dev;
}

static int xmfs_ctx_validate(struct fs_context *fc)
{
	struct xmfs_fs_context *ctx = fc->fs_private;
	const char *source = fc->source;

	if (!source)
		return invalf(fc, "xmfs: source is required");
	if (!strstr(source, "/dev/pmem") &&
	    !strstr(source, "/dev/obmm") &&
	    !strstr(source, "/dev/loop"))
		return invalf(fc, "xmfs: unsupported source %s", source);

	if (ctx->mode < XMFS_SHARED_MEM_MODE ||
	    ctx->mode > XMFS_HYBRID_MODE)
		return invalf(fc, "xmfs: invalid mode %d", ctx->mode);
	if (ctx->host && ctx->mode != XMFS_SHARED_MEM_MODE) {
		errorf(fc, "xmfs: host requires shared-memory mode");
		return -EOPNOTSUPP;
	}
	if (!IS_ENABLED(CONFIG_XMFS_FS_URMA) &&
	    ctx->mode != XMFS_SHARED_MEM_MODE) {
		errorf(fc, "xmfs: mode %d requires URMA support", ctx->mode);
		return -EOPNOTSUPP;
	}
	return 0;
}

static int xmfs_prepare_slave_transport(struct xmfs_sb_info *sbi,
					struct xmfs_fs_context *ctx)
{
	int err;

	if (ctx->host || ctx->mode == XMFS_SHARED_MEM_MODE)
		return 0;

	err = xmfs_urma_register(sbi, ctx);
	if (err)
		goto unregister;

	err = xmfs_urma_import(sbi);
	if (!err)
		return 0;

	sbi->magic = 0;
	xmfs_urma_unimport(sbi);
unregister:
	xmfs_urma_unregister(sbi);
	return err;
}

static int xmfs_prepare_host_transport(struct xmfs_sb_info *sbi,
				       struct xmfs_fs_context *ctx)
{
	if (!ctx->host)
		return 0;

#ifdef CONFIG_XMFS_FS_URMA
	return xmfs_urma_register(sbi, ctx);
#else
	return 0;
#endif
}

static int xmfs_map_pmem_device(struct super_block *sb, struct fs_context *fc)
{
	struct xmfs_fs_context *ctx = fc->fs_private;
	struct xmfs_sb_info *sbi = XMFS_SB(sb);
	struct bdev_handle *handlep;
	struct dax_device *dax_dev;
	struct pmem_device *pmem;
	u64 start_off = 0;
	int err;

	handlep = bdev_open_by_path(fc->source, FMODE_READ | FMODE_WRITE,
				    sbi, NULL);
	if (!handlep || IS_ERR(handlep))
		return handlep ? PTR_ERR(handlep) : -ENODEV;
	if (!handlep->bdev || IS_ERR(handlep->bdev)) {
		xmfs_error(NULL, "%s: failed blkdev_get_by_path(%s)",
			   __func__, fc->source);
		err = handlep->bdev ? PTR_ERR(handlep->bdev) : -EINVAL;
		bdev_release(handlep);
		return err;
	}

	sbi->dev = sb->s_dev;
	sb->s_dev = handlep->bdev->bd_dev;
	dax_dev = fs_dax_get_by_bdev(handlep->bdev, &start_off,
				     sbi /* holder */,
				     &xmfs_blk_dax_holder_ops);
	if (IS_ERR(dax_dev) || !dax_dev) {
		xmfs_error(NULL,
			   "%s: unable to get pmem dax dev from handlep->bdev",
			   __func__);
		bdev_release(handlep);
		return -ENODEV;
	}

	pmem = dax_get_private(dax_dev);
	if (IS_ERR(pmem) || !pmem) {
		xmfs_error(NULL,
			   "%s: unable to get pmem dev from handlep->bdev",
			   __func__);
		fs_put_dax(dax_dev, sbi);
		bdev_release(handlep);
		return -ENODEV;
	}

	xmfs_info(NULL, "pmem %ld %lld", pmem->size, pmem->data_offset);
	if (!ctx->size || ctx->size > pmem->size - pmem->data_offset)
		ctx->size = pmem->size - pmem->data_offset;

	sbi->start = pmem->phys_addr + pmem->data_offset;
	sbi->len = ctx->size;
	sbi->kaddr = (void *)ioremap_cache(sbi->start, ctx->size);
	xmfs_info(NULL, "pmem %p", sbi->kaddr);
	/* sbi->dax_dev = dax_dev; */
	sbi->handlep = handlep;
	fs_put_dax(dax_dev, sbi);
	/* bdev_release(handlep); */

	return 0;
}

static int xmfs_probe_slave_backing(struct xmfs_sb_info *sbi,
				    struct xmfs_fs_context *ctx,
				    struct xmfs_sb_index *xmsi)
{
	int err;

	err = get_xmsi_and_copy(sbi, xmsi);
	if (unlikely(err))
		return err;
	if (xmsi->magic != XMFS_MAGIC) {
		xmfs_info(NULL, "no host mount yet");
		return -EINVAL;
	}
	if (xmsi->len != ctx->size) {
		xmfs_info(NULL,
			  "slave mount uses size %ld should use same size %ld as host",
			  ctx->size, xmsi->len);
		ctx->size = xmsi->len;
	}

	return 0;
}

static int xmfs_map_pmem_address(struct xmfs_sb_info *sbi,
				 struct xmfs_fs_context *ctx,
				 struct xmfs_sb_index *xmsi)
{
	int err;

	if (!ctx->host) {
		sbi->kaddr = (void *)ioremap_cache(ctx->addr, SB_SIZE);
		err = xmfs_probe_slave_backing(sbi, ctx, xmsi);
		if (err) {
			xmfs_error(NULL,
				   "hardware memory error when reading xmsi during slave mount 1.");
			return err;
		}
		iounmap(sbi->kaddr);
		sbi->kaddr = NULL;
	}

	sbi->kaddr = (void *)ioremap_cache(ctx->addr, ctx->size);
	sbi->start = ctx->addr;
	sbi->len = ctx->size;
	xmfs_info(NULL, "pmem addr %p", sbi->kaddr);

	return 0;
}

static int xmfs_map_obmm(struct xmfs_sb_info *sbi,
			 struct xmfs_fs_context *ctx,
			 struct xmfs_sb_index *xmsi)
{
	int err;

	if (!ctx->host) {
		sbi->kaddr = (void *)memremap(ctx->addr, SB_SIZE, MEMREMAP_WB);
		sbi->debug = ctx->debug;
		err = xmfs_probe_slave_backing(sbi, ctx, xmsi);
		if (err) {
			xmfs_error(NULL,
				   "hardware memory error when reading xmsi during slave mount 2.");
			return err;
		}
		iounmap(sbi->kaddr);
		sbi->kaddr = NULL;
	}

	sbi->kaddr = memremap(ctx->addr, ctx->size, MEMREMAP_WB);
	sbi->start = ctx->addr;
	sbi->len = ctx->size;
	xmfs_info(NULL, "obmm addr %p", sbi->kaddr);

	return 0;
}

static int xmfs_map_loop(struct xmfs_sb_info *sbi,
			 struct xmfs_fs_context *ctx,
			 struct xmfs_sb_index *xmsi)
{
	int err;

	if (ctx->host) {
		sbi->kaddr = vzalloc(ctx->size);
		xmfs_info(NULL, "mem addr %p", sbi->kaddr);
		sbi->len = ctx->size;
		return 0;
	}

	sbi->debug = ctx->debug;
	err = get_xmsi_and_copy(sbi, xmsi);
	print_hex_dump(KERN_DEBUG, "", DUMP_PREFIX_ADDRESS, 16, 1,
		       xmsi, sizeof(struct xmfs_sb_index), true);
	if (unlikely(err)) {
		xmfs_error(NULL,
			   "hardware memory error when reading xmsi during slave mount 1.");
		return err;
	}
	if (xmsi->magic != XMFS_MAGIC) {
		xmfs_info(NULL, "no host mount yet");
		return -EINVAL;
	}
	if (xmsi->len != ctx->size)
		ctx->size = xmsi->len;

	sbi->kaddr = sbi;
	return 0;
}

static int xmfs_map_backing(struct super_block *sb, struct fs_context *fc,
			    struct xmfs_sb_index *xmsi)
{
	struct xmfs_fs_context *ctx = fc->fs_private;
	struct xmfs_sb_info *sbi = XMFS_SB(sb);
	const char *source = fc->source;
	int err;

	if (!source)
		return -EINVAL;

	if (strstr(source, "/dev/pmem")) {
		if (ctx->addr)
			err = xmfs_map_pmem_address(sbi, ctx, xmsi);
		else
			err = xmfs_map_pmem_device(sb, fc);
	} else if (strstr(source, "/dev/obmm")) {
		err = xmfs_map_obmm(sbi, ctx, xmsi);
	} else if (strstr(source, "/dev/loop")) {
		err = xmfs_map_loop(sbi, ctx, xmsi);
	} else {
		xmfs_info(NULL, "other");
		return -EINVAL;
	}
	if (err)
		return err;

	if (IS_ERR_OR_NULL(sbi->kaddr)) {
		xmfs_error(NULL, "map error %ld", PTR_ERR(sbi->kaddr));
		return sbi->kaddr ? PTR_ERR(sbi->kaddr) : -EINVAL;
	}

	if (ctx->host)
		print_hex_dump(KERN_DEBUG, "", DUMP_PREFIX_ADDRESS, 16, 1,
			       sbi->kaddr, 512, true);

	return 0;
}

static int xmfs_prepare_host_mount(struct xmfs_sb_info *sbi,
				   struct xmfs_fs_context *ctx)
{
	struct mount_msg_info *mm_info_real = get_mnt_msg(sbi);
	struct mount_msg_info *mm_info = kmalloc(sizeof(*mm_info), GFP_KERNEL);
	int loop_count = 0;
	int old, tmp;
	int err;

host_get_id:
	err = copy_mc_to_kernel(mm_info, get_mnt_msg(sbi), sizeof(*mm_info));
	if (unlikely(err)) {
		xmfs_error(NULL,
			   "hardware memory error when reading superblock at beginning of mounting host and getting id");
		goto free_msg;
	}

	old = atomic_read(&mm_info->host_mount);
	if (old < 0 || old > 1) {
		kfree(mm_info);
		goto host_continue;
	} else if (old == 1) {
		schedule_timeout_interruptible(msecs_to_jiffies(10));
		goto host_get_id;
	}

	tmp = atomic_cmpxchg(&mm_info_real->host_mount, old, 1);
	if (tmp == old) {
		mm_info_real->msg[0] = 'T';
	} else {
		xmfs_info(NULL,
			  "exchange failed, someone else mounting parallel, skip this mount");
		err = -EINVAL;
		goto free_msg;
	}

	do {
		err = copy_mc_to_kernel(mm_info, get_mnt_msg(sbi),
					sizeof(*mm_info));
		if (unlikely(err)) {
			xmfs_error(NULL,
				   "hardware memory error when reading superblock at beginning of mounting");
			goto free_msg;
		}
		loop_count++;
		schedule_timeout_interruptible(msecs_to_jiffies(10));
	} while (mm_info->msg[0] != 'D' && loop_count <= 10);

	if (mm_info->msg[0] == 'D') {
		mm_info_real->msg[0] = 'C';
		tmp = atomic_cmpxchg(&mm_info_real->host_mount, 1, old);
		xmfs_info(NULL,
			  "host msg returns meaning host already mounted %d %d",
			  tmp, old);
		err = -EINVAL;
		goto free_msg;
	}

	tmp = atomic_cmpxchg(&mm_info_real->host_mount, 1, old);
	if (tmp != 1)
		xmfs_info(NULL,
			  "when checking mounting host done, there is a bug when exchange host mount value to %d",
			  old);
	kfree(mm_info);

	if (ctx->replay) {
		sbi->id = 0;
		/*
		 * memset(get_mnt_msg(sbi), 0,
		 *	  sizeof(struct mount_msg_info));
		 */
		return 0;
	}

host_continue:
	memset(sbi->kaddr, 0, DATA_AREA_OFFSET);
	sbi->id = 0;
	memcpy(sbi->kaddr + MOUNT_MSG_OFFSET +
		       offsetof(struct mount_msg_info, send_infos[0]),
	       &sbi->urma_cfg.send_info, sizeof(struct urma_info));
#ifdef CONFIG_X86
	xmfs_info(NULL, "x86 mount");
	/*
	 * err = set_memory_uc((unsigned long)sbi->kaddr,
	 *		     DATA_AREA_OFFSET >> PAGE_SHIFT);
	 */
	/* xmfs_info(sbi->sb, "qyf set memory return err %d", err); */
#elif defined(CONFIG_ARM) || defined(CONFIG_ARM64)
	xmfs_info(NULL, "arm mount host");
	/*
	 * set_memory_noncacheable((unsigned long)sbi->kaddr,
	 *			 DATA_AREA_OFFSET);
	 */
#endif
	return 0;

free_msg:
	kfree(mm_info);
	return err;
}

static int xmfs_prepare_slave_mount(struct xmfs_sb_info *sbi,
				    struct xmfs_fs_context *ctx,
				    struct xmfs_sb_index *xmsi)
{
	struct mount_msg_info *mm_info = kmalloc(sizeof(*mm_info), GFP_KERNEL);
	int loop_count = 0;
	int old, new;
	int index;
	int err;

	err = get_mnt_msg_and_copy(sbi, mm_info);
	if (unlikely(err)) {
		xmfs_error(NULL,
			   "hardware memory error when reading superblock at beginning of mounting slave");
		goto free_msg;
	}

slave_get_id:
	old = xmfs_atomic_read(sbi, mm_info);
	new = old + 1;
	xmfs_info(NULL, "------> cas target %d", new);
	if (xmfs_cas(sbi, old, new) == old) {
		index = xmfs_find_index(sbi, mm_info, false);
		xmfs_info(NULL, "------> slave index %d", index);
		if (index == -1) {
			xmfs_info(NULL,
				  "Already have 255 mount nodes, cannot mount more");
			err = -EINVAL;
			goto free_msg;
		}
		sbi->id = index;
		xmfs_info(NULL, "------> slave id is %d", index);
	} else {
		err = get_mnt_msg_and_copy(sbi, mm_info);
		if (unlikely(err)) {
			xmfs_error(NULL,
				   "hardware memory error when reading superblock during mount slave");
			goto free_msg;
		}
		goto slave_get_id;
	}

	while (mm_info->msg[sbi->id] != 'D' && loop_count < 10) {
		loop_count++;
		schedule_timeout_interruptible(msecs_to_jiffies(1000));
		err = get_mnt_msg_and_copy(sbi, mm_info);
		if (unlikely(err)) {
			xmfs_error(NULL,
				   "hardware memory error when reading superblock during mount slave %d",
				   index);
			goto free_msg;
		}
	}
	if (loop_count == 10) {
		xmfs_info(NULL, "try 10 times to check msg mount but failed");
		err = -EINVAL;
		goto free_msg;
	}

	err = get_xmsi_and_copy(sbi, xmsi);
	if (unlikely(err)) {
		xmfs_error(NULL,
			   "hardware memory error when reading xmsi during slave mount.");
		goto free_msg;
	}
	if (xmsi->magic != XMFS_MAGIC || xmsi->len != ctx->size) {
		xmfs_info(NULL,
			  "using the wrong size %ld to mount slave, should use %ld",
			  ctx->size, xmsi->len);
		err = -EINVAL;
		goto free_msg;
	}

	xmfs_info(NULL, "mount slave id %d", sbi->id);
	sbi->log_start = xmsi->log_start;
	sbi->last_replayed_slot = (unsigned long)-1;
	get_mnt_msg_and_copy(sbi, mm_info);
	xmfs_info(NULL, "host send info %lld %lld %lld %lld %d %d %d",
		  mm_info->send_infos[0].urma_send_va,
		  mm_info->send_infos[0].urma_recv_va,
		  mm_info->send_infos[0].urma_eid_subnet_prefix,
		  mm_info->send_infos[0].urma_eid_interface_id,
		  mm_info->send_infos[0].urma_jetty_id,
		  mm_info->send_infos[0].urma_send_token_id,
		  mm_info->send_infos[0].urma_recv_token_id);
	xmfs_import_jetty(sbi, mm_info->send_infos[0], 0, true);
	xmfs_pmem_write(sbi,
			MOUNT_MSG_OFFSET +
				offsetof(struct mount_msg_info, urma_infos[index]),
			&sbi->urma_cfg.own_info, sizeof(struct urma_info));
	xmfs_pmem_write(sbi,
			MOUNT_MSG_OFFSET +
				offsetof(struct mount_msg_info, send_infos[index]),
			&sbi->urma_cfg.send_info, sizeof(struct urma_info));

free_msg:
	kfree(mm_info);
	return err;
}

static void xmfs_init_runtime_state(struct xmfs_sb_info *sbi,
				    const struct xmfs_fs_context *ctx)
{
	sbi->last_update = 0;
	sbi->last_replayed_slot = (unsigned long)-1;
	sbi->debug = ctx->debug;
	sbi->fallocate_size = ctx->fallocate_size;
	spin_lock_init(&sbi->space_lock);
	init_rwsem(&sbi->chain_rwsem);
	spin_lock_init(&sbi->version_lock);
	mutex_init(&sbi->replay_lock);
	spin_lock_init(&sbi->local_ring_lock);
	xa_init(&sbi->pinned_inodes);
#ifdef CONFIG_XMFS_FS_URMA
	memcpy(sbi->dev_name, ctx->dev_name, sizeof(ctx->dev_name));
#endif
	sbi->host_worker = NULL;
	sbi->max_slave_id = 0;

	spin_lock_init(&sbi->gc_ctl.gc_lock);
	init_waitqueue_head(&sbi->gc_ctl.wait);
	sbi->gc_ctl.gc_running = false;
	sbi->gc_ctl.gc_batch = XMFS_GC_BATCH;
	sbi->gc_ctl.gc_cursor = 0;
	sbi->gc_interval = XMFS_GC_INTERVAL_DEFAULT;
	sbi->mount_slave = 0;
}

static void xmfs_init_log_geometry(struct xmfs_sb_info *sbi, int log_size)
{
	if (!log_size) {
		sbi->log_size = XMFS_DEFAULT_CHUNK_SIZE;
		sbi->trunk_size = XMFS_DEFAULT_CHUNK_SIZE;
	} else if (log_size > XMFS_DEFAULT_CHUNK_SIZE) {
		sbi->trunk_size = log_size;
		sbi->log_size = log_size;
	} else {
		sbi->trunk_size = XMFS_DEFAULT_CHUNK_SIZE;
		sbi->log_size = log_size;
	}
}

static int xmfs_configure_super(struct super_block *sb,
				struct xmfs_fs_context *ctx,
				struct xmfs_sb_index *xmsi)
{
	struct xmfs_sb_info *sbi = XMFS_SB(sb);
	int err;

	xmfs_init_runtime_state(sbi, ctx);
	if (ctx->host)
		xmfs_init_log_geometry(sbi, ctx->log_size);

	err = get_xmsi_and_copy(sbi, xmsi);
	if (unlikely(err)) {
		xmfs_error(NULL,
			   "hardware memory error when reading xmsi during host mount 2.");
		return err;
	}

	if (ctx->host && ctx->replay && xmsi->magic == XMFS_MAGIC) {
		xmfs_info(NULL, "replay");
		if (sbi->log_size != xmsi->log_size || ctx->size != xmsi->len) {
			memset(sbi->kaddr, 0, DATA_AREA_OFFSET);
			sbi->id = 0;
			ctx->replay = false;
		}
	}
	if (ctx->host && ctx->replay && xmsi->magic != XMFS_MAGIC) {
		memset(sbi->kaddr, 0, DATA_AREA_OFFSET);
		sbi->id = 0;
		ctx->replay = false;
	}

	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_magic = XMFS_MAGIC;
	sb->s_op = &xmfs_ops;
	xmfs_init_dentry_operations(sb);
	sb->s_time_gran = 1;
	if (sbi->id == 0)
		uuid_gen(&sb->s_uuid);
	if (ctx->host) {
		sb->s_xattr = xmfs_host_xattr_handlers;
	} else {
		sb->s_xattr = xmfs_slave_xattr_handlers;
		sbi->log_size = xmsi->log_size;
		ctx->log_size = sbi->log_size;
		xmfs_init_log_geometry(sbi, ctx->log_size);
	}

	sbi->max_chunk =
		(ctx->size - DATA_AREA_OFFSET) / sbi->trunk_size - 1;
	xmfs_info(NULL, "max chunk %ld %ld %ld %ld", sbi->max_chunk,
		  sizeof(struct xmfs_log_trunk), sizeof(struct mount_msg_info),
		  DATA_AREA_OFFSET);

	return 0;
}

static int xmfs_create_root(struct super_block *sb)
{
	struct xmfs_sb_info *sbi = XMFS_SB(sb);

	sb->s_root = d_make_root(xmfs_get_root(sb, sbi->id));
	if (!sb->s_root)
		return -ENOMEM;

	return 0;
}

static int xmfs_alloc_workqueues(struct xmfs_sb_info *sbi)
{
	sbi->prefetch_wq = alloc_workqueue("xmfs-prefetch",
					   WQ_UNBOUND | WQ_MEM_RECLAIM, 16);
	if (!sbi->prefetch_wq)
		return -ENOMEM;

	sbi->send_wq = alloc_workqueue("xmfs-send-req",
				       WQ_UNBOUND | WQ_MEM_RECLAIM, 128);
	if (!sbi->send_wq)
		return -ENOMEM;

	return 0;
}

static int xmfs_start_workers(struct xmfs_sb_info *sbi)
{
	struct xmfs_inode_table *tbl;
	int err;

	err = xmfs_init_msg_queue(sbi);
	if (sbi->id == 0) {
		sbi->gc_thread = kthread_run(xmfs_gc_thread_fn, sbi, "xmfs_gc");
		if (IS_ERR(sbi->gc_thread))
			sbi->gc_thread = NULL;
		sbi->max_slave_id = 63;

		tbl = (struct xmfs_inode_table *)get_meta(sbi,
						     INODE_TABLE_OFFSET,
						     INODE_TABLE_SIZE);
		tbl->magic = XMFS_INODE_TABLE_MAGIC;
	} else {
		sbi->gc_thread = kthread_run(xmfs_gc_thread_slave_fn, sbi,
					     "xmfs_slave_gc");
		if (IS_ERR(sbi->gc_thread))
			sbi->gc_thread = NULL;
	}

	return err;
}

static int xmfs_finalize_host_mount(struct super_block *sb,
				    struct xmfs_fs_context *ctx,
				    struct xmfs_sb_index *xmsi)
{
	struct xmfs_sb_info *sbi = XMFS_SB(sb);
	struct xmfs_sb_index *xmsi_real;

	if (ctx->replay && xmsi->magic == XMFS_MAGIC) {
		xmfs_info(NULL, "replay");
		if (sbi->log_size != xmsi->log_size) {
			xmfs_info(NULL,
				  "mount host again with wrong log size %d should be %d",
				  sbi->log_size, xmsi->log_size);
			return -EINVAL;
		}
		if (ctx->size != xmsi->len) {
			xmfs_info(NULL,
				  "mount host again with wrong size %ld should be %ld",
				  ctx->size, xmsi->len);
			return -EINVAL;
		}
		sbi->during_replay = true;
		xmfs_replay_new_entries(sbi, NULL, -1);
		sbi->during_replay = false;
	}

	if (ctx->replay) {
		struct xmfs_sb_index *xmsi_r = get_xmsi(sbi);

		if (xmsi_r) {
			unsigned long used_trunks = 0;
			unsigned long i;

			for (i = 0;
			     i < xmsi_r->count && i < XMFS_BITMAP_CAPACITY;
			     i++) {
				if (xmsi_r->bitmap[i] != 'u')
					used_trunks++;
			}
			xmsi_r->used_trunk_count = used_trunks;
			put_xmsi(sbi, xmsi_r);
		}
	}

	xmsi_real = get_xmsi(sbi);
	xmsi_real->magic = XMFS_MAGIC;
	xmsi_real->log_size = sbi->log_size;
	xmsi_real->len = ctx->size;
	xmsi_real->fsid = uuid_to_fsid(sb->s_uuid.b);
	if (!ctx->replay) {
		memset(xmsi_real->bitmap, 'u', XMFS_BITMAP_CAPACITY);
		xmsi_real->bitmap_size = 0;
		xmsi_real->used_trunk_count = 0;
	}

	/* xmfs_populate_from_inode_table(sbi); */
	if (!ctx->replay && XMFS_I(sb->s_root->d_inode)->log_start == 0) {
		struct xmfs_log xm_log;
		unsigned long xmlt_addr;

		xm_log.ops = XMFS_ROOT;
		xmfs_info(NULL, "root 1 %p", XMFS_I(sb->s_root->d_inode));
		xmlt_addr = xmfs_add_log_v3(&xm_log, sbi, 0,
					    sb->s_root->d_inode->i_ino, true);
		if (xmlt_addr < 0)
			return -EINVAL;
		sbi->last_update = 0;
		xmfs_info(NULL, "log start %ld",
			  XMFS_I(sb->s_root->d_inode)->log_start);
	}
	/*
	 * sbi->xmfs_log_wq = alloc_ordered_workqueue("xmfs_log_wq",
	 *					       WQ_MEM_RECLAIM);
	 */

	return 0;
}

static int xmfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct xmfs_fs_context *ctx = fc->fs_private;
	struct xmfs_sb_index *xmsi;
	struct xmfs_sb_info *sbi;
	int err;

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;
	xmfs_init_log_ratelimits(sbi);
	sbi->mode = ctx->mode;
	sbi->magic = XMFS_MAGIC;
	xmsi = kmalloc(sizeof(*xmsi), GFP_KERNEL);

	err = xmfs_prepare_slave_transport(sbi, ctx);
	if (err)
		goto free_sbi;

	err = xmfs_map_backing(sb, fc, xmsi);
	if (err)
		goto free_kaddr;

	err = xmfs_prepare_host_transport(sbi, ctx);
	if (err)
		goto release_dev;

	if (ctx->host)
		err = xmfs_prepare_host_mount(sbi, ctx);
	else
		err = xmfs_prepare_slave_mount(sbi, ctx, xmsi);
	if (err)
		goto release_dev;

	err = xmfs_configure_super(sb, ctx, xmsi);
	if (err)
		goto stop_thread;

	err = xmfs_create_root(sb);
	if (err)
		goto release_dev;

	err = xmfs_alloc_workqueues(sbi);
	if (err)
		goto release_root;

	err = security_inode_init_security(sb->s_root->d_inode,
					   sb->s_root->d_parent->d_inode,
					   &sb->s_root->d_name, xmfs_initxattrs,
					   NULL);
	if (err)
		goto release_root;

	sbi->sb = sb;
	sbi->used_ino = 0;
	sbi->freq = ctx->freq;
	sbi->always_hot = ctx->always_hot;
	err = xmfs_start_workers(sbi);
	if (err)
		goto stop_thread;

	if (ctx->host) {
		err = super_setup_bdi(sb);
		if (err) {
			xmfs_info(NULL, "setup bdi failed");
			goto stop_thread;
		}

		err = xmfs_finalize_host_mount(sb, ctx, xmsi);
		if (err)
			goto stop_thread;
	}

	kfree(xmsi);
	if (ctx->host)
		print_hex_dump(KERN_DEBUG, "", DUMP_PREFIX_ADDRESS, 16, 1,
			       sbi->kaddr, 400, true);
	return 0;

stop_thread:
	xmfs_stop_threads(sbi);
release_root:
	dput(sb->s_root);
	sb->s_root = NULL;
	xmfs_destroy_workqueue(&sbi->prefetch_wq);
	xmfs_destroy_workqueue(&sbi->send_wq);
release_dev:
	xmfs_release_backing_resources(sbi);
free_kaddr:
	if (ctx->host)
		vfree(sbi->kaddr);
	sbi->magic = 0;
	xmfs_urma_unimport(sbi);
	xmfs_urma_unregister(sbi);
free_sbi:
	xmfs_restore_anon_dev(sb, sbi);
	if (sb->s_fs_info == sbi)
		sb->s_fs_info = NULL;
	kfree(sbi);
	kfree(xmsi);
	return err;
}

static int xmfs_get_tree(struct fs_context *fc)
{
	int err;

	err = xmfs_ctx_validate(fc);
	if (err)
		return err;

	return get_tree_nodev(fc, xmfs_fill_super);
}

static const struct fs_parameter_spec xmfs_param_specs[] = {
	fsparam_bool("host", Opt_host),
	fsparam_bool("replay", Opt_replay),
	fsparam_bool("debug", Opt_debug),
	fsparam_bool("hot", Opt_always_hot),
	fsparam_u64("addr", Opt_addr),
	fsparam_u64("size", Opt_size),
	fsparam_u64("fallocate", Opt_fallocate),
	fsparam_s32("freq", Opt_freq),
	fsparam_s32("log", Opt_log),
	fsparam_s32("mode", Opt_mode),
#ifdef CONFIG_XMFS_FS_URMA
	fsparam_string("urma_dev_name", Opt_urma_dev),
	fsparam_u64("urma_va", Opt_urma_va),
	fsparam_u64("urma_eid_subnet_prefix", Opt_urma_eid_subnet_prefix),
	fsparam_u64("urma_eid_interface_id", Opt_urma_eid_interface_id),
	fsparam_u32("urma_jetty_id", Opt_urma_jetty_id),
	fsparam_u32("urma_token_id", Opt_urma_token_id),
#endif
	{}
};

static int xmfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct xmfs_fs_context *ctx = fc->fs_private;
	struct fs_parse_result result;
	int opt;

	opt = fs_parse(fc, xmfs_param_specs, param, &result);
	if (opt < 0)
		return opt;

	switch (opt) {
	case Opt_host:
		ctx->host = result.boolean;
		break;
	case Opt_replay:
		ctx->replay = result.boolean;
		break;
	case Opt_addr:
		ctx->addr = result.uint_64;
		break;
	case Opt_size:
		ctx->size = result.uint_64;
		break;
	case Opt_freq:
		ctx->freq = result.int_32;
		break;
	case Opt_log:
		ctx->log_size = result.int_32;
		break;
	case Opt_debug:
		ctx->debug = result.boolean;
		break;
	case Opt_always_hot:
		ctx->always_hot = result.boolean;
		break;
	case Opt_fallocate:
		ctx->fallocate_size = result.uint_64;
		break;
	case Opt_mode:
		ctx->mode = result.int_32;
		break;
#ifdef CONFIG_XMFS_FS_URMA
	case Opt_urma_va:
		ctx->urma_va = result.uint_64;
		break;
	case Opt_urma_eid_subnet_prefix:
		ctx->urma_eid_subnet_prefix = result.uint_64;
		break;
	case Opt_urma_eid_interface_id:
		ctx->urma_eid_interface_id = result.uint_64;
		break;
	case Opt_urma_jetty_id:
		ctx->urma_jetty_id = result.uint_32;
		break;
	case Opt_urma_token_id:
		ctx->urma_token_id = result.uint_32;
		break;
	case Opt_urma_dev:
		memcpy(ctx->dev_name, param->string, strlen(param->string));
		break;
#endif
	default:
		return -ENOPARAM;
	}

	return 0;
}

static void xmfs_fs_context_free(struct fs_context *fc)
{
	kfree(fc->fs_private);
}

static const struct fs_context_operations xmfs_fs_context_ops = {
	.get_tree	= xmfs_get_tree,
	.parse_param	= xmfs_parse_param,
	.free		= xmfs_fs_context_free,
};

static int xmfs_init_fs_context(struct fs_context *fc)
{
	struct xmfs_fs_context *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	fc->fs_private = ctx;
	fc->ops = &xmfs_fs_context_ops;

	return 0;
}

static void xmfs_unpin_all_inodes(struct xmfs_sb_info *sbi)
{
	struct inode *inode;
	unsigned long index;

	/* Each xarray entry retains one inode reference until it is erased here. */
	xa_for_each(&sbi->pinned_inodes, index, inode) {
		inode = xa_erase(&sbi->pinned_inodes, index);
		if (!inode)
			continue;

		iput(inode);
		cond_resched();
	}

	xa_destroy(&sbi->pinned_inodes);
}

static void xmfs_shutdown_sb(struct super_block *sb,
			     struct xmfs_sb_info *sbi)
{
	mnt_msg_write(sbi, sbi->id, 'C');
	sbi->magic = 0;

	xmfs_stop_threads(sbi);
	xmfs_destroy_workqueue(&sbi->prefetch_wq);
	xmfs_destroy_workqueue(&sbi->send_wq);

	xmfs_restore_anon_dev(sb, sbi);
	xmfs_unpin_all_inodes(sbi);
}

static void xmfs_release_sb_info(struct xmfs_sb_info *sbi)
{
	xmfs_release_backing_resources(sbi);
	if (sbi->kaddr != sbi)
		vfree(sbi->kaddr);
	sbi->kaddr = NULL;
	xmfs_urma_unimport(sbi);
	xmfs_urma_unregister(sbi);
	kfree(sbi);
}

static void xmfs_kill_sb(struct super_block *sb)
{
	struct xmfs_sb_info *sbi = XMFS_SB(sb);

	if (sbi)
		xmfs_shutdown_sb(sb, sbi);

	kill_litter_super(sb);

	if (sbi)
		xmfs_release_sb_info(sbi);
}

static struct file_system_type xmfs_fs_type = {
	.name			= "xmfs",
	.owner			= THIS_MODULE,
	.init_fs_context	= xmfs_init_fs_context,
	.parameters		= xmfs_param_specs,
	.kill_sb		= xmfs_kill_sb,
};

static void xmfs_init_once(void *data)
{
	struct xmfs_inode_info *xmfs_inode = data;

	inode_init_once(&xmfs_inode->vfs_inode);
}

static int __init init_xmfs(void)
{
	int err;

	xmfs_inode_cachep = kmem_cache_create("xmfs_inode_cache",
					      sizeof(struct xmfs_inode_info), 0,
					      SLAB_ACCOUNT, xmfs_init_once);
	if (!xmfs_inode_cachep)
		return -ENOMEM;

	err = register_filesystem(&xmfs_fs_type);
	if (err)
		kmem_cache_destroy(xmfs_inode_cachep);

	return err;
}

static void __exit exit_xmfs(void)
{
	unregister_filesystem(&xmfs_fs_type);
	kmem_cache_destroy(xmfs_inode_cachep);
}

module_init(init_xmfs);
module_exit(exit_xmfs);

MODULE_AUTHOR("Yifan Qiao <qiaoyifan4@huawei.com>");
MODULE_DESCRIPTION("Log Structured Filesystem for UB");
MODULE_LICENSE("GPL");
