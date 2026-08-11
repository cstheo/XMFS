// SPDX-License-Identifier: GPL-2.0-only
/*
 * XMFS: Log Structured Filesystem for UB
 * Copyright (C) 2024. Huawei Technologies Co., Ltd
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

#include "xmfs_i.h"

#include <linux/pagemap.h>

static void xmfs_put_link(void *arg)
{
	/* xmfs_info(NULL, "qyf put folio %p", arg); */
	folio_put(arg);
}

static void xmfs_put_page(void *arg)
{
	/* xmfs_info(NULL, "qyf put page %p", page_address(arg)); */
	put_page(arg);
}

static const char *xmfs_get_link(struct dentry *dentry, struct inode *inode,
				 struct delayed_call *done)
{
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);

	if (!dentry) {
		struct folio *folio = filemap_get_folio(inode->i_mapping, 0);

		if (IS_ERR(folio))
			return ERR_PTR(-ECHILD);
		set_delayed_call(done, xmfs_put_link, folio);
		return folio_address(folio);
	}
	struct folio *folio = filemap_get_folio(inode->i_mapping, 0);

	if (IS_ERR(folio)) {
		struct xmfs_log *ulog = get_log(sbi, XMFS_I(inode)->symname);
		struct page *page = alloc_page(GFP_KERNEL);
		char *ret = kmap_local_page(page);

		if (ulog->end_index > sbi->len) {
			xmfs_info(inode->i_sb, "get link wrong %ld %ld",
				  inode->i_ino, XMFS_I(inode)->symname);
		}
		if (!ulog->end_index) {
			memcpy(ret, ulog->name, strlen(ulog->name) + 1);
		} else {
			memcpy(ret, get_data(sbi, ulog->end_index, ulog->index),
			       ulog->index);
		}
		kunmap_local(ret);
		set_page_dirty(page);

		set_delayed_call(done, xmfs_put_page, page);
		if (!ret || !page_address(page)) {
			xmfs_info(inode->i_sb, "get link 1 return null %s",
				  dentry->d_name.name);
		}
		return page_address(page);
	}

	set_delayed_call(done, xmfs_put_link, folio);
	return folio_address(folio);
}

static const struct inode_operations xmfs_symlink_inode_operations = {
	.setattr	= xmfs_setattr,
	.getattr	= xmfs_getattr,
	.get_link	= xmfs_get_link,
	.listxattr	= xmfs_listxattr,
	.update_time	= xmfs_update_time,
};

void xmfs_init_symlink_mapping(struct inode *inode)
{
	xmfs_init_file_mapping(inode);
}

void xmfs_init_symlink_operations(struct inode *inode)
{
	inode->i_op = &xmfs_symlink_inode_operations;
}
