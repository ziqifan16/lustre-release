/*
 *  linux/fs/ext4/namei.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/namei.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 *  Directory entry file type support and forward compatibility hooks
 *	for B-tree directories by Theodore Ts'o (tytso@mit.edu), 1998
 *  Hash Tree Directory indexing (c)
 *	Daniel Phillips, 2001
 *  Hash Tree Directory indexing porting
 *	Christopher Li, 2002
 *  Hash Tree Directory indexing cleanup
 *	Theodore Ts'o, 2002
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/jbd2.h>
#include <linux/time.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/quotaops.h>
#include <linux/buffer_head.h>
#include <linux/bio.h>
#include "ext4.h"
#include "ext4_jbd2.h"

#include "xattr.h"
#include "acl.h"

/*
 * define how far ahead to read directories while searching them.
 */
#define NAMEI_RA_CHUNKS  2
#define NAMEI_RA_BLOCKS  4
#define NAMEI_RA_SIZE	     (NAMEI_RA_CHUNKS * NAMEI_RA_BLOCKS)
#define NAMEI_RA_INDEX(c,b)  (((c) * NAMEI_RA_BLOCKS) + (b))

struct buffer_head *ext4_append(handle_t *handle,
				struct inode *inode,
				ext4_lblk_t *block, int *err)
{
	struct buffer_head *bh;
	struct ext4_inode_info *ei = EXT4_I(inode);

	/* with parallel dir operations all appends
	* have to be serialized -bzzz */
	down(&ei->i_append_sem);

	if (unlikely(S_ISDIR(inode->i_mode) &&
		     EXT4_SB(inode->i_sb)->s_max_dir_size &&
		     (inode->i_size >=
		     EXT4_SB(inode->i_sb)->s_max_dir_size))) {
		*err = -ENOSPC;
		up(&ei->i_append_sem);
		return NULL;
	}

	*block = inode->i_size >> inode->i_sb->s_blocksize_bits;

	bh = ext4_bread(handle, inode, *block, 1, err);
	if (bh) {
		inode->i_size += inode->i_sb->s_blocksize;
		EXT4_I(inode)->i_disksize = inode->i_size;
		*err = ext4_journal_get_write_access(handle, bh);
		if (*err) {
			brelse(bh);
			bh = NULL;
		}
		ei->i_disksize = inode->i_size;
	}
	up(&ei->i_append_sem);
	return bh;
}
EXPORT_SYMBOL(ext4_append);

#ifndef assert
#define assert(test) J_ASSERT(test)
#endif

#ifdef DX_DEBUG
#define dxtrace(command) command
#else
#define dxtrace(command)
#endif

struct fake_dirent
{
	__le32 inode;
	__le16 rec_len;
	u8 name_len;
	u8 file_type;
};

struct dx_countlimit
{
	__le16 limit;
	__le16 count;
};

struct dx_entry
{
	__le32 hash;
	__le32 block;
};

/*
 * dx_root_info is laid out so that if it should somehow get overlaid by a
 * dirent the two low bits of the hash version will be zero.  Therefore, the
 * hash version mod 4 should never be 0.  Sincerely, the paranoia department.
 */

struct dx_root_info
{
	__le32 reserved_zero;
	u8 hash_version;
	u8 info_length; /* 8 */
	u8 indirect_levels;
	u8 unused_flags;
};

struct dx_node
{
	struct fake_dirent fake;
	struct dx_entry	entries[0];
};


struct dx_frame
{
	struct buffer_head *bh;
	struct dx_entry *entries;
	struct dx_entry *at;
};

struct dx_map_entry
{
	u32 hash;
	u16 offs;
	u16 size;
};

static inline ext4_lblk_t dx_get_block(struct dx_entry *entry);
static void dx_set_block(struct dx_entry *entry, ext4_lblk_t value);
static inline unsigned dx_get_hash(struct dx_entry *entry);
static void dx_set_hash(struct dx_entry *entry, unsigned value);
static unsigned dx_get_count(struct dx_entry *entries);
static unsigned dx_get_limit(struct dx_entry *entries);
static void dx_set_count(struct dx_entry *entries, unsigned value);
static void dx_set_limit(struct dx_entry *entries, unsigned value);
static inline unsigned dx_root_limit(__u32 blocksize,
		struct ext4_dir_entry_2 *dot_de, unsigned infosize);
static unsigned dx_node_limit(struct inode *dir);
static struct dx_frame *dx_probe(const struct qstr *d_name,
				 struct inode *dir,
				 struct dx_hash_info *hinfo,
				 struct dx_frame *frame,
				 struct htree_lock *lck, int *err);
static void dx_release(struct dx_frame *frames);
static int dx_make_map(struct ext4_dir_entry_2 *de, unsigned blocksize,
		       struct dx_hash_info *hinfo, struct dx_map_entry map[]);
static void dx_sort_map(struct dx_map_entry *map, unsigned count);
static struct ext4_dir_entry_2 *dx_move_dirents(char *from, char *to,
		struct dx_map_entry *offsets, int count, unsigned blocksize);
static struct ext4_dir_entry_2* dx_pack_dirents(char *base, unsigned blocksize);
static void dx_insert_block(struct dx_frame *frame,
					u32 hash, ext4_lblk_t block);
static int ext4_htree_next_block(struct inode *dir, __u32 hash,
				 struct dx_frame *frame,
				 struct dx_frame *frames,
				 __u32 *start_hash, struct htree_lock *lck);
static struct buffer_head * ext4_dx_find_entry(struct inode *dir,
		const struct qstr *d_name,
		struct ext4_dir_entry_2 **res_dir,
		struct htree_lock *lck, int *err);
static int ext4_dx_add_entry(handle_t *handle, struct dentry *dentry,
			     struct inode *inode, struct htree_lock *lck);

/*
 * p is at least 6 bytes before the end of page
 */
static inline struct ext4_dir_entry_2 *
ext4_next_entry(struct ext4_dir_entry_2 *p, unsigned long blocksize)
{
	return (struct ext4_dir_entry_2 *)((char *)p +
		ext4_rec_len_from_disk(p->rec_len, blocksize));
}

/*
 * Future: use high four bits of block for coalesce-on-delete flags
 * Mask them off for now.
 */
struct dx_root_info * dx_get_dx_info(struct ext4_dir_entry_2 *de)
{
	BUG_ON(de->name_len != 1);
	/* get dotdot first */
	de = (struct ext4_dir_entry_2 *)((char *)de + EXT4_DIR_REC_LEN(de));

	/* dx root info is after dotdot entry */
	de = (struct ext4_dir_entry_2 *)((char *)de + EXT4_DIR_REC_LEN(de));

       return (struct dx_root_info *) de;
}

static inline ext4_lblk_t dx_get_block(struct dx_entry *entry)
{
	return le32_to_cpu(entry->block) & 0x0fffffff;
}

static inline void dx_set_block(struct dx_entry *entry, ext4_lblk_t value)
{
	entry->block = cpu_to_le32(value);
}

static inline unsigned dx_get_hash(struct dx_entry *entry)
{
	return le32_to_cpu(entry->hash);
}

static inline void dx_set_hash(struct dx_entry *entry, unsigned value)
{
	entry->hash = cpu_to_le32(value);
}

static inline unsigned dx_get_count(struct dx_entry *entries)
{
	return le16_to_cpu(((struct dx_countlimit *) entries)->count);
}

static inline unsigned dx_get_limit(struct dx_entry *entries)
{
	return le16_to_cpu(((struct dx_countlimit *) entries)->limit);
}

static inline void dx_set_count(struct dx_entry *entries, unsigned value)
{
	((struct dx_countlimit *) entries)->count = cpu_to_le16(value);
}

static inline void dx_set_limit(struct dx_entry *entries, unsigned value)
{
	((struct dx_countlimit *) entries)->limit = cpu_to_le16(value);
}

static inline unsigned dx_root_limit(__u32 blocksize,
		struct ext4_dir_entry_2 *dot_de, unsigned infosize)
{
	struct ext4_dir_entry_2 *dotdot_de;
	unsigned entry_space;

	BUG_ON(dot_de->name_len != 1);
	dotdot_de = ext4_next_entry(dot_de, blocksize);
	entry_space = blocksize - EXT4_DIR_REC_LEN(dot_de) -
			 EXT4_DIR_REC_LEN(dotdot_de) - infosize;

	return entry_space / sizeof(struct dx_entry);
}

static inline unsigned dx_node_limit(struct inode *dir)
{
	unsigned entry_space = dir->i_sb->s_blocksize - __EXT4_DIR_REC_LEN(0);
	return entry_space / sizeof(struct dx_entry);
}

/*
 * Debug
 */
#ifdef DX_DEBUG
static void dx_show_index(char * label, struct dx_entry *entries)
{
	int i, n = dx_get_count (entries);
	printk(KERN_DEBUG "%s index ", label);
	for (i = 0; i < n; i++) {
		printk("%x->%lu ", i ? dx_get_hash(entries + i) :
				0, (unsigned long)dx_get_block(entries + i));
	}
	printk("\n");
}

struct stats
{
	unsigned names;
	unsigned space;
	unsigned bcount;
};

static struct stats dx_show_leaf(struct dx_hash_info *hinfo, struct ext4_dir_entry_2 *de,
				 int size, int show_names)
{
	unsigned names = 0, space = 0;
	char *base = (char *) de;
	struct dx_hash_info h = *hinfo;

	printk("names: ");
	while ((char *) de < base + size)
	{
		if (de->inode)
		{
			if (show_names)
			{
				int len = de->name_len;
				char *name = de->name;
				while (len--) printk("%c", *name++);
				ext4fs_dirhash(de->name, de->name_len, &h);
				printk(":%x.%u ", h.hash,
				       ((char *) de - base));
			}
			space += EXT4_DIR_REC_LEN(de);
			names++;
		}
		de = ext4_next_entry(de, size);
	}
	printk("(%i)\n", names);
	return (struct stats) { names, space, 1 };
}

struct stats dx_show_entries(struct dx_hash_info *hinfo, struct inode *dir,
			     struct dx_entry *entries, int levels)
{
	unsigned blocksize = dir->i_sb->s_blocksize;
	unsigned count = dx_get_count(entries), names = 0, space = 0, i;
	unsigned bcount = 0;
	struct buffer_head *bh;
	int err;
	printk("%i indexed blocks...\n", count);
	for (i = 0; i < count; i++, entries++)
	{
		ext4_lblk_t block = dx_get_block(entries);
		ext4_lblk_t hash  = i ? dx_get_hash(entries): 0;
		u32 range = i < count - 1? (dx_get_hash(entries + 1) - hash): ~hash;
		struct stats stats;
		printk("%s%3u:%03u hash %8x/%8x ",levels?"":"   ", i, block, hash, range);
		if (!(bh = ext4_bread (NULL,dir, block, 0,&err))) continue;
		stats = levels?
		   dx_show_entries(hinfo, dir, ((struct dx_node *) bh->b_data)->entries, levels - 1):
		   dx_show_leaf(hinfo, (struct ext4_dir_entry_2 *) bh->b_data, blocksize, 0);
		names += stats.names;
		space += stats.space;
		bcount += stats.bcount;
		brelse(bh);
	}
	if (bcount)
		printk(KERN_DEBUG "%snames %u, fullness %u (%u%%)\n", 
		       levels ? "" : "   ", names, space/bcount,
		       (space/bcount)*100/blocksize);
	return (struct stats) { names, space, bcount};
}
#endif /* DX_DEBUG */

/* private data for htree_lock */
struct ext4_dir_lock_data {
	unsigned		ld_flags;  /* bits-map for lock types */
	unsigned		ld_count;  /* # entries of the last DX block */
	struct dx_entry		ld_at_entry; /* copy of leaf dx_entry */
	struct dx_entry		*ld_at;	   /* position of leaf dx_entry */
};

#define ext4_htree_lock_data(l) ((struct ext4_dir_lock_data *)(l)->lk_private)
#define ext4_find_entry(dir, name, dirent) __ext4_find_entry(dir, name, dirent, NULL)
#define ext4_add_entry(handle, dentry, inode) __ext4_add_entry(handle, dentry, inode, NULL)

/* NB: ext4_lblk_t is 32 bits so we use high bits to identify invalid blk */
#define EXT4_HTREE_NODE_CHANGED	(0xcafeULL << 32)

static void ext4_htree_event_cb(void *target, void *event)
{
	u64 *block = (u64 *)target;

	if (*block == dx_get_block((struct dx_entry *)event))
		*block = EXT4_HTREE_NODE_CHANGED;
}

struct htree_lock_head *ext4_htree_lock_head_alloc(unsigned hbits)
{
	struct htree_lock_head *lhead;

	lhead = htree_lock_head_alloc(EXT4_LK_MAX, hbits, 0);
	if (lhead != NULL) {
		htree_lock_event_attach(lhead, EXT4_LK_SPIN, HTREE_EVENT_WR,
					ext4_htree_event_cb);
	}
	return lhead;
}
EXPORT_SYMBOL(ext4_htree_lock_head_alloc);

struct htree_lock *ext4_htree_lock_alloc(void)
{
	return htree_lock_alloc(EXT4_LK_MAX,
				sizeof(struct ext4_dir_lock_data));
}
EXPORT_SYMBOL(ext4_htree_lock_alloc);

static htree_lock_mode_t ext4_htree_mode(unsigned flags)
{
	switch (flags) {
	default: /* 0 or unknown flags require EX lock */
		return HTREE_LOCK_EX;
	case EXT4_HLOCK_READDIR:
		return HTREE_LOCK_PR;
	case EXT4_HLOCK_LOOKUP:
		return HTREE_LOCK_CR;
	case EXT4_HLOCK_DEL:
	case EXT4_HLOCK_ADD:
		return HTREE_LOCK_CW;
	}
}

/* return PR for read-only operations, otherwise return EX */
static inline htree_lock_mode_t ext4_htree_safe_mode(unsigned flags)
{
	int writer = (flags & EXT4_LB_DE) == EXT4_LB_DE;

	/* 0 requires EX lock */
	return (flags == 0 || writer) ? HTREE_LOCK_EX : HTREE_LOCK_PR;
}

static int ext4_htree_safe_locked(struct htree_lock *lck)
{
	int writer;

	if (lck == NULL || lck->lk_mode == HTREE_LOCK_EX)
		return 1;

	writer = (ext4_htree_lock_data(lck)->ld_flags & EXT4_LB_DE) ==
		 EXT4_LB_DE;
	if (writer) /* all readers & writers are excluded? */
		return lck->lk_mode == HTREE_LOCK_EX;

	/* all writers are excluded? */
	return lck->lk_mode == HTREE_LOCK_PR ||
	       lck->lk_mode == HTREE_LOCK_PW ||
	       lck->lk_mode == HTREE_LOCK_EX;
}

/* relock htree_lock with EX mode if it's change operation, otherwise
 * relock it with PR mode. It's noop if PDO is disabled. */
static void ext4_htree_safe_relock(struct htree_lock *lck)
{
	if (!ext4_htree_safe_locked(lck)) {
		unsigned flags = ext4_htree_lock_data(lck)->ld_flags;

		htree_change_lock(lck, ext4_htree_safe_mode(flags));
	}
}

void ext4_htree_lock(struct htree_lock *lck, struct htree_lock_head *lhead,
		     struct inode *dir, unsigned flags)
{
	htree_lock_mode_t mode = is_dx(dir) ? ext4_htree_mode(flags) :
					      ext4_htree_safe_mode(flags);

	ext4_htree_lock_data(lck)->ld_flags = flags;
	htree_lock(lck, lhead, mode);
	if (!is_dx(dir))
		ext4_htree_safe_relock(lck); /* make sure it's safe locked */
}
EXPORT_SYMBOL(ext4_htree_lock);

static int ext4_htree_node_lock(struct htree_lock *lck, struct dx_entry *at,
				unsigned lmask, int wait, void *ev)
{
	u32	key = (at == NULL) ? 0 : dx_get_block(at);
	u32	mode;

	/* NOOP if htree is well protected or caller doesn't require the lock */
	if (ext4_htree_safe_locked(lck) ||
	   !(ext4_htree_lock_data(lck)->ld_flags & lmask))
		return 1;

	mode = (ext4_htree_lock_data(lck)->ld_flags & lmask) == lmask ?
		HTREE_LOCK_PW : HTREE_LOCK_PR;
	while (1) {
		if (htree_node_lock_try(lck, mode, key, ffz(~lmask), wait, ev))
			return 1;
		if (!(lmask & EXT4_LB_SPIN)) /* not a spinlock */
			return 0;
		cpu_relax(); /* spin until granted */
	}
}

static int ext4_htree_node_locked(struct htree_lock *lck, unsigned lmask)
{
	return ext4_htree_safe_locked(lck) ||
	       htree_node_is_granted(lck, ffz(~lmask));
}

static void ext4_htree_node_unlock(struct htree_lock *lck,
				   unsigned lmask, void *buf)
{
	/* NB: it's safe to call mutiple times or even it's not locked */
	if (!ext4_htree_safe_locked(lck) &&
	     htree_node_is_granted(lck, ffz(~lmask)))
		htree_node_unlock(lck, ffz(~lmask), buf);
}

#define ext4_htree_dx_lock(lck, key)		\
	ext4_htree_node_lock(lck, key, EXT4_LB_DX, 1, NULL)
#define ext4_htree_dx_lock_try(lck, key)	\
	ext4_htree_node_lock(lck, key, EXT4_LB_DX, 0, NULL)
#define ext4_htree_dx_unlock(lck)		\
	ext4_htree_node_unlock(lck, EXT4_LB_DX, NULL)
#define ext4_htree_dx_locked(lck)		\
	ext4_htree_node_locked(lck, EXT4_LB_DX)

static void ext4_htree_dx_need_lock(struct htree_lock *lck)
{
	struct ext4_dir_lock_data *ld;

	if (ext4_htree_safe_locked(lck))
		return;

	ld = ext4_htree_lock_data(lck);
	switch (ld->ld_flags) {
	default:
		return;
	case EXT4_HLOCK_LOOKUP:
		ld->ld_flags = EXT4_HLOCK_LOOKUP_SAFE;
		return;
	case EXT4_HLOCK_DEL:
		ld->ld_flags = EXT4_HLOCK_DEL_SAFE;
		return;
	case EXT4_HLOCK_ADD:
		ld->ld_flags = EXT4_HLOCK_SPLIT;
		return;
	}
}

#define ext4_htree_de_lock(lck, key)		\
	ext4_htree_node_lock(lck, key, EXT4_LB_DE, 1, NULL)
#define ext4_htree_de_unlock(lck)		\
	ext4_htree_node_unlock(lck, EXT4_LB_DE, NULL)

#define ext4_htree_spin_lock(lck, key, event)	\
	ext4_htree_node_lock(lck, key, EXT4_LB_SPIN, 0, event)
#define ext4_htree_spin_unlock(lck)		\
	ext4_htree_node_unlock(lck, EXT4_LB_SPIN, NULL)
#define ext4_htree_spin_unlock_listen(lck, p)	\
	ext4_htree_node_unlock(lck, EXT4_LB_SPIN, p)

static void ext4_htree_spin_stop_listen(struct htree_lock *lck)
{
	if (!ext4_htree_safe_locked(lck) &&
	    htree_node_is_listening(lck, ffz(~EXT4_LB_SPIN)))
		htree_node_stop_listen(lck, ffz(~EXT4_LB_SPIN));
}

enum {
	DX_HASH_COL_IGNORE,	/* ignore collision while probing frames */
	DX_HASH_COL_YES,	/* there is collision and it does matter */
	DX_HASH_COL_NO,		/* there is no collision */
};

static int dx_probe_hash_collision(struct htree_lock *lck,
				   struct dx_entry *entries,
				   struct dx_entry *at, u32 hash)
{
	if (!(lck && ext4_htree_lock_data(lck)->ld_flags & EXT4_LB_EXACT)) {
		return DX_HASH_COL_IGNORE; /* don't care about collision */

	} else if (at == entries + dx_get_count(entries) - 1) {
		return DX_HASH_COL_IGNORE; /* not in any leaf of this DX */

	} else { /* hash collision? */
		return ((dx_get_hash(at + 1) & ~1) == hash) ?
			DX_HASH_COL_YES : DX_HASH_COL_NO;
	}
}

/*
 * Probe for a directory leaf block to search.
 *
 * dx_probe can return ERR_BAD_DX_DIR, which means there was a format
 * error in the directory index, and the caller should fall back to
 * searching the directory normally.  The callers of dx_probe **MUST**
 * check for this error code, and make sure it never gets reflected
 * back to userspace.
 */
static struct dx_frame *
dx_probe(const struct qstr *d_name, struct inode *dir,
	 struct dx_hash_info *hinfo, struct dx_frame *frame_in,
	 struct htree_lock *lck, int *err)
{
	unsigned count, indirect;
	struct dx_entry *at, *entries, *p, *q, *m, *dx = NULL;
	struct dx_root_info * info;
	struct buffer_head *bh;
	struct dx_frame *frame = frame_in;
	u32 hash;

	memset(frame_in, 0, EXT4_HTREE_LEVEL * sizeof(frame_in[0]));
	if (!(bh = ext4_bread (NULL,dir, 0, 0, err)))
		goto fail;

	info = dx_get_dx_info((struct ext4_dir_entry_2*)bh->b_data);
	if (info->hash_version != DX_HASH_TEA &&
	    info->hash_version != DX_HASH_HALF_MD4 &&
	    info->hash_version != DX_HASH_LEGACY) {
		ext4_warning(dir->i_sb, "Unrecognised inode hash code %d for directory "
                             "#%lu", info->hash_version, dir->i_ino);
		brelse(bh);
		*err = ERR_BAD_DX_DIR;
		goto fail;
	}
	hinfo->hash_version = info->hash_version;
	if (hinfo->hash_version <= DX_HASH_TEA)
		hinfo->hash_version += EXT4_SB(dir->i_sb)->s_hash_unsigned;
	hinfo->seed = EXT4_SB(dir->i_sb)->s_hash_seed;
	if (d_name)
		ext4fs_dirhash(d_name->name, d_name->len, hinfo);
	hash = hinfo->hash;

	if (info->unused_flags & 1) {
		ext4_warning(dir->i_sb, "Unimplemented inode hash flags: %#06x",
			     info->unused_flags);
		brelse(bh);
		*err = ERR_BAD_DX_DIR;
		goto fail;
	}

	indirect = info->indirect_levels;
	if (indirect >= ext4_dir_htree_level(dir->i_sb)) {
		ext4_warning(dir->i_sb,
			     "Directory (ino: %lu) htree depth %#06x exceed "
			     "supported value", dir->i_ino,
			     ext4_dir_htree_level(dir->i_sb));
		if (ext4_dir_htree_level(dir->i_sb) < EXT4_HTREE_LEVEL) {
			ext4_warning(dir->i_sb, "Enable large directory "
						"feature to access it");
		}
		brelse(bh);
		*err = ERR_BAD_DX_DIR;
		goto fail;
	}

	entries = (struct dx_entry *) (((char *)info) + info->info_length);

	if (dx_get_limit(entries) != dx_root_limit(dir->i_sb->s_blocksize,
						   (struct ext4_dir_entry_2*)bh->b_data,
						   info->info_length)) {
		ext4_warning(dir->i_sb, "dx entry: limit != root limit");
		brelse(bh);
		*err = ERR_BAD_DX_DIR;
		goto fail;
	}

	dxtrace(printk("Look up %x", hash));
	while (1)
	{
		if (indirect == 0) { /* the last index level */
			/* NB: ext4_htree_dx_lock() could be noop if
			 * DX-lock flag is not set for current operation */
			ext4_htree_dx_lock(lck, dx);
			ext4_htree_spin_lock(lck, dx, NULL);
		}
		count = dx_get_count(entries);
		if (count == 0 || count > dx_get_limit(entries)) {
			ext4_htree_spin_unlock(lck); /* release spin */
			ext4_warning(dir->i_sb,
				     "dx entry: no count or count > limit");
			brelse(bh);
			*err = ERR_BAD_DX_DIR;
			goto fail2;
		}

		p = entries + 1;
		q = entries + count - 1;
		while (p <= q)
		{
			m = p + (q - p)/2;
			dxtrace(printk("."));
			if (dx_get_hash(m) > hash)
				q = m - 1;
			else
				p = m + 1;
		}

		if (0) // linear search cross check
		{
			unsigned n = count - 1;
			at = entries;
			while (n--)
			{
				dxtrace(printk(","));
				if (dx_get_hash(++at) > hash)
				{
					at--;
					break;
				}
			}
			assert (at == p - 1);
		}

		at = p - 1;
		dxtrace(printk(" %x->%u\n", at == entries? 0: dx_get_hash(at), dx_get_block(at)));
		frame->bh = bh;
		frame->entries = entries;
		frame->at = at;

		if (indirect == 0) { /* the last index level */
			struct ext4_dir_lock_data *ld;
			u64 myblock;

			/* By default we only lock DE-block, however, we will
			 * also lock the last level DX-block if:
			 * a) there is hash collision
			 *    we will set DX-lock flag (a few lines below)
			 *    and redo to lock DX-block
			 *    see detail in dx_probe_hash_collision()
			 * b) it's a retry from splitting
			 *    we need to lock the last level DX-block so nobody
			 *    else can split any leaf blocks under the same
			 *    DX-block, see detail in ext4_dx_add_entry()
			 */
			if (ext4_htree_dx_locked(lck)) {
				/* DX-block is locked, just lock DE-block
				 * and return */
				ext4_htree_spin_unlock(lck);
				if (!ext4_htree_safe_locked(lck))
					ext4_htree_de_lock(lck, frame->at);
				return frame;
			}
			/* it's pdirop and no DX lock */
			if (dx_probe_hash_collision(lck, entries, at, hash) ==
			    DX_HASH_COL_YES) {
				/* found hash collision, set DX-lock flag
				 * and retry to abtain DX-lock */
				ext4_htree_spin_unlock(lck);
				ext4_htree_dx_need_lock(lck);
				continue;
			}
			ld = ext4_htree_lock_data(lck);
			/* because I don't lock DX, so @at can't be trusted
			 * after I release spinlock so I have to save it */
			ld->ld_at = at;
			ld->ld_at_entry = *at;
			ld->ld_count = dx_get_count(entries);

			frame->at = &ld->ld_at_entry;
			myblock = dx_get_block(at);

			/* NB: ordering locking */
			ext4_htree_spin_unlock_listen(lck, &myblock);
			/* other thread can split this DE-block because:
			 * a) I don't have lock for the DE-block yet
			 * b) I released spinlock on DX-block
			 * if it happened I can detect it by listening
			 * splitting event on this DE-block */
			ext4_htree_de_lock(lck, frame->at);
			ext4_htree_spin_stop_listen(lck);

			if (myblock == EXT4_HTREE_NODE_CHANGED) {
				/* someone split this DE-block before
				 * I locked it, I need to retry and lock
				 * valid DE-block */
				ext4_htree_de_unlock(lck);
				continue;
			}
			return frame;
		}
		dx = at;
		indirect--;
		if (!(bh = ext4_bread (NULL,dir, dx_get_block(at), 0, err)))
			goto fail2;

		at = entries = ((struct dx_node *) bh->b_data)->entries;
		if (dx_get_limit(entries) != dx_node_limit (dir)) {
			ext4_warning(dir->i_sb,
				     "dx entry: limit != node limit");
			brelse(bh);
			*err = ERR_BAD_DX_DIR;
			goto fail2;
		}
		frame++;
		frame->bh = NULL;
	}
fail2:
	while (frame >= frame_in) {
		brelse(frame->bh);
		frame--;
	}
fail:
	if (*err == ERR_BAD_DX_DIR)
		ext4_warning(dir->i_sb,
			     "Corrupt dir inode %ld, running e2fsck is "
			     "recommended.", dir->i_ino);
	return NULL;
}

static void dx_release (struct dx_frame *frames)
{
	struct dx_root_info *info;
	int i;

	if (frames[0].bh == NULL)
		return;

	info = dx_get_dx_info((struct ext4_dir_entry_2*)frames[0].bh->b_data);
	for (i = 0; i <= info->indirect_levels; i++) {
		if (frames[i].bh == NULL)
			break;
		brelse(frames[i].bh);
		frames[i].bh = NULL;
	}
}

/*
 * This function increments the frame pointer to search the next leaf
 * block, and reads in the necessary intervening nodes if the search
 * should be necessary.  Whether or not the search is necessary is
 * controlled by the hash parameter.  If the hash value is even, then
 * the search is only continued if the next block starts with that
 * hash value.  This is used if we are searching for a specific file.
 *
 * If the hash value is HASH_NB_ALWAYS, then always go to the next block.
 *
 * This function returns 1 if the caller should continue to search,
 * or 0 if it should not.  If there is an error reading one of the
 * index blocks, it will a negative error code.
 *
 * If start_hash is non-null, it will be filled in with the starting
 * hash of the next page.
 */
static int ext4_htree_next_block(struct inode *dir, __u32 hash,
				 struct dx_frame *frame,
				 struct dx_frame *frames,
				 __u32 *start_hash, struct htree_lock *lck)
{
	struct dx_frame *p;
	struct buffer_head *bh;
	int err, num_frames = 0;
	__u32 bhash;

	p = frame;
	/*
	 * Find the next leaf page by incrementing the frame pointer.
	 * If we run out of entries in the interior node, loop around and
	 * increment pointer in the parent node.  When we break out of
	 * this loop, num_frames indicates the number of interior
	 * nodes need to be read.
	 */
	ext4_htree_de_unlock(lck);
	while (1) {
		if (num_frames > 0 || ext4_htree_dx_locked(lck)) {
			/* num_frames > 0 :
			 *   DX block
			 * ext4_htree_dx_locked:
			 *   frame->at is reliable pointer returned by dx_probe,
			 *   otherwise dx_probe already knew no collision */
			if (++(p->at) < p->entries + dx_get_count(p->entries))
				break;
		}
		if (p == frames)
			return 0;
		num_frames++;
		if (num_frames == 1)
			ext4_htree_dx_unlock(lck);
		p--;
	}

	/*
	 * If the hash is 1, then continue only if the next page has a
	 * continuation hash of any value.  This is used for readdir
	 * handling.  Otherwise, check to see if the hash matches the
	 * desired contiuation hash.  If it doesn't, return since
	 * there's no point to read in the successive index pages.
	 */
	bhash = dx_get_hash(p->at);
	if (start_hash)
		*start_hash = bhash;
	if ((hash & 1) == 0) {
		if ((bhash & ~1) != hash)
			return 0;
	}
	/*
	 * If the hash is HASH_NB_ALWAYS, we always go to the next
	 * block so no check is necessary
	 */
	while (num_frames--) {
		if (num_frames == 0) {
			/* it's not always necessary, we just don't want to
			 * detect hash collision again */
			ext4_htree_dx_need_lock(lck);
			ext4_htree_dx_lock(lck, p->at);
		}

		if (!(bh = ext4_bread(NULL, dir, dx_get_block(p->at),
				      0, &err)))
			return err; /* Failure */
		p++;
		brelse(p->bh);
		p->bh = bh;
		p->at = p->entries = ((struct dx_node *) bh->b_data)->entries;
	}
	ext4_htree_de_lock(lck, p->at);
	return 1;
}


/*
 * This function fills a red-black tree with information from a
 * directory block.  It returns the number directory entries loaded
 * into the tree.  If there is an error it is returned in err.
 */
static int htree_dirblock_to_tree(struct file *dir_file,
				  struct inode *dir, ext4_lblk_t block,
				  struct dx_hash_info *hinfo,
				  __u32 start_hash, __u32 start_minor_hash)
{
	struct buffer_head *bh;
	struct ext4_dir_entry_2 *de, *top;
	int err, count = 0;

	dxtrace(printk(KERN_INFO "In htree dirblock_to_tree: block %lu\n",
							(unsigned long)block));
	if (!(bh = ext4_bread (NULL, dir, block, 0, &err)))
		return err;

	de = (struct ext4_dir_entry_2 *) bh->b_data;
	top = (struct ext4_dir_entry_2 *) ((char *) de +
					   dir->i_sb->s_blocksize -
					   __EXT4_DIR_REC_LEN(0));
	for (; de < top; de = ext4_next_entry(de, dir->i_sb->s_blocksize)) {
		if (!ext4_check_dir_entry("htree_dirblock_to_tree", dir, de, bh,
					(block<<EXT4_BLOCK_SIZE_BITS(dir->i_sb))
						+((char *)de - bh->b_data))) {
			/* On error, skip the f_pos to the next block. */
			dir_file->f_pos = (dir_file->f_pos |
					(dir->i_sb->s_blocksize - 1)) + 1;
			brelse(bh);
			return count;
		}
		ext4fs_dirhash(de->name, de->name_len, hinfo);
		if ((hinfo->hash < start_hash) ||
		    ((hinfo->hash == start_hash) &&
		     (hinfo->minor_hash < start_minor_hash)))
			continue;
		if (de->inode == 0)
			continue;
		if ((err = ext4_htree_store_dirent(dir_file,
				   hinfo->hash, hinfo->minor_hash, de)) != 0) {
			brelse(bh);
			return err;
		}
		count++;
	}
	brelse(bh);
	return count;
}


/*
 * This function fills a red-black tree with information from a
 * directory.  We start scanning the directory in hash order, starting
 * at start_hash and start_minor_hash.
 *
 * This function returns the number of entries inserted into the tree,
 * or a negative error code.
 */
int ext4_htree_fill_tree(struct file *dir_file, __u32 start_hash,
			 __u32 start_minor_hash, __u32 *next_hash)
{
	struct dx_hash_info hinfo;
	struct ext4_dir_entry_2 *de;
	struct dx_frame frames[EXT4_HTREE_LEVEL], *frame;
	struct inode *dir;
	ext4_lblk_t block;
	int count = 0;
	int ret, err;
	__u32 hashval;

	dxtrace(printk(KERN_DEBUG "In htree_fill_tree, start hash: %x:%x\n", 
		       start_hash, start_minor_hash));
	dir = dir_file->f_path.dentry->d_inode;
	if (!(ext4_test_inode_flag(dir, EXT4_INODE_INDEX))) {
		hinfo.hash_version = EXT4_SB(dir->i_sb)->s_def_hash_version;
		if (hinfo.hash_version <= DX_HASH_TEA)
			hinfo.hash_version +=
				EXT4_SB(dir->i_sb)->s_hash_unsigned;
		hinfo.seed = EXT4_SB(dir->i_sb)->s_hash_seed;
		count = htree_dirblock_to_tree(dir_file, dir, 0, &hinfo,
					       start_hash, start_minor_hash);
		*next_hash = ~0;
		return count;
	}
	hinfo.hash = start_hash;
	hinfo.minor_hash = 0;
	/* assume it's PR locked */
	frame = dx_probe(NULL, dir, &hinfo, frames, NULL, &err);
	if (!frame)
		return err;
	/* Add '.' and '..' from the htree header */
	if (!start_hash && !start_minor_hash) {
		de = (struct ext4_dir_entry_2 *) frames[0].bh->b_data;
		if ((err = ext4_htree_store_dirent(dir_file, 0, 0, de)) != 0)
			goto errout;
		count++;
	}
	if (start_hash < 2 || (start_hash ==2 && start_minor_hash==0)) {
		de = (struct ext4_dir_entry_2 *) frames[0].bh->b_data;
		de = ext4_next_entry(de, dir->i_sb->s_blocksize);
		if ((err = ext4_htree_store_dirent(dir_file, 2, 0, de)) != 0)
			goto errout;
		count++;
	}

	while (1) {
		block = dx_get_block(frame->at);
		ret = htree_dirblock_to_tree(dir_file, dir, block, &hinfo,
					     start_hash, start_minor_hash);
		if (ret < 0) {
			err = ret;
			goto errout;
		}
		count += ret;
		hashval = ~0;
		ret = ext4_htree_next_block(dir, HASH_NB_ALWAYS,
					    frame, frames, &hashval, NULL);
		*next_hash = hashval;
		if (ret < 0) {
			err = ret;
			goto errout;
		}
		/*
		 * Stop if:  (a) there are no more entries, or
		 * (b) we have inserted at least one entry and the
		 * next hash value is not a continuation
		 */
		if ((ret == 0) ||
		    (count && ((hashval & 1) == 0)))
			break;
	}
	dx_release(frames);
	dxtrace(printk(KERN_DEBUG "Fill tree: returned %d entries, "
		       "next hash: %x\n", count, *next_hash));
	return count;
errout:
	dx_release(frames);
	return (err);
}


/*
 * Directory block splitting, compacting
 */

/*
 * Create map of hash values, offsets, and sizes, stored at end of block.
 * Returns number of entries mapped.
 */
static int dx_make_map(struct ext4_dir_entry_2 *de, unsigned blocksize,
		       struct dx_hash_info *hinfo,
		       struct dx_map_entry *map_tail)
{
	int count = 0;
	char *base = (char *) de;
	struct dx_hash_info h = *hinfo;

	while ((char *) de < base + blocksize) {
		if (de->name_len && de->inode) {
			ext4fs_dirhash(de->name, de->name_len, &h);
			map_tail--;
			map_tail->hash = h.hash;
			map_tail->offs = ((char *) de - base)>>2;
			map_tail->size = le16_to_cpu(de->rec_len);
			count++;
			cond_resched();
		}
		/* XXX: do we need to check rec_len == 0 case? -Chris */
		de = ext4_next_entry(de, blocksize);
	}
	return count;
}

/* Sort map by hash value */
static void dx_sort_map (struct dx_map_entry *map, unsigned count)
{
	struct dx_map_entry *p, *q, *top = map + count - 1;
	int more;
	/* Combsort until bubble sort doesn't suck */
	while (count > 2) {
		count = count*10/13;
		if (count - 9 < 2) /* 9, 10 -> 11 */
			count = 11;
		for (p = top, q = p - count; q >= map; p--, q--)
			if (p->hash < q->hash)
				swap(*p, *q);
	}
	/* Garden variety bubble sort */
	do {
		more = 0;
		q = top;
		while (q-- > map) {
			if (q[1].hash >= q[0].hash)
				continue;
			swap(*(q+1), *q);
			more = 1;
		}
	} while(more);
}

static void dx_insert_block(struct dx_frame *frame, u32 hash, ext4_lblk_t block)
{
	struct dx_entry *entries = frame->entries;
	struct dx_entry *old = frame->at, *new = old + 1;
	int count = dx_get_count(entries);

	assert(count < dx_get_limit(entries));
	assert(old < entries + count);
	memmove(new + 1, new, (char *)(entries + count) - (char *)(new));
	dx_set_hash(new, hash);
	dx_set_block(new, block);
	dx_set_count(entries, count + 1);
}

static void ext4_update_dx_flag(struct inode *inode)
{
	/* Disable it for ldiskfs, because going from a DX directory to
	 * a non-DX directory while it is in use will completely break
	 * the htree-locking.
	 * If we really want to support this operation in the future,
	 * we need to exclusively lock the directory at here which will
	 * increase complexity of code */
#if 0
	if (!EXT4_HAS_COMPAT_FEATURE(inode->i_sb,
				     EXT4_FEATURE_COMPAT_DIR_INDEX))
		ext4_clear_inode_flag(inode, EXT4_INODE_INDEX);
#endif
}

/*
 * NOTE! unlike strncmp, ext4_match returns 1 for success, 0 for failure.
 *
 * `len <= EXT4_NAME_LEN' is guaranteed by caller.
 * `de != NULL' is guaranteed by caller.
 */
static inline int ext4_match (int len, const char * const name,
			      struct ext4_dir_entry_2 * de)
{
	if (len != de->name_len)
		return 0;
	if (!de->inode)
		return 0;
	return !memcmp(name, de->name, len);
}

/*
 * Returns 0 if not found, -1 on failure, and 1 on success
 */
static inline int search_dirblock(struct buffer_head *bh,
				  struct inode *dir,
				  const struct qstr *d_name,
				  unsigned int offset,
				  struct ext4_dir_entry_2 ** res_dir)
{
	struct ext4_dir_entry_2 * de;
	char * dlimit;
	int de_len;
	const char *name = d_name->name;
	int namelen = d_name->len;

	de = (struct ext4_dir_entry_2 *) bh->b_data;
	dlimit = bh->b_data + dir->i_sb->s_blocksize;
	while ((char *) de < dlimit) {
		/* this code is executed quadratically often */
		/* do minimal checking `by hand' */

		if ((char *) de + namelen <= dlimit &&
		    ext4_match (namelen, name, de)) {
			/* found a match - just to be sure, do a full check */
			if (!ext4_check_dir_entry("ext4_find_entry",
						  dir, de, bh, offset))
				return -1;
			*res_dir = de;
			return 1;
		}
		/* prevent looping on a bad block */
		de_len = ext4_rec_len_from_disk(de->rec_len,
						dir->i_sb->s_blocksize);
		if (de_len <= 0)
			return -1;
		offset += de_len;
		de = (struct ext4_dir_entry_2 *) ((char *) de + de_len);
	}
	return 0;
}


/*
 *	ext4_find_entry()
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 *
 * The returned buffer_head has ->b_count elevated.  The caller is expected
 * to brelse() it when appropriate.
 */
struct buffer_head * __ext4_find_entry(struct inode *dir,
					const struct qstr *d_name,
					struct ext4_dir_entry_2 **res_dir,
					struct htree_lock *lck)
{
	struct super_block *sb;
	struct buffer_head *bh_use[NAMEI_RA_SIZE];
	struct buffer_head *bh, *ret = NULL;
	ext4_lblk_t start, block, b;
	int ra_max = 0;		/* Number of bh's in the readahead
				   buffer, bh_use[] */
	int ra_ptr = 0;		/* Current index into readahead
				   buffer */
	int num = 0;
	ext4_lblk_t  nblocks;
	int i, err;
	int namelen;

	*res_dir = NULL;
	sb = dir->i_sb;
	namelen = d_name->len;
	if (namelen > EXT4_NAME_LEN)
		return NULL;
	if (is_dx(dir)) {
		bh = ext4_dx_find_entry(dir, d_name, res_dir, lck, &err);
		/*
		 * On success, or if the error was file not found,
		 * return.  Otherwise, fall back to doing a search the
		 * old fashioned way.
		 */
		if (bh || (err != ERR_BAD_DX_DIR))
			return bh;
		dxtrace(printk(KERN_DEBUG "ext4_find_entry: dx failed, "
			       "falling back\n"));
		ext4_htree_safe_relock(lck);
	}
	nblocks = dir->i_size >> EXT4_BLOCK_SIZE_BITS(sb);
	start = EXT4_I(dir)->i_dir_start_lookup;
	if (start >= nblocks)
		start = 0;
	block = start;
restart:
	do {
		/*
		 * We deal with the read-ahead logic here.
		 */
		if (ra_ptr >= ra_max) {
			/* Refill the readahead buffer */
			ra_ptr = 0;
			b = block;
			for (ra_max = 0; ra_max < NAMEI_RA_SIZE; ra_max++) {
				/*
				 * Terminate if we reach the end of the
				 * directory and must wrap, or if our
				 * search has finished at this block.
				 */
				if (b >= nblocks || (num && block == start)) {
					bh_use[ra_max] = NULL;
					break;
				}
				num++;
				bh = ext4_getblk(NULL, dir, b++, 0, &err);
				bh_use[ra_max] = bh;
				if (bh)
					ll_rw_block(READ_META, 1, &bh);
			}
		}
		if ((bh = bh_use[ra_ptr++]) == NULL)
			goto next;
		wait_on_buffer(bh);
		if (!buffer_uptodate(bh)) {
			/* read error, skip block & hope for the best */
			ext4_error(sb, "reading directory #%lu offset %lu",
				   dir->i_ino, (unsigned long)block);
			brelse(bh);
			goto next;
		}
		i = search_dirblock(bh, dir, d_name,
			    block << EXT4_BLOCK_SIZE_BITS(sb), res_dir);
		if (i == 1) {
			EXT4_I(dir)->i_dir_start_lookup = block;
			ret = bh;
			goto cleanup_and_exit;
		} else {
			brelse(bh);
			if (i < 0)
				goto cleanup_and_exit;
		}
	next:
		if (++block >= nblocks)
			block = 0;
	} while (block != start);

	/*
	 * If the directory has grown while we were searching, then
	 * search the last part of the directory before giving up.
	 */
	block = nblocks;
	nblocks = dir->i_size >> EXT4_BLOCK_SIZE_BITS(sb);
	if (block < nblocks) {
		start = 0;
		goto restart;
	}

cleanup_and_exit:
	/* Clean up the read-ahead blocks */
	for (; ra_ptr < ra_max; ra_ptr++)
		brelse(bh_use[ra_ptr]);
	return ret;
}
EXPORT_SYMBOL(__ext4_find_entry);

static struct buffer_head * ext4_dx_find_entry(struct inode *dir,
				const struct qstr *d_name,
				struct ext4_dir_entry_2 **res_dir,
				struct htree_lock *lck, int *err)
{
	struct super_block * sb;
	struct dx_hash_info	hinfo;
	u32 hash;
	struct dx_frame frames[EXT4_HTREE_LEVEL], *frame;
	struct ext4_dir_entry_2 *de, *top;
	struct buffer_head *bh;
	ext4_lblk_t block;
	int retval;
	int namelen = d_name->len;
	const u8 *name = d_name->name;

	sb = dir->i_sb;
	/* NFS may look up ".." - look at dx_root directory block */
	if (namelen > 2 || name[0] != '.'||(name[1] != '.' && name[1] != '\0')){
		if (!(frame = dx_probe(d_name, dir, &hinfo, frames, lck, err)))
			return NULL;
	} else {
		frame = frames;
		frame->bh = NULL;			/* for dx_release() */
		frame->at = (struct dx_entry *)frames;	/* hack for zero entry*/
		dx_set_block(frame->at, 0);		/* dx_root block is 0 */
		/* "." and ".." are stored in root DX lock */
		ext4_htree_dx_need_lock(lck);
		ext4_htree_dx_lock(lck, NULL);
	}
	hash = hinfo.hash;
	do {
		block = dx_get_block(frame->at);
		if (!(bh = ext4_bread (NULL,dir, block, 0, err)))
			goto errout;
		de = (struct ext4_dir_entry_2 *) bh->b_data;
		top = (struct ext4_dir_entry_2 *) ((char *) de + sb->s_blocksize -
				       __EXT4_DIR_REC_LEN(0));
		for (; de < top; de = ext4_next_entry(de, sb->s_blocksize)) {
			int off = (block << EXT4_BLOCK_SIZE_BITS(sb))
				  + ((char *) de - bh->b_data);

			if (!ext4_check_dir_entry(__func__, dir, de, bh, off)) {
				brelse(bh);
				*err = ERR_BAD_DX_DIR;
				goto errout;
			}

			if (ext4_match(namelen, name, de)) {
				*res_dir = de;
				dx_release(frames);
				return bh;
			}
		}
		brelse(bh);
		/* Check to see if we should continue to search */
		retval = ext4_htree_next_block(dir, hash, frame,
					       frames, NULL, lck);
		if (retval < 0) {
			ext4_warning(sb,
			     "error reading index page in directory #%lu",
			     dir->i_ino);
			*err = retval;
			goto errout;
		}
	} while (retval == 1);

	*err = -ENOENT;
errout:
	dxtrace(printk(KERN_DEBUG "%s not found\n", name));
	dx_release (frames);
	return NULL;
}

static struct dentry *ext4_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *nd)
{
	struct inode *inode;
	struct ext4_dir_entry_2 *de;
	struct buffer_head *bh;

	if (dentry->d_name.len > EXT4_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	bh = ext4_find_entry(dir, &dentry->d_name, &de);
	inode = NULL;
	if (bh) {
		__u32 ino = le32_to_cpu(de->inode);
		brelse(bh);
		if (!ext4_valid_inum(dir->i_sb, ino)) {
			ext4_error(dir->i_sb, "bad inode number: %u", ino);
			return ERR_PTR(-EIO);
		}
		inode = ext4_iget(dir->i_sb, ino);
		if (unlikely(IS_ERR(inode))) {
			if (PTR_ERR(inode) == -ESTALE) {
				ext4_error(dir->i_sb,
						"deleted inode referenced: %u",
						ino);
				return ERR_PTR(-EIO);
			} else {
				return ERR_CAST(inode);
			}
		}
	}
	/* ".." shouldn't go into dcache to preserve dcache hierarchy
	 * otherwise we'll get parent being a child of actual child.
	 * see bug 10458 for details -bzzz */
	if (inode && (dentry->d_name.name[0] == '.' && (dentry->d_name.len == 1 ||
		(dentry->d_name.len == 2 && dentry->d_name.name[1] == '.')))) {
		struct dentry *tmp, *goal = NULL;
		struct list_head *lp;

		/* first, look for an existing dentry - any one is good */
		spin_lock(&dcache_lock);
		list_for_each(lp, &inode->i_dentry) {
			tmp = list_entry(lp, struct dentry, d_alias);
			goal = tmp;
			dget_locked(goal);
			break;
		}
		if (goal == NULL) {
			/* there is no alias, we need to make current dentry:
			 *  a) inaccessible for __d_lookup()
			 *  b) inaccessible for iopen */
			J_ASSERT(list_empty(&dentry->d_alias));
			dentry->d_flags |= DCACHE_NFSFS_RENAMED;
			/* this is d_instantiate() ... */
			list_add(&dentry->d_alias, &inode->i_dentry);
			dentry->d_inode = inode;
		}
		spin_unlock(&dcache_lock);
		if (goal)
			iput(inode);
		return goal;
	}

	return d_splice_alias(inode, dentry);
}


struct dentry *ext4_get_parent(struct dentry *child)
{
	__u32 ino;
	struct inode *inode;
	static const struct qstr dotdot = {
		.name = "..",
		.len = 2,
	};
	struct ext4_dir_entry_2 * de;
	struct buffer_head *bh;

	bh = ext4_find_entry(child->d_inode, &dotdot, &de);
	inode = NULL;
	if (!bh)
		return ERR_PTR(-ENOENT);
	ino = le32_to_cpu(de->inode);
	brelse(bh);

	if (!ext4_valid_inum(child->d_inode->i_sb, ino)) {
		ext4_error(child->d_inode->i_sb,
			   "bad inode number: %u", ino);
		return ERR_PTR(-EIO);
	}

	return d_obtain_alias(ext4_iget(child->d_inode->i_sb, ino));
}

#define S_SHIFT 12
static unsigned char ext4_type_by_mode[S_IFMT >> S_SHIFT] = {
	[S_IFREG >> S_SHIFT]	= EXT4_FT_REG_FILE,
	[S_IFDIR >> S_SHIFT]	= EXT4_FT_DIR,
	[S_IFCHR >> S_SHIFT]	= EXT4_FT_CHRDEV,
	[S_IFBLK >> S_SHIFT]	= EXT4_FT_BLKDEV,
	[S_IFIFO >> S_SHIFT]	= EXT4_FT_FIFO,
	[S_IFSOCK >> S_SHIFT]	= EXT4_FT_SOCK,
	[S_IFLNK >> S_SHIFT]	= EXT4_FT_SYMLINK,
};

static inline void ext4_set_de_type(struct super_block *sb,
				struct ext4_dir_entry_2 *de,
				umode_t mode) {
	if (EXT4_HAS_INCOMPAT_FEATURE(sb, EXT4_FEATURE_INCOMPAT_FILETYPE))
		de->file_type = ext4_type_by_mode[(mode & S_IFMT)>>S_SHIFT];
}

/*
 * Move count entries from end of map between two memory locations.
 * Returns pointer to last entry moved.
 */
static struct ext4_dir_entry_2 *
dx_move_dirents(char *from, char *to, struct dx_map_entry *map, int count,
		unsigned blocksize)
{
	unsigned rec_len = 0;

	while (count--) {
		struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *) 
						(from + (map->offs<<2));
		rec_len = EXT4_DIR_REC_LEN(de);
		memcpy (to, de, rec_len);
		((struct ext4_dir_entry_2 *) to)->rec_len =
				ext4_rec_len_to_disk(rec_len, blocksize);
		de->inode = 0;
		map++;
		to += rec_len;
	}
	return (struct ext4_dir_entry_2 *) (to - rec_len);
}

/*
 * Compact each dir entry in the range to the minimal rec_len.
 * Returns pointer to last entry in range.
 */
static struct ext4_dir_entry_2* dx_pack_dirents(char *base, unsigned blocksize)
{
	struct ext4_dir_entry_2 *next, *to, *prev, *de = (struct ext4_dir_entry_2 *) base;
	unsigned rec_len = 0;

	prev = to = de;
	while ((char*)de < base + blocksize) {
		next = ext4_next_entry(de, blocksize);
		if (de->inode && de->name_len) {
			rec_len = EXT4_DIR_REC_LEN(de);
			if (de > to)
				memmove(to, de, rec_len);
			to->rec_len = ext4_rec_len_to_disk(rec_len, blocksize);
			prev = to;
			to = (struct ext4_dir_entry_2 *) (((char *) to) + rec_len);
		}
		de = next;
	}
	return prev;
}

/*
 * Split a full leaf block to make room for a new dir entry.
 * Allocate a new block, and move entries so that they are approx. equally full.
 * Returns pointer to de in block into which the new entry will be inserted.
 */
static struct ext4_dir_entry_2 *do_split(handle_t *handle, struct inode *dir,
			struct buffer_head **bh, struct dx_frame *frames,
			struct dx_frame *frame, struct dx_hash_info *hinfo,
			struct htree_lock *lck, int *error)
{
	unsigned blocksize = dir->i_sb->s_blocksize;
	unsigned count, continued;
	struct buffer_head *bh2;
	ext4_lblk_t newblock;
	u32 hash2;
	struct dx_map_entry *map;
	char *data1 = (*bh)->b_data, *data2;
	unsigned split, move, size;
	struct ext4_dir_entry_2 *de = NULL, *de2;
	int	err = 0, i;

	bh2 = ext4_append (handle, dir, &newblock, &err);
	if (!(bh2)) {
		brelse(*bh);
		*bh = NULL;
		goto errout;
	}

	BUFFER_TRACE(*bh, "get_write_access");
	err = ext4_journal_get_write_access(handle, *bh);
	if (err)
		goto journal_error;

	BUFFER_TRACE(frame->bh, "get_write_access");
	err = ext4_journal_get_write_access(handle, frame->bh);
	if (err)
		goto journal_error;

	data2 = bh2->b_data;

	/* create map in the end of data2 block */
	map = (struct dx_map_entry *) (data2 + blocksize);
	count = dx_make_map((struct ext4_dir_entry_2 *) data1,
			     blocksize, hinfo, map);
	map -= count;
	dx_sort_map(map, count);
	/* Split the existing block in the middle, size-wise */
	size = 0;
	move = 0;
	for (i = count-1; i >= 0; i--) {
		/* is more than half of this entry in 2nd half of the block? */
		if (size + map[i].size/2 > blocksize/2)
			break;
		size += map[i].size;
		move++;
	}
	/* map index at which we will split */
	split = count - move;
	hash2 = map[split].hash;
	continued = hash2 == map[split - 1].hash;
	dxtrace(printk(KERN_INFO "Split block %lu at %x, %i/%i\n",
			(unsigned long)dx_get_block(frame->at),
					hash2, split, count-split));

	/* Fancy dance to stay within two buffers */
	if (hinfo->hash < hash2) {
		de2 = dx_move_dirents(data1, data2, map + split,
				      count - split, blocksize);
	} else {
		/* make sure we will add entry to the same block which
		 * we have already locked */
		de2 = dx_move_dirents(data1, data2, map, split, blocksize);
	}
	de = dx_pack_dirents(data1, blocksize);
	de->rec_len = ext4_rec_len_to_disk(data1 + blocksize - (char *) de,
					   blocksize);
	de2->rec_len = ext4_rec_len_to_disk(data2 + blocksize - (char *) de2,
					    blocksize);
	dxtrace(dx_show_leaf (hinfo, (struct ext4_dir_entry_2 *) data1, blocksize, 1));
	dxtrace(dx_show_leaf (hinfo, (struct ext4_dir_entry_2 *) data2, blocksize, 1));

	ext4_htree_spin_lock(lck, frame > frames ? (frame - 1)->at : NULL,
			     frame->at); /* notify block is being split */
	if (hinfo->hash < hash2) {
		dx_insert_block(frame, hash2 + continued, newblock);

	} else {
		/* switch block number */
		dx_insert_block(frame, hash2 + continued,
				dx_get_block(frame->at));
		dx_set_block(frame->at, newblock);
		(frame->at)++;
	}
	ext4_htree_spin_unlock(lck);
	ext4_htree_dx_unlock(lck);

	err = ext4_handle_dirty_metadata(handle, dir, bh2);
	if (err)
		goto journal_error;
	err = ext4_handle_dirty_metadata(handle, dir, frame->bh);
	if (err)
		goto journal_error;
	brelse(bh2);
	dxtrace(dx_show_index("frame", frame->entries));
	return de;

journal_error:
	brelse(*bh);
	brelse(bh2);
	*bh = NULL;
	ext4_std_error(dir->i_sb, err);
errout:
	*error = err;
	return NULL;
}

/*
 * Add a new entry into a directory (leaf) block.  If de is non-NULL,
 * it points to a directory entry which is guaranteed to be large
 * enough for new directory entry.  If de is NULL, then
 * add_dirent_to_buf will attempt search the directory block for
 * space.  It will return -ENOSPC if no space is available, and -EIO
 * and -EEXIST if directory entry already exists.
 */
static int add_dirent_to_buf(handle_t *handle, struct dentry *dentry,
			     struct inode *inode, struct ext4_dir_entry_2 *de,
			     struct buffer_head *bh)
{
	struct inode	*dir = dentry->d_parent->d_inode;
	const char	*name = dentry->d_name.name;
	int		namelen = dentry->d_name.len;
	unsigned int	offset = 0;
	unsigned int	blocksize = dir->i_sb->s_blocksize;
	unsigned short	reclen, dotdot_reclen = 0;
	int		nlen, rlen, err, dlen = 0;
	bool		is_dotdot = false, write_short_dotdot = false;
	unsigned char	*data;
	char		*top;

	data = ext4_dentry_get_data(inode->i_sb, (struct ext4_dentry_param *)
						dentry->d_fsdata);
	if (data)
		dlen = (*data) + 1;

	is_dotdot = (namelen == 2 &&
		     memcmp(dentry->d_name.name, "..", 2) == 0);

	/* dotdot entries must be in the second place in a directory block,
	 * so calculate an alternate length without the FID so they can
	 * always be made to fit in the existing slot - LU-5626 */
	if (is_dotdot)
		dotdot_reclen = __EXT4_DIR_REC_LEN(namelen);

	reclen = __EXT4_DIR_REC_LEN(namelen + dlen);

	if (!de) {
		de = (struct ext4_dir_entry_2 *)bh->b_data;
		top = bh->b_data + blocksize - reclen;
		while ((char *) de <= top) {
			if (!ext4_check_dir_entry("ext4_add_entry", dir, de,
						  bh, offset))
				return -EIO;
			if (ext4_match(namelen, name, de))
				return -EEXIST;
			nlen = EXT4_DIR_REC_LEN(de);
			rlen = ext4_rec_len_from_disk(de->rec_len, blocksize);
			/* Check first for enough space for the full entry */
			if ((de->inode ? rlen - nlen : rlen) >= reclen)
				break;
			/* Then for dotdot entries, check for the smaller space
			 * required for just the entry, no FID */
			if (is_dotdot) {
				if ((de->inode ? rlen - nlen : rlen) >=
				    dotdot_reclen) {
					write_short_dotdot = true;
					break;
				}
				/* The new ".." entry mut be written over the
				 * previous ".." entry, which is the first
				 * entry traversed by this scan.  If it doesn't
				 * fit, something is badly wrong, so -EIO. */
				return -EIO;
			}
			de = (struct ext4_dir_entry_2 *)((char *)de + rlen);
			offset += rlen;
		}
		if ((char *) de > top)
			return -ENOSPC;
	}
	BUFFER_TRACE(bh, "get_write_access");
	err = ext4_journal_get_write_access(handle, bh);
	if (err) {
		ext4_std_error(dir->i_sb, err);
		return err;
	}

	/* By now the buffer is marked for journaling */
	nlen = EXT4_DIR_REC_LEN(de);
	rlen = ext4_rec_len_from_disk(de->rec_len, blocksize);
	if (de->inode) {
		struct ext4_dir_entry_2 *de1 = (struct ext4_dir_entry_2 *)((char *)de + nlen);
		de1->rec_len = ext4_rec_len_to_disk(rlen - nlen, blocksize);
		de->rec_len = ext4_rec_len_to_disk(nlen, blocksize);
		de = de1;
	}
	de->file_type = EXT4_FT_UNKNOWN;
	if (inode) {
		de->inode = cpu_to_le32(inode->i_ino);
		ext4_set_de_type(dir->i_sb, de, inode->i_mode);
	} else
		de->inode = 0;
	de->name_len = namelen;
	memcpy(de->name, name, namelen);
	/* If we're writing the short form of "dotdot", don't add the data section */
	if (data && !write_short_dotdot) {
		de->name[namelen] = 0;
		memcpy(&de->name[namelen + 1], data, *(char *) data);
		de->file_type |= EXT4_DIRENT_LUFID;
	}

	/*
	 * XXX shouldn't update any times until successful
	 * completion of syscall, but too many callers depend
	 * on this.
	 *
	 * XXX similarly, too many callers depend on
	 * ext4_new_inode() setting the times, but error
	 * recovery deletes the inode, so the worst that can
	 * happen is that the times are slightly out of date
	 * and/or different from the directory change time.
	 */
	dir->i_mtime = dir->i_ctime = ext4_current_time(dir);
	ext4_update_dx_flag(dir);
	inode_inc_iversion(dir);
	ext4_mark_inode_dirty(handle, dir);
	BUFFER_TRACE(bh, "call ext4_handle_dirty_metadata");
	err = ext4_handle_dirty_metadata(handle, dir, bh);
	if (err)
		ext4_std_error(dir->i_sb, err);
	return 0;
}

/*
 * This converts a one block unindexed directory to a 3 block indexed
 * directory, and adds the dentry to the indexed directory.
 */
static int make_indexed_dir(handle_t *handle, struct dentry *dentry,
			    struct inode *inode, struct buffer_head *bh)
{
	struct inode	*dir = dentry->d_parent->d_inode;
	const char	*name = dentry->d_name.name;
	int		namelen = dentry->d_name.len;
	struct buffer_head *bh2;
	struct dx_frame	frames[EXT4_HTREE_LEVEL], *frame;
	struct dx_entry *entries;
	struct ext4_dir_entry_2 *de, *de2, *dot_de, *dotdot_de;
	char		*data1, *top;
	unsigned	len;
	int		retval;
	unsigned	blocksize;
	struct dx_hash_info hinfo;
	ext4_lblk_t  block;
	struct dx_root_info *dx_info;

	blocksize =  dir->i_sb->s_blocksize;
	dxtrace(printk(KERN_DEBUG "Creating index: inode %lu\n", dir->i_ino));
	retval = ext4_journal_get_write_access(handle, bh);
	if (retval) {
		ext4_std_error(dir->i_sb, retval);
		brelse(bh);
		return retval;
	}

	dot_de = (struct ext4_dir_entry_2 *) bh->b_data;
	dotdot_de = ext4_next_entry(dot_de, blocksize);

	/* The 0th block becomes the root, move the dirents out */
	de = (struct ext4_dir_entry_2 *)((char *)dotdot_de +
		ext4_rec_len_from_disk(dotdot_de->rec_len, blocksize));
	if ((char *) de >= (((char *) dot_de) + blocksize)) {
		ext4_error(dir->i_sb,
			   "invalid rec_len for '..' in inode %lu",
			   dir->i_ino);
		brelse(bh);
		return -EIO;
	}
	len = ((char *) dot_de) + blocksize - (char *) de;

	/* Allocate new block for the 0th block's dirents */
	bh2 = ext4_append(handle, dir, &block, &retval);
	if (!(bh2)) {
		brelse(bh);
		return retval;
	}
	ext4_set_inode_flag(dir, EXT4_INODE_INDEX);
	data1 = bh2->b_data;

	memcpy (data1, de, len);
	de = (struct ext4_dir_entry_2 *) data1;
	top = data1 + len;
	while ((char *)(de2 = ext4_next_entry(de, blocksize)) < top)
		de = de2;
	de->rec_len = ext4_rec_len_to_disk(data1 + blocksize - (char *) de,
					   blocksize);
	/* Initialize the root; the dot dirents already exist */
	dotdot_de->rec_len = ext4_rec_len_to_disk(blocksize -
			le16_to_cpu(dot_de->rec_len), blocksize);

	/* initialize hashing info */
	dx_info = dx_get_dx_info(dot_de);
	memset (dx_info, 0, sizeof(*dx_info));
	dx_info->info_length = sizeof(*dx_info);
	dx_info->hash_version = EXT4_SB(dir->i_sb)->s_def_hash_version;

	entries = (void *)dx_info + sizeof(*dx_info);

	dx_set_block(entries, 1);
	dx_set_count(entries, 1);
	dx_set_limit(entries, dx_root_limit(dir->i_sb->s_blocksize,
					 dot_de, sizeof(*dx_info)));

	/* Initialize as for dx_probe */
	hinfo.hash_version = dx_info->hash_version;
	if (hinfo.hash_version <= DX_HASH_TEA)
		hinfo.hash_version += EXT4_SB(dir->i_sb)->s_hash_unsigned;
	hinfo.seed = EXT4_SB(dir->i_sb)->s_hash_seed;
	ext4fs_dirhash(name, namelen, &hinfo);
	frame = frames;
	frame->entries = entries;
	frame->at = entries;
	frame->bh = bh;
	bh = bh2;

	ext4_handle_dirty_metadata(handle, dir, frame->bh);
	ext4_handle_dirty_metadata(handle, dir, bh);

	de = do_split(handle,dir, &bh, frames, frame, &hinfo, NULL, &retval);
	if (!de) {
		/*
		 * Even if the block split failed, we have to properly write
		 * out all the changes we did so far. Otherwise we can end up
		 * with corrupted filesystem.
		 */
		ext4_mark_inode_dirty(handle, dir);
		dx_release(frames);
		return retval;
	}
	dx_release(frames);

	retval = add_dirent_to_buf(handle, dentry, inode, de, bh);
	brelse(bh);
	return retval;
}

/* update ".." for hash-indexed directory, split the item "." if necessary */
static int ext4_update_dotdot(handle_t *handle, struct dentry *dentry,
				 struct inode *inode)
{
	struct inode * dir = dentry->d_parent->d_inode;
	struct buffer_head * dir_block;
	struct ext4_dir_entry_2 * de;
	int len, journal = 0, err = 0;
	int dlen = 0;
	char *data;

	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		handle->h_sync = 1;

	dir_block = ext4_bread(handle, dir, 0, 0, &err);
	if (!dir_block)
		goto out;

	de = (struct ext4_dir_entry_2 *)dir_block->b_data;
	/* the first item must be "." */
	assert(de->name_len == 1 && de->name[0] == '.');
	len = le16_to_cpu(de->rec_len);
	assert(len >= __EXT4_DIR_REC_LEN(1));
	if (len > __EXT4_DIR_REC_LEN(1)) {
		BUFFER_TRACE(dir_block, "get_write_access");
		err = ext4_journal_get_write_access(handle, dir_block);
		if (err)
			goto out_journal;

		journal = 1;
		de->rec_len = cpu_to_le16(EXT4_DIR_REC_LEN(de));
	}

	len -= EXT4_DIR_REC_LEN(de);
	data = ext4_dentry_get_data(dir->i_sb,
			(struct ext4_dentry_param *) dentry->d_fsdata);
	if (data)
		dlen = *data + 1;
	assert(len == 0 || len >= __EXT4_DIR_REC_LEN(2 + dlen));

	de = (struct ext4_dir_entry_2 *)
			((char *) de + le16_to_cpu(de->rec_len));
	if (!journal) {
		BUFFER_TRACE(dir_block, "get_write_access");
		err = ext4_journal_get_write_access(handle, dir_block);
		if (err)
			goto out_journal;
	}

	de->inode = cpu_to_le32(inode->i_ino);
	if (len > 0)
		de->rec_len = cpu_to_le16(len);
	else
		assert(le16_to_cpu(de->rec_len) >= __EXT4_DIR_REC_LEN(2));
	de->name_len = 2;
	strcpy (de->name, "..");
	if (data != NULL && ext4_get_dirent_data_len(de) >= dlen) {
		de->name[2] = 0;
		memcpy(&de->name[2 + 1], data, *data);
		ext4_set_de_type(dir->i_sb, de, S_IFDIR);
		de->file_type |= EXT4_DIRENT_LUFID;
	}

out_journal:
	if (journal) {
		BUFFER_TRACE(dir_block, "call ext4_handle_dirty_metadata");
		err = ext4_handle_dirty_metadata(handle, dir, dir_block);
		ext4_mark_inode_dirty(handle, dir);
	}
	brelse (dir_block);

out:
	return err;
}

/*
 *	ext4_add_entry()
 *
 * adds a file entry to the specified directory, using the same
 * semantics as ext4_find_entry(). It returns NULL if it failed.
 *
 * NOTE!! The inode part of 'de' is left at 0 - which means you
 * may not sleep between calling this and putting something into
 * the entry, as someone else might have used it while you slept.
 */
int __ext4_add_entry(handle_t *handle, struct dentry *dentry,
		      struct inode *inode, struct htree_lock *lck)
{
	struct inode *dir = dentry->d_parent->d_inode;
	struct buffer_head *bh;
	struct ext4_dir_entry_2 *de;
	struct super_block *sb;
	int	retval;
	int	dx_fallback=0;
	unsigned blocksize;
	ext4_lblk_t block, blocks;

	sb = dir->i_sb;
	blocksize = sb->s_blocksize;
	if (!dentry->d_name.len)
		return -EINVAL;
	if (is_dx(dir)) {
		if (dentry->d_name.len == 2 &&
		    memcmp(dentry->d_name.name, "..", 2) == 0)
			return ext4_update_dotdot(handle, dentry, inode);
		retval = ext4_dx_add_entry(handle, dentry, inode, lck);
		if (!retval || (retval != ERR_BAD_DX_DIR))
			return retval;
		ext4_htree_safe_relock(lck);
		ext4_clear_inode_flag(dir, EXT4_INODE_INDEX);
		dx_fallback++;
		ext4_mark_inode_dirty(handle, dir);
	}
	blocks = dir->i_size >> sb->s_blocksize_bits;
	for (block = 0; block < blocks; block++) {
		bh = ext4_bread(handle, dir, block, 0, &retval);
		if(!bh)
			return retval;
		retval = add_dirent_to_buf(handle, dentry, inode, NULL, bh);
		if (retval != -ENOSPC) {
			brelse(bh);
			return retval;
		}

		if (blocks == 1 && !dx_fallback &&
		    EXT4_HAS_COMPAT_FEATURE(sb, EXT4_FEATURE_COMPAT_DIR_INDEX))
			return make_indexed_dir(handle, dentry, inode, bh);
		brelse(bh);
	}
	bh = ext4_append(handle, dir, &block, &retval);
	if (!bh)
		return retval;
	de = (struct ext4_dir_entry_2 *) bh->b_data;
	de->inode = 0;
	de->rec_len = ext4_rec_len_to_disk(blocksize, blocksize);
	retval = add_dirent_to_buf(handle, dentry, inode, de, bh);
	brelse(bh);
	return retval;
}
EXPORT_SYMBOL(__ext4_add_entry);

/*
 * Returns 0 for success, or a negative error value
 */
static int ext4_dx_add_entry(handle_t *handle, struct dentry *dentry,
			     struct inode *inode, struct htree_lock *lck)
{
	struct dx_frame frames[EXT4_HTREE_LEVEL], *frame;
	struct dx_entry *entries, *at;
	struct dx_hash_info hinfo;
	struct buffer_head *bh;
	struct inode *dir = dentry->d_parent->d_inode;
	struct super_block *sb = dir->i_sb;
	struct ext4_dir_entry_2 *de;
	int restart;
	int err;

again:
	restart = 0;
	frame = dx_probe(&dentry->d_name, dir, &hinfo, frames, lck, &err);
	if (!frame)
		return err;
	entries = frame->entries;
	at = frame->at;

	if (!(bh = ext4_bread(handle,dir, dx_get_block(frame->at), 0, &err)))
		goto cleanup;

	err = add_dirent_to_buf(handle, dentry, inode, NULL, bh);
	if (err != -ENOSPC)
		goto cleanup;

	err = 0;
	/* Block full, should compress but for now just split */
	dxtrace(printk(KERN_DEBUG "using %u of %u node entries\n",
		       dx_get_count(entries), dx_get_limit(entries)));
	/* Need to split index? */
	if (dx_get_count(entries) == dx_get_limit(entries)) {
		ext4_lblk_t newblock;
		int levels = frame - frames + 1;
		unsigned icount;
		int add_level = 1;
		struct dx_entry *entries2;
		struct dx_node *node2;
		struct buffer_head *bh2;

		if (!ext4_htree_safe_locked(lck)) { /* retry with EX lock */
			ext4_htree_safe_relock(lck);
			restart = 1;
			goto cleanup;
		}
		while (frame > frames) {
			if (dx_get_count((frame - 1)->entries) <
			    dx_get_limit((frame - 1)->entries)) {
				add_level = 0;
				break;
			}
			frame--; /* split higher index block */
			at = frame->at;
			entries = frame->entries;
			restart = 1;
		}
		if (add_level && levels == ext4_dir_htree_level(sb)) {
			ext4_warning(sb, "Directory (ino: %lu) index full, "
					 "reach max htree level :%d",
					 dir->i_ino, levels);
			if (ext4_dir_htree_level(sb) < EXT4_HTREE_LEVEL) {
				ext4_warning(sb, "Large directory feature is"
						 "not enabled on this "
						 "filesystem");
			}
			err = -ENOSPC;
			goto cleanup;
		}
		icount = dx_get_count(entries);
		bh2 = ext4_append (handle, dir, &newblock, &err);
		if (!(bh2))
			goto cleanup;
		node2 = (struct dx_node *)(bh2->b_data);
		entries2 = node2->entries;
		memset(&node2->fake, 0, sizeof(struct fake_dirent));
		node2->fake.rec_len = ext4_rec_len_to_disk(sb->s_blocksize,
							   sb->s_blocksize);
		BUFFER_TRACE(frame->bh, "get_write_access");
		err = ext4_journal_get_write_access(handle, frame->bh);
		if (err)
			goto journal_error;
		if (!add_level) {
			unsigned icount1 = icount/2, icount2 = icount - icount1;
			unsigned hash2 = dx_get_hash(entries + icount1);
			dxtrace(printk(KERN_DEBUG "Split index %i/%i\n",
				       icount1, icount2));

			BUFFER_TRACE(frame->bh, "get_write_access"); /* index root */
			err = ext4_journal_get_write_access(handle,
							    (frame - 1)->bh);
			if (err)
				goto journal_error;

			memcpy((char *) entries2, (char *) (entries + icount1),
			       icount2 * sizeof(struct dx_entry));
			dx_set_count(entries, icount1);
			dx_set_count(entries2, icount2);
			dx_set_limit(entries2, dx_node_limit(dir));

			/* Which index block gets the new entry? */
			if (at - entries >= icount1) {
				frame->at = at = at - entries - icount1 + entries2;
				frame->entries = entries = entries2;
				swap(frame->bh, bh2);
			}
			dx_insert_block((frame - 1), hash2, newblock);
			dxtrace(dx_show_index("node", frame->entries));
			dxtrace(dx_show_index("node",
			       ((struct dx_node *) bh2->b_data)->entries));
			err = ext4_handle_dirty_metadata(handle, dir, bh2);
			if (err)
				goto journal_error;
			brelse (bh2);
			ext4_handle_dirty_metadata(handle, dir,
						   (frame - 1)->bh);
			if (restart) {
				ext4_handle_dirty_metadata(handle, dir,
							   frame->bh);
				goto cleanup;
			}
		} else {
			struct dx_root_info * info;

			memcpy((char *) entries2, (char *) entries,
			       icount * sizeof(struct dx_entry));
			dx_set_limit(entries2, dx_node_limit(dir));

			/* Set up root */
			dx_set_count(entries, 1);
			dx_set_block(entries + 0, newblock);
			info = dx_get_dx_info((struct ext4_dir_entry_2*)
					frames[0].bh->b_data);
			info->indirect_levels += 1;
			dxtrace(printk(KERN_DEBUG
				       "Creating %d level index...\n",
				       info->indirect_levels));
			ext4_handle_dirty_metadata(handle, dir, frame->bh);
			ext4_handle_dirty_metadata(handle, dir, bh2);
			brelse(bh2);
			restart = 1;
			goto cleanup;
		}
		if (err) {
			ext4_std_error(inode->i_sb, err);
			goto cleanup;
		}
	} else if (!ext4_htree_dx_locked(lck)) {
		struct ext4_dir_lock_data *ld = ext4_htree_lock_data(lck);

		/* not well protected, require DX lock */
		ext4_htree_dx_need_lock(lck);
		at = frame > frames ? (frame - 1)->at : NULL;

		/* NB: no risk of deadlock because it's just a try.
		 *
		 * NB: we check ld_count for twice, the first time before
		 * having DX lock, the second time after holding DX lock.
		 *
		 * NB: We never free blocks for directory so far, which
		 * means value returned by dx_get_count() should equal to
		 * ld->ld_count if nobody split any DE-block under @at,
		 * and ld->ld_at still points to valid dx_entry. */
		if ((ld->ld_count != dx_get_count(entries)) ||
		    !ext4_htree_dx_lock_try(lck, at) ||
		    (ld->ld_count != dx_get_count(entries))) {
			restart = 1;
			goto cleanup;
		}
		/* OK, I've got DX lock and nothing changed */
		frame->at = ld->ld_at;
	}
	de = do_split(handle, dir, &bh, frames, frame, &hinfo, lck, &err);
	if (!de)
		goto cleanup;

	err = add_dirent_to_buf(handle, dentry, inode, de, bh);
	goto cleanup;

journal_error:
	ext4_std_error(dir->i_sb, err);
cleanup:
	ext4_htree_dx_unlock(lck);
	ext4_htree_de_unlock(lck);
	if (bh)
		brelse(bh);
	dx_release(frames);
	/* @restart is true means htree-path has been changed, we need to
	 * repeat dx_probe() to find out valid htree-path */
	if (restart && err == 0)
		goto again;
	return err;
}

/*
 * ext4_delete_entry deletes a directory entry by merging it with the
 * previous entry
 */
int ext4_delete_entry(handle_t *handle,
		      struct inode *dir,
		      struct ext4_dir_entry_2 *de_del,
		      struct buffer_head *bh)
{
	struct ext4_dir_entry_2 *de, *pde;
	unsigned int blocksize = dir->i_sb->s_blocksize;
	int i;

	i = 0;
	pde = NULL;
	de = (struct ext4_dir_entry_2 *) bh->b_data;
	while (i < bh->b_size) {
		if (!ext4_check_dir_entry("ext4_delete_entry", dir, de, bh, i))
			return -EIO;
		if (de == de_del)  {
			BUFFER_TRACE(bh, "get_write_access");
			ext4_journal_get_write_access(handle, bh);
			if (pde)
				pde->rec_len = ext4_rec_len_to_disk(
					ext4_rec_len_from_disk(pde->rec_len,
							       blocksize) +
					ext4_rec_len_from_disk(de->rec_len,
							       blocksize),
					blocksize);
			else
				de->inode = 0;
			inode_inc_iversion(dir);
			BUFFER_TRACE(bh, "call ext4_handle_dirty_metadata");
			ext4_handle_dirty_metadata(handle, dir, bh);
			return 0;
		}
		i += ext4_rec_len_from_disk(de->rec_len, blocksize);
		pde = de;
		de = ext4_next_entry(de, blocksize);
	}
	return -ENOENT;
}
EXPORT_SYMBOL(ext4_delete_entry);
/*
 * DIR_NLINK feature is set if 1) nlinks > EXT4_LINK_MAX or 2) nlinks == 2,
 * since this indicates that nlinks count was previously 1.
 */
void ext4_inc_count(handle_t *handle, struct inode *inode)
{
	inc_nlink(inode);
	if (is_dx(inode) && inode->i_nlink > 1) {
		/* limit is 16-bit i_links_count */
		if (inode->i_nlink >= EXT4_LINK_MAX || inode->i_nlink == 2) {
			inode->i_nlink = 1;
			EXT4_SET_RO_COMPAT_FEATURE(inode->i_sb,
					      EXT4_FEATURE_RO_COMPAT_DIR_NLINK);
		}
	}
}
EXPORT_SYMBOL(ext4_inc_count);

/*
 * If a directory had nlink == 1, then we should let it be 1. This indicates
 * directory has >EXT4_LINK_MAX subdirs.
 */
void ext4_dec_count(handle_t *handle, struct inode *inode)
{
	if (!S_ISDIR(inode->i_mode) || inode->i_nlink > 2)
		drop_nlink(inode);
}
EXPORT_SYMBOL(ext4_dec_count);


static int ext4_add_nondir(handle_t *handle,
		struct dentry *dentry, struct inode *inode)
{
	int err = ext4_add_entry(handle, dentry, inode);
	if (!err) {
		ext4_mark_inode_dirty(handle, inode);
		d_instantiate(dentry, inode);
		unlock_new_inode(inode);
		return 0;
	}
	drop_nlink(inode);
	unlock_new_inode(inode);
	iput(inode);
	return err;
}

 /* Return locked inode, then the caller can modify the inode's states/flags
  * before others finding it. The caller should unlock the inode by itself. */
struct inode *ext4_create_inode(handle_t *handle, struct inode *dir, int mode)
{
	struct inode *inode;

	inode = ext4_new_inode(handle, dir, mode, 0,
			       EXT4_SB(dir->i_sb)->s_inode_goal);
	if (!IS_ERR(inode)) {
		if (S_ISCHR(mode) || S_ISBLK(mode) || S_ISFIFO(mode)) {
#ifdef CONFIG_EXT4_FS_XATTR
			inode->i_op = &ext4_special_inode_operations;
#endif
		} else {
			inode->i_op = &ext4_file_inode_operations;
			inode->i_fop = &ext4_file_operations;
			ext4_set_aops(inode);
		}
	}
	return inode;
}
EXPORT_SYMBOL(ext4_create_inode);

/*
 * By the time this is called, we already have created
 * the directory cache entry for the new file, but it
 * is so far negative - it has no inode.
 *
 * If the create succeeds, we fill in the inode information
 * with d_instantiate().
 */
static int ext4_create(struct inode *dir, struct dentry *dentry, int mode,
		       struct nameidata *nd)
{
	handle_t *handle;
	struct inode *inode;
	int err, retries = 0;

retry:
	handle = ext4_journal_start(dir, EXT4_DATA_TRANS_BLOCKS(dir->i_sb) +
					EXT4_INDEX_EXTRA_TRANS_BLOCKS + 3 +
					EXT4_MAXQUOTAS_INIT_BLOCKS(dir->i_sb));
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		ext4_handle_sync(handle);

	inode = ext4_new_inode(handle, dir, mode, &dentry->d_name, 0);
	err = PTR_ERR(inode);
	if (!IS_ERR(inode)) {
		inode->i_op = &ext4_file_inode_operations;
		inode->i_fop = &ext4_file_operations;
		ext4_set_aops(inode);
		err = ext4_add_nondir(handle, dentry, inode);
	}
	ext4_journal_stop(handle);
	if (err == -ENOSPC && ext4_should_retry_alloc(dir->i_sb, &retries))
		goto retry;
	return err;
}

static int ext4_mknod(struct inode *dir, struct dentry *dentry,
		      int mode, dev_t rdev)
{
	handle_t *handle;
	struct inode *inode;
	int err, retries = 0;

	if (!new_valid_dev(rdev))
		return -EINVAL;

retry:
	handle = ext4_journal_start(dir, EXT4_DATA_TRANS_BLOCKS(dir->i_sb) +
					EXT4_INDEX_EXTRA_TRANS_BLOCKS + 3 +
					EXT4_MAXQUOTAS_INIT_BLOCKS(dir->i_sb));
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		ext4_handle_sync(handle);

	inode = ext4_new_inode(handle, dir, mode, &dentry->d_name, 0);
	err = PTR_ERR(inode);
	if (!IS_ERR(inode)) {
		init_special_inode(inode, inode->i_mode, rdev);
#ifdef CONFIG_EXT4_FS_XATTR
		inode->i_op = &ext4_special_inode_operations;
#endif
		err = ext4_add_nondir(handle, dentry, inode);
	}
	ext4_journal_stop(handle);
	if (err == -ENOSPC && ext4_should_retry_alloc(dir->i_sb, &retries))
		goto retry;
	return err;
}

/* Initialize @inode as a subdirectory of @dir, and add the
 * "." and ".." entries into the first directory block. */
int ext4_add_dot_dotdot(handle_t *handle, struct inode * dir,
			struct inode *inode,
                        const void *data1, const void *data2)
{
	struct buffer_head * dir_block;
	struct ext4_dir_entry_2 * de;
	unsigned int blocksize = dir->i_sb->s_blocksize;
	int err = 0, dot_reclen;

	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		ext4_handle_sync(handle);

	inode->i_op = &ext4_dir_inode_operations;
	inode->i_fop = &ext4_dir_operations;
	inode->i_size = EXT4_I(inode)->i_disksize = inode->i_sb->s_blocksize;
	dir_block = ext4_bread(handle, inode, 0, 1, &err);
	if (!dir_block)
		goto get_out;
	BUFFER_TRACE(dir_block, "get_write_access");
	err = ext4_journal_get_write_access(handle, dir_block);
	if (err)
		goto get_out;
	de = (struct ext4_dir_entry_2 *) dir_block->b_data;
	de->inode = cpu_to_le32(inode->i_ino);
	de->name_len = 1;
	strcpy(de->name, ".");
	ext4_set_de_type(dir->i_sb, de, S_IFDIR);
	/* get packed fid data*/
	data1 = ext4_dentry_get_data(dir->i_sb,
				(struct ext4_dentry_param *) data1);
	if (data1) {
		de->name[1] = 0;
		memcpy(&de->name[2], data1, *(char *) data1);
		de->file_type |= EXT4_DIRENT_LUFID;
	}
	de->rec_len = cpu_to_le16(EXT4_DIR_REC_LEN(de));
	dot_reclen = cpu_to_le16(de->rec_len);
	de = ext4_next_entry(de, blocksize);
	de->inode = cpu_to_le32(dir->i_ino);
	de->rec_len = ext4_rec_len_to_disk(blocksize - dot_reclen,
					   blocksize);
	de->name_len = 2;
	strcpy(de->name, "..");
	ext4_set_de_type(dir->i_sb, de, S_IFDIR);
	data2 = ext4_dentry_get_data(dir->i_sb,
			(struct ext4_dentry_param *) data2);
	if (data2) {
		de->name[2] = 0;
		memcpy(&de->name[3], data2, *(char *) data2);
		de->file_type |= EXT4_DIRENT_LUFID;
	}
	inode->i_nlink = 2;
	BUFFER_TRACE(dir_block, "call ext4_handle_dirty_metadata");
	err = ext4_handle_dirty_metadata(handle, inode, dir_block);
	if (err)
		goto get_out;
	err = ext4_mark_inode_dirty(handle, inode);
get_out:
	brelse(dir_block);
	return err;
}
EXPORT_SYMBOL(ext4_add_dot_dotdot);


static int ext4_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	handle_t *handle;
	struct inode *inode;
	int err, retries = 0;

	if (EXT4_DIR_LINK_MAX(dir))
		return -EMLINK;

retry:
	handle = ext4_journal_start(dir, EXT4_DATA_TRANS_BLOCKS(dir->i_sb) +
					EXT4_INDEX_EXTRA_TRANS_BLOCKS + 3 +
					2*EXT4_QUOTA_INIT_BLOCKS(dir->i_sb));
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		ext4_handle_sync(handle);

	inode = ext4_new_inode(handle, dir, S_IFDIR | mode, &dentry->d_name, 0);
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		goto out_stop;

	err = ext4_add_dot_dotdot(handle, dir, inode, NULL, NULL);
	if (err)
		goto out_clear_inode;

	err = ext4_add_entry(handle, dentry, inode);
	if (err)
		goto out_clear_inode;
	ext4_inc_count(handle, dir);
	ext4_update_dx_flag(dir);
	err = ext4_mark_inode_dirty(handle, dir);
	if (err)
		goto out_clear_inode;
	d_instantiate(dentry, inode);
	unlock_new_inode(inode);
out_stop:
	ext4_journal_stop(handle);
	if (err == -ENOSPC && ext4_should_retry_alloc(dir->i_sb, &retries))
		goto retry;
	return err;
out_clear_inode:
	clear_nlink(inode);
	unlock_new_inode(inode);
	ext4_mark_inode_dirty(handle, inode);
	iput(inode);
	goto out_stop;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
static int empty_dir(struct inode *inode)
{
	unsigned int offset;
	struct buffer_head *bh;
	struct ext4_dir_entry_2 *de, *de1;
	struct super_block *sb;
	int err = 0;

	sb = inode->i_sb;
	if (inode->i_size < __EXT4_DIR_REC_LEN(1) + __EXT4_DIR_REC_LEN(2) ||
	    !(bh = ext4_bread(NULL, inode, 0, 0, &err))) {
		if (err)
			ext4_error(inode->i_sb,
				   "error %d reading directory #%lu offset 0",
				   err, inode->i_ino);
		else
			ext4_warning(inode->i_sb,
				     "bad directory (dir #%lu) - no data block",
				     inode->i_ino);
		return 1;
	}
	de = (struct ext4_dir_entry_2 *) bh->b_data;
	de1 = ext4_next_entry(de, sb->s_blocksize);
	if (le32_to_cpu(de->inode) != inode->i_ino ||
			!le32_to_cpu(de1->inode) ||
			strcmp(".", de->name) ||
			strcmp("..", de1->name)) {
		ext4_warning(inode->i_sb,
			     "bad directory (dir #%lu) - no `.' or `..'",
			     inode->i_ino);
		brelse(bh);
		return 1;
	}
	offset = ext4_rec_len_from_disk(de->rec_len, sb->s_blocksize) +
		 ext4_rec_len_from_disk(de1->rec_len, sb->s_blocksize);
	de = ext4_next_entry(de1, sb->s_blocksize);
	while (offset < inode->i_size) {
		if (!bh ||
			(void *) de >= (void *) (bh->b_data+sb->s_blocksize)) {
			err = 0;
			brelse(bh);
			bh = ext4_bread(NULL, inode,
				offset >> EXT4_BLOCK_SIZE_BITS(sb), 0, &err);
			if (!bh) {
				if (err)
					ext4_error(sb,
						   "error %d reading directory"
						   " #%lu offset %u",
						   err, inode->i_ino, offset);
				offset += sb->s_blocksize;
				continue;
			}
			de = (struct ext4_dir_entry_2 *) bh->b_data;
		}
		if (!ext4_check_dir_entry("empty_dir", inode, de, bh, offset)) {
			de = (struct ext4_dir_entry_2 *)(bh->b_data +
							 sb->s_blocksize);
			offset = (offset | (sb->s_blocksize - 1)) + 1;
			continue;
		}
		if (le32_to_cpu(de->inode)) {
			brelse(bh);
			return 0;
		}
		offset += ext4_rec_len_from_disk(de->rec_len, sb->s_blocksize);
		de = ext4_next_entry(de, sb->s_blocksize);
	}
	brelse(bh);
	return 1;
}

/* ext4_orphan_add() links an unlinked or truncated inode into a list of
 * such inodes, starting at the superblock, in case we crash before the
 * file is closed/deleted, or in case the inode truncate spans multiple
 * transactions and the last transaction is not recovered after a crash.
 *
 * At filesystem recovery time, we walk this list deleting unlinked
 * inodes and truncating linked inodes in ext4_orphan_cleanup().
 */
int ext4_orphan_add(handle_t *handle, struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ext4_iloc iloc;
	int err = 0, rc;

	if (!ext4_handle_valid(handle))
		return 0;

	mutex_lock(&EXT4_SB(sb)->s_orphan_lock);
	if (!list_empty(&EXT4_I(inode)->i_orphan))
		goto out_unlock;

	/* Orphan handling is only valid for files with data blocks
	 * being truncated, or files being unlinked. */

	/* @@@ FIXME: Observation from aviro:
	 * I think I can trigger J_ASSERT in ext4_orphan_add().  We block
	 * here (on s_orphan_lock), so race with ext4_link() which might bump
	 * ->i_nlink. For, say it, character device. Not a regular file,
	 * not a directory, not a symlink and ->i_nlink > 0.
	 *
	 * tytso, 4/25/2009: I'm not sure how that could happen;
	 * shouldn't the fs core protect us from these sort of
	 * unlink()/link() races?
	 */
	J_ASSERT((S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
		  S_ISLNK(inode->i_mode)) || inode->i_nlink == 0);

	BUFFER_TRACE(EXT4_SB(sb)->s_sbh, "get_write_access");
	err = ext4_journal_get_write_access(handle, EXT4_SB(sb)->s_sbh);
	if (err)
		goto out_unlock;

	err = ext4_reserve_inode_write(handle, inode, &iloc);
	if (err)
		goto out_unlock;
	/*
	 * Due to previous errors inode may be already a part of on-disk
	 * orphan list. If so skip on-disk list modification.
	 */
	if (NEXT_ORPHAN(inode) && NEXT_ORPHAN(inode) <=
		(le32_to_cpu(EXT4_SB(sb)->s_es->s_inodes_count)))
			goto mem_insert;

	/* Insert this inode at the head of the on-disk orphan list... */
	NEXT_ORPHAN(inode) = le32_to_cpu(EXT4_SB(sb)->s_es->s_last_orphan);
	EXT4_SB(sb)->s_es->s_last_orphan = cpu_to_le32(inode->i_ino);
	err = ext4_handle_dirty_metadata(handle, NULL, EXT4_SB(sb)->s_sbh);
	rc = ext4_mark_iloc_dirty(handle, inode, &iloc);
	if (!err)
		err = rc;

	/* Only add to the head of the in-memory list if all the
	 * previous operations succeeded.  If the orphan_add is going to
	 * fail (possibly taking the journal offline), we can't risk
	 * leaving the inode on the orphan list: stray orphan-list
	 * entries can cause panics at unmount time.
	 *
	 * This is safe: on error we're going to ignore the orphan list
	 * anyway on the next recovery. */
mem_insert:
	if (!err)
		list_add(&EXT4_I(inode)->i_orphan, &EXT4_SB(sb)->s_orphan);

	jbd_debug(4, "superblock will point to %lu\n", inode->i_ino);
	jbd_debug(4, "orphan inode %lu will point to %d\n",
			inode->i_ino, NEXT_ORPHAN(inode));
out_unlock:
	mutex_unlock(&EXT4_SB(sb)->s_orphan_lock);
	ext4_std_error(inode->i_sb, err);
	return err;
}

/*
 * ext4_orphan_del() removes an unlinked or truncated inode from the list
 * of such inodes stored on disk, because it is finally being cleaned up.
 */
int ext4_orphan_del(handle_t *handle, struct inode *inode)
{
	struct list_head *prev;
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_sb_info *sbi;
	__u32 ino_next;
	struct ext4_iloc iloc;
	int err = 0;

	/* ext4_handle_valid() assumes a valid handle_t pointer */
	if (handle && !ext4_handle_valid(handle))
		return 0;

	mutex_lock(&EXT4_SB(inode->i_sb)->s_orphan_lock);
	if (list_empty(&ei->i_orphan))
		goto out;

	ino_next = NEXT_ORPHAN(inode);
	prev = ei->i_orphan.prev;
	sbi = EXT4_SB(inode->i_sb);

	jbd_debug(4, "remove inode %lu from orphan list\n", inode->i_ino);

	list_del_init(&ei->i_orphan);

	/* If we're on an error path, we may not have a valid
	 * transaction handle with which to update the orphan list on
	 * disk, but we still need to remove the inode from the linked
	 * list in memory. */
	if (sbi->s_journal && !handle)
		goto out;

	err = ext4_reserve_inode_write(handle, inode, &iloc);
	if (err)
		goto out_err;

	if (prev == &sbi->s_orphan) {
		jbd_debug(4, "superblock will point to %u\n", ino_next);
		BUFFER_TRACE(sbi->s_sbh, "get_write_access");
		err = ext4_journal_get_write_access(handle, sbi->s_sbh);
		if (err)
			goto out_brelse;
		sbi->s_es->s_last_orphan = cpu_to_le32(ino_next);
		err = ext4_handle_dirty_metadata(handle, NULL, sbi->s_sbh);
	} else {
		struct ext4_iloc iloc2;
		struct inode *i_prev =
			&list_entry(prev, struct ext4_inode_info, i_orphan)->vfs_inode;

		jbd_debug(4, "orphan inode %lu will point to %u\n",
			  i_prev->i_ino, ino_next);
		err = ext4_reserve_inode_write(handle, i_prev, &iloc2);
		if (err)
			goto out_brelse;
		NEXT_ORPHAN(i_prev) = ino_next;
		err = ext4_mark_iloc_dirty(handle, i_prev, &iloc2);
	}
	if (err)
		goto out_brelse;
	NEXT_ORPHAN(inode) = 0;
	err = ext4_mark_iloc_dirty(handle, inode, &iloc);

out_err:
	ext4_std_error(inode->i_sb, err);
out:
	mutex_unlock(&EXT4_SB(inode->i_sb)->s_orphan_lock);
	return err;

out_brelse:
	brelse(iloc.bh);
	goto out_err;
}

static int ext4_rmdir(struct inode *dir, struct dentry *dentry)
{
	int retval;
	struct inode *inode;
	struct buffer_head *bh;
	struct ext4_dir_entry_2 *de;
	handle_t *handle;

	/* Initialize quotas before so that eventual writes go in
	 * separate transaction */
	vfs_dq_init(dentry->d_inode);
	handle = ext4_journal_start(dir, EXT4_DELETE_TRANS_BLOCKS(dir->i_sb));
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	retval = -ENOENT;
	bh = ext4_find_entry(dir, &dentry->d_name, &de);
	if (!bh)
		goto end_rmdir;

	if (IS_DIRSYNC(dir))
		ext4_handle_sync(handle);

	inode = dentry->d_inode;

	retval = -EIO;
	if (le32_to_cpu(de->inode) != inode->i_ino)
		goto end_rmdir;

	retval = -ENOTEMPTY;
	if (!empty_dir(inode))
		goto end_rmdir;

	retval = ext4_delete_entry(handle, dir, de, bh);
	if (retval)
		goto end_rmdir;
	if (!EXT4_DIR_LINK_EMPTY(inode))
		ext4_warning(inode->i_sb,
			     "empty directory has too many links (%d)",
			     inode->i_nlink);
	inode->i_version++;
	clear_nlink(inode);
	/* There's no need to set i_disksize: the fact that i_nlink is
	 * zero will ensure that the right thing happens during any
	 * recovery. */
	inode->i_size = 0;
	ext4_orphan_add(handle, inode);
	inode->i_ctime = dir->i_ctime = dir->i_mtime = ext4_current_time(inode);
	ext4_mark_inode_dirty(handle, inode);
	ext4_dec_count(handle, dir);
	ext4_update_dx_flag(dir);
	ext4_mark_inode_dirty(handle, dir);

end_rmdir:
	ext4_journal_stop(handle);
	brelse(bh);
	return retval;
}

static int ext4_unlink(struct inode *dir, struct dentry *dentry)
{
	int retval;
	struct inode *inode;
	struct buffer_head *bh;
	struct ext4_dir_entry_2 *de;
	handle_t *handle;

	/* Initialize quotas before so that eventual writes go
	 * in separate transaction */
	vfs_dq_init(dentry->d_inode);
	handle = ext4_journal_start(dir, EXT4_DELETE_TRANS_BLOCKS(dir->i_sb));
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		ext4_handle_sync(handle);

	retval = -ENOENT;
	bh = ext4_find_entry(dir, &dentry->d_name, &de);
	if (!bh)
		goto end_unlink;

	inode = dentry->d_inode;

	retval = -EIO;
	if (le32_to_cpu(de->inode) != inode->i_ino)
		goto end_unlink;

	if (!inode->i_nlink) {
		ext4_warning(inode->i_sb,
			     "Deleting nonexistent file (%lu), %d",
			     inode->i_ino, inode->i_nlink);
		inode->i_nlink = 1;
	}
	retval = ext4_delete_entry(handle, dir, de, bh);
	if (retval)
		goto end_unlink;
	dir->i_ctime = dir->i_mtime = ext4_current_time(dir);
	ext4_update_dx_flag(dir);
	ext4_mark_inode_dirty(handle, dir);
	drop_nlink(inode);
	if (!inode->i_nlink)
		ext4_orphan_add(handle, inode);
	inode->i_ctime = ext4_current_time(inode);
	ext4_mark_inode_dirty(handle, inode);
	retval = 0;

end_unlink:
	ext4_journal_stop(handle);
	brelse(bh);
	return retval;
}

static int ext4_symlink(struct inode *dir,
			struct dentry *dentry, const char *symname)
{
	handle_t *handle;
	struct inode *inode;
	int l, err, retries = 0;

	l = strlen(symname)+1;
	if (l > dir->i_sb->s_blocksize)
		return -ENAMETOOLONG;

retry:
	handle = ext4_journal_start(dir, EXT4_DATA_TRANS_BLOCKS(dir->i_sb) +
					EXT4_INDEX_EXTRA_TRANS_BLOCKS + 5 +
					EXT4_MAXQUOTAS_INIT_BLOCKS(dir->i_sb));
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		ext4_handle_sync(handle);

	inode = ext4_new_inode(handle, dir, S_IFLNK|S_IRWXUGO,
			       &dentry->d_name, 0);
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		goto out_stop;

	if (l > sizeof(EXT4_I(inode)->i_data)) {
		inode->i_op = &ext4_symlink_inode_operations;
		ext4_set_aops(inode);
		/*
		 * page_symlink() calls into ext4_prepare/commit_write.
		 * We have a transaction open.  All is sweetness.  It also sets
		 * i_size in generic_commit_write().
		 */
		err = __page_symlink(inode, symname, l, 1);
		if (err) {
			clear_nlink(inode);
			unlock_new_inode(inode);
			ext4_mark_inode_dirty(handle, inode);
			iput(inode);
			goto out_stop;
		}
	} else {
		/* clear the extent format for fast symlink */
		ext4_clear_inode_flag(inode, EXT4_INODE_EXTENTS);
		inode->i_op = &ext4_fast_symlink_inode_operations;
		memcpy((char *)&EXT4_I(inode)->i_data, symname, l);
		inode->i_size = l-1;
	}
	EXT4_I(inode)->i_disksize = inode->i_size;
	err = ext4_add_nondir(handle, dentry, inode);
out_stop:
	ext4_journal_stop(handle);
	if (err == -ENOSPC && ext4_should_retry_alloc(dir->i_sb, &retries))
		goto retry;
	return err;
}

static int ext4_link(struct dentry *old_dentry,
		     struct inode *dir, struct dentry *dentry)
{
	handle_t *handle;
	struct inode *inode = old_dentry->d_inode;
	int err, retries = 0;

	if (inode->i_nlink >= EXT4_LINK_MAX)
		return -EMLINK;

	/*
	 * Return -ENOENT if we've raced with unlink and i_nlink is 0.  Doing
	 * otherwise has the potential to corrupt the orphan inode list.
	 */
	if (inode->i_nlink == 0)
		return -ENOENT;

retry:
	handle = ext4_journal_start(dir, EXT4_DATA_TRANS_BLOCKS(dir->i_sb) +
					EXT4_INDEX_EXTRA_TRANS_BLOCKS);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		ext4_handle_sync(handle);

	inode->i_ctime = ext4_current_time(inode);
	ext4_inc_count(handle, inode);
	atomic_inc(&inode->i_count);

	err = ext4_add_entry(handle, dentry, inode);
	if (!err) {
		ext4_mark_inode_dirty(handle, inode);
		d_instantiate(dentry, inode);
	} else {
		drop_nlink(inode);
		iput(inode);
	}
	ext4_journal_stop(handle);
	if (err == -ENOSPC && ext4_should_retry_alloc(dir->i_sb, &retries))
		goto retry;
	return err;
}

#define PARENT_INO(buffer, size) \
	(ext4_next_entry((struct ext4_dir_entry_2 *)(buffer), size)->inode)

/*
 * Anybody can rename anything with this: the permission checks are left to the
 * higher-level routines.
 */
static int ext4_rename(struct inode *old_dir, struct dentry *old_dentry,
		       struct inode *new_dir, struct dentry *new_dentry)
{
	handle_t *handle;
	struct inode *old_inode, *new_inode;
	struct buffer_head *old_bh, *new_bh, *dir_bh;
	struct ext4_dir_entry_2 *old_de, *new_de;
	int retval, force_da_alloc = 0;

	old_bh = new_bh = dir_bh = NULL;

	/* Initialize quotas before so that eventual writes go
	 * in separate transaction */
	if (new_dentry->d_inode)
		vfs_dq_init(new_dentry->d_inode);
	handle = ext4_journal_start(old_dir, 2 *
					EXT4_DATA_TRANS_BLOCKS(old_dir->i_sb) +
					EXT4_INDEX_EXTRA_TRANS_BLOCKS + 2);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(old_dir) || IS_DIRSYNC(new_dir))
		ext4_handle_sync(handle);

	old_bh = ext4_find_entry(old_dir, &old_dentry->d_name, &old_de);
	/*
	 *  Check for inode number is _not_ due to possible IO errors.
	 *  We might rmdir the source, keep it as pwd of some process
	 *  and merrily kill the link to whatever was created under the
	 *  same name. Goodbye sticky bit ;-<
	 */
	old_inode = old_dentry->d_inode;
	retval = -ENOENT;
	if (!old_bh || le32_to_cpu(old_de->inode) != old_inode->i_ino)
		goto end_rename;

	new_inode = new_dentry->d_inode;
	new_bh = ext4_find_entry(new_dir, &new_dentry->d_name, &new_de);
	if (new_bh) {
		if (!new_inode) {
			brelse(new_bh);
			new_bh = NULL;
		}
	}
	if (S_ISDIR(old_inode->i_mode)) {
		if (new_inode) {
			retval = -ENOTEMPTY;
			if (!empty_dir(new_inode))
				goto end_rename;
		}
		retval = -EIO;
		dir_bh = ext4_bread(handle, old_inode, 0, 0, &retval);
		if (!dir_bh)
			goto end_rename;
		if (le32_to_cpu(PARENT_INO(dir_bh->b_data,
				old_dir->i_sb->s_blocksize)) != old_dir->i_ino)
			goto end_rename;
		retval = -EMLINK;
		if (!new_inode && new_dir != old_dir &&
		    EXT4_DIR_LINK_MAX(new_dir))
			goto end_rename;
	}
	if (!new_bh) {
		retval = ext4_add_entry(handle, new_dentry, old_inode);
		if (retval)
			goto end_rename;
	} else {
		BUFFER_TRACE(new_bh, "get write access");
		ext4_journal_get_write_access(handle, new_bh);
		new_de->inode = cpu_to_le32(old_inode->i_ino);
		if (EXT4_HAS_INCOMPAT_FEATURE(new_dir->i_sb,
					      EXT4_FEATURE_INCOMPAT_FILETYPE))
			new_de->file_type = old_de->file_type;
		new_dir->i_version++;
		new_dir->i_ctime = new_dir->i_mtime =
					ext4_current_time(new_dir);
		ext4_mark_inode_dirty(handle, new_dir);
		BUFFER_TRACE(new_bh, "call ext4_handle_dirty_metadata");
		ext4_handle_dirty_metadata(handle, new_dir, new_bh);
		brelse(new_bh);
		new_bh = NULL;
	}

	/*
	 * Like most other Unix systems, set the ctime for inodes on a
	 * rename.
	 */
	old_inode->i_ctime = ext4_current_time(old_inode);
	ext4_mark_inode_dirty(handle, old_inode);

	/*
	 * ok, that's it
	 */
	if (le32_to_cpu(old_de->inode) != old_inode->i_ino ||
	    old_de->name_len != old_dentry->d_name.len ||
	    strncmp(old_de->name, old_dentry->d_name.name, old_de->name_len) ||
	    (retval = ext4_delete_entry(handle, old_dir,
					old_de, old_bh)) == -ENOENT) {
		/* old_de could have moved from under us during htree split, so
		 * make sure that we are deleting the right entry.  We might
		 * also be pointing to a stale entry in the unused part of
		 * old_bh so just checking inum and the name isn't enough. */
		struct buffer_head *old_bh2;
		struct ext4_dir_entry_2 *old_de2;

		old_bh2 = ext4_find_entry(old_dir, &old_dentry->d_name, &old_de2);
		if (old_bh2) {
			retval = ext4_delete_entry(handle, old_dir,
						   old_de2, old_bh2);
			brelse(old_bh2);
		}
	}
	if (retval) {
		ext4_warning(old_dir->i_sb,
				"Deleting old file (%lu), %d, error=%d",
				old_dir->i_ino, old_dir->i_nlink, retval);
	}

	if (new_inode) {
		ext4_dec_count(handle, new_inode);
		new_inode->i_ctime = ext4_current_time(new_inode);
	}
	old_dir->i_ctime = old_dir->i_mtime = ext4_current_time(old_dir);
	ext4_update_dx_flag(old_dir);
	if (dir_bh) {
		BUFFER_TRACE(dir_bh, "get_write_access");
		ext4_journal_get_write_access(handle, dir_bh);
		PARENT_INO(dir_bh->b_data, new_dir->i_sb->s_blocksize) =
						cpu_to_le32(new_dir->i_ino);
		BUFFER_TRACE(dir_bh, "call ext4_handle_dirty_metadata");
		ext4_handle_dirty_metadata(handle, old_dir, dir_bh);
		ext4_dec_count(handle, old_dir);
		if (new_inode) {
			/* checked empty_dir above, can't have another parent,
			 * ext4_dec_count() won't work for many-linked dirs */
			new_inode->i_nlink = 0;
		} else {
			ext4_inc_count(handle, new_dir);
			ext4_update_dx_flag(new_dir);
			ext4_mark_inode_dirty(handle, new_dir);
		}
	}
	ext4_mark_inode_dirty(handle, old_dir);
	if (new_inode) {
		ext4_mark_inode_dirty(handle, new_inode);
		if (!new_inode->i_nlink)
			ext4_orphan_add(handle, new_inode);
		if (!test_opt(new_dir->i_sb, NO_AUTO_DA_ALLOC))
			force_da_alloc = 1;
	}
	retval = 0;

end_rename:
	brelse(dir_bh);
	brelse(old_bh);
	brelse(new_bh);
	ext4_journal_stop(handle);
	if (retval == 0 && force_da_alloc)
		ext4_alloc_da_blocks(old_inode);
	return retval;
}

/*
 * directories can handle most operations...
 */
const struct inode_operations ext4_dir_inode_operations = {
	.create		= ext4_create,
	.lookup		= ext4_lookup,
	.link		= ext4_link,
	.unlink		= ext4_unlink,
	.symlink	= ext4_symlink,
	.mkdir		= ext4_mkdir,
	.rmdir		= ext4_rmdir,
	.mknod		= ext4_mknod,
	.rename		= ext4_rename,
	.setattr	= ext4_setattr,
#ifdef CONFIG_EXT4_FS_XATTR
	.setxattr	= generic_setxattr,
	.getxattr	= generic_getxattr,
	.listxattr	= ext4_listxattr,
	.removexattr	= generic_removexattr,
#endif
	.check_acl	= ext4_check_acl,
	.fiemap         = ext4_fiemap,
};

const struct inode_operations ext4_special_inode_operations = {
	.setattr	= ext4_setattr,
#ifdef CONFIG_EXT4_FS_XATTR
	.setxattr	= generic_setxattr,
	.getxattr	= generic_getxattr,
	.listxattr	= ext4_listxattr,
	.removexattr	= generic_removexattr,
#endif
	.check_acl	= ext4_check_acl,
};
