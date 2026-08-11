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

int xmfs_inode_eq(struct inode *inode, void *_ino)
{
	u64 ino = *(u64 *)_ino;

	if (XMFS_I(inode)->i_ino == ino && XMFS_I(inode)->pinned &&
	    !XMFS_I(inode)->die)
		return 1;
	else
		return 0;
}

int xmfs_inode_set(struct inode *inode, void *_ino)
{
	u64 ino = *(u64 *)_ino;

	XMFS_I(inode)->i_ino = ino;
	inode->i_ino = ino;
	XMFS_I(inode)->leaf = true;
	XMFS_I(inode)->tmp = false;
	XMFS_I(inode)->die = false;
	XMFS_I(inode)->fallocate = false;
	XMFS_I(inode)->pinned = false;
	XMFS_I(inode)->start_write = 0;
	XMFS_I(inode)->second_write = 0;
	XMFS_I(inode)->count = 0;
	xa_init(&XMFS_I(inode)->shared_pages);
	xmfs_simple_xattrs_init(&XMFS_I(inode)->xattrs);
	XMFS_I(inode)->write_wq = NULL;

	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);

	if (!sbi->always_hot)
		xa_init(&XMFS_I(inode)->page_freq);

	sbi->used_ino++;
	return 0;
}

unsigned int xmfs_inode_table_sync_nlink(struct xmfs_inode_table *tbl,
					 unsigned long ino)
{
	unsigned long i;
	unsigned int nlink = 0;

	for (i = 0; i < tbl->count; i++) {
		if (tbl->entries[i].i_ino == ino)
			nlink++;
	}
	for (i = 0; i < tbl->count; i++) {
		if (tbl->entries[i].i_ino == ino)
			tbl->entries[i].nlink = nlink;
	}

	return nlink;
}

bool xmfs_inode_table_remove(struct xmfs_inode_table *tbl, unsigned long ino,
			     unsigned long p_ino, const char *name)
{
	unsigned long i;

	for (i = 0; i < tbl->count; i++) {
		struct xmfs_inode_entry *entry = &tbl->entries[i];

		if (entry->i_ino != ino || entry->p_ino != p_ino ||
		    strcmp(entry->name, name))
			continue;

		tbl->count--;
		if (i < tbl->count)
			tbl->entries[i] = tbl->entries[tbl->count];
		memset(&tbl->entries[tbl->count], 0,
		       sizeof(struct xmfs_inode_entry));
		return true;
	}

	return false;
}

int xmfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		 struct iattr *attr)
{
	void *ret;
	struct inode *inode = dentry->d_inode;
	int err;

	if (inode->i_ino == 0)
		return simple_setattr(idmap, dentry, attr);

	struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);

	if (sbi->id != 0)
		return xmfs_slave_setattr(idmap, dentry, attr);

	err = setattr_prepare(idmap, dentry, attr);
	if (err) {
		xmfs_info(inode->i_sb, "set attr wrong %d %d %d", err,
			  attr->ia_mode, attr->ia_valid);
		return err;
	}
	setattr_copy(idmap, d_inode(dentry), attr);
	mark_inode_dirty(inode);
	unsigned int ia_valid = attr->ia_valid;

	if (ia_valid & ATTR_SIZE) {
		loff_t new_size = attr->ia_size;

		if (new_size > sbi->len)
			return -EFBIG;

		if (i_size_read(inode) > new_size) {
			for (unsigned long i =
				     (new_size + PAGE_SIZE - 1) >> PAGE_SHIFT;
			     i <= (i_size_read(inode) >> PAGE_SHIFT); i++) {
				ret = xa_erase(&xmfs_inode->shared_pages, i);
				if (ret)
					inode->i_blocks--;

			}
			i_size_write(inode, new_size);
			truncate_pagecache(inode, new_size);

		} else {
			i_size_write(inode, new_size);
		}
		{
			struct xmfs_inode_table *tbl =
				(void *)(sbi->kaddr + INODE_TABLE_OFFSET);
			unsigned long ci;

			for (ci = 0; ci < tbl->count; ci++) {
				if (tbl->entries[ci].i_ino == inode->i_ino) {
					tbl->entries[ci].size =
						i_size_read(inode);
					tbl->entries[ci].ctime =
						inode_get_ctime(inode);
					tbl->entries[ci].mtime = inode->i_mtime;
					break;
				}
			}
		}
	}

	return err;
}

int xmfs_getattr(struct mnt_idmap *idmap, const struct path *path,
		 struct kstat *stat, u32 request_mask, unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);

	generic_fillattr(idmap, request_mask, inode, stat);
	if (request_mask & STATX_BTIME) {
		stat->result_mask |= STATX_BTIME;
		stat->btime = XMFS_I(inode)->btime;
	}
	return 0;
}

int xmfs_slave_getattr(struct mnt_idmap *idmap, const struct path *path,
		       struct kstat *stat, u32 request_mask,
		       unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);

	generic_fillattr(idmap, request_mask, inode, stat);
	if (request_mask & STATX_BTIME) {
		stat->result_mask |= STATX_BTIME;
		stat->btime = XMFS_I(inode)->btime;
	}
	return 0;
}

int xmfs_update_time(struct inode *inode, int flags)
{
	struct iattr attr = {};
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
	struct xmfs_log xm_log;

	if (inode->i_ino == 0)
		return 0;

	if (flags == S_ATIME)
		return 0;

	if ((flags & (S_MTIME | S_CTIME | S_ATIME)) == 0)
		return 0;

	generic_update_time(inode, flags);
	return 0;
	if (inode->i_ino == 0)
		return 0;

	if (flags == S_ATIME)
		return 0;

	int err;

	if ((flags & (S_MTIME | S_CTIME | S_ATIME)) == 0)
		return err;

	if (flags & S_MTIME) {
		attr.ia_mtime.tv_sec = inode->i_mtime.tv_sec;
		attr.ia_mtime.tv_nsec = inode->i_mtime.tv_nsec;
		attr.ia_valid |= ATTR_MTIME;
	}
	if (flags & S_CTIME) {
		attr.ia_ctime.tv_sec = inode->__i_ctime.tv_sec;
		attr.ia_ctime.tv_nsec = inode->__i_ctime.tv_nsec;
		attr.ia_valid |= ATTR_CTIME;
	}
	if (flags & S_ATIME) {
		attr.ia_atime.tv_sec = inode->i_atime.tv_sec;
		attr.ia_atime.tv_nsec = inode->i_atime.tv_nsec;
		attr.ia_valid |= ATTR_ATIME;
	}

	struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);

	if (xmfs_inode->die) {
		xmfs_info(inode->i_sb,
			  "inode logs have been interrupted before update time");
		return -EINVAL;
	}

	struct xmfs_sb_index *xmsi;

	spin_lock(&sbi->space_lock);
	xmsi = get_xmsi(sbi);
	if (!xmsi) {
		spin_unlock(&sbi->space_lock);
		return -EINVAL;
	}
	err = 0;
	int logcount = 1;

	if (logcount + xmsi->used_trunk_count > sbi->max_chunk) {
		sbi->full = true;
		spin_unlock(&sbi->space_lock);
		put_xmsi(sbi, xmsi);
		return -ENOSPC;
	}
	spin_unlock(&sbi->space_lock);
	put_xmsi(sbi, xmsi);

	xm_log.ops = XMFS_SETATTR;
	memcpy(xm_log.name, &attr, sizeof(struct iattr));

	unsigned long xmlt_addr = xmfs_add_log_v3(&xm_log, sbi,
						  sizeof(struct iattr),
						  inode->i_ino, true);
	if (xmlt_addr < 0) {
		xmfs_inode->die = true;
		return -EINVAL;
	}

	return 0;
}

static const struct inode_operations xmfs_special_inode_operations = {
	.getattr	= xmfs_getattr,
	.setattr	= xmfs_setattr,
	.listxattr	= xmfs_listxattr,
	.update_time	= xmfs_update_time,
};

void xmfs_init_special_inode_operations(struct inode *inode)
{
	inode->i_op = &xmfs_special_inode_operations;
}

int xmfs_slave_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		       struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
	struct xmfs_rw_msg req = {};
	int err;

	err = setattr_prepare(idmap, dentry, attr);
	if (err)
		return err;

	req.ops = XMFS_SETATTR;
	req.i_ino = inode->i_ino;
	req.p_ino = 0;
	req.data_pages = 0;
	req.log_entries = 0;
	memcpy(req.name, attr, sizeof(struct iattr));

	req.type = MSG_RW_REQUEST;
	req.slave_id = sbi->id;
	spin_lock(&sbi->local_ring_lock);
	req.request_id = ++sbi->rw_request_id;
	spin_unlock(&sbi->local_ring_lock);
	err = xmfs_send_request(sbi, &req, NULL, 0);
	if (err)
		return err;

	setattr_copy(idmap, inode, attr);

	unsigned int ia_valid = attr->ia_valid;

	if (ia_valid & ATTR_SIZE) {
		xmfs_info(inode->i_sb, "slave setattr size %lld",
			  (long long)attr->ia_size);
		loff_t new_size = attr->ia_size;

		if (new_size > sbi->len)
			return -EFBIG;

		if (i_size_read(inode) > new_size) {
			for (unsigned long i =
				     (new_size + PAGE_SIZE - 1) >> PAGE_SHIFT;
			     i <= (i_size_read(inode) >> PAGE_SHIFT); i++) {
				xa_erase(&XMFS_I(inode)->shared_pages, i);
			}
			i_size_write(inode, new_size);
			truncate_pagecache(inode, new_size);
		} else {
			i_size_write(inode, new_size);
		}
	}
	return 0;
}
