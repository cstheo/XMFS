/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * XMFS: Log Structured Filesystem for UB
 * Copyright (C) 2024. Huawei Technologies Co., Ltd
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */
#ifndef _XMFS_H
#define _XMFS_H

#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/xarray.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/ratelimit.h>
#include <linux/xattr.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include <linux/pgtable.h>
#include <linux/atomic.h>
#include <linux/libnvdimm.h>

#ifdef CONFIG_XMFS_FS_URMA
#include <ub/urma/ubcore_types.h>
#include <ub/urma/ubcore_uapi.h>
#include <ub/urma/ubcore_api.h>
#endif

/* #define PER_FS 1 */

#define XMFS_MAGIC 0x53464D58

#define TOTAL_SIZE (8UL << 30)
#define DATA_ALIGN_KB 4UL

#ifdef PER_FS
#define MAX_LOG_PER_TRUNK 18724
#else
#define MAX_LOG_PER_TRUNK 6096
#endif

#define ALIGN_TO_MB(size) \
	(((size) + (2 * 1024 * 1024 - 1)) & ~(2 * 1024 * 1024 - 1))

#define ALIGN_TO_2MB_LOWER(size) ((size) & ~(2 * 1024 * 1024 - 1))

/* sb is 4MB, supports up to ~8TB */
#define SB_OFFSET 0
#define SB_SIZE (4UL << 20)

/* mount msg is 2MB */
#define MOUNT_MSG_OFFSET (SB_OFFSET + SB_SIZE)
#define MOUNT_MSG_SIZE (1UL << 21)

/* msg ring is 2MB */
#define MSG_RING_OFFSET (MOUNT_MSG_OFFSET + MOUNT_MSG_SIZE)
#define MSG_RING_SIZE (1 << 21)

#define INODE_TABLE_OFFSET (MSG_RING_OFFSET + MSG_RING_SIZE)
#define INODE_TABLE_SIZE (2UL << 21)
#define XMFS_INODE_TABLE_MAGIC 0x494E4F44UL

#define DATA_AREA_OFFSET (INODE_TABLE_OFFSET + INODE_TABLE_SIZE)

/* XMFS trunk is 2MB */
#define XMFS_DEFAULT_CHUNK_SIZE (1UL << 21)

/* XMFS data align is 64KB */
#define XMFS_DATA_SHIFT 16

/* XMFS log ops */
#define XMFS_CREATE (1 << 0)
#define XMFS_DELETE (1 << 1)
#define XMFS_UPDATE (1 << 2)
#define XMFS_LINK (1 << 3)
#define XMFS_MKDIR (1 << 4)
#define XMFS_RENAME (1 << 5)
#define XMFS_SETATTR (1 << 6)
#define XMFS_SYMLINK (1 << 7)
#define XMFS_XATTR (1 << 8)
#define XMFS_ROOT (1 << 9)
#define XMFS_GC (1 << 10)

#define RENAME_HOLD (1 << 4)

#define XMFS_DATA_TRUNK_MAGIC 0x44415441UL
#define XMFS_GC_INTERVAL_DEFAULT 5
#define XMFS_GC_BATCH 51200
#define XMFS_BITMAP_CAPACITY (SB_SIZE - offsetof(struct xmfs_sb_index, bitmap))

enum xmfs_rw_msg_type {
	MSG_RW_REQUEST = 1,
	MSG_RW_GRANT,
	MSG_RW_COMPLETE,
	MSG_RW_ERROR,
};

struct xmfs_data_trunk_header {
	unsigned long magic;
	unsigned long live_page_count;
	unsigned int total_page_count;
	unsigned long last_modify_version;
};

struct xmfs_gc_ctl {
	spinlock_t gc_lock;
	wait_queue_head_t wait;
	unsigned long gc_cursor;
	unsigned long gc_batch;
	bool gc_running;
	bool need_replay;
};

struct xmfs_rw_msg {
	enum xmfs_rw_msg_type type;
	int magic;
	int slave_id;
	unsigned long request_id;
	unsigned int ops;
	unsigned long data_pages;
	unsigned long log_entries;
	umode_t mode;
	unsigned long p_ino;
	unsigned long i_ino;
	char name[256];
	loff_t size;
	unsigned long log_trunk_addr;
	int log_slot_start;
	int log_slot_count;
	unsigned long data_addr;
	unsigned long data_page_count;
	unsigned long second_data_addr;
	unsigned long second_data_pages;
	unsigned long assigned_ino;
	int status;
	unsigned long version;
};

struct xmfs_notify_msg {
	int slave_id;
	unsigned long request_id;
	int msg_type;
	int _pad[5];
};

extern const struct xattr_handler *xmfs_host_xattr_handlers[];
extern const struct xattr_handler *xmfs_slave_xattr_handlers[];

enum xmfs_msg_type {
	MSG_STOP_READ = 1,
	MSG_READ,
};

enum xmfs_mode {
	XMFS_SHARED_MEM_MODE,
	XMFS_URMA_MODE,
	XMFS_HYBRID_MODE,
};

enum xmfs_param {
	Opt_host,
	Opt_replay,
	Opt_addr,
	Opt_size,
	Opt_freq,
	Opt_log,
	Opt_debug,
	Opt_always_hot,
	Opt_fallocate,
	Opt_mode,
#ifdef CONFIG_XMFS_FS_URMA
	Opt_urma_va,
	Opt_urma_eid_subnet_prefix,
	Opt_urma_eid_interface_id,
	Opt_urma_jetty_id,
	Opt_urma_token_id,
	Opt_urma_dev,
#endif
};

/* 4KB */
struct xmfs_msg_queue {
	unsigned short len;
	unsigned short type;
	unsigned long addr;
	char pad[4072];
};

struct xmfs_fs_context {
	bool host;
	bool replay;
	unsigned long addr;
	unsigned long size;
	unsigned long fallocate_size;
	int freq;
	int log_size;
	int mode;
	bool debug;
	bool always_hot;
#ifdef CONFIG_XMFS_FS_URMA
	uint64_t urma_va;
	uint64_t urma_eid_subnet_prefix;
	uint64_t urma_eid_interface_id;
	uint32_t urma_jetty_id;
	uint32_t urma_token_id;
	char dev_name[100];
#endif
};

#ifdef PER_FS
struct xmfs_sb_index {
	unsigned long count;
	unsigned long log_start;
	unsigned long log_current;
	unsigned long data_current;
	unsigned long data_count;
	unsigned long last_update;
	char pad[4048];
};

struct xmfs_log {
	unsigned long index;
	unsigned long end_index;
	unsigned long data_offset;
	loff_t size;
	unsigned int ops;
	umode_t mode;
	char name[256];
	unsigned long p_ino;
	unsigned long i_ino;
	unsigned long offset;
	unsigned long version;
	unsigned long writer_id;
};

struct xmfs_log_trunk {
	unsigned long used;
	unsigned long next_trunk;
	unsigned long version_base;
	unsigned long version_max;
	char pad[1192];
	struct xmfs_log log[MAX_LOG_PER_TRUNK];
};

#else
struct xmfs_sb_index {
	unsigned long magic;
	/* High-water mark: next slot index to allocate. */
	unsigned long count;
	/* Actual number of used trunks (bitmap != 'u'). */
	unsigned long used_trunk_count;
	/* Log of start inode. */
	unsigned long log_start;
	/* Log of last inode. */
	unsigned long log_current;
	/* Current address of data. */
	unsigned long data_current;
	/* Number of 4 KiB data blocks in the current trunk. */
	unsigned long data_count;
	/* Timestamp. */
	unsigned long last_update;
	/* Filesystem ID. */
	__kernel_fsid_t fsid;
	int log_size;
	unsigned long len;
	int bitmap_size;
	char bitmap[];
};

struct xmfs_log {
	unsigned long index;
	unsigned long end_index;
	unsigned long data_offset;
	loff_t size;
	unsigned int ops;
	umode_t mode;
	char name[256];
	unsigned long p_ino;
	unsigned long i_ino;
	unsigned long offset;
	unsigned long version;
	unsigned long writer_id;
}; /* 336 */

struct xmfs_xattr {
	unsigned long size;
	unsigned long name_size;
	char *name;
	char value[];
};

#define MAX_LOG (4096 / (sizeof(struct xmfs_log)))

struct xmfs_log_trunk {
	unsigned long used;
	unsigned long next_trunk;
	unsigned long version_base;
	unsigned long version_max;
	struct xmfs_log log[];
};
#endif

#define XMLT_INDEX_SIZE (offsetof(struct xmfs_log_trunk, log[0]))

enum xmfs_urma_ops {
	XMFS_URMA_READ,
	XMFS_URMA_WRITE,
	XMFS_URMA_CAS,
	XMFS_URMA_SEND,
};

struct xmfs_urma_msg {
	enum xmfs_urma_ops ops;
	uint64_t offset;
	uint64_t len;
	void *local_addr;
	void *write_buffer;
	int wr_cnt;
	int slot;
	uint64_t cas_cmp_data;
	uint64_t cas_swap_data;
};

struct xmfs_urma_pool {
	void *base;
	size_t total_size;
	size_t slot_size;
	u32 nr_slots;
	u32 free_slots;
	unsigned long *bitmap;
	spinlock_t lock;
	wait_queue_head_t waitq;
};

struct urma_info {
	uint64_t urma_remote_va;
	uint64_t urma_send_va;
	uint64_t urma_recv_va;
	uint64_t urma_eid_subnet_prefix;
	uint64_t urma_eid_interface_id;
	uint32_t urma_jetty_id;
	uint32_t urma_token_id;
	uint32_t urma_send_token_id;
	uint32_t urma_recv_token_id;
};

struct xmfs_grant_wait {
	struct completion done;
	struct xmfs_rw_msg grant;
};

#ifdef CONFIG_XMFS_FS_URMA

struct xmfs_urma_ctl {
	struct ubcore_client ubc_client;
	int ubc_index;
	struct ubcore_device *ubc_dev;
	struct ubcore_jetty *jetty[5];
	struct ubcore_jetty *send_jetty;
	struct ubcore_jfc *jfc;
	struct ubcore_jfc *send_jfc;
	struct ubcore_jfc *recv_jfc;
	struct ubcore_jfr *jfr;
	struct ubcore_jfr *send_jfr;
	struct ubcore_target_seg *local_seg;
	struct ubcore_target_seg *send_seg;
	struct ubcore_target_seg *recv_seg;
	struct ubcore_tjetty *tjetty;
	struct ubcore_tjetty *send_tjettys[64];
	struct ubcore_tjetty *tjettys[64];
	struct ubcore_target_seg *remote_seg;
	struct xmfs_urma_pool pool;
	struct xmfs_urma_pool send_pool;
	struct xarray grant_waits;
	struct urma_info own_info;
	struct urma_info send_info;
	void *urma_local_va;
	void *recv_buf;
	void *send_buf;
	uint64_t urma_remote_va;
	uint64_t urma_eid_subnet_prefix;
	uint64_t urma_eid_interface_id;
	uint32_t urma_jetty_id;
	uint32_t urma_token_id;
	uint64_t last_log_start;
	uint64_t last_data_start;
};

#define XMFS_URMA_DEFAULT_WR_SIZE 104857600 /* 100 MiB */
#else
struct xmfs_urma_ctl {
	struct xmfs_urma_pool pool;
	struct xmfs_urma_pool send_pool;
	struct xarray grant_waits;
	struct urma_info own_info;
	struct urma_info send_info;
	int ubc_index;
	void *urma_local_va;
	uint64_t urma_remote_va;
	uint64_t urma_eid_subnet_prefix;
	uint64_t urma_eid_interface_id;
	uint32_t urma_jetty_id;
	uint32_t urma_token_id;
	uint64_t last_log_start;
	uint64_t last_data_start;
};
#endif

struct xmfs_inode_entry {
	unsigned long i_ino;
	unsigned long p_ino;
	umode_t mode;
	unsigned int nlink;
	loff_t size;
	struct timespec64 ctime;
	struct timespec64 mtime;
	struct timespec64 atime;
	char name[256];
};

struct xmfs_inode_table {
	unsigned long magic;
	unsigned long count;
	struct xmfs_inode_entry entries[];
};

struct xmfs_sb_info {
	int magic;
	int id;
	struct super_block *sb;
	void *kaddr;
	phys_addr_t start;
	unsigned long len;
	struct task_struct *thread;
	struct file *dax_filp;
	struct dax_device *dax_dev;
	struct bdev_handle *handlep;
	unsigned long used_ino;
	unsigned long max_chunk;
	dev_t dev;
	int freq;
	int log_size;
	bool debug;
	bool always_hot;
	unsigned long trunk_size;
	int mode;
	struct xarray pinned_inodes;
	char dev_name[100];

	/* Ratelimit XMFS diagnostics. */
	struct ratelimit_state s_error_ratelimit_state;
	struct ratelimit_state s_warning_ratelimit_state;
	struct ratelimit_state s_msg_ratelimit_state;
	atomic_t s_error_count;
	atomic_t s_warning_count;
	atomic_t s_msg_count;

	/* host uses */
	spinlock_t space_lock;
	struct rw_semaphore chain_rwsem;
	spinlock_t version_lock;
	struct mutex replay_lock;
	bool full;
	unsigned long log_current;
	unsigned long fallocate_size;
	bool during_replay;
	struct xmfs_gc_ctl gc_ctl;
	struct task_struct *host_worker;
	int max_slave_id;
	struct task_struct *gc_thread;
	unsigned long gc_interval;
	int mount_slave;

	/* slave uses */
	int stop_cnt;
	unsigned long addr;
	unsigned long last_update;
	unsigned long log_start;
	unsigned long last_replayed_slot;
	unsigned long skip_no_ops;
	spinlock_t local_ring_lock;
	unsigned long rw_request_id;

	/* urma related */
	struct xmfs_urma_ctl urma_cfg;

	struct workqueue_struct *prefetch_wq;
	struct workqueue_struct *send_wq;
};

static inline struct xmfs_sb_info *XMFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

void xmfs_init_log_ratelimits(struct xmfs_sb_info *sbi);

__printf(5, 6) void __xmfs_error(struct super_block *sb, const char *function,
				 unsigned int line, int error, const char *fmt,
				 ...);
__printf(5, 6) void __xmfs_error_inode(struct inode *inode,
				       const char *function, unsigned int line,
				       int error, const char *fmt, ...);
__printf(5, 6) void __xmfs_error_file(struct file *file, const char *function,
				      unsigned int line, int error,
				      const char *fmt, ...);
__printf(4, 5) void __xmfs_warning(struct super_block *sb, const char *function,
				   unsigned int line, const char *fmt, ...);
__printf(4, 5) void __xmfs_warning_inode(const struct inode *inode,
					 const char *function,
					 unsigned int line, const char *fmt,
					 ...);
__printf(2, 3) void __xmfs_msg(struct super_block *sb, const char *fmt, ...);

#ifdef CONFIG_PRINTK
#define xmfs_error(sb, fmt, ...) \
	__xmfs_error((sb), __func__, __LINE__, 0, (fmt), ##__VA_ARGS__)
#define xmfs_error_err(sb, err, fmt, ...) \
	__xmfs_error((sb), __func__, __LINE__, (err), (fmt), ##__VA_ARGS__)
#define xmfs_error_inode(inode, fmt, ...) \
	__xmfs_error_inode((inode), __func__, __LINE__, 0, (fmt), ##__VA_ARGS__)
#define xmfs_error_inode_err(inode, err, fmt, ...)                    \
	__xmfs_error_inode((inode), __func__, __LINE__, (err), (fmt), \
			   ##__VA_ARGS__)
#define xmfs_error_file(file, fmt, ...) \
	__xmfs_error_file((file), __func__, __LINE__, 0, (fmt), ##__VA_ARGS__)
#define xmfs_warning(sb, fmt, ...) \
	__xmfs_warning((sb), __func__, __LINE__, (fmt), ##__VA_ARGS__)
#define xmfs_warning_inode(inode, fmt, ...) \
	__xmfs_warning_inode((inode), __func__, __LINE__, (fmt), ##__VA_ARGS__)
#define xmfs_msg(sb, fmt, ...) \
	__xmfs_msg((sb), (fmt), ##__VA_ARGS__)
#else
#define xmfs_error(sb, fmt, ...) no_printk((fmt), ##__VA_ARGS__)
#define xmfs_error_err(sb, err, fmt, ...) no_printk((fmt), ##__VA_ARGS__)
#define xmfs_error_inode(inode, fmt, ...) no_printk((fmt), ##__VA_ARGS__)
#define xmfs_error_inode_err(inode, err, fmt, ...) \
	no_printk((fmt), ##__VA_ARGS__)
#define xmfs_error_file(file, fmt, ...) no_printk((fmt), ##__VA_ARGS__)
#define xmfs_warning(sb, fmt, ...) no_printk((fmt), ##__VA_ARGS__)
#define xmfs_warning_inode(inode, fmt, ...) no_printk((fmt), ##__VA_ARGS__)
#define xmfs_msg(sb, fmt, ...) no_printk((fmt), ##__VA_ARGS__)
#endif

#define xmfs_info(sb, fmt, ...) xmfs_msg((sb), KERN_INFO fmt, ##__VA_ARGS__)

struct mount_msg_info {
	atomic_t id;
	atomic_t host_mount;
	/* 'D' for done, 'C' for unmount, 'T' for during mount. */
	char msg[256];
	/* Each slave writes its last_update here. */
	unsigned long slave_versions[64];
	struct urma_info urma_infos[64];
	struct urma_info send_infos[64];
};

struct page_freq {
	atomic_t read_count;
	bool in_cache;
	bool never_in_cache;
	bool already_in_cache;
};

struct xmfs_cache_work {
	struct work_struct work;
	struct inode *inode;
	pgoff_t index;
	pgoff_t end_index;
};

struct xmfs_write_work {
	struct work_struct work;
	struct inode *inode;
	pgoff_t index;
	pgoff_t end_index;
	size_t size;
};

struct xmfs_send_work {
	struct work_struct work;
	struct xmfs_rw_msg grant;
	struct xmfs_sb_info *sbi;
};

struct xmfs_inode_info {
	unsigned long i_ino;
	struct inode vfs_inode;
	/* Inode log start address. */
	unsigned long log_start;
	/* Log trunk where the last replay ends. */
	unsigned long log_replay;
	/* Symlink name address. */
	unsigned long symname;
	struct xarray shared_pages;
	struct simple_xattrs xattrs;
	struct timespec64 btime;
#ifdef CONFIG_XMFS_FS_URMA
	struct ubcore_jfs_wr *wr;
	void *write_buffer;
	int wr_cnt;
	int *slots;
#endif
	struct xarray page_freq;
	unsigned long start_write;
	unsigned long second_write;
	unsigned long second_write_count;
	unsigned long count;
	unsigned long data_start;
	unsigned long data_size;
	bool leaf;
	bool tmp;
	bool die;
	bool fallocate;
	bool pinned;
	unsigned long inode_table_slot;
	struct workqueue_struct *write_wq;
};

struct xmfs_read_extent {
	pgoff_t start_index;
	unsigned long start_offset;
	unsigned int nr_pages;
};

struct host_mount_args {
	struct xmfs_sb_info *sbi;
	int id;
};

void xmfs_init_file_inode(struct inode *inode);
void xmfs_init_file_mapping(struct inode *inode);
void xmfs_init_dir_inode(struct inode *inode, bool host);
void xmfs_init_dir_file(struct inode *inode);
void xmfs_init_dentry_operations(struct super_block *sb);
void xmfs_init_symlink_mapping(struct inode *inode);
void xmfs_init_symlink_operations(struct inode *inode);
void xmfs_init_special_inode_operations(struct inode *inode);
int xmfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		 struct iattr *attr);
int xmfs_getattr(struct mnt_idmap *idmap, const struct path *path,
		 struct kstat *stat, u32 request_mask, unsigned int flags);
int xmfs_slave_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		       struct iattr *attr);
int xmfs_slave_getattr(struct mnt_idmap *idmap, const struct path *path,
		       struct kstat *stat, u32 request_mask,
		       unsigned int flags);
int xmfs_update_time(struct inode *inode, int flags);
struct inode *xmfs_get_root(struct super_block *sb, int host_id);
int xmfs_send_req_wait_reply(struct xmfs_sb_info *sbi, int type);
void arch_invalidate_pmem(void *addr, size_t size);
void xmfs_mark_page_dead(struct xmfs_sb_info *sbi, unsigned long data_offset);
void xmfs_mark_page_live(struct xmfs_sb_info *sbi, unsigned long data_offset);

struct xmfs_data_reserve_result {
	unsigned long data_addr;
	unsigned long data_page_count;
	unsigned long second_data_addr;
	unsigned long second_data_pages;
	unsigned long trunk_addrs[4];
	int trunk_count;
};

unsigned long xmfs_add_log_v3(struct xmfs_log *xm_log, struct xmfs_sb_info *sbi,
			      size_t namelen, unsigned long ino,
			      bool update_version);
long xmfs_find_free_slot(struct xmfs_sb_index *xmsi, struct xmfs_sb_info *sbi);
long xmfs_alloc_and_mark_slot(struct xmfs_sb_index *xmsi,
			      struct xmfs_sb_info *sbi, char type);
int xmfs_gc_thread_fn(void *data);
int xmfs_gc_thread_slave_fn(void *data);
int xmfs_quiesce_wait(struct xmfs_sb_info *sbi);
int xmfs_host_worker_fn(void *data);
void xmfs_host_process_ring(struct xmfs_sb_info *sbi);
int xmfs_slave_request_write(struct xmfs_sb_info *sbi, struct xmfs_rw_msg *req);
int xmfs_slave_wait_grant(struct xmfs_sb_info *sbi, struct xmfs_rw_msg *grant,
			  unsigned long grant_slot, unsigned long request_id);
void xmfs_slave_send_complete(struct xmfs_sb_info *sbi,
			      unsigned long request_id, loff_t comp_size,
			      struct xmfs_rw_msg *orig_req,
			      struct xmfs_rw_msg *grant);
int xmfs_pmem_write(struct xmfs_sb_info *sbi, unsigned long offset,
		    const void *buf, unsigned long size);
int xmfs_pmem_write_multi_pages(struct xmfs_sb_info *sbi, unsigned long offset,
				struct page **pages, unsigned long page_num);
int xmfs_pmem_read_multi_pages(struct xmfs_sb_info *sbi, unsigned long *offsets,
			       struct page **pages, unsigned long page_num);
int xmfs_pmem_read(struct xmfs_sb_info *sbi, unsigned long offset, void *buf,
		   unsigned long size);
int xmfs_pmem_cas(struct xmfs_sb_info *sbi, unsigned long offset,
		  uint64_t old_val, uint64_t new_val);
int xmfs_import_jetty(struct xmfs_sb_info *sbi, struct urma_info info,
		      int index, bool is_send);
int xmfs_send_request(struct xmfs_sb_info *sbi, struct xmfs_rw_msg *req,
		      struct xmfs_grant_wait *gw, int id);
#ifdef CONFIG_XMFS_FS_URMA
int xmfs_urma_send_write_pages(struct xmfs_sb_info *sbi,
			       struct ubcore_jfs_wr *wr);
#endif
void *urma_pool_acquire_nslots(struct xmfs_urma_pool *pool, int nr, u32 *slots);
void urma_pool_release_nslots(struct xmfs_urma_pool *pool, int nr,
			      const u32 *slots);
int xmfs_urma_read_ubuf(struct xmfs_sb_info *sbi, void __user *buf,
			struct xmfs_read_extent *exts, int ext_cnt,
			size_t size);
void xmfs_prefetch_worker(struct work_struct *work);

static inline int xmfs_data_write(struct xmfs_sb_info *sbi,
				  unsigned long offset, const void *buf,
				  unsigned long size)
{
	if (sbi->mode != XMFS_URMA_MODE) {
		memcpy(sbi->kaddr + offset, buf, size);
		return 0;
	}
	return xmfs_pmem_write(sbi, offset, buf, size);
}

static inline int xmfs_data_read(struct xmfs_sb_info *sbi, unsigned long offset,
				 void *buf, unsigned long size)
{
	if (sbi->mode != XMFS_URMA_MODE) {
		memcpy(buf, sbi->kaddr + offset, size);
		return 0;
	}
	return xmfs_pmem_read(sbi, offset, buf, size);
}

int xmfs_gc_main(struct xmfs_sb_info *sbi);
int xmfs_inode_eq(struct inode *inode, void *_ino);
int xmfs_inode_set(struct inode *inode, void *_ino);
unsigned int xmfs_inode_table_sync_nlink(struct xmfs_inode_table *tbl,
					 unsigned long ino);
bool xmfs_inode_table_remove(struct xmfs_inode_table *tbl, unsigned long ino,
			     unsigned long p_ino, const char *name);
void xmfs_replay_plays_gc(struct xmfs_sb_info *sbi);
void xmfs_clear_inode_bits(struct xmfs_sb_info *sbi, unsigned long ino);
ssize_t xmfs_listxattr(struct dentry *dentry, char *buffer, size_t size);
int xmfs_initxattrs(struct inode *inode, const struct xattr *xattr_array,
		    void *fs_info);
int xmfs_replay_new_entries(struct xmfs_sb_info *sbi, struct dentry *dentry,
			    unsigned long last_update);
void xmfs_populate_from_inode_table(struct xmfs_sb_info *sbi);
ssize_t xmfs_simple_xattr_list(struct inode *inode,
			       struct simple_xattrs *xattrs, char *buffer,
			       size_t size);
int xmfs_xattr_list_one(char **buffer, ssize_t *remaining_size,
			const char *name);
int xmfs_simple_xattr_get(struct simple_xattrs *xattrs, const char *name,
			  void *buffer, size_t size);
struct simple_xattr *xmfs_simple_xattr_set(struct simple_xattrs *xattrs,
					   const char *name, const void *value,
					   size_t size, int flags);
struct simple_xattr *xmfs_simple_xattr_alloc(const void *value, size_t size);
void xmfs_simple_xattrs_init(struct simple_xattrs *xattrs);
void xmfs_simple_xattrs_free(struct simple_xattrs *xattrs, size_t *freed_space);
size_t xmfs_simple_xattr_space(const char *name, size_t size);
void xmfs_simple_xattr_free(struct simple_xattr *xattr);
long xmfs_common_fallocate(struct inode *inode, int mode, loff_t offset,
			   loff_t len);
int xmfs_urma_register(struct xmfs_sb_info *sbi, struct xmfs_fs_context *ctx);
void xmfs_urma_unregister(struct xmfs_sb_info *sbi);
int xmfs_urma_import(struct xmfs_sb_info *sbi);
void xmfs_urma_unimport(struct xmfs_sb_info *sbi);
int xmfs_urma_send(struct xmfs_sb_info *sbi, struct xmfs_urma_msg *msg);
void xmfs_host_handle_urma_request(struct xmfs_sb_info *sbi, int slave_i,
				   struct xmfs_rw_msg *req,
				   struct xmfs_rw_msg *grant);
#ifdef CONFIG_XMFS_FS_URMA
int xmfs_urma_fill_write_page(struct xmfs_sb_info *sbi,
			      struct ubcore_jfs_wr *wr,
			      struct xmfs_urma_msg *msg, int slot);
#endif
int xmfs_urma_write_ubuf(struct xmfs_sb_info *sbi, void __user *buf,
			 size_t size, unsigned long offset);

static inline struct xmfs_inode_info *XMFS_I(struct inode *inode)
{
	return container_of(inode, struct xmfs_inode_info, vfs_inode);
}

static inline int local_update_log_and_data(struct xmfs_sb_info *sbi,
					    unsigned long offset,
					    unsigned long size)
{
	switch (sbi->mode) {
	case XMFS_SHARED_MEM_MODE:
	case XMFS_HYBRID_MODE:
		return 0;
	case XMFS_URMA_MODE:
		return xmfs_pmem_read(sbi, offset, sbi->urma_cfg.urma_local_va,
				      size);
	default:
		return -EINVAL;
	}
}

static inline int get_meta_and_copy(struct xmfs_sb_info *sbi,
				    unsigned long offset, void *addr,
				    unsigned long size, void *local_addr,
				    unsigned long local_offset)
{
	int err;
	struct xmfs_urma_msg msg;

	if (addr == NULL)
		return -EINVAL;
	if (offset > sbi->len - 2097152)
		return -EINVAL;

	switch (sbi->mode) {
	case XMFS_SHARED_MEM_MODE:
	case XMFS_HYBRID_MODE:
		return copy_mc_to_kernel(addr, sbi->kaddr + offset, size);
	case XMFS_URMA_MODE:
		if (!local_addr) {
			msg.ops = XMFS_URMA_READ;
			msg.offset = offset;
			msg.len = size;
			msg.local_addr = addr;
			err = xmfs_urma_send(sbi, &msg);
			return err;
		} else {
			return copy_mc_to_kernel(addr,
						 local_addr + offset -
							 local_offset,
						 size);
		}
	default:
		return -EINVAL;
	}
}

static inline int get_data_and_copy(struct xmfs_sb_info *sbi,
				    unsigned long offset, void *addr,
				    unsigned long size, void *local_addr,
				    unsigned long local_offset)
{
	int err;
	struct xmfs_urma_msg msg;

	if (addr == NULL)
		return -EINVAL;
	if (offset > sbi->len - 2097152)
		return -EINVAL;

	switch (sbi->mode) {
	case XMFS_SHARED_MEM_MODE:
		return copy_mc_to_kernel(addr, sbi->kaddr + offset, size);
	case XMFS_URMA_MODE:
	case XMFS_HYBRID_MODE:
		if (!local_addr) {
			msg.ops = XMFS_URMA_READ;
			msg.offset = offset;
			msg.len = size;
			msg.local_addr = addr;
			err = xmfs_urma_send(sbi, &msg);
			return err;
		} else {
			return copy_mc_to_kernel(addr,
						 local_addr + offset -
							 local_offset,
						 size);
		}
	default:
		return -EINVAL;
	}
}

static inline void *get_meta(struct xmfs_sb_info *sbi, unsigned long offset,
			     unsigned long size)
{
	struct xmfs_urma_msg msg;
	int err = 0;
	void *local_addr;

	if (offset > sbi->len - 2097152)
		return NULL;

	switch (sbi->mode) {
	case XMFS_SHARED_MEM_MODE:
	case XMFS_HYBRID_MODE:
		return sbi->kaddr + offset;
	case XMFS_URMA_MODE:
		local_addr = vmalloc(size);
		if (!local_addr)
			return local_addr;
		msg.ops = XMFS_URMA_READ;
		msg.offset = offset;
		msg.len = size;
		msg.local_addr = local_addr;
		err = xmfs_urma_send(sbi, &msg);
		if (err) {
			xmfs_info(sbi->sb, "get meta err out %d", err);
			vfree(local_addr);
			local_addr = NULL;
		}
		return local_addr;
	default:
		return NULL;
	}
}

static inline void put_meta(struct xmfs_sb_info *sbi, void *local_addr)
{
	if (sbi->mode == XMFS_URMA_MODE)
		vfree(local_addr);
}

static inline void *get_data(struct xmfs_sb_info *sbi, unsigned long offset,
			     unsigned long size)
{
	struct xmfs_urma_msg msg;
	int err = 0;
	void *local_addr;

	if (offset > sbi->len - 2097152)
		return NULL;

	switch (sbi->mode) {
	case XMFS_SHARED_MEM_MODE:
		return sbi->kaddr + offset;
	case XMFS_URMA_MODE:
	case XMFS_HYBRID_MODE:
		local_addr = vmalloc(size);
		if (!local_addr)
			return local_addr;
		msg.ops = XMFS_URMA_READ;
		msg.offset = offset;
		msg.len = size;
		msg.local_addr = local_addr;
		err = xmfs_urma_send(sbi, &msg);
		if (err) {
			vfree(local_addr);
			local_addr = NULL;
		}
		return local_addr;
	default:
		return NULL;
	}
}

static inline void put_data(struct xmfs_sb_info *sbi, void *local_addr)
{
	if (sbi->mode == XMFS_URMA_MODE || sbi->mode == XMFS_HYBRID_MODE)
		vfree(local_addr);
}

static inline struct xmfs_sb_index *get_xmsi(struct xmfs_sb_info *sbi)
{
	return (struct xmfs_sb_index *)get_meta(sbi, SB_OFFSET,
						sizeof(struct xmfs_sb_index));
}

static inline void put_xmsi(struct xmfs_sb_info *sbi, void *local_buf)
{
	put_meta(sbi, local_buf);
}

static inline int get_xmsi_and_copy(struct xmfs_sb_info *sbi,
				    struct xmfs_sb_index *xmsi)
{
	return get_meta_and_copy(sbi, SB_OFFSET, xmsi,
				 sizeof(struct xmfs_sb_index), NULL, 0);
}

static inline struct xmfs_log *get_log(struct xmfs_sb_info *sbi,
				       unsigned long offset)
{
	return (struct xmfs_log *)get_meta(sbi, offset,
					   sizeof(struct xmfs_log));
}

static inline void put_log(struct xmfs_sb_info *sbi, void *local_buf)
{
	put_meta(sbi, local_buf);
}

static inline int get_log_and_copy(struct xmfs_sb_info *sbi,
				   unsigned long offset, struct xmfs_log *log)
{
	return get_meta_and_copy(sbi, offset, log, sizeof(struct xmfs_log),
				 NULL, 0);
}

static inline struct xmfs_log_trunk *get_log_trunk(struct xmfs_sb_info *sbi,
						   unsigned long offset)
{
	return (struct xmfs_log_trunk *)get_meta(sbi, offset, 2097152);
}

static inline struct xmfs_log_trunk *
get_log_trunk_meta(struct xmfs_sb_info *sbi, unsigned long offset)
{
	return (struct xmfs_log_trunk *)get_meta(sbi, offset,
						 sizeof(struct xmfs_log_trunk));
}

static inline struct xmfs_log_trunk *
get_log_trunk_data(struct xmfs_sb_info *sbi, unsigned long offset,
		   unsigned long idx)
{
	return (struct xmfs_log_trunk *)get_meta(sbi, offset,
						 sizeof(struct xmfs_log_trunk) +
						 idx * sizeof(struct xmfs_log));
}

static inline void put_log_trunk(struct xmfs_sb_info *sbi, void *local_buf)
{
	put_meta(sbi, local_buf);
}

static inline int get_log_trunk_and_copy(struct xmfs_sb_info *sbi,
					 unsigned long offset,
					 struct xmfs_log_trunk *xmlt,
					 bool get_full_trunk)
{
	if (get_full_trunk)
		return get_meta_and_copy(sbi, offset, xmlt, sbi->trunk_size,
					 NULL, 0);
	return get_meta_and_copy(sbi, offset, xmlt,
				 sizeof(struct xmfs_log_trunk), NULL, 0);
}

static inline struct mount_msg_info *get_mnt_msg(struct xmfs_sb_info *sbi)
{
	return (struct mount_msg_info *)get_meta(sbi, MOUNT_MSG_OFFSET,
						 sizeof(struct mount_msg_info));
}

static inline void put_mnt_msg(struct xmfs_sb_info *sbi, void *local_buf)
{
	put_meta(sbi, local_buf);
}

static inline int get_mnt_msg_and_copy(struct xmfs_sb_info *sbi,
				       struct mount_msg_info *mm_info)
{
	return get_meta_and_copy(sbi, MOUNT_MSG_OFFSET, mm_info,
				 sizeof(struct mount_msg_info), NULL, 0);
}

static inline int mnt_msg_write(struct xmfs_sb_info *sbi, int id, char c)
{
	switch (sbi->mode) {
	case XMFS_SHARED_MEM_MODE:
	case XMFS_HYBRID_MODE: {
		struct mount_msg_info *mm_info = sbi->kaddr + MOUNT_MSG_OFFSET;

		return mm_info->msg[id] = c;
	}
	case XMFS_URMA_MODE:
		return xmfs_pmem_write(sbi,
				       MOUNT_MSG_OFFSET +
					       offsetof(struct mount_msg_info,
							msg) +
					       id,
				       &c, 1) ?
			       -1 :
			       c;
	default:
		return -1;
	}
}

static inline int xmfs_cas(struct xmfs_sb_info *sbi, int old, int new)
{
	switch (sbi->mode) {
	case XMFS_SHARED_MEM_MODE:
	case XMFS_HYBRID_MODE: {
		struct mount_msg_info *mm_info = sbi->kaddr + MOUNT_MSG_OFFSET;

		return atomic_cmpxchg(&mm_info->id, old, new);
	}
	case XMFS_URMA_MODE: {
		int ret = xmfs_pmem_cas(sbi,
					MOUNT_MSG_OFFSET +
						offsetof(struct mount_msg_info,
							 id),
					(uint64_t)old, (uint64_t)new);

		return (ret == 0) ? old : 0;
	}
	default:
		return 0;
	}
}

static inline int xmfs_atomic_read(struct xmfs_sb_info *sbi,
				   struct mount_msg_info *mm_info)
{
	return atomic_read(&mm_info->id);
}

#endif
