#ifndef __CIFSD_COMPAT_H
#define __CIFSD_COMPAT_H

#include <linux/mount.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 5, 0)
typedef struct {
	uid_t val;
} kuid_t;

typedef struct {
	gid_t val;
} kgid_t;

static inline uid_t __kuid_val(kuid_t uid)
{
	return uid.val;
}

static inline gid_t __kgid_val(kgid_t gid)
{
	return gid.val;
}

static inline uid_t from_kuid(struct user_namespace *to, kuid_t kuid)
{
	return __kuid_val(kuid);
}

static inline gid_t from_kgid(struct user_namespace *to, kgid_t kgid)
{
	return __kgid_val(kgid);
}

static inline void i_uid_write(struct inode *inode, uid_t uid)
{
	inode->i_uid = uid;
}

static inline void i_gid_write(struct inode *inode, gid_t gid)
{
	inode->i_gid = gid;
}

#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 6, 0)

static inline void done_path_create(struct path *path, struct dentry *dentry)
{
	dput(dentry);
	mutex_unlock(&path->dentry->d_inode->i_mutex);
	mnt_drop_write(path->mnt);
	path_put(path);
}

#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,15,0)
static inline void kvfree(const void *addr)
{
	if (is_vmalloc_addr(addr))
		vfree(addr);
	else
		kfree(addr);
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,9,0)
#define compat_hlist_for_each_entry		hlist_for_each_entry
#define compat_hlist_for_each_entry_safe	hlist_for_each_entry_safe
#define compat_hlist_for_each_entry_rcu	hlist_for_each_entry_rcu
static inline struct inode *file_inode(const struct file *f)
{
  return f->f_path.dentry->d_inode;
}
#else
#define compat_hlist_for_each_entry(a,pos,c,d)	hlist_for_each_entry(a,c,d)
#define compat_hlist_for_each_entry_safe(a,pos,c,d,e)	hlist_for_each_entry_safe(a,c,d,e)
#define compat_hlist_for_each_entry_rcu(a,pos,c,d)	hlist_for_each_entry_rcu(a,c,d)
#endif

#endif /* __CIFSD_COMPAT_H */
