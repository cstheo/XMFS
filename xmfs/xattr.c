// SPDX-License-Identifier: GPL-2.0-only
/*
 * XMFS: Log Structured Filesystem for UB
 * Copyright (C) 2024. Huawei Technologies Co., Ltd
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

#include "xmfs_i.h"

void xmfs_simple_xattr_free(struct simple_xattr *xattr)
{
	if (xattr)
		kfree(xattr->name);
	kvfree(xattr);
}

size_t xmfs_simple_xattr_space(const char *name, size_t size)
{
	/*
	 * Use "40" instead of sizeof(struct simple_xattr), to return the
	 * same result on 32-bit and 64-bit, and even if simple_xattr grows.
	 */
	return 40 + size + strlen(name);
}

void xmfs_simple_xattrs_free(struct simple_xattrs *xattrs, size_t *freed_space)
{
	struct rb_node *rbp;

	if (freed_space)
		*freed_space = 0;
	rbp = rb_first(&xattrs->rb_root);
	while (rbp) {
		struct simple_xattr *xattr;
		struct rb_node *rbp_next;

		rbp_next = rb_next(rbp);
		xattr = rb_entry(rbp, struct simple_xattr, rb_node);
		rb_erase(&xattr->rb_node, &xattrs->rb_root);
		if (freed_space)
			*freed_space += xmfs_simple_xattr_space(xattr->name,
								xattr->size);
		xmfs_simple_xattr_free(xattr);
		rbp = rbp_next;
	}
}

void xmfs_simple_xattrs_init(struct simple_xattrs *xattrs)
{
	xattrs->rb_root = RB_ROOT;
	rwlock_init(&xattrs->lock);
}

struct simple_xattr *xmfs_simple_xattr_alloc(const void *value, size_t size)
{
	struct simple_xattr *new_xattr;
	size_t len;

	/* wrap around? */
	len = sizeof(*new_xattr) + size;
	if (len < sizeof(*new_xattr))
		return NULL;

	new_xattr = kvmalloc(len, GFP_KERNEL_ACCOUNT);
	if (!new_xattr)
		return NULL;

	new_xattr->size = size;
	memcpy(new_xattr->value, value, size);
	return new_xattr;
}

static int rbtree_simple_xattr_cmp(const void *key, const struct rb_node *node)
{
	const char *xattr_name = key;
	const struct simple_xattr *xattr;

	xattr = rb_entry(node, struct simple_xattr, rb_node);
	return strcmp(xattr->name, xattr_name);
}

struct simple_xattr *xmfs_simple_xattr_set(struct simple_xattrs *xattrs,
					   const char *name, const void *value,
					   size_t size, int flags)
{
	struct simple_xattr *old_xattr = NULL, *new_xattr = NULL;
	struct rb_node *parent = NULL, **rbp;
	int err = 0, ret;

	/* value == NULL means remove */
	if (value) {
		new_xattr = xmfs_simple_xattr_alloc(value, size);
		if (!new_xattr)
			return ERR_PTR(-ENOMEM);

		new_xattr->name = kstrdup(name, GFP_KERNEL_ACCOUNT);
		if (!new_xattr->name) {
			xmfs_simple_xattr_free(new_xattr);
			return ERR_PTR(-ENOMEM);
		}
	}

	write_lock(&xattrs->lock);
	rbp = &xattrs->rb_root.rb_node;
	while (*rbp) {
		parent = *rbp;
		ret = rbtree_simple_xattr_cmp(name, *rbp);
		if (ret < 0)
			rbp = &(*rbp)->rb_left;
		else if (ret > 0)
			rbp = &(*rbp)->rb_right;
		else
			old_xattr =
				rb_entry(*rbp, struct simple_xattr, rb_node);
		if (old_xattr)
			break;
	}

	if (old_xattr) {
		/* Fail if XATTR_CREATE is requested and the xattr exists. */
		if (flags & XATTR_CREATE) {
			err = -EEXIST;
			goto out_unlock;
		}

		if (new_xattr)
			rb_replace_node(&old_xattr->rb_node,
					&new_xattr->rb_node, &xattrs->rb_root);
		else
			rb_erase(&old_xattr->rb_node, &xattrs->rb_root);
	} else {
		/* Fail if XATTR_REPLACE is requested but no xattr is found. */
		if (flags & XATTR_REPLACE) {
			err = -ENODATA;
			goto out_unlock;
		}

		/*
		 * If XATTR_CREATE or no flags are specified together with a
		 * new value simply insert it.
		 */
		if (new_xattr) {
			rb_link_node(&new_xattr->rb_node, parent, rbp);
			rb_insert_color(&new_xattr->rb_node, &xattrs->rb_root);
		}

		/*
		 * If XATTR_CREATE or no flags are specified and neither an
		 * old or new xattr exist then we don't need to do anything.
		 */
	}

out_unlock:
	write_unlock(&xattrs->lock);
	if (!err)
		return old_xattr;
	xmfs_simple_xattr_free(new_xattr);
	return ERR_PTR(err);
}

int xmfs_simple_xattr_get(struct simple_xattrs *xattrs, const char *name,
			  void *buffer, size_t size)
{
	struct simple_xattr *xattr = NULL;
	struct rb_node *rbp;
	int ret = -ENODATA;

	read_lock(&xattrs->lock);
	rbp = rb_find(name, &xattrs->rb_root, rbtree_simple_xattr_cmp);
	if (rbp) {
		xattr = rb_entry(rbp, struct simple_xattr, rb_node);
		ret = xattr->size;
		if (buffer) {
			if (size < xattr->size)
				ret = -ERANGE;
			else
				memcpy(buffer, xattr->value, xattr->size);
		}
	}
	read_unlock(&xattrs->lock);
	return ret;
}

static bool xattr_is_trusted(const char *name)
{
	return !strncmp(name, XATTR_TRUSTED_PREFIX, XATTR_TRUSTED_PREFIX_LEN);
}

int xmfs_xattr_list_one(char **buffer, ssize_t *remaining_size,
			const char *name)
{
	size_t len;

	len = strlen(name) + 1;
	if (*buffer) {
		if (*remaining_size < len)
			return -ERANGE;
		memcpy(*buffer, name, len);
		*buffer += len;
	}
	*remaining_size -= len;
	return 0;
}

ssize_t xmfs_simple_xattr_list(struct inode *inode,
			       struct simple_xattrs *xattrs, char *buffer,
			       size_t size)
{
	bool trusted = ns_capable_noaudit(&init_user_ns, CAP_SYS_ADMIN);
	struct simple_xattr *xattr;
	struct rb_node *rbp;
	ssize_t remaining_size = size;
	int err = 0;

	read_lock(&xattrs->lock);
	for (rbp = rb_first(&xattrs->rb_root); rbp; rbp = rb_next(rbp)) {
		xattr = rb_entry(rbp, struct simple_xattr, rb_node);

		/* skip "trusted." attributes for unprivileged callers */
		if (!trusted && xattr_is_trusted(xattr->name))
			continue;

		err = xmfs_xattr_list_one(&buffer, &remaining_size,
					  xattr->name);
		if (err)
			break;
	}
	read_unlock(&xattrs->lock);

	return err ? err : size - remaining_size;
}

static unsigned long xmfs_xattr_write_value(struct inode *inode,
					    const char *name, const void *value,
					    size_t size,
					    struct xmfs_log *xm_log,
					    struct xmfs_sb_index *xmsi)
{
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);

	if (xmfs_quiesce_wait(sbi))
		return 0;
	unsigned long data_current = xmsi->data_current;
	unsigned long data_count = xmsi->data_count;
	long slot;

	WARN_ON(xmsi->count == 0);

	if (data_current == 0 || data_count == 511) {
		slot = xmfs_alloc_and_mark_slot(xmsi, sbi, 'd');
		if (slot < 0)
			return -ENOSPC;
		xmsi->data_current =
			DATA_AREA_OFFSET + slot * sbi->trunk_size + PAGE_SIZE;
		xmsi->data_count = 0;
	} else {
		xmsi->data_current = data_current + PAGE_SIZE;
	}
	xm_log->data_offset = xmsi->data_current;
	xmsi->data_count = xmsi->data_count + 1;

	xmfs_data_write(sbi, xmsi->data_current, value, size);
	xmfs_mark_page_live(sbi, xmsi->data_current);

	return 0;
}

static int xmfs_xattr_set_log(struct inode *inode, const char *name,
			      const void *value, size_t size, int flags)
{
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);

	if (xmfs_quiesce_wait(sbi))
		return -EINTR;
	struct xmfs_inode_info *xmfs_inode = XMFS_I(inode);
	struct xmfs_log xm_log = { 0 };

	xm_log.index = size;
	xm_log.end_index = strlen(name);
	xm_log.ops = XMFS_XATTR;
	xm_log.size = flags;
	xm_log.p_ino = inode->__i_ctime.tv_sec;
	xm_log.offset = inode->__i_ctime.tv_nsec;
	memcpy(xm_log.name, name, strlen(name));

	spin_lock(&sbi->space_lock);
	if (xmfs_inode->die) {
		xmfs_info(inode->i_sb,
			  "inode logs have been interrupted before set xattr");
		spin_unlock(&sbi->space_lock);
		return -EINVAL;
	}

	struct xmfs_sb_index *xmsi = get_xmsi(sbi);

	if (!xmsi) {
		spin_unlock(&sbi->space_lock);
		return -EINVAL;
	}
	int logcount = 1;
	int need_data_trunk =
		(xmsi->data_count == 511 || xmsi->data_current == 0) ? 1 : 0;
	if (xmsi->used_trunk_count + logcount + need_data_trunk >
	    sbi->max_chunk) {
		sbi->full = true;
		spin_unlock(&sbi->space_lock);
		put_xmsi(sbi, xmsi);
		return -ENOSPC;
	}

	xmfs_xattr_write_value(inode, name, value, size, &xm_log, xmsi);
	spin_unlock(&sbi->space_lock);
	put_xmsi(sbi, xmsi);
	unsigned long xmlt_addr =
		xmfs_add_log_v3(&xm_log, sbi, strlen(name), inode->i_ino, true);
	if (xmlt_addr < 0) {
		xmfs_inode->die = true;
		return -EINVAL;
	}

	return 0;
}

int xmfs_initxattrs(struct inode *inode, const struct xattr *xattr_array,
		    void *fs_info)
{
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
	const struct xattr *xattr;
	struct simple_xattr *new_xattr;
	struct xmfs_inode_info *info = XMFS_I(inode);
	size_t len;
	int err = 0;

	for (xattr = xattr_array; xattr->name != NULL; xattr++) {
		new_xattr =
			xmfs_simple_xattr_alloc(xattr->value, xattr->value_len);
		if (!new_xattr)
			break;

		len = strlen(xattr->name) + 1;
		if ((len + XATTR_SECURITY_PREFIX_LEN) > 255 ||
		    new_xattr->size > PAGE_SIZE) {
			continue;
		}
		new_xattr->name = kmalloc(XATTR_SECURITY_PREFIX_LEN + len,
					  GFP_KERNEL_ACCOUNT);
		if (!new_xattr->name) {
			kvfree(new_xattr);
			break;
		}

		memcpy(new_xattr->name, XATTR_SECURITY_PREFIX,
		       XATTR_SECURITY_PREFIX_LEN);
		memcpy(new_xattr->name + XATTR_SECURITY_PREFIX_LEN, xattr->name,
		       len);

		new_xattr = xmfs_simple_xattr_set(&info->xattrs,
						  new_xattr->name,
						  new_xattr->value,
						  new_xattr->size, 0);
		if (!IS_ERR(new_xattr)) {
			inode_set_ctime_current(inode);
			if (sbi->id == 0) {
				err = xmfs_xattr_set_log(inode, new_xattr->name,
							 new_xattr->value,
							 new_xattr->size, 0);
				if (err < 0) {
					xmfs_simple_xattr_free(new_xattr);
					break;
				}
			}
			xmfs_simple_xattr_free(new_xattr);
		} else {
			break;
		}
	}

	if (xattr->name != NULL) {
		xmfs_simple_xattrs_free(&info->xattrs, NULL);
		return -ENOMEM;
	}

	return err;
}

static int xmfs_xattr_slave_get(const struct xattr_handler *handler,
				struct dentry *unused, struct inode *inode,
				const char *name, void *buffer, size_t size)
{
	struct xmfs_inode_info *info = XMFS_I(inode);

	if (inode->i_ino == 0) {
		struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
		struct xmfs_sb_index *xmsi =
			kmalloc(sizeof(struct xmfs_sb_index), GFP_KERNEL);
		int err = get_xmsi_and_copy(sbi, xmsi);

		if (unlikely(err)) {
			xmfs_error(inode->i_sb,
				   "hardware memory error when reading superblock during slave dir iterate");
			kfree(xmsi);
			return err;
		}

		sbi->stop_cnt++;
		if (xmsi->last_update != sbi->last_update) {
			if (!sbi->debug)
				xmfs_replay_new_entries(sbi, NULL,
							xmsi->last_update);
			else
				xmfs_replay_new_entries(sbi, NULL,
							xmsi->last_update);
		}
		kfree(xmsi);
	}
	name = xattr_full_name(handler, name);
	return xmfs_simple_xattr_get(&info->xattrs, name, buffer, size);
}

static int xmfs_xattr_get(const struct xattr_handler *handler,
			  struct dentry *unused, struct inode *inode,
			  const char *name, void *buffer, size_t size)
{
	struct xmfs_inode_info *info = XMFS_I(inode);

	name = xattr_full_name(handler, name);
	return xmfs_simple_xattr_get(&info->xattrs, name, buffer, size);
}

static int xmfs_xattr_set(const struct xattr_handler *handler,
			  struct mnt_idmap *idmap, struct dentry *unused,
			  struct inode *inode, const char *name,
			  const void *value, size_t size, int flags)
{
	struct simple_xattr *old_xattr;
	struct xmfs_inode_info *info = XMFS_I(inode);
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
	int err = 0;

	name = xattr_full_name(handler, name);
	if (strlen(name) > 255 || size > PAGE_SIZE)
		return -EINVAL;

	old_xattr =
		xmfs_simple_xattr_set(&info->xattrs, name, value, size, flags);
	if (!IS_ERR(old_xattr)) {
		inode_set_ctime_current(inode);
		if (sbi->id == 0)
			err = xmfs_xattr_set_log(inode, name, value, size,
						 flags);
		xmfs_simple_xattr_free(old_xattr);
		old_xattr = NULL;
	}

	return err == 0 ? PTR_ERR(old_xattr) : err;
}

ssize_t xmfs_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
	struct xmfs_inode_info *info = XMFS_I(d_inode(dentry));

	return xmfs_simple_xattr_list(d_inode(dentry), &info->xattrs, buffer,
				      size);
}

static const struct xattr_handler xmfs_security_xattr_handler = {
	.prefix	= XATTR_SECURITY_PREFIX,
	.get	= xmfs_xattr_get,
	.set	= xmfs_xattr_set,
};

static const struct xattr_handler xmfs_trusted_xattr_handler = {
	.prefix	= XATTR_TRUSTED_PREFIX,
	.get	= xmfs_xattr_get,
	.set	= xmfs_xattr_set,
};

static const struct xattr_handler xmfs_user_xattr_handler = {
	.prefix	= XATTR_USER_PREFIX,
	.get	= xmfs_xattr_get,
	.set	= xmfs_xattr_set,
};

static int xmfs_xattr_set_slave(const struct xattr_handler *handler,
				struct mnt_idmap *idmap, struct dentry *dentry,
				struct inode *inode, const char *name,
				const void *value, size_t size, int flags)
{
	struct xmfs_sb_info *sbi = XMFS_SB(inode->i_sb);
	struct xmfs_rw_msg req = {}, grant = {};
	struct xmfs_log xm_log;
	unsigned long log_off;
	unsigned long pages;
	int err;

	if (!sbi || sbi->id == 0)
		return -EINVAL;

	if (value && size > 0)
		pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	else
		pages = 0;

	req.ops = XMFS_XATTR;
	req.i_ino = inode->i_ino;
	req.data_pages = pages;
	req.log_entries = 1;
	req.size = size;
	req.mode = flags;
	snprintf(req.name, 256, "%s%s", handler->prefix, name);

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

	if (value && size > 0 && grant.data_addr != 0)
		xmfs_data_write(sbi, grant.data_addr, (void *)value, size);

	log_off = grant.log_trunk_addr +
		  offsetof(struct xmfs_log_trunk, log[0]) +
		  grant.log_slot_start * sizeof(struct xmfs_log);
	memset(&xm_log, 0, sizeof(xm_log));
	xm_log.ops = XMFS_XATTR;
	xm_log.i_ino = inode->i_ino;
	xm_log.version = grant.version;
	xm_log.data_offset = grant.data_addr;
	xm_log.size = size;
	xm_log.mode = flags;
	xm_log.writer_id = sbi->id;
	snprintf(xm_log.name, 256, "%s%s", handler->prefix, name);
	xmfs_data_write(sbi, log_off, &xm_log, sizeof(xm_log));

	{
		const char *full_name = xattr_full_name(handler, name);
		struct simple_xattr *old_xattr;

		old_xattr = xmfs_simple_xattr_set(&XMFS_I(inode)->xattrs,
						  full_name, value, size,
						  flags);
		if (IS_ERR(old_xattr))
			err = PTR_ERR(old_xattr);
		else
			xmfs_simple_xattr_free(old_xattr);
	}

	return err;
}

static const struct xattr_handler xmfs_slave_security_xattr_handler = {
	.prefix	= XATTR_SECURITY_PREFIX,
	.get	= xmfs_xattr_slave_get,
	.set	= xmfs_xattr_set_slave,
};

static const struct xattr_handler xmfs_slave_trusted_xattr_handler = {
	.prefix	= XATTR_TRUSTED_PREFIX,
	.get	= xmfs_xattr_slave_get,
	.set	= xmfs_xattr_set_slave,
};

static const struct xattr_handler xmfs_slave_user_xattr_handler = {
	.prefix	= XATTR_USER_PREFIX,
	.get	= xmfs_xattr_slave_get,
	.set	= xmfs_xattr_set_slave,
};

const struct xattr_handler *xmfs_host_xattr_handlers[] = {
	&xmfs_security_xattr_handler,
	&xmfs_trusted_xattr_handler,
	&xmfs_user_xattr_handler,
	NULL
};

const struct xattr_handler *xmfs_slave_xattr_handlers[] = {
	&xmfs_slave_security_xattr_handler,
	&xmfs_slave_trusted_xattr_handler,
	&xmfs_slave_user_xattr_handler,
	NULL
};
