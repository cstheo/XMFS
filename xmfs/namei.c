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

#include <linux/file.h>
#include <linux/kernel.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/security.h>
#include <linux/slab.h>

static const struct inode_operations xmfs_host_dir_inode_operations;
static const struct inode_operations xmfs_dir_inode_operations;

static int xmfs_do_create_slave(struct xmfs_sb_info *sbi, struct inode *dir,
				struct dentry *dentry, umode_t mode);
static int xmfs_do_mkdir_slave(struct xmfs_sb_info *sbi, struct inode *dir,
			       struct dentry *dentry, umode_t mode);
static int xmfs_do_rename_slave(struct xmfs_sb_info *sbi, struct inode *old_dir,
				struct dentry *old_dentry,
				struct inode *new_dir,
				struct dentry *new_dentry, unsigned int flags);
static int xmfs_do_mknod_slave(struct xmfs_sb_info *sbi, struct inode *dir,
			       struct dentry *dentry, umode_t mode, dev_t dev);
static int xmfs_do_unlink_slave(struct xmfs_sb_info *sbi, struct inode *dir,
				struct dentry *dentry);
static int xmfs_do_symlink_slave(struct xmfs_sb_info *sbi, struct inode *dir,
				 struct dentry *dentry, const char *symname);
static int xmfs_do_link_slave(struct xmfs_sb_info *sbi,
			      struct dentry *old_dentry, struct inode *dir,
			      struct dentry *dentry);

static void xmfs_unpin_inode(struct xmfs_sb_info *sbi, struct inode *inode)
{
	if (!XMFS_I(inode)->pinned)
		return;

	XMFS_I(inode)->pinned = false;
	xa_erase(&sbi->pinned_inodes, inode->i_ino);
	iput(inode);
}

static unsigned long xmfs_symlink_write_symname(struct inode *inode,
						const char *name,
						struct xmfs_log *xm_log)
{
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
	struct xmfs_sb_index *xmsi;
	unsigned long data_current, data_count;
	long slot;
	int err = 0;

	spin_lock(&sbi->space_lock);
	xmsi = get_xmsi(sbi);
	if (!xmsi) {
		spin_unlock(&sbi->space_lock);
		return -EINVAL;
	}

	data_current = xmsi->data_current;
	data_count = xmsi->data_count;

	WARN_ON(xmsi->count == 0);

	if (data_current == 0 || data_count == 511) {
		slot = xmfs_alloc_and_mark_slot(xmsi, sbi, 'd');
		if (slot < 0) {
			spin_unlock(&sbi->space_lock);
			put_xmsi(sbi, xmsi);
			return -ENOSPC;
		}
		xmsi->data_current =
			DATA_AREA_OFFSET + slot * sbi->trunk_size + PAGE_SIZE;
		xmsi->data_count = 0;
	} else {
		xmsi->data_current = data_current + PAGE_SIZE;
	}

	xmfs_data_write(sbi, xmsi->data_current, name,
			min(strlen(name) + 1, PATH_MAX));
	xmfs_mark_page_live(sbi, xmsi->data_current);

	xmsi->data_count = xmsi->data_count + 1;
	xm_log->end_index = xmsi->data_current;
	spin_unlock(&sbi->space_lock);
	put_xmsi(sbi, xmsi);
	return err;
}

static int xmfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, const char *symname)
{
	struct xmfs_sb_info *sbi = XMFS_SB(dir->i_sb);

	if (xmfs_quiesce_wait(sbi))
		return -EINTR;
	if (sbi->id != 0)
		return xmfs_do_symlink_slave(sbi, dir, dentry, symname);
	struct xmfs_inode_info *xmfs_inode;
	unsigned int len = strlen(symname) + 1;

	if (len > PAGE_SIZE)
		return -ENAMETOOLONG;

	const char *name = dentry->d_name.name;
	struct inode *inode;
	struct xmfs_log xm_log;
	unsigned long xmlt_addr;

	spin_lock(&sbi->space_lock);
	if (sbi->full) {
		spin_unlock(&sbi->space_lock);
		return -ENOSPC;
	}

	struct xmfs_sb_index *xmsi = get_xmsi(sbi);

	if (!xmsi) {
		spin_unlock(&sbi->space_lock);
		return -EINVAL;
	}
	int err = 0;

	int logcount = 1;
	int need_data_trunk =
		(xmsi->data_count == 511 || xmsi->data_current == 0) ? 1 : 0;
	if (len >= 256 && xmsi->used_trunk_count + logcount + need_data_trunk >
				  sbi->max_chunk) {
		spin_unlock(&sbi->space_lock);
		put_xmsi(sbi, xmsi);
		return -ENOSPC;
	} else if (len < 256 &&
		   xmsi->used_trunk_count + logcount > sbi->max_chunk) {
		sbi->full = true;
		spin_unlock(&sbi->space_lock);
		put_xmsi(sbi, xmsi);
		return -ENOSPC;
	}
	spin_unlock(&sbi->space_lock);
	put_xmsi(sbi, xmsi);

	unsigned long ino = get_next_ino();

	inode = iget5_locked(dir->i_sb, ino, xmfs_inode_eq, xmfs_inode_set,
			     &ino);
	if (!inode)
		return -ENOMEM;

	xmfs_inode = XMFS_I(inode);
	inode->i_ino = ino;

	i_size_write(inode, 0);
	inode_init_owner(idmap, inode, dir, S_IFLNK | 0777);

	xmfs_init_symlink_operations(inode);
	err = security_inode_init_security(inode, dir, &dentry->d_name,
					   xmfs_initxattrs, NULL);
	if (err) {
		unlock_new_inode(inode);
		iput(inode);
		return err;
	}
	inode_nohighmem(inode);
	xmfs_init_symlink_mapping(inode);

	xmfs_inode->btime = inode->i_atime = inode->i_mtime =
		inode_set_ctime_current(inode);
	dir->i_mtime = inode_set_ctime_current(dir);
	unlock_new_inode(inode);
	ihold(inode);
	xa_store(&sbi->pinned_inodes, inode->i_ino, inode, GFP_KERNEL);
	XMFS_I(inode)->pinned = true;

	xm_log.index = 0;
	xm_log.end_index = 0;
	xm_log.ops = XMFS_SYMLINK;
	xm_log.data_offset = xmfs_inode->btime.tv_sec;
	xm_log.mode = S_IFLNK | 0777;
	memcpy(xm_log.name, name, min(strlen(name), 255));
	xm_log.p_ino = parent_ino(dentry);

	struct xmfs_log xm_log1;

	xm_log1.index = 1;
	xm_log1.ops = XMFS_SYMLINK;
	xm_log1.data_offset = xmfs_inode->btime.tv_nsec;
	xm_log1.mode = S_IFLNK | 0777;
	xm_log1.p_ino = parent_ino(dentry);

	xmlt_addr = xmfs_add_log_v3(&xm_log, sbi, min(strlen(name), 255),
				    inode->i_ino, false);
	if (xmlt_addr < 0) {
		xmfs_inode->die = true;
		clear_nlink(inode);
		iput(inode);
		return -EINVAL;
	}

	if (len <= 255) {
		memcpy(xm_log1.name, symname, min(strlen(symname), 255));
		xm_log1.name[min(strlen(symname), 255)] = '\0';
		xm_log1.end_index = 0;
		xmlt_addr = xmfs_add_log_v3(&xm_log1, sbi,
					    min(strlen(symname), 255),
					    inode->i_ino, true);
	} else {
		err = xmfs_symlink_write_symname(inode, symname, &xm_log1);
		xm_log1.index = len;
		if (err) {
			xmfs_inode->die = true;
			clear_nlink(inode);
			iput(inode);
			return err;
		}
		xmlt_addr =
			xmfs_add_log_v3(&xm_log1, sbi, 0, inode->i_ino, true);
	}
	if (xmlt_addr < 0) {
		xmfs_inode->die = true;
		clear_nlink(inode);
		iput(inode);
		return -EINVAL;
	}
	xmfs_inode->symname = xmlt_addr;

	err = page_symlink(inode, symname, len);
	if (err) {
		clear_nlink(inode);
		iput(inode);
		return err;
	}

	d_instantiate(dentry, inode);
	{
		struct xmfs_inode_table *tbl =
			(void *)(sbi->kaddr + INODE_TABLE_OFFSET);
		unsigned long slot = tbl->count;

		tbl->count++;
		{
			struct xmfs_inode_entry *e = &tbl->entries[slot];

			memset(e, 0, sizeof(*e));
			e->i_ino = inode->i_ino;
			e->p_ino = dir->i_ino;
			e->mode = S_IFLNK | 0777;
			e->nlink = 1;
			e->size = 0;
			e->ctime = inode_get_ctime(inode);
			e->mtime = inode->i_mtime;
			e->atime = inode->i_atime;
			memcpy(e->name, dentry->d_name.name,
			       dentry->d_name.len);
			if (dentry->d_name.len < 256)
				e->name[dentry->d_name.len] = '\0';
		}
	}

	return 0;
}

static int xmfs_link(struct dentry *old_dentry, struct inode *dir,
		     struct dentry *dentry)
{
	struct xmfs_sb_info *sbi = XMFS_SB(dir->i_sb);

	if (xmfs_quiesce_wait(sbi))
		return -EINTR;
	if (sbi->id != 0)
		return xmfs_do_link_slave(sbi, old_dentry, dir, dentry);
	const char *name = dentry->d_name.name;
	struct inode *inode = d_inode(old_dentry);
	struct xmfs_log xm_log;
	unsigned long xmlt_addr;

	if (XMFS_I(inode)->die) {
		xmfs_info(sbi->sb,
			  "inode logs have been interrupted before link");
		return -EINVAL;
	}
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
	spin_unlock(&sbi->space_lock);
	put_xmsi(sbi, xmsi);

	dir->i_mtime =
		inode_set_ctime_to_ts(dir, inode_set_ctime_current(inode));
	inc_nlink(inode);
	ihold(inode);

	d_instantiate(dentry, inode);

	{
		struct xmfs_inode_table *tbl =
			(void *)(sbi->kaddr + INODE_TABLE_OFFSET);
		unsigned long slot;

		spin_lock(&sbi->space_lock);
		slot = tbl->count;
		tbl->count++;
		{
			struct xmfs_inode_entry *e = &tbl->entries[slot];

			memset(e, 0, sizeof(*e));
			e->i_ino = inode->i_ino;
			e->p_ino = dir->i_ino;
			e->mode = inode->i_mode;
			e->nlink = inode->i_nlink;
			e->size = i_size_read(inode);
			e->ctime = inode_get_ctime(inode);
			e->mtime = inode->i_mtime;
			e->atime = inode->i_atime;
			memcpy(e->name, dentry->d_name.name,
			       dentry->d_name.len);
			if (dentry->d_name.len < 256)
				e->name[dentry->d_name.len] = '\0';
		}
		xmfs_inode_table_sync_nlink(tbl, inode->i_ino);
		spin_unlock(&sbi->space_lock);
	}

	xm_log.index = 0;
	xm_log.end_index = inode->i_ino;
	xm_log.ops = XMFS_LINK;
	xm_log.data_offset = dir->i_mtime.tv_sec;
	xm_log.mode = inode->i_mode;
	xm_log.offset = dir->i_mtime.tv_nsec;
	memcpy(xm_log.name, name, min(strlen(name), 255UL));
	xm_log.p_ino = parent_ino(dentry);

	xmlt_addr = xmfs_add_log_v3(&xm_log, sbi,
				    min(strlen(name), 255UL),
				    inode->i_ino, true);
	if (xmlt_addr < 0) {
		struct xmfs_inode_table *tbl =
			(void *)(sbi->kaddr + INODE_TABLE_OFFSET);

		spin_lock(&sbi->space_lock);
		xmfs_inode_table_remove(tbl, inode->i_ino, dir->i_ino, name);
		xmfs_inode_table_sync_nlink(tbl, inode->i_ino);
		spin_unlock(&sbi->space_lock);
		XMFS_I(inode)->die = true;
		drop_nlink(inode);
		iput(inode);
		dput(dentry);
		return -EINVAL;
	}

	return 0;
}

static int xmfs_do_create_slave(struct xmfs_sb_info *sbi, struct inode *dir,
				struct dentry *dentry, umode_t mode)
{
	struct xmfs_rw_msg req = {}, grant = {};
	struct inode *inode;
	struct xmfs_inode_info *xmfs_inode;
	int err;
	struct xmfs_log xm_log;
	unsigned long log_off;

	req.ops = XMFS_CREATE;
	req.mode = mode | S_IFREG;
	req.p_ino = dir->i_ino;
	req.i_ino = 0;
	req.data_pages = 0;
	req.log_entries = 1;
	memcpy(req.name, dentry->d_name.name, dentry->d_name.len + 1);
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

	log_off = grant.log_trunk_addr +
		  offsetof(struct xmfs_log_trunk, log[0]) +
		  grant.log_slot_start * sizeof(struct xmfs_log);
	xm_log.index = 0;
	xm_log.end_index = 0;
	xm_log.ops = XMFS_CREATE;
	xm_log.data_offset = ktime_get_real_seconds();
	xm_log.mode = mode | S_IFREG;
	xm_log.offset = ktime_get_real_ns() % NSEC_PER_SEC;
	xm_log.version = grant.version;
	memcpy(xm_log.name, dentry->d_name.name,
	       min(dentry->d_name.len, 255UL));
	if (dentry->d_name.len < 256)
		xm_log.name[dentry->d_name.len] = '\0';
	xm_log.p_ino = dir->i_ino;
	xm_log.i_ino = grant.assigned_ino;
	xm_log.writer_id = sbi->id;
	xm_log.size = 0;
	xmfs_data_write(sbi, log_off, &xm_log, sizeof(xm_log));

	inode = iget5_locked(dir->i_sb, grant.assigned_ino, xmfs_inode_eq,
			     xmfs_inode_set, &grant.assigned_ino);
	if (!inode)
		return -ENOMEM;

	if (!(inode->i_state & I_NEW))
		return 0;

	xmfs_inode = XMFS_I(inode);
	i_size_write(inode, 0);
	inode->i_ino = grant.assigned_ino;
	inode_init_owner(&nop_mnt_idmap, inode, dir, mode | S_IFREG);
	xmfs_init_file_inode(inode);
	/*
	 * xmfs_inode->write_wq = alloc_workqueue("xmfs-prefetch-inode",
	 *					  WQ_UNBOUND | WQ_MEM_RECLAIM,
	 *					  8);
	 */
	security_inode_init_security(inode, dir, &dentry->d_name,
				     xmfs_initxattrs, NULL);
	xmfs_inode->btime = inode->i_mtime = inode->i_atime =
		inode_set_ctime_current(inode);
	dir->i_mtime = inode_set_ctime_current(dir);
	unlock_new_inode(inode);
	d_instantiate(dentry, inode);
	ihold(inode);
	xa_store(&sbi->pinned_inodes, inode->i_ino, inode, GFP_KERNEL);
	XMFS_I(inode)->pinned = true;

	return 0;
}

static int xmfs_do_mkdir_slave(struct xmfs_sb_info *sbi, struct inode *dir,
			       struct dentry *dentry, umode_t mode)
{
	struct xmfs_rw_msg req = {}, grant = {};
	struct inode *inode;
	struct xmfs_inode_info *xmfs_inode;
	int err;
	struct xmfs_log xm_log;
	unsigned long log_off;

	req.ops = XMFS_MKDIR;
	req.mode = mode | S_IFDIR;
	req.p_ino = dir->i_ino;
	req.i_ino = 0;
	req.data_pages = 0;
	req.log_entries = 1;
	req.size = 4096;
	memcpy(req.name, dentry->d_name.name, dentry->d_name.len + 1);

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

	log_off = grant.log_trunk_addr +
		  offsetof(struct xmfs_log_trunk, log[0]) +
		  grant.log_slot_start * sizeof(struct xmfs_log);
	xm_log.index = 0;
	xm_log.end_index = 0;
	xm_log.ops = XMFS_MKDIR;
	xm_log.data_offset = ktime_get_real_seconds();
	xm_log.mode = mode | S_IFDIR;
	xm_log.offset = ktime_get_real_ns() % NSEC_PER_SEC;
	xm_log.version = grant.version;
	memcpy(xm_log.name, dentry->d_name.name,
	       min(dentry->d_name.len, 255UL));
	if (dentry->d_name.len < 256)
		xm_log.name[dentry->d_name.len] = '\0';
	xm_log.p_ino = dir->i_ino;
	xm_log.i_ino = grant.assigned_ino;
	xm_log.writer_id = sbi->id;
	xm_log.size = 4096;
	xmfs_data_write(sbi, log_off, &xm_log, sizeof(xm_log));

	inode = iget5_locked(dir->i_sb, grant.assigned_ino, xmfs_inode_eq,
			     xmfs_inode_set, &grant.assigned_ino);
	if (!inode)
		return -ENOMEM;

	if (!(inode->i_state & I_NEW))
		return 0;

	xmfs_inode = XMFS_I(inode);
	i_size_write(inode, 4096);
	inode->i_ino = grant.assigned_ino;
	inode_init_owner(&nop_mnt_idmap, inode, dir, S_IFDIR | mode);
	xmfs_init_dir_inode(inode, false);
	xmfs_inode->btime = inode->i_mtime = inode->i_atime =
		inode_set_ctime_current(inode);
	dir->i_mtime = inode_set_ctime_current(dir);
	inc_nlink(dir);
	inc_nlink(inode);
	unlock_new_inode(inode);
	d_instantiate(dentry, inode);
	ihold(inode);
	xa_store(&sbi->pinned_inodes, inode->i_ino, inode, GFP_KERNEL);
	XMFS_I(inode)->pinned = true;

	return 0;
}

static int xmfs_do_unlink_slave(struct xmfs_sb_info *sbi, struct inode *dir,
				struct dentry *dentry)
{
	struct xmfs_rw_msg req = {}, grant = {};
	int err;
	struct xmfs_log xm_log;
	unsigned long log_off;

	req.ops = XMFS_DELETE;
	req.p_ino = dir->i_ino;
	req.i_ino = dentry->d_inode->i_ino;
	req.data_pages = 0;
	req.log_entries = 1;
	memcpy(req.name, dentry->d_name.name, dentry->d_name.len + 1);

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

	log_off = grant.log_trunk_addr +
		  offsetof(struct xmfs_log_trunk, log[0]) +
		  grant.log_slot_start * sizeof(struct xmfs_log);
	xm_log.ops = XMFS_DELETE;
	xm_log.i_ino = dentry->d_inode->i_ino;
	xm_log.p_ino = dir->i_ino;
	xm_log.version = grant.version;
	memcpy(xm_log.name, dentry->d_name.name,
	       min(dentry->d_name.len, 255UL));
	if (dentry->d_name.len < 256)
		xm_log.name[dentry->d_name.len] = '\0';
	xm_log.writer_id = sbi->id;
	xmfs_data_write(sbi, log_off, &xm_log, sizeof(xm_log));

	if (d_inode(dentry)->i_nlink > 0)
		drop_nlink(d_inode(dentry));
	if (!d_inode(dentry)->i_nlink)
		xmfs_unpin_inode(sbi, d_inode(dentry));
	return 0;
}

static int xmfs_do_symlink_slave(struct xmfs_sb_info *sbi, struct inode *dir,
				 struct dentry *dentry, const char *symname)
{
	struct xmfs_rw_msg req = {};
	struct inode *inode;
	struct xmfs_inode_info *xmfs_inode;
	unsigned int len = strlen(symname) + 1;
	struct xmfs_log xm_log;
	unsigned long log_off;
	int err;

	if (len > PAGE_SIZE)
		return -ENAMETOOLONG;

	req.ops = XMFS_SYMLINK;
	req.mode = S_IFLNK | 0777;
	req.p_ino = dir->i_ino;
	req.i_ino = 0;
	req.data_pages = (len > 255) ? 1 : 0;
	req.log_entries = 2;
	memcpy(req.name, dentry->d_name.name, dentry->d_name.len + 1);

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

	inode = iget5_locked(dir->i_sb, gw.grant.assigned_ino, xmfs_inode_eq,
			     xmfs_inode_set, &gw.grant.assigned_ino);
	if (!inode)
		return -ENOMEM;

	if (!(inode->i_state & I_NEW))
		return 0;

	xmfs_inode = XMFS_I(inode);
	inode->i_ino = gw.grant.assigned_ino;
	i_size_write(inode, 0);
	inode_init_owner(&nop_mnt_idmap, inode, dir, S_IFLNK | 0777);
	xmfs_init_symlink_operations(inode);
	inode_nohighmem(inode);
	xmfs_init_symlink_mapping(inode);
	xmfs_inode->btime = inode->i_atime = inode->i_mtime =
		inode_set_ctime_current(inode);
	dir->i_mtime = inode_set_ctime_current(dir);
	unlock_new_inode(inode);
	ihold(inode);
	xa_store(&sbi->pinned_inodes, inode->i_ino, inode, GFP_KERNEL);
	XMFS_I(inode)->pinned = true;

	log_off = gw.grant.log_trunk_addr +
		  offsetof(struct xmfs_log_trunk, log[0]) +
		  gw.grant.log_slot_start * sizeof(struct xmfs_log);
	xm_log.index = 0;
	xm_log.end_index = 0;
	xm_log.ops = XMFS_SYMLINK;
	xm_log.data_offset = ktime_get_real_seconds();
	xm_log.mode = S_IFLNK | 0777;
	xm_log.version = gw.grant.version;
	memcpy(xm_log.name, dentry->d_name.name,
	       min(dentry->d_name.len, 255UL));
	if (dentry->d_name.len < 256)
		xm_log.name[dentry->d_name.len] = '\0';
	xm_log.p_ino = dir->i_ino;
	xm_log.i_ino = gw.grant.assigned_ino;
	xm_log.writer_id = sbi->id;
	xmfs_data_write(sbi, log_off, &xm_log, sizeof(xm_log));

	if (gw.grant.log_slot_count >= 2) {
		struct xmfs_log xm_log1 = {};

		xm_log1.index = 1;
		xm_log1.ops = XMFS_SYMLINK;
		xm_log1.version = gw.grant.version + 1;
		xm_log1.i_ino = gw.grant.assigned_ino;
		xm_log1.p_ino = dir->i_ino;
		xm_log1.writer_id = sbi->id;
		if (len <= 255) {
			memcpy(xm_log1.name, symname,
			       min(len, 255UL));
			xm_log1.name[min(len - 1, 254UL)] = '\0';
			xm_log1.end_index = 0;
		} else {
			xm_log1.end_index = gw.grant.data_addr;
			xm_log1.index = len;
			XMFS_I(inode)->start_write = gw.grant.data_addr;
			XMFS_I(inode)->count = gw.grant.data_page_count;
		}
		log_off = gw.grant.log_trunk_addr +
			  offsetof(struct xmfs_log_trunk, log[0]) +
			  (gw.grant.log_slot_start + 1) * sizeof(struct xmfs_log);
		xmfs_inode->symname = log_off;
		xmfs_data_write(sbi, log_off, &xm_log1, sizeof(xm_log1));
	}

	if (len > 255 && gw.grant.data_addr != 0) {
		xmfs_data_write(sbi, gw.grant.data_addr, symname,
				min(len, (unsigned long)PAGE_SIZE));
	}

	err = page_symlink(inode, symname, len);
	if (err) {
		clear_nlink(inode);
		iput(inode);
		return err;
	}

	d_instantiate(dentry, inode);

	return 0;
}

static int xmfs_do_link_slave(struct xmfs_sb_info *sbi,
			      struct dentry *old_dentry, struct inode *dir,
			      struct dentry *dentry)
{
	struct xmfs_rw_msg req = {}, grant = {};
	struct inode *inode = d_inode(old_dentry);
	int err;
	struct xmfs_log xm_log;
	unsigned long log_off;

	req.ops = XMFS_LINK;
	req.p_ino = dir->i_ino;
	req.i_ino = inode->i_ino;
	req.mode = inode->i_mode;
	req.data_pages = 0;
	req.log_entries = 1;
	memcpy(req.name, dentry->d_name.name, dentry->d_name.len + 1);

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

	log_off = grant.log_trunk_addr +
		  offsetof(struct xmfs_log_trunk, log[0]) +
		  grant.log_slot_start * sizeof(struct xmfs_log);
	xm_log.index = 0;
	xm_log.end_index = inode->i_ino;
	xm_log.ops = XMFS_LINK;
	xm_log.data_offset = ktime_get_real_seconds();
	xm_log.mode = inode->i_mode;
	xm_log.offset = ktime_get_real_ns() % NSEC_PER_SEC;
	xm_log.version = grant.version;
	memcpy(xm_log.name, dentry->d_name.name,
	       min(dentry->d_name.len, 255UL));
	if (dentry->d_name.len < 256)
		xm_log.name[dentry->d_name.len] = '\0';
	xm_log.p_ino = dir->i_ino;
	xm_log.i_ino = inode->i_ino;
	xm_log.writer_id = sbi->id;
	xmfs_data_write(sbi, log_off, &xm_log, sizeof(xm_log));

	inc_nlink(inode);
	ihold(inode);

	d_instantiate(dentry, inode);

	return 0;
}

static int xmfs_create(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode, bool excl)
{
	struct xmfs_sb_info *sbi = XMFS_SB(dir->i_sb);

	if (xmfs_quiesce_wait(sbi))
		return -EINTR;
	if (sbi->id != 0)
		return xmfs_do_create_slave(sbi, dir, dentry, mode);
	struct xmfs_log xm_log;
	unsigned long xmlt_addr;
	struct xmfs_sb_index *xmsi;
	int err = 0;
	struct xmfs_inode_info *xmfs_inode;
	const char *name = dentry->d_name.name;
	struct inode *inode;

	spin_lock(&sbi->space_lock);
	if (sbi->full) {
		spin_unlock(&sbi->space_lock);
		return -ENOSPC;
	}

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

	spin_unlock(&sbi->space_lock);
	put_xmsi(sbi, xmsi);
	unsigned long ino = get_next_ino();

	inode = iget5_locked(dir->i_sb, ino, xmfs_inode_eq, xmfs_inode_set,
			     &ino);
	if (!inode)
		return -ENOMEM;

	xmfs_inode = XMFS_I(inode);
	i_size_write(inode, 0);

	inode->i_ino = ino;
	inode_init_owner(idmap, inode, dir, mode | S_IFREG);

	xmfs_init_file_inode(inode);
	/*
	 * xmfs_inode->write_wq = alloc_workqueue("xmfs-prefetch-inode",
	 *					  WQ_UNBOUND | WQ_MEM_RECLAIM,
	 *					  8);
	 */
	err = security_inode_init_security(inode, dir, &dentry->d_name,
					   xmfs_initxattrs, NULL);
	if (err) {
		unlock_new_inode(inode);
		iput(inode);
		return err;
	}
	xmfs_inode->btime = inode->i_mtime = inode->i_atime =
		inode_set_ctime_current(inode);

	dir->i_mtime = inode_set_ctime_current(dir);
	unlock_new_inode(inode);
	d_instantiate(dentry, inode);
	ihold(inode);
	xa_store(&sbi->pinned_inodes, inode->i_ino, inode, GFP_KERNEL);
	XMFS_I(inode)->pinned = true;

	{
		struct xmfs_inode_table *tbl =
			(void *)(sbi->kaddr + INODE_TABLE_OFFSET);
		unsigned long slot = tbl->count;

		tbl->count++;
		{
			struct xmfs_inode_entry *e = &tbl->entries[slot];

			memset(e, 0, sizeof(*e));
			e->i_ino = inode->i_ino;
			e->p_ino = dir->i_ino;
			e->mode = mode | S_IFREG;
			e->nlink = 1;
			e->size = 0;
			e->ctime = inode_get_ctime(inode);
			e->mtime = inode->i_mtime;
			e->atime = inode->i_atime;
			memcpy(e->name, dentry->d_name.name,
			       dentry->d_name.len);
			if (dentry->d_name.len < 256)
				e->name[dentry->d_name.len] = '\0';
		}
	}

	xm_log.index = 0;
	xm_log.end_index = 0;
	xm_log.ops = XMFS_CREATE;
	xm_log.data_offset = inode->i_mtime.tv_sec;
	xm_log.mode = mode | S_IFREG;
	xm_log.offset = inode->i_mtime.tv_nsec;
	memcpy(xm_log.name, name, min(strlen(name), 255UL));
	xm_log.p_ino = parent_ino(dentry);
	xm_log.writer_id = sbi->id;
	xm_log.size = 0;

	xmlt_addr = xmfs_add_log_v3(&xm_log, sbi,
				    min(strlen(name), 255UL),
				    inode->i_ino, true);
	if (xmlt_addr < 0) {
		xmfs_inode->die = true;
		iput(inode);
		dput(dentry);
		return -EINVAL;
	}

	if (sbi->fallocate_size)
		xmfs_common_fallocate(inode, 0x01, 0, sbi->fallocate_size);

	return 0;
}

static int xmfs_do_mknod_slave(struct xmfs_sb_info *sbi, struct inode *dir,
			       struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct xmfs_rw_msg req = {}, grant = {};
	struct inode *inode;
	struct xmfs_inode_info *xmfs_inode;
	int err;
	struct xmfs_log xm_log;
	unsigned long log_off;

	req.ops = XMFS_CREATE;
	req.mode = mode;
	req.p_ino = dir->i_ino;
	req.i_ino = 0;
	req.data_pages = 0;
	req.log_entries = 1;
	memcpy(req.name, dentry->d_name.name, dentry->d_name.len + 1);

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

	log_off = grant.log_trunk_addr +
		  offsetof(struct xmfs_log_trunk, log[0]) +
		  grant.log_slot_start * sizeof(struct xmfs_log);
	memset(&xm_log, 0, sizeof(xm_log));
	xm_log.ops = XMFS_CREATE;
	xm_log.mode = mode;
	xm_log.p_ino = dir->i_ino;
	xm_log.i_ino = grant.assigned_ino;
	xm_log.index = 1;
	xm_log.end_index = dev;
	xm_log.data_offset = ktime_get_real_seconds();
	xm_log.offset = ktime_get_real_ns() % NSEC_PER_SEC;
	xm_log.version = grant.version;
	memcpy(xm_log.name, dentry->d_name.name,
	       min(dentry->d_name.len, 255UL));
	if (dentry->d_name.len < 256)
		xm_log.name[dentry->d_name.len] = '\0';
	xm_log.writer_id = sbi->id;
	xmfs_data_write(sbi, log_off, &xm_log, sizeof(xm_log));

	inode = iget5_locked(dir->i_sb, grant.assigned_ino, xmfs_inode_eq,
			     xmfs_inode_set, &grant.assigned_ino);
	if (!inode)
		return -ENOMEM;

	if (!(inode->i_state & I_NEW))
		return 0;

	xmfs_inode = XMFS_I(inode);
	i_size_write(inode, 0);
	inode->i_ino = grant.assigned_ino;
	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	init_special_inode(inode, mode, dev);
	xmfs_init_special_inode_operations(inode);
	xmfs_inode->btime = inode->i_mtime = inode->i_atime =
		inode_set_ctime_current(inode);
	unlock_new_inode(inode);
	d_instantiate(dentry, inode);
	ihold(inode);
	xa_store(&sbi->pinned_inodes, inode->i_ino, inode, GFP_KERNEL);
	XMFS_I(inode)->pinned = true;

	return 0;
}

static int xmfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct xmfs_sb_info *sbi = XMFS_SB(dir->i_sb);

	if (xmfs_quiesce_wait(sbi))
		return -EINTR;
	if (sbi->id != 0)
		return xmfs_do_mknod_slave(sbi, dir, dentry, mode, dev);
	struct xmfs_log xm_log;
	unsigned long xmlt_addr;
	struct xmfs_sb_index *xmsi;
	int err = 0;
	struct xmfs_inode_info *xmfs_inode;
	const char *name = dentry->d_name.name;
	struct inode *inode;

	spin_lock(&sbi->space_lock);
	if (sbi->full) {
		spin_unlock(&sbi->space_lock);
		return -ENOSPC;
	}

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
	spin_unlock(&sbi->space_lock);
	put_xmsi(sbi, xmsi);
	unsigned long ino = get_next_ino();

	inode = iget5_locked(dir->i_sb, ino, xmfs_inode_eq, xmfs_inode_set,
			     &ino);
	if (!inode)
		return -ENOMEM;

	xmfs_inode = XMFS_I(inode);
	i_size_write(inode, 0);

	inode->i_ino = ino;
	inode_init_owner(idmap, inode, dir, mode);
	init_special_inode(inode, mode, dev);
	xmfs_init_special_inode_operations(inode);
	err = security_inode_init_security(inode, dir, &dentry->d_name,
					   xmfs_initxattrs, NULL);
	if (err) {
		unlock_new_inode(inode);
		iput(inode);
		return err;
	}
	xmfs_inode->btime = inode->i_mtime = inode->i_atime =
		inode_set_ctime_current(inode);

	dir->i_mtime = inode_set_ctime_current(dir);
	unlock_new_inode(inode);
	d_instantiate(dentry, inode);
	ihold(inode);
	xa_store(&sbi->pinned_inodes, inode->i_ino, inode, GFP_KERNEL);
	XMFS_I(inode)->pinned = true;

	{
		struct xmfs_inode_table *tbl =
			(void *)(sbi->kaddr + INODE_TABLE_OFFSET);
		unsigned long slot = tbl->count;

		tbl->count++;
		{
			struct xmfs_inode_entry *e = &tbl->entries[slot];

			memset(e, 0, sizeof(*e));
			e->i_ino = inode->i_ino;
			e->p_ino = dir->i_ino;
			e->mode = mode;
			e->nlink = 1;
			e->size = 0;
			e->ctime = inode_get_ctime(inode);
			e->mtime = inode->i_mtime;
			e->atime = inode->i_atime;
			memcpy(e->name, dentry->d_name.name,
			       dentry->d_name.len);
			if (dentry->d_name.len < 256)
				e->name[dentry->d_name.len] = '\0';
		}
	}

	xm_log.index = 1;
	xm_log.end_index = dev;
	xm_log.ops = XMFS_CREATE;
	xm_log.data_offset = inode->i_mtime.tv_sec;
	xm_log.mode = mode;
	xm_log.offset = inode->i_mtime.tv_nsec;
	memcpy(xm_log.name, name, min(strlen(name), 255UL));
	xm_log.p_ino = parent_ino(dentry);

	xmlt_addr = xmfs_add_log_v3(&xm_log, sbi,
				    min(strlen(name), 255UL),
				    inode->i_ino, true);
	if (xmlt_addr < 0) {
		xmfs_inode->die = true;
		iput(inode);
		dput(dentry);
		return -EINVAL;
	}

	return 0;
}

static int xmfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode)
{
	struct xmfs_sb_info *sbi = XMFS_SB(dir->i_sb);

	if (xmfs_quiesce_wait(sbi))
		return -EINTR;
	if (sbi->id != 0)
		return xmfs_do_mkdir_slave(sbi, dir, dentry, mode);
	struct xmfs_log xm_log;
	struct xmfs_sb_index *xmsi;
	int err = 0;
	struct xmfs_inode_info *xmfs_inode;
	const char *name = dentry->d_name.name;
	struct inode *inode;
	unsigned long xmlt_addr;

	spin_lock(&sbi->space_lock);
	if (sbi->full) {
		spin_unlock(&sbi->space_lock);
		return -ENOSPC;
	}

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
	spin_unlock(&sbi->space_lock);
	put_xmsi(sbi, xmsi);

	unsigned long ino = get_next_ino();

	inode = iget5_locked(dir->i_sb, ino, xmfs_inode_eq, xmfs_inode_set,
			     &ino);
	if (!inode)
		return -ENOMEM;

	xmfs_inode = XMFS_I(inode);
	i_size_write(inode, 4096);

	inode->i_ino = ino;
	inode_init_owner(idmap, inode, dir, S_IFDIR | mode);
	xmfs_init_dir_inode(inode, true);

	xmfs_inode->btime = inode->i_mtime = inode->i_atime =
		inode_set_ctime_current(inode);

	dir->i_mtime = inode_set_ctime_current(dir);

	err = security_inode_init_security(inode, dir, &dentry->d_name,
					   xmfs_initxattrs, NULL);
	if (err) {
		unlock_new_inode(inode);
		iput(inode);
		return err;
	}
	inc_nlink(dir);
	inc_nlink(inode);
	unlock_new_inode(inode);
	d_instantiate(dentry, inode);
	ihold(inode);
	xa_store(&sbi->pinned_inodes, inode->i_ino, inode, GFP_KERNEL);
	XMFS_I(inode)->pinned = true;

	{
		struct xmfs_inode_table *tbl =
			(void *)(sbi->kaddr + INODE_TABLE_OFFSET);
		unsigned long slot = tbl->count;

		tbl->count++;
		{
			struct xmfs_inode_entry *e = &tbl->entries[slot];

			memset(e, 0, sizeof(*e));
			e->i_ino = inode->i_ino;
			e->p_ino = dir->i_ino;
			e->mode = mode | S_IFDIR;
			e->nlink = 1;
			e->size = 4096;
			e->ctime = inode_get_ctime(inode);
			e->mtime = inode->i_mtime;
			e->atime = inode->i_atime;
			memcpy(e->name, dentry->d_name.name,
			       dentry->d_name.len);
			if (dentry->d_name.len < 256)
				e->name[dentry->d_name.len] = '\0';
		}
	}

	xm_log.index = 0;
	xm_log.end_index = 0;
	xm_log.ops = XMFS_MKDIR;
	xm_log.data_offset = dir->i_mtime.tv_sec;
	xm_log.mode = mode | S_IFDIR;
	xm_log.offset = dir->i_mtime.tv_nsec;
	memcpy(xm_log.name, name, min(strlen(name), 255UL));
	xm_log.p_ino = parent_ino(dentry);
	xm_log.size = 4096;

	xmlt_addr = xmfs_add_log_v3(&xm_log, sbi,
				    min(strlen(name), 255UL),
				    inode->i_ino, true);
	if (xmlt_addr < 0) {
		xmfs_inode->die = true;
		drop_nlink(dir);
		drop_nlink(inode);
		iput(inode);
		dput(dentry);
		return 0;
	}

	return 0;
}

static int xmfs_do_rename_slave(struct xmfs_sb_info *sbi, struct inode *old_dir,
				struct dentry *old_dentry,
				struct inode *new_dir,
				struct dentry *new_dentry, unsigned int flags)
{
	struct xmfs_rw_msg req = {}, grant = {};
	const char *old_name = old_dentry->d_name.name;
	const char *new_name = new_dentry->d_name.name;
	unsigned long old_len = old_dentry->d_name.len;
	unsigned long new_len = new_dentry->d_name.len;
	int err;
	struct xmfs_log xm_log;
	unsigned long log_off;

	if (old_len + 1 + new_len + 1 > 256)
		return -ENAMETOOLONG;
	if (flags & ~(RENAME_NOREPLACE))
		return -EINVAL;

	req.ops = XMFS_RENAME;
	req.p_ino = old_dir->i_ino;
	req.i_ino = old_dentry->d_inode->i_ino;
	req.data_pages = 0;
	req.log_entries = 2;
	memcpy(req.name, old_name, old_len + 1);
	memcpy(req.name + old_len + 1, new_name, new_len + 1);

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

	log_off = grant.log_trunk_addr +
		  offsetof(struct xmfs_log_trunk, log[0]) +
		  grant.log_slot_start * sizeof(struct xmfs_log);
	memset(&xm_log, 0, sizeof(xm_log));
	xm_log.ops = XMFS_RENAME;
	xm_log.p_ino = old_dir->i_ino;
	xm_log.i_ino = old_dentry->d_inode->i_ino;
	xm_log.end_index = -1;
	xm_log.version = grant.version;
	memcpy(xm_log.name, old_name, min(old_len, 255UL));
	if (old_len < 256)
		xm_log.name[old_len] = '\0';
	xm_log.writer_id = sbi->id;
	xmfs_data_write(sbi, log_off, &xm_log, sizeof(xm_log));

	log_off = grant.log_trunk_addr +
		  offsetof(struct xmfs_log_trunk, log[0]) +
		  (grant.log_slot_start + 1) * sizeof(struct xmfs_log);
	memset(&xm_log, 0, sizeof(xm_log));
	xm_log.ops = XMFS_RENAME;
	xm_log.p_ino = new_dir->i_ino;
	xm_log.i_ino = old_dentry->d_inode->i_ino;
	xm_log.version = grant.version + 1;
	memcpy(xm_log.name, new_name, min(new_len, 255UL));
	if (new_len < 256)
		xm_log.name[new_len] = '\0';
	xm_log.writer_id = sbi->id;
	xmfs_data_write(sbi, log_off, &xm_log, sizeof(xm_log));

	err = simple_rename(&nop_mnt_idmap, old_dir, old_dentry, new_dir,
			    new_dentry, flags);
	if (err)
		return err;

	return 0;
}

static int xmfs_rename(struct mnt_idmap *idmap, struct inode *old_dir,
		       struct dentry *old_dentry, struct inode *new_dir,
		       struct dentry *new_dentry, unsigned int flags)
{
	/* RENAME_EXCHANGE is disabled because d_exchange() is not exported. */
	if (flags & ~(RENAME_NOREPLACE))
		return -EINVAL;
	struct xmfs_sb_info *sbi = XMFS_SB(old_dir->i_sb);

	if (xmfs_quiesce_wait(sbi))
		return -EINTR;
	if (sbi->id != 0)
		return xmfs_do_rename_slave(sbi, old_dir, old_dentry, new_dir,
					    new_dentry, flags);
	if (XMFS_I(old_dentry->d_inode)->die) {
		xmfs_info(sbi->sb,
			  "inode logs have been interrupted before rename");
		return -EINVAL;
	}
	struct xmfs_sb_index *xmsi;
	int err = 0;

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
	spin_unlock(&sbi->space_lock);
	put_xmsi(sbi, xmsi);

	err = simple_rename(idmap, old_dir, old_dentry, new_dir, new_dentry,
			    flags);
	{
		struct xmfs_inode_table *tbl =
			(void *)(sbi->kaddr + INODE_TABLE_OFFSET);
		unsigned long j;

		for (j = 0; j < tbl->count; j++) {
			if (tbl->entries[j].i_ino ==
				    old_dentry->d_inode->i_ino &&
			    tbl->entries[j].p_ino == old_dir->i_ino &&
			    strcmp(tbl->entries[j].name,
				   old_dentry->d_name.name) == 0) {
				tbl->entries[j].p_ino = new_dir->i_ino;
				memset(tbl->entries[j].name, 0, 256);
				memcpy(tbl->entries[j].name,
				       new_dentry->d_name.name,
				       min(new_dentry->d_name.len,
					   255UL));
				if (new_dentry->d_name.len < 256)
					tbl->entries[j]
						.name[new_dentry->d_name.len] =
						'\0';
				break;
			}
		}
	}
	if (err < 0)
		return err;

	struct xmfs_log xm_log_old;

	xm_log_old.index = flags;
	xm_log_old.ops = XMFS_RENAME;
	xm_log_old.data_offset = old_dir->i_mtime.tv_sec;
	xm_log_old.end_index = -1;

	const char *name = old_dentry->d_name.name;

	memcpy(xm_log_old.name, name, min(strlen(name), 255UL));
	xm_log_old.p_ino = old_dir->i_ino;

	struct xmfs_log xm_log_new;

	xm_log_new.ops = XMFS_RENAME;
	xm_log_new.data_offset = old_dir->i_mtime.tv_nsec;
	xm_log_new.p_ino = new_dir->i_ino;
	xm_log_new.end_index = old_dentry->d_inode->i_ino;

	const char *new_name = new_dentry->d_name.name;

	memcpy(xm_log_new.name, new_name,
	       min(strlen(new_name), 255UL));

	unsigned long addr;

	addr = xmfs_add_log_v3(&xm_log_old, sbi,
			       min(strlen(name), 255UL),
			       old_dentry->d_inode->i_ino, false);
	if (addr < 0) {
		XMFS_I(old_dentry->d_inode)->die = true;
		return 0;
	}
	addr = xmfs_add_log_v3(&xm_log_new, sbi,
			       min(strlen(new_name), 255UL),
			       old_dentry->d_inode->i_ino, true);
	if (addr < 0) {
		XMFS_I(old_dentry->d_inode)->die = true;
		return 0;
	}

	return 0;
}

static int xmfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct xmfs_sb_info *sbi = XMFS_SB(dir->i_sb);

	if (xmfs_quiesce_wait(sbi))
		return -EINTR;
	if (sbi->id != 0)
		return xmfs_do_unlink_slave(sbi, dir, dentry);
	if (XMFS_I(dentry->d_inode)->die) {
		xmfs_info(sbi->sb,
			  "inode logs have been interrupted before unlink");
		return -EINVAL;
	}
	struct xmfs_sb_index *xmsi;

	struct xmfs_log xm_log = { 0 };
	const char *name = dentry->d_name.name;
	unsigned long ino = d_inode(dentry)->i_ino;

	spin_lock(&sbi->space_lock);
	xmsi = get_xmsi(sbi);
	if (!xmsi) {
		spin_unlock(&sbi->space_lock);
		return -EINVAL;
	}
	/*
	 * int logcount = 0;
	 *
	 * if (logcount + xmsi->used_trunk_count > sbi->max_chunk) {
	 *	sbi->full = true;
	 *	spin_unlock(&sbi->space_lock);
	 *	put_xmsi(sbi, xmsi);
	 *	return -ENOSPC;
	 * }
	 */
	spin_unlock(&sbi->space_lock);
	put_xmsi(sbi, xmsi);

	if (d_inode(dentry)->__i_nlink > 1)
		xm_log.index = -1;
	else
		xm_log.index = 0;
	xm_log.end_index = -1;
	xm_log.ops = XMFS_DELETE;
	xm_log.data_offset = dir->i_mtime.tv_sec;
	xm_log.offset = dir->i_mtime.tv_nsec;
	memcpy(xm_log.name, name, min(strlen(name), 255UL));
	xm_log.p_ino = parent_ino(dentry);

	unsigned long addr =
		xmfs_add_log_v3(&xm_log, sbi,
				min(strlen(name), 255UL), ino,
				true);
	if (addr < 0) {
		XMFS_I(d_inode(dentry))->die = true;
		return 0;
	}

	spin_lock(&sbi->space_lock);
	xmsi = get_xmsi(sbi);
	if (xmsi)
		put_xmsi(sbi, xmsi);

	spin_unlock(&sbi->space_lock);

	if (d_inode(dentry)->i_nlink > 0)
		drop_nlink(d_inode(dentry));
	{
		struct xmfs_inode_table *tbl =
			(void *)(sbi->kaddr + INODE_TABLE_OFFSET);
		bool removed;

		spin_lock(&sbi->space_lock);
		removed = xmfs_inode_table_remove(tbl, ino, dir->i_ino, name);
		xmfs_inode_table_sync_nlink(tbl, ino);
		spin_unlock(&sbi->space_lock);
		if (!removed)
			xmfs_warning(sbi->sb, "inode %lu entry %s not found", ino,
				     name);
	}
	if (!d_inode(dentry)->i_nlink)
		xmfs_unpin_inode(sbi, d_inode(dentry));

	return 0;
}

static int xmfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	if (!simple_empty(dentry))
		return -ENOTEMPTY;

	drop_nlink(d_inode(dentry));
	int err = xmfs_unlink(dir, dentry);

	if (!err)
		drop_nlink(dir);

	return err;
}

static struct dentry *xmfs_lookup(struct inode *dir, struct dentry *dentry,
				  unsigned int flags)
{
	struct xmfs_sb_info *sbi = XMFS_SB(dir->i_sb);
	struct xmfs_sb_index *xmsi;
	struct dentry *p_dentry, *res = NULL;
	struct xmfs_inode_table *tbl;
	unsigned long max_entries, i;
	struct inode *inode;

	if (dentry->d_name.len > NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);

	xmsi = kmalloc(sizeof(struct xmfs_sb_index), GFP_KERNEL);
	if (get_xmsi_and_copy(sbi, xmsi)) {
		kfree(xmsi);
		return ERR_PTR(-EIO);
	}
	/*
	 * xmfs_info(sbi->sb, "qyf id %ld lookup %ld %ld", sbi->id,
	 *	     xmsi->last_update, sbi->last_update);
	 */
	sbi->stop_cnt++;
	if (xmsi->last_update != sbi->last_update)
		xmfs_replay_new_entries(sbi, NULL, xmsi->last_update);
	kfree(xmsi);

	struct qstr qname = QSTR_INIT(dentry->d_name.name, dentry->d_name.len);

	hlist_for_each_entry(p_dentry, &dir->i_dentry, d_u.d_alias) {
		res = d_hash_and_lookup(p_dentry, &qname);
		if (res) {
			sbi->stop_cnt--;
			return res;
		}
	}

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
	/*
	 * xmfs_info(sbi->sb, "qyf slave lookup find inode table %d",
	 *	     tbl->count);
	 */
	if (tbl && tbl->magic == XMFS_INODE_TABLE_MAGIC) {
		max_entries = (INODE_TABLE_SIZE - 4096) /
			      sizeof(struct xmfs_inode_entry);
		for (i = 0; i < tbl->count && i < max_entries; i++) {
			struct xmfs_inode_entry *e = &tbl->entries[i];
			/*
			 * xmfs_info(sbi->sb,
			 *	     "qyf slave lookup iter inode table %d %ld %ld %s %s",
			 *	     i, e->p_ino, dir->i_ino, e->name,
			 *	     dentry->d_name.name);
			 */
			if (e->p_ino != dir->i_ino ||
			    strcmp(e->name, dentry->d_name.name) != 0)
				continue;
			inode = ilookup5(sbi->sb, e->i_ino, xmfs_inode_eq,
					 &e->i_ino);
			/* xmfs_info(sbi->sb, "qyf find inode? %p", inode); */
			if (!inode)
				inode = iget5_locked(sbi->sb, e->i_ino,
						     xmfs_inode_eq,
						     xmfs_inode_set, &e->i_ino);
			if (inode && (inode->i_state & I_NEW)) {
				struct xmfs_inode_info *xmfs_inode =
					XMFS_I(inode);
				xmfs_inode->leaf = (e->i_ino != 0);
				inode->i_ino = e->i_ino;
				inode->i_mode = e->mode;
				if (S_ISREG(e->mode)) {
					/*
					 * xmfs_inode->write_wq = alloc_workqueue(
					 *	"xmfs-prefetch-inode",
					 *	WQ_UNBOUND | WQ_MEM_RECLAIM, 8);
					 */
					xmfs_init_file_inode(inode);
				} else if (S_ISDIR(e->mode)) {
					xmfs_init_dir_inode(inode,
							    sbi->id == 0);
				} else if (S_ISLNK(e->mode)) {
					xmfs_init_symlink_operations(inode);
				} else {
					init_special_inode(inode, e->mode, 0);
				}
				unlock_new_inode(inode);
				ihold(inode);
				xa_store(&sbi->pinned_inodes, inode->i_ino,
					 inode, GFP_KERNEL);
				XMFS_I(inode)->pinned = true;
			}

			if (inode) {
				inode_lock(inode);
				set_nlink(inode, e->nlink);
				i_size_write(inode, e->size);
				inode->__i_ctime.tv_sec = e->ctime.tv_sec;
				inode->__i_ctime.tv_nsec = e->ctime.tv_nsec;
				inode->i_mtime.tv_sec = e->mtime.tv_sec;
				inode->i_mtime.tv_nsec = e->mtime.tv_nsec;
				inode->i_atime.tv_sec = e->atime.tv_sec;
				inode->i_atime.tv_nsec = e->atime.tv_nsec;
				inode_unlock(inode);
				res = d_splice_alias(inode, dentry);
			}
			break;
		}
	}
	if (tbl && sbi->mode == XMFS_URMA_MODE)
		put_meta(sbi, tbl);

	sbi->stop_cnt--;
	return res;
}

static const struct inode_operations xmfs_dir_inode_operations = {
	.create		= xmfs_create,
	.lookup		= xmfs_lookup,
	.link		= xmfs_link,
	.unlink		= xmfs_unlink,
	.symlink	= xmfs_symlink,
	.mkdir		= xmfs_mkdir,
	.rmdir		= xmfs_rmdir,
	.mknod		= xmfs_mknod,
	.rename		= xmfs_rename,
	.setattr	= xmfs_slave_setattr,
	.getattr	= xmfs_slave_getattr,
	.update_time	= xmfs_update_time,
	.listxattr	= xmfs_listxattr,
};

static int xmfs_tmpfile(struct mnt_idmap *idmap, struct inode *dir,
			struct file *file, umode_t mode)
{
	struct xmfs_sb_info *sbi = XMFS_SB(dir->i_sb);
	struct xmfs_sb_index *xmsi;
	int err = 0;
	struct xmfs_inode_info *xmfs_inode;
	struct dentry *dentry = file->f_path.dentry;
	const char *name = dentry->d_name.name;
	struct xmfs_log xm_log;
	struct inode *inode;

	spin_lock(&sbi->space_lock);
	if (sbi->full) {
		spin_unlock(&sbi->space_lock);
		return -ENOSPC;
	}

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
	spin_unlock(&sbi->space_lock);
	put_xmsi(sbi, xmsi);
	unsigned long ino = get_next_ino();

	inode = iget5_locked(dir->i_sb, ino, xmfs_inode_eq, xmfs_inode_set,
			     &ino);
	if (!inode)
		return -ENOMEM;

	xmfs_inode = XMFS_I(inode);
	xmfs_inode->tmp = true;

	i_size_write(inode, 0);

	inode->i_ino = ino;
	inode_init_owner(idmap, inode, dir, mode);
	xmfs_init_file_inode(inode);
	/*
	 * xmfs_inode->write_wq = alloc_workqueue("xmfs-prefetch-inode",
	 *					  WQ_UNBOUND | WQ_MEM_RECLAIM,
	 *					  8);
	 */
	err = security_inode_init_security(inode, dir, &dentry->d_name,
					   xmfs_initxattrs, NULL);
	if (err) {
		unlock_new_inode(inode);
		iput(inode);
		return err;
	}
	xmfs_inode->btime = inode->i_mtime = inode->i_atime =
		inode_set_ctime_current(inode);
	dir->i_mtime = inode_set_ctime_current(dir);
	unlock_new_inode(inode);
	d_tmpfile(file, inode);
	ihold(inode);
	xa_store(&sbi->pinned_inodes, inode->i_ino, inode, GFP_KERNEL);
	XMFS_I(inode)->pinned = true;

	err = finish_open_simple(file, 0);

	xm_log.index = 0;
	xm_log.end_index = 0;
	xm_log.ops = XMFS_CREATE;
	xm_log.data_offset = dir->i_mtime.tv_sec;
	xm_log.mode = mode;
	xm_log.offset = dir->i_mtime.tv_nsec;
	memcpy(xm_log.name, name, min(strlen(name), 255UL));
	xm_log.p_ino = parent_ino(dentry);

	unsigned long xmlt_addr =
		xmfs_add_log_v3(&xm_log, sbi,
				min(strlen(name), 255UL),
				inode->i_ino, true);
	if (xmlt_addr < 0) {
		xmfs_inode->die = true;
		iput(inode);
		return -EINVAL;
	}

	return err;
}

static const struct inode_operations xmfs_host_dir_inode_operations = {
	.create		= xmfs_create,
	.lookup		= xmfs_lookup,
	.link		= xmfs_link,
	.unlink		= xmfs_unlink,
	.symlink	= xmfs_symlink,
	.mkdir		= xmfs_mkdir,
	.rmdir		= xmfs_rmdir,
	.mknod		= xmfs_mknod,
	.rename		= xmfs_rename,
	.setattr	= xmfs_setattr,
	.tmpfile	= xmfs_tmpfile,
	.listxattr	= xmfs_listxattr,
	.getattr	= xmfs_getattr,
	.update_time	= xmfs_update_time,
};

struct inode *xmfs_get_root(struct super_block *sb, int host_id)
{
	struct inode *inode;

	inode = new_inode(sb);
	if (inode) {
		inode->i_ino = 0;
		inode->i_mode = S_IFDIR | 0755;
		inode->i_uid = make_kuid(current_user_ns(), 0);
		inode->i_gid = make_kgid(current_user_ns(), 0);
		XMFS_I(inode)->btime = inode->i_atime = inode->i_mtime =
			inode_set_ctime_current(inode);
		if (host_id == 0)
			xmfs_init_dir_inode(inode, true);
		else
			xmfs_init_dir_inode(inode, false);

		/* directory inodes start off with i_nlink == 2 (for "." entry) */
		inc_nlink(inode);
		lockdep_annotate_inode_mutex_key(inode);
		XMFS_I(inode)->leaf = false;
		XMFS_I(inode)->i_ino = 0;
		XMFS_I(inode)->tmp = false;
		XMFS_I(inode)->die = false;
		XMFS_I(inode)->log_start = 0;

		xmfs_simple_xattrs_init(&XMFS_I(inode)->xattrs);

		i_size_write(inode, 4096);
	}
	return inode;
}

void xmfs_init_dir_inode(struct inode *inode, bool host)
{
	inode->i_op = host ? &xmfs_host_dir_inode_operations :
			     &xmfs_dir_inode_operations;
	xmfs_init_dir_file(inode);
}
