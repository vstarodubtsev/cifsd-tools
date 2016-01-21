/*
 *   fs/cifssrv/vfs.c
 *
 *   Copyright (C) 2015 Samsung Electronics Co., Ltd.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/backing-dev.h>
#include <linux/writeback.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
#include <linux/xattr.h>
#endif

#include "glob.h"
#include "oplock.h"

/**
 * smb_vfs_create() - vfs helper for smb create file
 * @name:	file name
 * @mode:	file create mode
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_create(const char *name, umode_t mode)
{
	struct path path;
	struct dentry *dentry;
	int err;

	dentry = kern_path_create(AT_FDCWD, name, &path, 0);
	if (IS_ERR(dentry)) {
		err = PTR_ERR(dentry);
		cifssrv_err("path create failed for %s, err %d\n", name, err);
		return err;
	}

	mode = (mode & ~S_IFMT) | S_IFREG;
	err = vfs_create(path.dentry->d_inode, dentry, mode, true);
	if (err)
		cifssrv_err("File(%s): creation failed (err:%d)\n", name, err);

	done_path_create(&path, dentry);

	return err;
}

/**
 * smb_vfs_mkdir() - vfs helper for smb create directory
 * @name:	directory name
 * @mode:	directory create mode
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_mkdir(const char *name, umode_t mode)
{
	struct path path;
	struct dentry *dentry;
	int err;

	dentry = kern_path_create(AT_FDCWD, name, &path, LOOKUP_DIRECTORY);
	if (IS_ERR(dentry)) {
		err = PTR_ERR(dentry);
		if (err != -EEXIST)
			cifssrv_err("path create failed for %s, err %d\n",
					name, err);
		return err;
	}

	mode = (mode & ~S_IFMT) | S_IFDIR;
	err = vfs_mkdir(path.dentry->d_inode, dentry, mode);
	if (err)
		cifssrv_err("mkdir(%s): creation failed (err:%d)\n", name, err);

	done_path_create(&path, dentry);

	return err;
}

/**
 * smb_vfs_read() - vfs helper for smb file read
 * @server:     TCP server instance of connection
 * @fid:	file id of open file
 * @buf:	buf containing read data
 * @count:	read byte count
 * @pos:	file pos
 *
 * Return:	number of read bytes on success, otherwise error
 */
int smb_vfs_read(struct tcp_server_info *server, uint64_t fid, char **buf,
		size_t count, loff_t *pos)
{
	struct file *filp;
	ssize_t nbytes;
	mm_segment_t old_fs;
	struct cifssrv_file *fp;
	char *rbuf, *name;
	struct inode *inode;
	char namebuf[NAME_MAX];
	int ret;

	if (unlikely(count == 0))
		return 0;

	fp = get_id_from_fidtable(server, fid);
	if (!fp) {
		cifssrv_err("failed to get filp for fid %llu\n", fid);
		return -ENOENT;
	}
	filp = fp->filp;

	inode = filp->f_path.dentry->d_inode;
	if (S_ISDIR(inode->i_mode))
		return -EISDIR;

	ret = smb_vfs_locks_mandatory_area(filp, *pos, *pos + count - 1,
			F_RDLCK);
	if (ret == -EAGAIN) {
		cifssrv_err("%s: unable to read due to lock\n",
				__func__);
		return ret;
	}

	rbuf = vmalloc(count);
	if (!rbuf) {
		cifssrv_err("failed to alloc buf for size %zu\n", count);
		return -ENOENT;
	}

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	/*
	 * TODO : need to add special handling for Direct I/O.
	 * Direct I/O relies on MM Context of the "current" process to retrieve
	 * the pages corresponding to the user address
	 * do_direct_IO()->dio_get_page()->
				dio_refill_pages()->get_user_pages_fast()
	 * struct mm_struct *mm = current->mm;
	 * All work items in CIFSSRV are handled through default "kworker"
	 * - which do not have any MM Context.
	 * To handle Direct I/O will need to create another thread in kernel
	 * with MM context and redirect all direct I/O calls to thread. Since,
	 * this is Server and direct I/O not bottleneck. So, making default
	 * READ path to be buffered in all sequences (clearing direct IO flag).
	 */
	filp->f_flags &= ~O_DIRECT;
	nbytes = vfs_read(filp, rbuf, count, pos);
	set_fs(old_fs);
	if (nbytes < 0) {
		name = d_path(&filp->f_path, namebuf, sizeof(namebuf));
		if (IS_ERR(name))
			name = "(error)";
		cifssrv_err("smb read failed for (%s), err = %zd\n",
				name, nbytes);
		vfree(rbuf);
	} else
		*buf = rbuf;

	return nbytes;
}

/**
 * smb_vfs_write() - vfs helper for smb file write
 * @server:     TCP server instance of connection
 * @fid:	file id of open file
 * @buf:	buf containing data for writing
 * @count:	read byte count
 * @pos:	file pos
 * @sync:	fsync after write
 * @written:	number of bytes written
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_write(struct tcp_server_info *server, uint64_t fid, char *buf,
		size_t count, loff_t *pos,
		bool sync, ssize_t *written)
{
	struct file *filp;
	loff_t	offset = *pos;
	mm_segment_t old_fs;
	struct cifssrv_file *fp;
	int err;

	fp = get_id_from_fidtable(server, fid);
	if (!fp) {
		cifssrv_err("failed to get filp for fid %llu server = 0x%p\n",
				fid, server);
		return -ENOENT;
	}
	filp = fp->filp;

	err = smb_vfs_locks_mandatory_area(filp, *pos, *pos + count - 1,
			F_WRLCK);
	if (err == -EAGAIN) {
		cifssrv_err("%s: unable to write due to lock\n",
				__func__);
		return err;
	}

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	if (oplocks_enable) {
		/* Do we need to break any of a levelII oplock? */
		mutex_lock(&ofile_list_lock);
		smb_breakII_oplock(server, fp, NULL);
		mutex_unlock(&ofile_list_lock);
	}

	err = vfs_write(filp, buf, count, pos);
	set_fs(old_fs);
	if (err < 0) {
		cifssrv_err("smb write failed, err = %d\n", err);
		return err;
	}

	*written = err;
	err = 0;
	if (sync) {
		err = vfs_fsync_range(filp, offset, offset + *written, 0);
		if (err < 0)
			cifssrv_err("fsync failed for fid %llu, err = %d\n",
					fid, err);
	}

	return err;
}

/**
 * smb_check_attrs() - sanitize inode attributes
 * @inode:	inode
 * @attrs:	inode attributes
 */
void smb_check_attrs(struct inode *inode, struct iattr *attrs)
{
	/* sanitize the mode change */
	if (attrs->ia_valid & ATTR_MODE) {
		attrs->ia_mode &= S_IALLUGO;
		attrs->ia_mode |= (inode->i_mode & ~S_IALLUGO);
	}

	/* Revoke setuid/setgid on chown */
	if (!S_ISDIR(inode->i_mode) &&
		(((attrs->ia_valid & ATTR_UID) &&
				!uid_eq(attrs->ia_uid, inode->i_uid)) ||
		 ((attrs->ia_valid & ATTR_GID) &&
				!gid_eq(attrs->ia_gid, inode->i_gid)))) {
		attrs->ia_valid |= ATTR_KILL_PRIV;
		if (attrs->ia_valid & ATTR_MODE) {
			/* we're setting mode too, just clear the s*id bits */
			attrs->ia_mode &= ~S_ISUID;
			if (attrs->ia_mode & S_IXGRP)
				attrs->ia_mode &= ~S_ISGID;
		} else {
			/* set ATTR_KILL_* bits and let VFS handle it */
			attrs->ia_valid |= (ATTR_KILL_SUID | ATTR_KILL_SGID);
		}
	}
}

/**
 * smb_vfs_setattr() - vfs helper for smb setattr
 * @server:     TCP server instance of connection
 * @name:	file name
 * @fid:	file id of open file
 * @attrs:	inode attributes
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_setattr(struct tcp_server_info *server, const char *name,
		__u16 fid, struct iattr *attrs)
{
	struct file *filp;
	struct dentry *dentry;
	struct inode *inode;
	struct path path;
	bool update_size = false;
	int err = 0;
	struct cifssrv_file *fp;

	if (name) {
		err = kern_path(name, 0, &path);
		if (err) {
			cifssrv_err("lookup failed for %s, err = %d\n",
					name, err);
			return -ENOENT;
		}
		dentry = path.dentry;
		inode = dentry->d_inode;
	} else {

		fp = get_id_from_fidtable(server, fid);
		if (!fp) {
			cifssrv_err("failed to get filp for fid %u\n", fid);
			return -ENOENT;
		}
		filp = fp->filp;
		dentry = filp->f_path.dentry;
		inode = dentry->d_inode;
	}

	/* no need to update mode of symlink */
	if (S_ISLNK(inode->i_mode))
		attrs->ia_valid &= ~ATTR_MODE;

	/* skip setattr, if nothing to update */
	if (!attrs->ia_valid) {
		err = 0;
		goto out;
	}

	smb_check_attrs(inode, attrs);
	if (attrs->ia_valid & ATTR_SIZE) {
		err = get_write_access(inode);
		if (err)
			goto out;

		err = locks_verify_truncate(inode, NULL, attrs->ia_size);
		if (err) {
			put_write_access(inode);
			goto out;
		}
		update_size = true;
	}

	attrs->ia_valid |= ATTR_CTIME;

	mutex_lock(&inode->i_mutex);
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
	err = notify_change(dentry, attrs, NULL);
#else
	err = notify_change(dentry, attrs);
#endif
	mutex_unlock(&inode->i_mutex);

	if (update_size)
		put_write_access(inode);

	if (!err) {
		sync_inode_metadata(inode, 1);
		cifssrv_debug("fid %u, setattr done\n", fid);
	}

out:
	if (name)
		path_put(&path);
	return err;
}

/**
 * smb_vfs_getattr() - vfs helper for smb getattr
 * @server:     TCP server instance of connection
 * @fid:	file id of open file
 * @attrs:	inode attributes
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_getattr(struct tcp_server_info *server, __u16 fid,
		struct kstat *stat)
{
	struct file *filp;
	struct cifssrv_file *fp;
	int err;

	fp = get_id_from_fidtable(server, fid);
	if (!fp) {
		cifssrv_err("failed to get filp for fid %u\n", fid);
		return -ENOENT;
	}
	filp = fp->filp;

	err = vfs_getattr(&filp->f_path, stat);
	if (err)
		cifssrv_err("getattr failed for fid %u, err %d\n", fid, err);
	return err;
}

/**
 * smb_vfs_fsync() - vfs helper for smb fsync
 * @server:     TCP server instance of connection
 * @fid:	file id of open file
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_fsync(struct tcp_server_info *server, uint64_t fid)
{
	struct file *filp;
	struct cifssrv_file *fp;
	int err;

	fp = get_id_from_fidtable(server, fid);
	if (!fp) {
		cifssrv_err("failed to get filp for fid %llu\n", fid);
		return -ENOENT;
	}
	filp = fp->filp;

	err = vfs_fsync(filp, 0);
	if (err < 0)
		cifssrv_err("smb fsync failed, err = %d\n", err);

	return err;
}

/**
 * smb_vfs_rmdir() - vfs helper for smb rmdir
 * @name:	file name
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_rmdir(const char *name)
{
	struct path path;
	int err;

	err = kern_path(name, LOOKUP_DIRECTORY, &path);
	if (err) {
		cifssrv_err("cannot get linux path for %s, err = %d\n",
				name, err);
		return err;
	}

	mutex_lock_nested(&path.dentry->d_parent->d_inode->i_mutex,
			I_MUTEX_PARENT);
	err = vfs_rmdir(path.dentry->d_parent->d_inode, path.dentry);
	if (err && err != -ENOTEMPTY)
		cifssrv_err("rmdir failed for (%s), err %d\n", name, err);

	mutex_unlock(&path.dentry->d_parent->d_inode->i_mutex);
	path_put(&path);

	return err;
}

/**
 * smb_vfs_unlink() - vfs helper for smb unlink
 * @name:	file name
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_unlink(const char *name)
{
	struct path path;
	int err;

	err = kern_path(name, 0, &path);
	if (err) {
		cifssrv_debug("cannot get linux path for %s, err = %d\n",
				name, err);
		return err;
	}

	dget(path.dentry);
	mutex_lock_nested(&path.dentry->d_parent->d_inode->i_mutex,
			I_MUTEX_PARENT);
	if (path.dentry->d_inode->i_nlink == 0)
		goto skip;

#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
	err = vfs_unlink(path.dentry->d_parent->d_inode, path.dentry, NULL);
#else
	err = vfs_unlink(path.dentry->d_parent->d_inode, path.dentry);
#endif
	if (err)
		cifssrv_debug("unlink failed for %s, err %d\n", name, err);

skip:
	mutex_unlock(&path.dentry->d_parent->d_inode->i_mutex);
	dput(path.dentry);
	path_put(&path);

	return err;
}

/**
 * smb_vfs_link() - vfs helper for creating smb hardlink
 * @oldname:	source file name
 * @newname:	hardlink name
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_link(const char *oldname, const char *newname)
{
	struct path oldpath, newpath;
	struct dentry *dentry;
	int err;

	err = kern_path(oldname, LOOKUP_FOLLOW, &oldpath);
	if (err) {
		cifssrv_err("cannot get linux path for %s, err = %d\n",
				oldname, err);
		goto out1;
	}

	dentry = kern_path_create(AT_FDCWD, newname, &newpath,
			LOOKUP_FOLLOW & LOOKUP_REVAL);
	if (IS_ERR(dentry)) {
		err = PTR_ERR(dentry);
		cifssrv_err("path create err for %s, err %d\n", newname, err);
		goto out2;
	}

	err = -EXDEV;
	if (oldpath.mnt != newpath.mnt) {
		cifssrv_err("vfs_link failed err %d\n", err);
		goto out3;
	}

#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
	err = vfs_link(oldpath.dentry, newpath.dentry->d_inode, dentry, NULL);
#else
	err = vfs_link(oldpath.dentry, newpath.dentry->d_inode, dentry);
#endif
	if (err)
		cifssrv_err("vfs_link failed err %d\n", err);

out3:
	done_path_create(&newpath, dentry);
out2:
	path_put(&oldpath);

out1:
	return err;
}

/**
 * smb_vfs_symlink() - vfs helper for creating smb symlink
 * @name:	source file name
 * @symname:	symlink name
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_symlink(const char *name, const char *symname)
{
	struct path path;
	struct dentry *dentry;
	int err;

	dentry = kern_path_create(AT_FDCWD, symname, &path, 0);
	if (IS_ERR(dentry)) {
		err = PTR_ERR(dentry);
		cifssrv_err("path create failed for %s, err %d\n", name, err);
		return err;
	}

	err = vfs_symlink(dentry->d_parent->d_inode, dentry, name);
	if (err && (err != -EEXIST || err != -ENOSPC))
		cifssrv_err("failed to create symlink, err %d\n", err);

	done_path_create(&path, dentry);

	return err;
}

/**
 * smb_vfs_readlink() - vfs helper for reading value of symlink
 * @path:	path of symlink
 * @buf:	destination buffer for symlink value
 * @lenp:	destination buffer length
 *
 * Return:	symlink value length on success, otherwise error
 */
int smb_vfs_readlink(struct path *path, char *buf, int lenp)
{
	struct inode *inode;
	mm_segment_t old_fs;
	int err;

	if (!path)
		return -ENOENT;

	inode = path->dentry->d_inode;
	if (!S_ISLNK(inode->i_mode))
		return -EINVAL;

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	err = inode->i_op->readlink(path->dentry, (char __user *)buf, lenp);
	set_fs(old_fs);
	if (err < 0)
		cifssrv_err("readlink failed, err = %d\n", err);

	return err;
}

/**
 * smb_vfs_rename() - vfs helper for smb rename
 * @server:		TCP server instance of connection
 * @abs_oldname:	old filename
 * @abs_newname:	new filename
 * @oldfid:		file id of old file
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_rename(struct tcp_server_info *server, const char *abs_oldname,
		const char *abs_newname, __u16 oldfid)
{
	struct path oldpath_p, newpath_p;
	struct dentry *dold, *dnew, *dold_p, *dnew_p, *trap;
	const char *oldname = NULL, *newname = NULL;
	struct file *filp = NULL;
	struct cifssrv_file *fp;
	int err;

	if (abs_oldname) {
		/* normal case: rename with source filename */
		oldname = strrchr(abs_oldname, '/');
		if (oldname && oldname[1] != '\0')
			oldname++;
		else {
			cifssrv_err("can't get last component in path %s\n",
					abs_oldname);
			return -ENOENT;
		}

		err = kern_path(abs_oldname, LOOKUP_PARENT, &oldpath_p);
		if (err) {
			cifssrv_err("cannot get linux path for %s, err %d\n",
					oldname, err);
			return -ENOENT;
		}
		dold_p = oldpath_p.dentry;

		newname = strrchr(abs_newname, '/');
		if (newname && newname[1] != '\0')
			newname++;
		else {
			cifssrv_err("can't get last component in path %s\n",
					abs_newname);
			err = -ENOMEM;
			goto out1;
		}

		err = kern_path(abs_newname, LOOKUP_PARENT, &newpath_p);
		if (err) {
			cifssrv_err("cannot get linux path for %s, err = %d\n",
					newname, err);
			goto out1;
		}
		dnew_p = newpath_p.dentry;
	} else {
		/* rename by fid of source file instead of source filename */
		fp = get_id_from_fidtable(server, oldfid);
		if (!fp) {
			cifssrv_err("can't find filp for fid %u\n", oldfid);
			return -ENOENT;
		}
		filp = fp->filp;
		dold_p = filp->f_path.dentry->d_parent;

		newname = strrchr(abs_newname, '/');
		if (newname && newname[1] != '\0')
			newname++;
		else {
			cifssrv_err("can't get last component in path %s\n",
					abs_newname);
			return -ENOMEM;
		}

		err = kern_path(abs_newname, LOOKUP_PARENT, &newpath_p);
		if (err) {
			cifssrv_err("cannot get linux path for %s, err = %d\n",
					newname, err);
			return err;
		}
		dnew_p = newpath_p.dentry;
	}

	cifssrv_debug("oldname %s, newname %s\n", oldname, newname);
	trap = lock_rename(dold_p, dnew_p);
	if (abs_oldname) {
		dold = lookup_one_len(oldname, dold_p, strlen(oldname));
		err = PTR_ERR(dold);
		if (IS_ERR(dold)) {
			cifssrv_err("%s lookup failed with error = %d\n",
					oldname, err);
			goto out2;
		}
	} else {
		dold = filp->f_path.dentry;
		dget(dold);
	}

	err = -ENOENT;
	if (!dold->d_inode)
		goto out3;
	err = -EINVAL;
	if (dold == trap)
		goto out3;

	dnew = lookup_one_len(newname, dnew_p, strlen(newname));
	err = PTR_ERR(dnew);
	if (IS_ERR(dnew)) {
		cifssrv_err("%s lookup failed with error = %d\n",
				newname, err);
		goto out3;
	}

	err = -ENOTEMPTY;
	if (dnew == trap)
		goto out4;

#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
	err = vfs_rename(dold_p->d_inode, dold, dnew_p->d_inode, dnew, NULL, 0);
#else
	err = vfs_rename(dold_p->d_inode, dold, dnew_p->d_inode, dnew);
#endif
	if (err)
		cifssrv_err("vfs_rename failed err %d\n", err);

out4:
	dput(dnew);
out3:
	dput(dold);
out2:
	unlock_rename(dold_p, dnew_p);
	path_put(&newpath_p);
out1:
	if (abs_oldname)
		path_put(&oldpath_p);
	return err;
}

/**
 * smb_vfs_truncate() - vfs helper for smb file truncate
 * @server:	TCP server instance of connection
 * @name:	old filename
 * @fid:	file id of old file
 * @size:	truncate to given size
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_truncate(struct tcp_server_info *server, const char *name,
		__u16 fid, loff_t size)
{
	struct path path;
	struct file *filp;
	int err = 0;
	struct cifssrv_file *fp;
	struct inode *inode;

	if (name) {
		err = kern_path(name, 0, &path);
		if (err) {
			cifssrv_err("cannot get linux path for %s, err %d\n",
					name, err);
			return err;
		}
		err = vfs_truncate(&path, size);
		if (err)
			cifssrv_err("truncate failed for %s err %d\n",
					name, err);
		path_put(&path);
	} else {
		fp = get_id_from_fidtable(server, fid);
		if (!fp) {
			cifssrv_err("failed to get filp for fid %u\n", fid);
			return -ENOENT;
		}
		filp = fp->filp;

		if (oplocks_enable) {
			/* Do we need to break any of a levelII oplock? */
			mutex_lock(&ofile_list_lock);
			smb_breakII_oplock(server, fp, NULL);
			mutex_unlock(&ofile_list_lock);
		} else {
			inode = file_inode(filp);
			if (size < inode->i_size) {
				err = smb_vfs_locks_mandatory_area(filp, size,
						inode->i_size - 1, F_WRLCK);
			} else {
				err = smb_vfs_locks_mandatory_area(filp,
						inode->i_size, size - 1,
						F_WRLCK);
			}

			if (err == -EAGAIN) {
				cifssrv_err("failed due to lock\n");
				return err;
			}
		}
		err = vfs_truncate(&filp->f_path, size);
		if (err)
			cifssrv_err("truncate failed for fid %u err %d\n",
					fid, err);
	}

	return err;
}

/**
 * smb_vfs_listxattr() - vfs helper for smb list extended attributes
 * @dentry:	dentry of file for listing xattrs
 * @list:	destination buffer
 * @size:	destination buffer length
 *
 * Return:	xattr list length on success, otherwise error
 */
int smb_vfs_listxattr(struct dentry *dentry, char **list, int size)
{
	int err;
	char *vlist = NULL;

	if (size) {
		if (size > XATTR_LIST_MAX)
			size = XATTR_LIST_MAX;
		vlist = vmalloc(size);
		if (!vlist)
			return -ENOMEM;
	}

	err = vfs_listxattr(dentry, vlist, size);
	if (err > 0) {
		*list = vlist;
	} else if (err == -ERANGE) {
		/* The file system tried to returned a list bigger
		   than XATTR_LIST_MAX bytes. Not possible. */
		err = -E2BIG;
		cifssrv_debug("listxattr failed\n");
	}

	return err;
}

/**
 * smb_vfs_getxattr() - vfs helper for smb get extended attributes value
 * @dentry:	dentry of file for getting xattrs
 * @xattr_name:	name of xattr name to query
 * @xattr_buf:	destination buffer xattr value
 * @buf_len:	destination buffer length
 *
 * Return:	read xattr value length on success, otherwise error
 */
int smb_vfs_getxattr(struct dentry *dentry, char *xattr_name,
		char *xattr_buf, __u32 buf_len)
{
	ssize_t xattr_len;
	ssize_t ret;

	xattr_len = vfs_getxattr(dentry, xattr_name, NULL, 0);
	if (xattr_len <= 0)
		return xattr_len;

	if (xattr_len > buf_len)
		return -ENOMEM;

	ret = vfs_getxattr(dentry, xattr_name, xattr_buf, xattr_len);
	if (ret < 0)
		cifssrv_debug("getxattr failed, ret %zd\n", ret);

	return ret;
}

/**
 * smb_vfs_setxattr() - vfs helper for smb set extended attributes value
 * @filename:	file name
 * @fpath:	path of file for setxattr
 * @name:	xattr name for setxattr
 * @value:	xattr value to set
 * @size:	size of xattr value
 * @flags:	destination buffer length
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_setxattr(const char *filename, struct path *fpath, const char *name,
		const void *value, size_t size, int flags)
{
	struct path path;
	int err;

	if (filename) {
		err = kern_path(filename, 0, &path);
		if (err) {
			cifssrv_debug("cannot get linux path %s, err %d\n",
					filename, err);
			return err;
		}
		err = vfs_setxattr(path.dentry, name, value, size, flags);
		if (err)
			cifssrv_debug("setxattr failed, err %d\n", err);
		path_put(&path);
	} else {
		err = vfs_setxattr(fpath->dentry, name, value, size, flags);
		if (err)
			cifssrv_debug("setxattr failed, err %d\n", err);
	}

	return err;
}

/**
 * smb_vfs_setxattr() - convert smb IO caching options to linux options
 * @filp:	file pointer for IO
 * @options:	smb IO options
 */
void smb_vfs_set_fadvise(struct file *filp, int option)
{
	struct address_space *mapping;
	mapping = filp->f_mapping;

	if (!option || !mapping)
		return;

	if (option & FILE_WRITE_THROUGH_LE)
		filp->f_flags |= O_SYNC;
	else if (option & FILE_NO_INTERMEDIATE_BUFFERING_LE)
		filp->f_flags |= O_DIRECT;
	else if (option & FILE_SEQUENTIAL_ONLY_LE) {
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
		filp->f_ra.ra_pages = inode_to_bdi(mapping->host)->ra_pages * 2;
#else
		filp->f_ra.ra_pages = mapping->backing_dev_info->ra_pages * 2;
#endif
		spin_lock(&filp->f_lock);
		filp->f_mode &= ~FMODE_RANDOM;
		spin_unlock(&filp->f_lock);
	} else if (option & FILE_RANDOM_ACCESS_LE) {
		spin_lock(&filp->f_lock);
		filp->f_mode |= FMODE_RANDOM;
		spin_unlock(&filp->f_lock);
	}
}

/**
 * smb_vfs_lock() - vfs helper for smb file locking
 * @filp:	the file to apply the lock to
 * @cmd:	type of locking operation (F_SETLK, F_GETLK, etc.)
 * @flock:	The lock to be applied
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_lock(struct file *filp, int cmd,
			struct file_lock *flock)
{
	cifssrv_debug("%s: calling vfs_lock_file\n", __func__);
	return vfs_lock_file(filp, cmd, flock, NULL);
}

/**
 * smb_vfs_locks_mandatory_area() - vfs helper for smb byte range file locking
 * @filp:	the file to apply the lock to
 * @start:	lock start byte offset
 * @end:	lock end byte offset
 * @type:	byte range type read/write
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_locks_mandatory_area(struct file *filp, loff_t start, loff_t end,
		unsigned char type)
{
	struct file_lock fl;
	int error;

#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
	struct file_lock_context *ctx = file_inode(filp)->i_flctx;
	if (!ctx || list_empty_careful(&ctx->flc_posix))
		return 0;
#else
	if (!file_inode(filp)->i_flock)
		return 0;
#endif

	locks_init_lock(&fl);
	fl.fl_owner = (struct files_struct *)filp;
	fl.fl_pid = current->tgid;
	fl.fl_file = filp;
	fl.fl_flags = FL_POSIX | FL_ACCESS;
	fl.fl_type = type;
	fl.fl_start = start;
	fl.fl_end = end;
	error = posix_lock_file(filp, &fl, NULL);
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
	posix_unblock_lock(&fl);
#else
	locks_delete_block(&fl);
#endif
	return error;
}

int smb_vfs_readdir(struct file *file, filldir_t filler, void *buf)
{
	struct smb_readdir_data *r_data = (struct smb_readdir_data *)buf;
	int err;

#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
	err = iterate_dir(file, &r_data->ctx);
#else
	err = vfs_readdir(file, smb_filldir, r_data);
#endif

	return err;
}