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
#include <linux/slab.h>

static int xmfs_revalidate(struct dentry *dentry, unsigned int flags)
{
	struct xmfs_sb_info *sbi = XMFS_SB(dentry->d_sb);
	struct xmfs_sb_index *xmsi = kmalloc(sizeof(*xmsi), GFP_ATOMIC);
	int err = get_xmsi_and_copy(sbi, xmsi);

	if (unlikely(err)) {
		xmfs_error(sbi->sb,
			   "hardware memory error when reading superblock during revalidate");
		kfree(xmsi);
		return err;
	}
	/*
	 * xmfs_info(sbi->sb, "qyf revalidate %ld %ld",
	 *	     xmsi->last_update, sbi->last_update);
	 */
	if (xmsi->last_update != sbi->last_update) {
		kfree(xmsi);
		return 0;
	}
	kfree(xmsi);
	return 1;
}

static const struct dentry_operations xmfs_dentry_operations = {
	.d_delete	= always_delete_dentry,
	.d_revalidate	= xmfs_revalidate,
};

static int xmfs_iterate(struct file *file, struct dir_context *ctx)
{
	struct xmfs_sb_info *sbi = XMFS_SB(file->f_inode->i_sb);
	struct inode *dir = file->f_inode;
	struct xmfs_inode_table *tbl;
	unsigned long max_entries, i __maybe_unused;

	if (!dir_emit_dots(file, ctx))
		return 0;

	{
		struct xmfs_sb_index *xmsi = kmalloc(sizeof(*xmsi), GFP_KERNEL);

		if (xmsi) {
			int err = get_xmsi_and_copy(sbi, xmsi);

			if (!err && xmsi->last_update != sbi->last_update)
				xmfs_replay_new_entries(sbi, NULL,
							xmsi->last_update);
			kfree(xmsi);
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

	if (!tbl || tbl->magic != XMFS_INODE_TABLE_MAGIC) {
		if (tbl && sbi->mode == XMFS_URMA_MODE)
			put_meta(sbi, tbl);
		return 0;
	}
	/* xmfs_info(sbi->sb, "qyf iterate %d %d", tbl->count, ctx->pos); */
	/*
	 * for (i = 0; i < tbl->count; i++) {
	 *	struct xmfs_inode_entry *e = &tbl->entries[i];
	 *
	 *	xmfs_info(sbi->sb,
	 *		  "tbl id %d ino %d pino %d name %s size %d",
	 *		  i, e->i_ino, e->p_ino, e->name, e->size);
	 * }
	 */
	if (ctx->pos - 2 >= tbl->count)
		return 0;
	max_entries =
		(INODE_TABLE_SIZE - 4096) / sizeof(struct xmfs_inode_entry);
	for (; ctx->pos - 2 < tbl->count && ctx->pos - 2 < max_entries;
	     ctx->pos++) {
		struct xmfs_inode_entry *e = &tbl->entries[ctx->pos - 2];
		unsigned char d_type;
		/*
		 * xmfs_info(sbi->sb,
		 *	     "qyf iterate iter %d %d name: %s size %ld",
		 *	     e->i_ino, e->p_ino, e->name, e->size);
		 */
		if (e->i_ino == 0 || e->p_ino != dir->i_ino)
			continue;

		if (S_ISREG(e->mode))
			d_type = DT_REG;
		else if (S_ISDIR(e->mode))
			d_type = DT_DIR;
		else if (S_ISLNK(e->mode))
			d_type = DT_LNK;
		else if (S_ISCHR(e->mode))
			d_type = DT_CHR;
		else if (S_ISBLK(e->mode))
			d_type = DT_BLK;
		else if (S_ISFIFO(e->mode))
			d_type = DT_FIFO;
		else if (S_ISSOCK(e->mode))
			d_type = DT_SOCK;
		else
			d_type = DT_UNKNOWN;

		if (!dir_emit(ctx, e->name, strlen(e->name), e->i_ino, d_type))
			break;
	}

	if (sbi->mode == XMFS_URMA_MODE)
		put_meta(sbi, tbl);
	return 0;
}

static const struct file_operations xmfs_dir_operations = {
	.iterate_shared	= xmfs_iterate,
	.open		= dcache_dir_open,
	.release	= dcache_dir_close,
	.llseek		= dcache_dir_lseek,
	.read		= generic_read_dir,
	.fsync		= noop_fsync,
};

void xmfs_init_dir_file(struct inode *inode)
{
	inode->i_fop = &xmfs_dir_operations;
}

void xmfs_init_dentry_operations(struct super_block *sb)
{
	sb->s_d_op = &xmfs_dentry_operations;
}
