// SPDX-License-Identifier: GPL-2.0-only
/*
 * XMFS: Log Structured Filesystem for UB
 * Copyright (C) 2024. Huawei Technologies Co., Ltd
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "xmfs_i.h"

#include <linux/kernel.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/slab.h>

static struct inode *xmfs_get_inode_from_table(struct xmfs_sb_info *sbi,
					       unsigned long ino)
{
	struct xmfs_inode_table *tbl;
	struct xmfs_inode_entry *e;
	struct inode *inode;
	unsigned long max_entries, i;

	inode = ilookup5(sbi->sb, ino, xmfs_inode_eq, &ino);
	if (inode)
		return inode;

	if (sbi->mode == XMFS_SHARED_MEM_MODE || sbi->mode == XMFS_HYBRID_MODE)
		tbl = (struct xmfs_inode_table *)(sbi->kaddr +
						  INODE_TABLE_OFFSET);
	else {
		tbl = (struct xmfs_inode_table *)get_meta(sbi,
							  INODE_TABLE_OFFSET,
							  INODE_TABLE_SIZE);
		/*
		 * tbl = (struct xmfs_inode_table *)get_meta(
		 *	sbi, INODE_TABLE_OFFSET,
		 *	max(sizeof(struct xmfs_inode_table) +
		 *	    tbl->count * sizeof(struct xmfs_inode_entry),
		 *	    INODE_TABLE_SIZE));
		 */
	}

	if (!tbl || tbl->magic != XMFS_INODE_TABLE_MAGIC) {
		if (tbl && sbi->mode == XMFS_URMA_MODE)
			put_meta(sbi, tbl);
		return NULL;
	}

	max_entries =
		(INODE_TABLE_SIZE - 4096) / sizeof(struct xmfs_inode_entry);
	e = NULL;
	for (i = 0; i < tbl->count && i < max_entries; i++) {
		if (tbl->entries[i].i_ino == ino) {
			e = &tbl->entries[i];
			break;
		}
	}

	if (!e) {
		if (sbi->mode == XMFS_URMA_MODE)
			put_meta(sbi, tbl);
		return NULL;
	}

	inode = iget5_locked(sbi->sb, ino, xmfs_inode_eq, xmfs_inode_set, &ino);
	if (!inode) {
		if (sbi->mode == XMFS_URMA_MODE)
			put_meta(sbi, tbl);
		return NULL;
	}

	if (inode->i_state & I_NEW) {
		struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);

		xmfs_inode->leaf = (e->i_ino != 0);
		inode->i_ino = e->i_ino;
		inode->i_mode = e->mode;
		set_nlink(inode, e->nlink);
		i_size_write(inode, e->size);
		inode->__i_ctime.tv_sec = e->ctime.tv_sec;
		inode->__i_ctime.tv_nsec = e->ctime.tv_nsec;
		inode->i_mtime.tv_sec = e->mtime.tv_sec;
		inode->i_mtime.tv_nsec = e->mtime.tv_nsec;
		inode->i_atime.tv_sec = e->atime.tv_sec;
		inode->i_atime.tv_nsec = e->atime.tv_nsec;
		if (S_ISREG(e->mode)) {
			/*
			 * xmfs_inode->write_wq = alloc_workqueue(
			 *	"xmfs-prefetch-inode",
			 *	WQ_UNBOUND | WQ_MEM_RECLAIM, 8);
			 */
			xmfs_init_file_inode(inode);
		} else if (S_ISDIR(e->mode)) {
			xmfs_init_dir_inode(inode, sbi->id == 0);
		} else if (S_ISLNK(e->mode)) {
			xmfs_init_symlink_operations(inode);
		} else {
			init_special_inode(inode, e->mode, 0);
		}
		unlock_new_inode(inode);
		/* iput(inode); */
		ihold(inode);
		xa_store(&sbi->pinned_inodes, inode->i_ino, inode, GFP_KERNEL);
		XMFS_I(inode)->pinned = true;
	}

	if (sbi->mode == XMFS_URMA_MODE)
		put_meta(sbi, tbl);
	return inode;
}

static int xmfs_replay_link(struct inode *dir, struct dentry *pdentry,
			    struct xmfs_log log, unsigned long ino)
{
	struct xmfs_sb_info *sbi = XMFS_SB(dir->i_sb);
	struct inode *inode;

	if (log.writer_id == sbi->id)
		return 0;

	inode = ilookup5(dir->i_sb, ino, xmfs_inode_eq, &ino);
	if (inode) {
		inc_nlink(inode);
		iput(inode);
	}

	return 0;
}

static int xmfs_replay_mkdir(struct inode *dir, struct dentry *pdentry,
			     struct xmfs_log log, unsigned long replay,
			     unsigned long ino)
{
	struct xmfs_sb_info *sbi = XMFS_SB(dir->i_sb);

	struct inode *inode;

	if (log.writer_id == sbi->id)
		return 0;

	inode = ilookup5(dir->i_sb, ino, xmfs_inode_eq, &ino);
	if (!inode) {
		inode = iget5_locked(dir->i_sb, ino, xmfs_inode_eq,
				     xmfs_inode_set, &ino);
		if (inode && (inode->i_state & I_NEW)) {
			struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);

			inode->i_ino = ino;
			inode_init_owner(&nop_mnt_idmap, inode, dir, log.mode);
			xmfs_init_dir_inode(inode, false);
			i_size_write(inode, log.size);
			inode->__i_ctime.tv_sec = log.data_offset;
			inode->__i_ctime.tv_nsec = log.offset;
			xmfs_inode->btime = inode->i_mtime = inode->i_atime =
				inode->__i_ctime;
			unlock_new_inode(inode);
			xa_store(&sbi->pinned_inodes, inode->i_ino, inode,
				 GFP_KERNEL);
			XMFS_I(inode)->pinned = true;
		}
	} else {
		iput(inode);
	}

	return 0;
}

static int xmfs_replay_symlink(struct inode *dir, struct dentry *pdentry,
			       struct xmfs_log o_log, struct xmfs_log n_log,
			       unsigned long symname_addr)
{
	struct xmfs_sb_info *sbi = XMFS_SB(dir->i_sb);

	struct inode *inode;

	if (o_log.writer_id == sbi->id)
		return 0;

	inode = ilookup5(dir->i_sb, o_log.i_ino, xmfs_inode_eq, &o_log.i_ino);
	if (!inode) {
		inode = iget5_locked(dir->i_sb, o_log.i_ino, xmfs_inode_eq,
				     xmfs_inode_set, &o_log.i_ino);
		if (inode && (inode->i_state & I_NEW)) {
			struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);

			xmfs_inode->symname = symname_addr;
			inode->i_ino = o_log.i_ino;
			inode_init_owner(&nop_mnt_idmap, inode, dir,
					 S_IFLNK | 0777);
			xmfs_init_symlink_mapping(inode);
			i_size_write(inode, o_log.size);
			inode->__i_ctime.tv_sec = o_log.data_offset;
			inode->__i_ctime.tv_nsec = o_log.offset;
			xmfs_inode->btime = inode->i_mtime = inode->i_atime =
				inode->__i_ctime;
			unlock_new_inode(inode);
			xa_store(&sbi->pinned_inodes, inode->i_ino, inode,
				 GFP_KERNEL);
			XMFS_I(inode)->pinned = true;
		}
	} else {
		iput(inode);
	}

	return 0;
}

static int xmfs_replay_rename(struct inode *dir, struct dentry *pdentry,
			      struct xmfs_log o_log, struct xmfs_log n_log)
{
	struct xmfs_sb_info *sbi = XMFS_SB(dir->i_sb);

	if (o_log.writer_id == sbi->id)
		return 0;

	{
		struct inode *old_parent = ilookup5(sbi->sb, o_log.p_ino,
						    xmfs_inode_eq,
						    &o_log.p_ino);
		if (old_parent) {
			struct dentry *pd = d_find_alias(old_parent);

			if (pd) {
				struct qstr qname =
					QSTR_INIT(o_log.name,
						  strlen(o_log.name));
				struct dentry *d = d_lookup(pd, &qname);

				if (d) {
					d_drop(d);
					dput(d);
				}
				dput(pd);
			}
			iput(old_parent);
		}
	}
	return 0;
}

static int xmfs_replay_create(struct inode *dir, struct dentry *pdentry,
			      struct xmfs_log log, unsigned long replay,
			      unsigned long ino)
{
	struct xmfs_sb_info *sbi = XMFS_SB(dir->i_sb);

	struct inode *inode;
	/*
	 * xmfs_info(sbi->sb, "qyf replay create before check %ld %d %s",
	 *	     log.writer_id, sbi->id, log.name);
	 */
	if (log.writer_id == sbi->id)
		return 0;
	/*
	 * xmfs_info(sbi->sb, "qyf replay create %ld %s size %lld",
	 *	     log.i_ino, log.name, log.size);
	 */
	inode = ilookup5(dir->i_sb, ino, xmfs_inode_eq, &ino);
	if (!inode) {
		inode = iget5_locked(dir->i_sb, ino, xmfs_inode_eq,
				     xmfs_inode_set, &ino);
		if (inode && (inode->i_state & I_NEW)) {
			struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);

			inode->i_ino = ino;
			inode_init_owner(&nop_mnt_idmap, inode, dir, log.mode);
			if (log.index == 1)
				init_special_inode(inode, log.mode,
						   log.end_index);
			else if (S_ISREG(log.mode)) {
				/*
				 * xmfs_inode->write_wq = alloc_workqueue(
				 *	"xmfs-prefetch-inode",
				 *	WQ_UNBOUND | WQ_MEM_RECLAIM, 8);
				 */
				xmfs_init_file_inode(inode);
			} else if (S_ISDIR(log.mode)) {
				xmfs_init_dir_inode(inode, false);
			}
			i_size_write(inode, log.size);
			inode->__i_ctime.tv_sec = log.data_offset;
			inode->__i_ctime.tv_nsec = log.offset;
			xmfs_inode->btime = inode->i_mtime = inode->i_atime =
				inode->__i_ctime;
			unlock_new_inode(inode);
			xa_store(&sbi->pinned_inodes, inode->i_ino, inode,
				 GFP_KERNEL);
			XMFS_I(inode)->pinned = true;
		}
	} else {
		iput(inode);
	}

	return 0;
}

static int xmfs_replay_delete(struct inode *dir, struct dentry *pdentry,
			      struct xmfs_log log)
{
	struct xmfs_sb_info *sbi = XMFS_SB(dir->i_sb);

	if (log.writer_id == sbi->id)
		return 0;

	{
		struct inode *parent =
			ilookup5(sbi->sb, log.p_ino, xmfs_inode_eq, &log.p_ino);
		if (parent) {
			struct dentry *pd = d_find_alias(parent);

			if (pd) {
				struct qstr qname =
					QSTR_INIT(log.name, strlen(log.name));
				struct dentry *d = d_lookup(pd, &qname);

				if (d) {
					d_drop(d);
					dput(d);
				}
				dput(pd);
			}
			iput(parent);
		}
	}
	struct inode *inode;

	inode = ilookup5(sbi->sb, log.i_ino, xmfs_inode_eq, &log.i_ino);
	if (!inode)
		return 0;
	if (inode->i_nlink > 0)
		drop_nlink(inode);
	if (!inode->i_nlink && XMFS_I(inode)->pinned) {
		XMFS_I(inode)->pinned = false;
		unsigned long _idx;
		void *_entry;

		xa_for_each(&XMFS_I(inode)->shared_pages, _idx, _entry) {
			xmfs_mark_page_dead(sbi, (unsigned long)_entry);
		}
		xa_erase(&sbi->pinned_inodes, inode->i_ino);
		inode->i_state &= ~I_DIRTY_TIME;
		/* Release pin reference. */
		iput(inode);
	}
	iput(inode);
	return 0;
}

static int xmfs_replay_update(struct inode *dir, struct dentry *pdentry,
			      struct xmfs_log log, unsigned long *data_addr,
			      unsigned long ino)
{
	void *ret;
	unsigned long index = log.index;
	unsigned long end_index = log.end_index;
	unsigned long offset = log.data_offset;

	struct xmfs_sb_info *sbi = XMFS_SB(dir->i_sb);

	if (log.writer_id == sbi->id)
		return 0;

	struct inode *inode = xmfs_get_inode_from_table(sbi, ino);

	if (!inode) {
		xmfs_info(sbi->sb,
			  "cannot find inode in icache and inode table %ld",
			  ino);
		return -ENOENT;
	}
	struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);

	int err = 0;
	int chunk_header_cnt = 0;
	unsigned long i = 0;
	unsigned long page_off;
	/*
	 * if (!sbi->always_hot) {
	 *	invalidate_inode_pages2_range(inode->i_mapping, index,
	 *				      end_index - 1);
	 *	truncate_pagecache_range(inode, index << PAGE_SHIFT,
	 *				 (end_index - 1) << PAGE_SHIFT);
	 * }
	 */
	/*
	 * if (sbi->mode == 0)
	 *	arch_invalidate_pmem(sbi->kaddr + offset,
	 *			     (end_index - index + 1) * PAGE_SIZE);
	 */
	/*
	 * xmfs_info(sbi->sb, "replay update index %ld to %ld",
	 *	     index, end_index);
	 */
	for (i = index; i < end_index; i++) {
		page_off = offset + (i - index) * PAGE_SIZE +
			   chunk_header_cnt * PAGE_SIZE;
		if (page_off % XMFS_DEFAULT_CHUNK_SIZE == 0) {
			chunk_header_cnt++;
			page_off = offset + (i - index) * PAGE_SIZE +
				   chunk_header_cnt * PAGE_SIZE;
		}
		/*
		 * xmfs_info(sbi->sb, "replay update index %ld offset %ld",
		 *	     i, page_off);
		 */
		ret = xa_store(&xmfs_inode->shared_pages, i, (void *)page_off,
			       GFP_KERNEL);
		if (!xa_err(ret)) {
			inode->i_blocks++;
			if (sbi->id == 0) {
				xmfs_mark_page_dead(sbi, (unsigned long)ret);
				xmfs_mark_page_live(sbi, page_off);
				unsigned long trunk_off =
					log.data_offset &
					~(sbi->trunk_size - 1);
				struct xmfs_data_trunk_header *hdr =
					(void *)(sbi->kaddr + trunk_off);
				if (hdr->magic == XMFS_DATA_TRUNK_MAGIC &&
				    log.version > hdr->last_modify_version)
					hdr->last_modify_version = log.version;
			}
		} else {
			xmfs_info(sbi->sb, "replay cannot store %lu %lu", i,
				  page_off);
		}
		if (sbi->always_hot) {
			struct folio *folio =
				filemap_grab_folio(inode->i_mapping, i);
			if (IS_ERR(folio)) {
				err = -ENOMEM;
				goto done;
			}
			char *kaddr = kmap_local_page(folio_page(folio, 0));

			get_data_and_copy(sbi, offset + (i - index) * PAGE_SIZE,
					  kaddr, PAGE_SIZE, NULL, 0);
			kunmap_local(kaddr);
			folio_mark_uptodate(folio);
			folio_unlock(folio);
			folio_put(folio);
		}
	}

done:

	if (log.size > i_size_read(inode))
		i_size_write(inode, log.size);

	/* return start of data trunk */
	*data_addr = (offset + (end_index - index) * PAGE_SIZE +
		      (sbi->trunk_size - 1)) &
		     ~(sbi->trunk_size - 1);

	iput(inode);

	return err;
}

static int xmfs_replay_xattr(struct inode *dir, struct dentry *pdentry,
			     struct xmfs_log xm_log, unsigned long ino)
{
	struct inode *inode;
	struct xmfs_sb_info *sbi = XMFS_SB(dir->i_sb);

	if (xm_log.writer_id == sbi->id)
		return 0;

	if (ino != 0)
		inode = xmfs_get_inode_from_table(XMFS_SB(dir->i_sb), ino);
	else
		inode = dir;
	if (!inode) {
		xmfs_info(sbi->sb, "xattr cannot find inode xattr %ld", ino);
		return -ENOENT;
	}
	struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);

	size_t size = xm_log.index;
	size_t len = xm_log.end_index;
	int flags = xm_log.size;
	unsigned long offset1 = xm_log.data_offset;

	char *name = kmalloc(len + 1, GFP_KERNEL);

	int err = copy_mc_to_kernel(name, xm_log.name, len);

	if (unlikely(err)) {
		xmfs_error(sbi->sb,
			   "hardware memory error when reading superblock during slave xattr copy name.");
		kfree(name);
		if (ino != 0)
			iput(inode);
		goto set_replay;
	}
	name[len] = '\0';
	if (flags != 0) {
		struct simple_xattr *old_xattr =
			xmfs_simple_xattr_set(&xmfs_inode->xattrs, name, NULL,
					      size, flags);
		if (!IS_ERR(old_xattr)) {
			xmfs_simple_xattr_free(old_xattr);
			old_xattr = NULL;
			inode->__i_ctime.tv_sec = xm_log.p_ino;
			inode->__i_ctime.tv_nsec = xm_log.offset;
		}
		kfree(name);
		if (ino != 0)
			iput(inode);
		goto set_replay;
	}

	char *value = kmalloc(size, GFP_KERNEL);

	err = get_data_and_copy(sbi, offset1, value, size, NULL, 0);
	if (unlikely(err)) {
		xmfs_error(sbi->sb,
			   "hardware memory error when reading superblock during slave xattr copy value.");
		kfree(name);
		kfree(value);
		if (ino != 0)
			iput(inode);
		goto set_replay;
	}

	struct simple_xattr *old_xattr =
		xmfs_simple_xattr_set(&xmfs_inode->xattrs, name, (void *)value,
				      size, flags);
	if (!IS_ERR(old_xattr)) {
		xmfs_simple_xattr_free(old_xattr);
		old_xattr = NULL;
		inode->__i_ctime.tv_sec = xm_log.data_offset;
		inode->__i_ctime.tv_nsec = xm_log.offset;
	}
	kfree(name);
	kfree(value);
	if (ino != 0)
		iput(inode);

set_replay:
	if (sbi->id == 0) {
		xmfs_inode->log_start = sbi->log_start & ~(sbi->log_size - 1);
		sbi->log_current = sbi->log_start;
	}
	return 0;
}

static int xmfs_replay_root(struct inode *dir, struct dentry *pdentry,
			    struct xmfs_log xm_log, unsigned long ino)
{
	return 0;
}

int xmfs_replay_new_entries(struct xmfs_sb_info *sbi, struct dentry *dentry,
			    unsigned long last_update)
{
	struct super_block *sb = sbi->sb;
	struct xmfs_sb_index *xmsi;
	struct xmfs_log_trunk *xmlt;
	struct inode *dir;
	unsigned long data_addr;
	unsigned long trunk_offset;
	unsigned long start_slot;
	unsigned long target;
	int err = 0;
	struct xmfs_log *tmp_log;

	if (!mutex_trylock(&sbi->replay_lock))
		return 0;

	xmsi = kmalloc(sizeof(*xmsi), GFP_KERNEL);
	err = get_xmsi_and_copy(sbi, xmsi);
	if (unlikely(err)) {
		kfree(xmsi);
		mutex_unlock(&sbi->replay_lock);
		return err;
	}

	if (!dentry)
		dentry = sb->s_root;
	dir = dentry->d_inode;
	data_addr = sbi->addr;
	tmp_log = kmalloc(sizeof(struct xmfs_log), GFP_KERNEL);

	if (sbi->last_update >= xmsi->last_update) {
		kfree(xmsi);
		kfree(tmp_log);
		mutex_unlock(&sbi->replay_lock);
		return 0;
	}

	target = (last_update != -1) ? last_update : xmsi->last_update;
	if (target > xmsi->last_update)
		target = xmsi->last_update;
	if (target < sbi->last_update) {
		sbi->last_update = target;
		kfree(tmp_log);
		mutex_unlock(&sbi->replay_lock);
		return 0;
	}

	if (sbi->last_replayed_slot == (unsigned long)-1) {
		trunk_offset = xmsi->log_start;
		start_slot = 0;
		sbi->log_start = xmsi->log_start;
	} else if (sbi->last_replayed_slot < MAX_LOG_PER_TRUNK) {
		trunk_offset = sbi->log_start;
		start_slot = sbi->last_replayed_slot + 1;
	} else {
		struct xmfs_log_trunk *cur =
			get_log_trunk_meta(sbi, sbi->log_start);
		trunk_offset = (cur && cur->next_trunk) ? cur->next_trunk : 0;
		if (cur)
			put_log_trunk(sbi, cur);
		sbi->log_start = trunk_offset;
		start_slot = 0;
		sbi->last_replayed_slot = (unsigned long)-1;
	}
	kfree(xmsi);

	down_read(&sbi->chain_rwsem);

	while (trunk_offset != 0) {
		unsigned long next_trunk;
		unsigned long i;
		int no_ops = 0;

		xmlt = get_log_trunk(sbi, trunk_offset);
		if (!xmlt)
			break;

		next_trunk = xmlt->next_trunk;
		if (xmlt->version_base > target) {
			put_log_trunk(sbi, xmlt);
			break;
		}
		/* put_log_trunk(sbi, xmlt); */
		/* xmlt = get_log_trunk_data(sbi, trunk_offset, xmlt->used); */

		for (i = start_slot; i < xmlt->used; i++) {
			struct xmfs_log *replay_log = &xmlt->log[i];

			switch (replay_log->ops) {
			case XMFS_CREATE:
				err = xmfs_replay_create(dir, dentry,
							 *replay_log, 0,
							 replay_log->i_ino);
				break;
			case XMFS_DELETE:
				err = xmfs_replay_delete(dir, dentry,
							 *replay_log);
				break;
			case XMFS_UPDATE:
				err = xmfs_replay_update(dir, dentry,
							 *replay_log,
							 &data_addr,
							 replay_log->i_ino);
				break;
			case XMFS_LINK:
				err = xmfs_replay_link(dir, dentry, *replay_log,
						       replay_log->i_ino);
				break;
			case XMFS_MKDIR:
				err = xmfs_replay_mkdir(dir, dentry,
							*replay_log, 0,
							replay_log->i_ino);
				break;
			case XMFS_RENAME: {
				if (i + 1 < xmlt->used) {
					*tmp_log = xmlt->log[i + 1];
				} else if (xmlt->next_trunk != 0) {
					struct xmfs_log_trunk *next_xmlt =
						get_log_trunk_meta(sbi,
								   xmlt->next_trunk);
					if (next_xmlt && next_xmlt->used > 0) {
						*tmp_log = next_xmlt->log[0];
						put_log_trunk(sbi, next_xmlt);
					} else if (next_xmlt) {
						put_log_trunk(sbi, next_xmlt);
						break;
					} else {
						break;
					}
				} else {
					break;
				}
				err = xmfs_replay_rename(dir, dentry,
							 *replay_log, *tmp_log);
				i++;
				break;
			}
			case XMFS_SYMLINK: {
				if (i + 1 < xmlt->used) {
					*tmp_log = xmlt->log[i + 1];
				} else if (xmlt->next_trunk != 0) {
					struct xmfs_log_trunk *next_xmlt =
						get_log_trunk_meta(sbi,
								   xmlt->next_trunk);
					if (next_xmlt && next_xmlt->used > 0) {
						*tmp_log = next_xmlt->log[0];
						put_log_trunk(sbi, next_xmlt);
					} else if (next_xmlt) {
						put_log_trunk(sbi, next_xmlt);
						break;
					} else {
						break;
					}
				} else {
					break;
				}
				{
					unsigned long symname_addr;

					if (i + 1 < xmlt->used)
						symname_addr =
							trunk_offset +
							offsetof(struct xmfs_log_trunk,
								 log[0]) +
							(i +
							 1) * sizeof(struct xmfs_log);
					else
						symname_addr =
							xmlt->next_trunk +
							offsetof(struct xmfs_log_trunk,
								 log[0]);
					err = xmfs_replay_symlink(dir, dentry,
								  *replay_log,
								  *tmp_log,
								  symname_addr);
				}
				i++;
				break;
			}
			case XMFS_SETATTR:
				break;
			case XMFS_XATTR:
				err = xmfs_replay_xattr(dir, dentry,
							*replay_log,
							replay_log->i_ino);
				break;
			case XMFS_ROOT:
				err = xmfs_replay_root(dir, dentry, *replay_log,
						       replay_log->i_ino);
				break;
			default:
				no_ops = 1;
				sbi->skip_no_ops++;
				/*
				 * xmfs_error(sbi->sb,
				 *	      "qyf xmfs v2: unknown ops %d index %d %d",
				 *	      replay_log->ops, i,
				 *	      sbi->last_replayed_slot);
				 */
				if (sbi->skip_no_ops >= 50) {
					sbi->skip_no_ops = 0;
					xmfs_error(sbi->sb,
						   "replay fault: unknown ops %d, tried 50 times. This may happened when writing logs is too slow",
						   replay_log->ops);
				} else {
					target = replay_log->version;
					goto done;
				}
			}

			if (err != 0) {
				xmfs_error(sbi->sb,
					   "slave %d v2 play log ops %d err %d",
					   sbi->id, replay_log->ops, -err);
				target = replay_log->version;
				goto done;
			}
			err = 0;
			sbi->last_replayed_slot = i;
			if (replay_log->version > target) {
				put_log_trunk(sbi, xmlt);
				goto done;
			}
			/*
			 * xmfs_info(sbi->sb,
			 *	     "qyf replay sbi %d trunk %d replay ops %d "
			 *	     "name %s slot %d version %d owner %d "
			 *	     "total used %d ino %d index %d "
			 *	     "endindex %d size %d",
			 *	     sbi->id, xmlt->version_base, replay_log->ops,
			 *	     replay_log->name, sbi->last_replayed_slot,
			 *	     replay_log->version, replay_log->writer_id,
			 *	     xmlt->used, replay_log->i_ino,
			 *	     replay_log->index, replay_log->end_index,
			 *	     replay_log->size);
			 */
		}

		/* xmfs_info(sbi->sb, "qyf need to go to next trunk"); */
		start_slot = 0;
		if (next_trunk != 0) {
			sbi->log_start = next_trunk;
			sbi->last_replayed_slot = (unsigned long)-1;
		}
		put_log_trunk(sbi, xmlt);
		trunk_offset = next_trunk;
	}

done:
	up_read(&sbi->chain_rwsem);
	sbi->last_update = target;
	if (sbi->id != 0) {
		if (sbi->mode != XMFS_URMA_MODE) {
			struct mount_msg_info *mm_info =
				sbi->kaddr + MOUNT_MSG_OFFSET;
			mm_info->slave_versions[sbi->id] = sbi->last_update;
		} else {
			unsigned long ver = sbi->last_update;

			xmfs_pmem_write(sbi,
					MOUNT_MSG_OFFSET +
						offsetof(struct mount_msg_info,
							 slave_versions) +
						sbi->id * sizeof(unsigned long),
					&ver, sizeof(ver));
		}
	}

	kfree(tmp_log);
	mutex_unlock(&sbi->replay_lock);
	return 0;
}

void xmfs_populate_from_inode_table(struct xmfs_sb_info *sbi)
{
	struct xmfs_inode_table *tbl;
	struct inode *inode;
	unsigned long i;
	unsigned long max_entries;

	xmfs_info(sbi->sb, "xmfs_populate_from_inode_table");
	if (sbi->mode == XMFS_SHARED_MEM_MODE || sbi->mode == XMFS_HYBRID_MODE)
		tbl = (struct xmfs_inode_table *)(sbi->kaddr +
						  INODE_TABLE_OFFSET);
	else
		tbl = (struct xmfs_inode_table *)get_meta(sbi,
							  INODE_TABLE_OFFSET,
							  INODE_TABLE_SIZE);

	if (!tbl || tbl->magic != XMFS_INODE_TABLE_MAGIC) {
		xmfs_info(sbi->sb, "no valid inode table, skipping populate");
		if (tbl && sbi->mode == XMFS_URMA_MODE)
			put_meta(sbi, tbl);
		return;
	}

	max_entries =
		(INODE_TABLE_SIZE - 4096) / sizeof(struct xmfs_inode_entry);
	xmfs_info(sbi->sb, "populating from inode table, %lu entries",
		  tbl->count);

	for (i = 0; i < tbl->count && i < max_entries; i++) {
		struct xmfs_inode_entry *e = &tbl->entries[i];

		if (e->i_ino == 0)
			continue;

		inode = ilookup5(sbi->sb, e->i_ino, xmfs_inode_eq, &e->i_ino);
		if (inode) {
			iput(inode);
			continue;
		}

		inode = iget5_locked(sbi->sb, e->i_ino, xmfs_inode_eq,
				     xmfs_inode_set, &e->i_ino);
		if (!inode)
			continue;

		if (inode->i_state & I_NEW) {
			struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);

			xmfs_inode->leaf = (e->i_ino != 0);
			inode->i_ino = e->i_ino;
			inode->i_mode = e->mode;
			set_nlink(inode, e->nlink);
			i_size_write(inode, e->size);
			if (S_ISREG(e->mode)) {
				/*
				 * xmfs_inode->write_wq = alloc_workqueue(
				 *	"xmfs-prefetch-inode",
				 *	WQ_UNBOUND | WQ_MEM_RECLAIM, 8);
				 */
				xmfs_init_file_inode(inode);
			} else if (S_ISDIR(e->mode)) {
				xmfs_init_dir_inode(inode, sbi->id == 0);
			} else if (S_ISLNK(e->mode)) {
				xmfs_init_symlink_operations(inode);
			} else {
				init_special_inode(inode, e->mode, 0);
			}
			unlock_new_inode(inode);
			xa_store(&sbi->pinned_inodes, inode->i_ino, inode,
				 GFP_KERNEL);
			XMFS_I(inode)->pinned = true;
		}
	}

	if (sbi->mode == XMFS_URMA_MODE)
		put_meta(sbi, tbl);
}
