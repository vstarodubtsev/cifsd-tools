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

#define KUIDT_INIT(value) (kuid_t){ value }
#define KGIDT_INIT(value) (kgid_t){ value }

static inline uid_t __kuid_val(kuid_t uid)
{
	return uid.val;
}

static inline gid_t __kgid_val(kgid_t gid)
{
	return gid.val;
}

static inline kuid_t make_kuid(struct user_namespace *from, uid_t uid)
{
	return KUIDT_INIT(uid);
}

static inline kgid_t make_kgid(struct user_namespace *from, gid_t gid)
{
	return KGIDT_INIT(gid);
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

#define INVALID_UID KUIDT_INIT(-1)
#define INVALID_GID KGIDT_INIT(-1)

static inline bool uid_eq(kuid_t left, kuid_t right)
{
	return __kuid_val(left) == __kuid_val(right);
}

static inline bool gid_eq(kgid_t left, kgid_t right)
{
	return __kgid_val(left) == __kgid_val(right);
}

static inline bool uid_valid(kuid_t uid)
{
	return !uid_eq(uid, INVALID_UID);
}

static inline bool gid_valid(kgid_t gid)
{
	return !gid_eq(gid, INVALID_GID);
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

#if 0
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,15,0)
static inline void kvfree(const void *addr)
{
	if (is_vmalloc_addr(addr))
		vfree(addr);
	else
		kfree(addr);
}
#endif
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

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 5, 0)
static inline void key_invalidate(struct key *key){};
#endif

#endif /* __CIFSD_COMPAT_H */
