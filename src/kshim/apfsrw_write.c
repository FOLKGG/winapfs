/*
 * winapfs - read-only APFS for Windows
 * Copyright (C) 2026 winapfs contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * apfsrw: the writing half of the API. Each function plays the VFS's part
 * for one system call.
 *
 * Locking: namespace changes hold one global lock (g_ns_lock) and then the
 * directory inode locks exclusively, so cross-directory renames cannot
 * deadlock against each other. Writes and attribute changes hold the file's
 * inode lock, as generic_file_write_iter() and notify_change() do. The
 * driver adds its own container-wide semaphore around every transaction.
 */
#include "apfsrw_internal.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

static SRWLOCK g_ns_lock = SRWLOCK_INIT;

static bool name_ok(const char *name, size_t len)
{
	if (len == 0 || len > NAME_MAX || memchr(name, '/', len) || memchr(name, 0, len))
		return false;
	return !((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'));
}

/* Dentry for name in dir, positive or negative. Caller holds the dir lock. */
static int child_dentry(struct dentry *dir, const char *name, size_t len, struct dentry **out)
{
	struct inode *di = dir->d_inode;
	struct dentry *d = kshim_d_alloc(dir, name, len), *res;

	res = di->i_op->lookup(di, d, 0);
	if (IS_ERR(res)) {
		dput(d);
		return (int)PTR_ERR(res);
	}
	if (res) {
		dput(d);
		d = res;
	}
	*out = d;
	return 0;
}

/* ------------------------------------------------------------ new names */

enum mk_kind { MK_FILE, MK_DIR, MK_SYMLINK };

static int make(apfsrw_node *dir_node, const char *name, size_t len, enum mk_kind kind, uint32_t mode,
		const char *target, apfsrw_node **out)
{
	struct dentry *dir = D(dir_node), *d = NULL;
	struct inode *di = dir->d_inode;
	int err;

	if (sb_rdonly(di->i_sb))
		return -EROFS;
	if (!S_ISDIR(di->i_mode))
		return -ENOTDIR;
	if (!name_ok(name, len))
		return -EINVAL;

	AcquireSRWLockExclusive(&g_ns_lock);
	inode_lock(di);
	err = child_dentry(dir, name, len, &d);
	if (!err && d->d_inode)
		err = -EEXIST;
	if (!err) {
		switch (kind) {
		case MK_FILE:
			err = di->i_op->create(&nop_mnt_idmap, di, d, (umode_t)(S_IFREG | (mode & 07777)), true);
			break;
		case MK_DIR:
			err = di->i_op->mkdir(&nop_mnt_idmap, di, d, (umode_t)(S_IFDIR | (mode & 07777)));
			break;
		case MK_SYMLINK:
			err = di->i_op->symlink(&nop_mnt_idmap, di, d, target);
			break;
		}
	}
	inode_unlock(di);
	ReleaseSRWLockExclusive(&g_ns_lock);

	if (!err && !d->d_inode)
		err = -EIO;
	if (err) {
		if (d)
			dput(d);
		return err;
	}
	if (out)
		*out = N(d);
	else
		dput(d);
	return 0;
}

int apfsrw_create(apfsrw_node *dir, const char *name, size_t len, uint32_t mode, apfsrw_node **out)
{
	return make(dir, name, len, MK_FILE, mode, NULL, out);
}

int apfsrw_mkdir(apfsrw_node *dir, const char *name, size_t len, uint32_t mode, apfsrw_node **out)
{
	return make(dir, name, len, MK_DIR, mode, NULL, out);
}

int apfsrw_symlink(apfsrw_node *dir, const char *name, size_t len, const char *target, apfsrw_node **out)
{
	if (!target || !*target)
		return -EINVAL;
	return make(dir, name, len, MK_SYMLINK, 0777, target, out);
}

int apfsrw_link(apfsrw_node *src_node, apfsrw_node *dir_node, const char *name, size_t len, apfsrw_node **out)
{
	struct dentry *src = D(src_node), *dir = D(dir_node), *d = NULL;
	struct inode *si = src->d_inode, *di = dir->d_inode;
	int err;

	if (sb_rdonly(di->i_sb))
		return -EROFS;
	if (S_ISDIR(si->i_mode))
		return -EPERM;
	if (!name_ok(name, len))
		return -EINVAL;

	AcquireSRWLockExclusive(&g_ns_lock);
	inode_lock(di);
	if (si != di)
		inode_lock(si);
	err = child_dentry(dir, name, len, &d);
	if (!err && d->d_inode)
		err = -EEXIST;
	if (!err)
		err = di->i_op->link(src, di, d);
	if (si != di)
		inode_unlock(si);
	inode_unlock(di);
	ReleaseSRWLockExclusive(&g_ns_lock);

	if (err) {
		if (d)
			dput(d);
		return err;
	}
	if (out)
		*out = N(d);
	else
		dput(d);
	return 0;
}

/* ------------------------------------------------------------ removing names */

static int remove_name(apfsrw_node *dir_node, const char *name, size_t len, bool is_dir)
{
	struct dentry *dir = D(dir_node), *d = NULL;
	struct inode *di = dir->d_inode, *vi;
	int err;

	if (sb_rdonly(di->i_sb))
		return -EROFS;
	if (!name_ok(name, len))
		return -EINVAL;

	AcquireSRWLockExclusive(&g_ns_lock);
	inode_lock(di);
	err = child_dentry(dir, name, len, &d);
	if (!err && !d->d_inode)
		err = -ENOENT;
	if (!err) {
		vi = d->d_inode;
		if (is_dir && !S_ISDIR(vi->i_mode)) {
			err = -ENOTDIR;
		} else if (!is_dir && S_ISDIR(vi->i_mode)) {
			err = -EISDIR;
		} else {
			inode_lock(vi);
			err = is_dir ? di->i_op->rmdir(di, d) : di->i_op->unlink(di, d);
			inode_unlock(vi);
		}
	}
	inode_unlock(di);
	ReleaseSRWLockExclusive(&g_ns_lock);
	/* the last reference to an unlinked inode evicts it, which frees it on disk */
	if (d)
		dput(d);
	return err;
}

int apfsrw_unlink(apfsrw_node *dir, const char *name, size_t len)
{
	return remove_name(dir, name, len, false);
}

int apfsrw_rmdir(apfsrw_node *dir, const char *name, size_t len)
{
	return remove_name(dir, name, len, true);
}

/* ------------------------------------------------------------ rename */

/* Is `victim` the directory `dir` or one of its ancestors? */
static bool is_ancestor(struct inode *victim, struct inode *dir)
{
	struct super_block *sb = dir->i_sb;
	struct inode *cur = dir;
	bool found = false;
	int depth = 0;

	ihold(cur);
	while (depth++ < 4096) {
		struct inode *next;
		if (cur == victim) {
			found = true;
			break;
		}
		if (apfs_ino(cur) == APFS_ROOT_DIR_INO_NUM)
			break;
		next = apfs_iget(sb, APFS_I(cur)->i_parent_id);
		iput(cur);
		if (IS_ERR(next))
			return false;
		cur = next;
	}
	iput(cur);
	return found;
}

int apfsrw_rename(apfsrw_node *olddir_node, const char *oldname, size_t oldlen,
		  apfsrw_node *newdir_node, const char *newname, size_t newlen, unsigned flags)
{
	struct dentry *odir = D(olddir_node), *ndir = D(newdir_node), *od = NULL, *nd = NULL;
	struct inode *odi = odir->d_inode, *ndi = ndir->d_inode, *first, *second;
	struct inode *src = NULL, *tgt = NULL;
	bool noop = false;
	int err;

	if (sb_rdonly(odi->i_sb))
		return -EROFS;
	if (!name_ok(oldname, oldlen) || !name_ok(newname, newlen))
		return -EINVAL;

	AcquireSRWLockExclusive(&g_ns_lock);
	first = odi < ndi ? odi : ndi;
	second = odi < ndi ? ndi : odi;
	inode_lock(first);
	if (second != first)
		inode_lock(second);

	err = child_dentry(odir, oldname, oldlen, &od);
	if (!err && !od->d_inode)
		err = -ENOENT;
	if (!err)
		err = child_dentry(ndir, newname, newlen, &nd);
	if (!err) {
		src = od->d_inode;
		tgt = nd->d_inode;
		if (tgt == src && odi == ndi && oldlen == newlen && !memcmp(oldname, newname, oldlen))
			noop = true;	/* same name: nothing to do */
		else if (tgt && tgt != src && (flags & APFSRW_RENAME_NOREPLACE))
			err = -EEXIST;
		else if (tgt && tgt != src && S_ISDIR(src->i_mode) && !S_ISDIR(tgt->i_mode))
			err = -ENOTDIR;
		else if (tgt && tgt != src && !S_ISDIR(src->i_mode) && S_ISDIR(tgt->i_mode))
			err = -EISDIR;
		else if (S_ISDIR(src->i_mode) && odi != ndi && is_ancestor(src, ndi))
			err = -EINVAL;	/* into its own subtree */
		else if (tgt == src)
			noop = true;	/* another link to the same inode (POSIX: no-op) */
	}
	/*
	 * A case-only rename on a case-insensitive volume finds the source
	 * itself as the target; the driver handles that as a plain rename.
	 */
	if (!err && !noop) {
		inode_lock(src);
		if (tgt && tgt != src)
			inode_lock(tgt);
		err = odi->i_op->rename(&nop_mnt_idmap, odi, od, ndi, nd,
					(flags & APFSRW_RENAME_NOREPLACE) ? RENAME_NOREPLACE : 0);
		if (tgt && tgt != src)
			inode_unlock(tgt);
		inode_unlock(src);
	}

	if (second != first)
		inode_unlock(second);
	inode_unlock(first);
	ReleaseSRWLockExclusive(&g_ns_lock);
	if (od)
		dput(od);
	if (nd)
		dput(nd);	/* a replaced target loses its last name here */
	return err;
}

/* ------------------------------------------------------------ file data */

/* generic_perform_write(): one page at a time through write_begin/write_end */
int apfsrw_write(apfsrw_file *af, uint64_t off, const void *buf, size_t len, size_t *written)
{
	struct inode *inode = af->f.f_inode;
	struct address_space *mapping = af->f.f_mapping;
	const u8 *src = buf;
	int err = 0;

	*written = 0;
	if (!(af->f.f_mode & FMODE_WRITE))
		return -EBADF;
	if (S_ISDIR(inode->i_mode))
		return -EISDIR;
	if (!mapping->a_ops->write_begin || !mapping->a_ops->write_end)
		return -EOPNOTSUPP;	/* decmpfs-compressed files */
	if (len == 0)
		return 0;

	inode_lock(inode);
	file_update_time(&af->f);
	while (len > 0) {
		size_t in = (size_t)(off & (PAGE_SIZE - 1));
		unsigned bytes = (unsigned)min(len, PAGE_SIZE - in);
		struct folio *folio = NULL;
		void *fsdata = NULL;
		int status;

		err = mapping->a_ops->write_begin(&af->f, mapping, (loff_t)off, bytes, &folio, &fsdata);
		if (err)
			break;
		memcpy((char *)folio->page.data + in, src, bytes);
		status = mapping->a_ops->write_end(&af->f, mapping, (loff_t)off, bytes, bytes, folio, fsdata);
		if (status < 0) {
			err = status;
			break;
		}
		if (status == 0) {
			err = -EIO;
			break;
		}
		src += status;
		off += (uint64_t)status;
		len -= (size_t)status;
		*written += (size_t)status;
	}
	inode_unlock(inode);
	return *written ? 0 : err;
}

int apfsrw_fsync(apfsrw_file *af)
{
	if (!af->f.f_op || !af->f.f_op->fsync)
		return 0;
	return af->f.f_op->fsync(&af->f, 0, LLONG_MAX, 0);
}

/* notify_change(): fill in the times the VFS would, then ->setattr */
int apfsrw_setattr(apfsrw_node *n, const apfsrw_setattr_t *a)
{
	struct dentry *d = D(n);
	struct inode *inode = d->d_inode;
	struct timespec64 now = current_time(inode);
	struct iattr ia;
	int err;

	if (sb_rdonly(inode->i_sb))
		return -EROFS;
	memset(&ia, 0, sizeof(ia));
	if (a->mask & APFSRW_SET_MODE) {
		ia.ia_valid |= ATTR_MODE;
		ia.ia_mode = (umode_t)((inode->i_mode & S_IFMT) | (a->mode & 07777));
	}
	if (a->mask & APFSRW_SET_UID) {
		ia.ia_valid |= ATTR_UID;
		ia.ia_uid.val = a->uid;
	}
	if (a->mask & APFSRW_SET_GID) {
		ia.ia_valid |= ATTR_GID;
		ia.ia_gid.val = a->gid;
	}
	if (a->mask & APFSRW_SET_SIZE) {
		if (S_ISDIR(inode->i_mode))
			return -EISDIR;
		ia.ia_valid |= ATTR_SIZE | ATTR_MTIME;	/* do_truncate() */
		ia.ia_size = (loff_t)a->size;
		ia.ia_mtime = now;
	}
	if (a->mask & APFSRW_SET_ATIME) {
		ia.ia_valid |= ATTR_ATIME | ATTR_ATIME_SET;
		ia.ia_atime = ns_to_timespec64(a->atime_ns);
	}
	if (a->mask & APFSRW_SET_MTIME) {
		ia.ia_valid |= ATTR_MTIME | ATTR_MTIME_SET;
		ia.ia_mtime = ns_to_timespec64(a->mtime_ns);
	}
	ia.ia_valid |= ATTR_CTIME;
	ia.ia_ctime = now;

	inode_lock(inode);
	if (inode->i_op->setattr) {
		err = inode->i_op->setattr(&nop_mnt_idmap, d, &ia);
	} else {
		setattr_copy(&nop_mnt_idmap, inode, &ia);
		mark_inode_dirty(inode);
		err = 0;
	}
	inode_unlock(inode);
	return err;
}
