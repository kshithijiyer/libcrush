int ceph_debug_dir = -1;
#define DOUT_VAR ceph_debug_dir
#define DOUT_PREFIX "dir: "
#include "super.h"

#include <linux/namei.h>
#include <linux/sched.h>

const struct inode_operations ceph_dir_iops;
const struct file_operations ceph_dir_fops;
struct dentry_operations ceph_dentry_ops;

static int ceph_dentry_revalidate(struct dentry *dentry, struct nameidata *nd);

/*
 * build a dentry's path.  allocate on heap; caller must kfree.  based
 * on build_path_from_dentry in fs/cifs/dir.c.
 *
 * stop path construction as soon as we hit a dentry we do not have a
 * valid lease over.  races aside, this ensures we describe the
 * operation relative to a base inode that is likely to be cached by
 * the MDS, using a relative path that is known to be valid (e.g., not
 * munged up by a directory rename on another client).
 *
 * this is, unfortunately, both racy and inefficient.  dentries are
 * revalidated during path traversal, and revalidated _again_ when we
 * reconstruct the reverse path.  lame.  unfortunately the VFS doesn't
 * tell us the path it traversed, so i'm not sure we can do any better.
 */
char *ceph_build_dentry_path(struct dentry *dentry, int *plen, __u64 *base)
{
	struct dentry *temp;
	char *path;
	int len, pos;

	if (dentry == NULL)
		return ERR_PTR(-EINVAL);

retry:
	len = 0;
	for (temp = dentry; !IS_ROOT(temp);) {
		if (temp->d_inode &&
		    !ceph_dentry_revalidate(temp, 0))
			break;
		len += 1 + temp->d_name.len;
		temp = temp->d_parent;
		if (temp == NULL) {
			derr(1, "corrupt dentry %p\n", dentry);
			return ERR_PTR(-EINVAL);
		}
	}
	if (len)
		len--;  /* no leading '/' */

	path = kmalloc(len+1, GFP_NOFS);
	if (path == NULL)
		return ERR_PTR(-ENOMEM);
	pos = len;
	path[pos] = 0;	/* trailing null */
	for (temp = dentry; !IS_ROOT(temp) && pos != 0; ) {
		pos -= temp->d_name.len;
		if (pos < 0) {
			break;
		} else {
			strncpy(path + pos, temp->d_name.name,
				temp->d_name.len);
			dout(50, "build_path_dentry path+%d: %p '%.*s'\n",
			     pos, temp, temp->d_name.len, path + pos);
			if (pos)
				path[--pos] = '/';
		}
		temp = temp->d_parent;
		if (temp == NULL) {
			derr(1, "corrupt dentry\n");
			kfree(path);
			return ERR_PTR(-EINVAL);
		}
	}
	if (pos != 0) {
		derr(1, "did not end path lookup where expected, "
		     "namelen is %d, pos is %d\n", len, pos);
		/* presumably this is only possible if racing with a
		   rename of one of the parent directories (we can not
		   lock the dentries above us to prevent this, but
		   retrying should be harmless) */
		kfree(path);
		goto retry;
	}

	*base = ceph_ino(temp->d_inode);
	*plen = len;
	dout(10, "build_path_dentry on %p %d built %llx '%.*s'\n",
	     dentry, atomic_read(&dentry->d_count), *base, len, path);
	return path;
}


/*
 * build fpos from fragment id and offset within that fragment.
 */
static loff_t make_fpos(unsigned frag, unsigned off)
{
	return ((loff_t)frag << 32) | (loff_t)off;
}
static unsigned fpos_frag(loff_t p)
{
	return p >> 32;
}
static unsigned fpos_off(loff_t p)
{
	return p & 0xffffffff;
}

static int ceph_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct ceph_file_info *fi = filp->private_data;
	struct inode *inode = filp->f_dentry->d_inode;
	struct ceph_mds_client *mdsc = &ceph_inode_to_client(inode)->mdsc;
	unsigned frag = fpos_frag(filp->f_pos);
	unsigned off = fpos_off(filp->f_pos);
	unsigned skew;
	int err;
	__u32 ftype;
	struct ceph_mds_reply_info *rinfo;

nextfrag:
	dout(5, "dir_readdir filp %p at frag %u off %u\n", filp, frag, off);
	if (fi->frag != frag || fi->last_readdir == NULL) {
		struct ceph_mds_request *req;
		struct ceph_mds_request_head *rhead;

		frag = ceph_choose_frag(ceph_inode(inode), frag, 0);

		/* query mds */
		dout(10, "dir_readdir querying mds for ino %llx frag %x\n",
		     ceph_ino(inode), frag);
		req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_READDIR,
					       ceph_ino(inode), "", 0, 0,
					       filp->f_dentry, USE_AUTH_MDS);
		if (IS_ERR(req))
			return PTR_ERR(req);
		req->r_direct_hash = frag_value(frag);
		req->r_direct_is_hash = true;
		rhead = req->r_request->front.iov_base;
		rhead->args.readdir.frag = cpu_to_le32(frag);
		err = ceph_mdsc_do_request(mdsc, req);
		if (err < 0) {
			ceph_mdsc_put_request(req);
			return err;
		}
		dout(10, "dir_readdir got and parsed readdir result=%d"
		     " on frag %x\n", err, frag);
		if (fi->last_readdir)
			ceph_mdsc_put_request(fi->last_readdir);
		fi->last_readdir = req;
	}

	/* include . and .. with first fragment */
	if (frag_is_leftmost(frag)) {
		switch (off) {
		case 0:
			dout(10, "dir_readdir off 0 -> '.'\n");
			if (filldir(dirent, ".", 1, make_fpos(0, 0),
				    inode->i_ino, inode->i_mode >> 12) < 0)
				return 0;
			off++;
			filp->f_pos++;
		case 1:
			dout(10, "dir_readdir off 1 -> '..'\n");
			if (filp->f_dentry->d_parent != NULL &&
			    filldir(dirent, "..", 2, make_fpos(0, 1),
				    filp->f_dentry->d_parent->d_inode->i_ino,
				    inode->i_mode >> 12) < 0)
				return 0;
			off++;
			filp->f_pos++;
		}
		skew = -2;
	} else
		skew = 0;

	rinfo = &fi->last_readdir->r_reply_info;
	dout(10, "dir_readdir frag %x num %d off %d skew %d\n", frag,
	     rinfo->dir_nr, off, skew);
	while (off+skew < rinfo->dir_nr) {
		dout(10, "dir_readdir off %d -> %d / %d name '%.*s'\n",
		     off, off+skew,
		     rinfo->dir_nr, rinfo->dir_dname_len[off+skew],
		     rinfo->dir_dname[off+skew]);
		ftype = le32_to_cpu(rinfo->dir_in[off+skew].in->mode >> 12);
		if (filldir(dirent,
			    rinfo->dir_dname[off+skew],
			    rinfo->dir_dname_len[off+skew],
			    make_fpos(frag, off),
			    le64_to_cpu(rinfo->dir_in[off+skew].in->ino),
			    ftype) < 0) {
			dout(20, "filldir stopping us...\n");
			return 0;
		}
		off++;
		filp->f_pos++;
	}

	/* more frags? */
	if (frag_value(frag) != frag_mask(frag)) {
		frag = frag_next(frag);
		off = 0;
		filp->f_pos = make_fpos(frag, off);
		dout(10, "dir_readdir next frag is %x\n", frag);
		goto nextfrag;
	}

	dout(20, "dir_readdir done.\n");
	return 0;
}

loff_t ceph_dir_llseek(struct file *file, loff_t offset, int origin)
{
	struct ceph_file_info *fi = file->private_data;
	struct inode *inode = file->f_mapping->host;
	loff_t retval;

	mutex_lock(&inode->i_mutex);
	switch (origin) {
	case SEEK_END:
		offset += inode->i_size;
		break;
	case SEEK_CUR:
		offset += file->f_pos;
	}
	retval = -EINVAL;
	if (offset >= 0 && offset <= inode->i_sb->s_maxbytes) {
		if (offset != file->f_pos) {
			file->f_pos = offset;
			file->f_version = 0;
		}
		retval = offset;
		if (offset == 0 && fi->last_readdir) {
			dout(10, "llseek dropping %p readdir content\n", file);
			ceph_mdsc_put_request(fi->last_readdir);
			fi->last_readdir = 0;
		}
	}
	mutex_unlock(&inode->i_mutex);
	return retval;
}

struct dentry *ceph_finish_lookup(struct ceph_mds_request *req,
				  struct dentry *dentry, int err)
{
	if (err == -ENOENT) {
		/* no trace? */
		if (req->r_reply_info.trace_numd == 0) {
			dout(20, "ENOENT and no trace, dentry %p inode %p\n",
			     dentry, dentry->d_inode);
			ceph_init_dentry(dentry);
			if (dentry->d_inode) {
				dout(40, "d_drop %p\n", dentry);
				d_drop(dentry);
				req->r_last_dentry = d_alloc(dentry->d_parent,
							     &dentry->d_name);
				d_rehash(req->r_last_dentry);
			} else
				d_add(dentry, NULL);
		}
		err = 0;
	}
	if (err)
		dentry = ERR_PTR(err);
	else if (dentry != req->r_last_dentry) {
		dentry = req->r_last_dentry;   /* we got spliced */
		dget(dentry);
	} else
		dentry = 0;
	return dentry;
}

/*
 * do a lookup / lstat (same thing).
 * @on_inode indicates that we should stat the ino, and not a path
 * built from @dentry.
 */
struct dentry *ceph_do_lookup(struct super_block *sb, struct dentry *dentry, 
			      int mask, int on_inode)
{
	struct ceph_client *client = ceph_sb_to_client(sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	char *path;
	int pathlen;
	struct ceph_mds_request *req;
	struct ceph_mds_request_head *rhead;
	int err;

	if (dentry->d_name.len > NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);

	dout(10, "do_lookup %p mask %d\n", dentry, mask);
	if (on_inode) {
		/* stat ino directly */
		req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_LSTAT,
					       ceph_ino(dentry->d_inode), 0,
					       0, 0,
					       dentry, USE_CAP_MDS);
	} else {
		/* build path */
		u64 pathbase;
		path = ceph_build_dentry_path(dentry, &pathlen, &pathbase);
		if (IS_ERR(path))
			return ERR_PTR(PTR_ERR(path));
		req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_LSTAT,
					       pathbase, path, 0, 0,
					       dentry, USE_ANY_MDS);
		kfree(path);
	}
	if (IS_ERR(req))
		return ERR_PTR(PTR_ERR(req));
	rhead = req->r_request->front.iov_base;
	rhead->args.stat.mask = cpu_to_le32(mask);
	dget(dentry);                /* to match put_request below */
	req->r_last_dentry = dentry; /* use this dentry in fill_trace */
	err = ceph_mdsc_do_request(mdsc, req);
	dentry = ceph_finish_lookup(req, dentry, err);
	ceph_mdsc_put_request(req);  /* will dput(dentry) */
	dout(20, "do_lookup result=%p\n", dentry);
	return dentry;
}

static struct dentry *ceph_lookup(struct inode *dir, struct dentry *dentry,
				  struct nameidata *nd)
{
	dout(5, "dir_lookup in dir %p dentry %p '%.*s'\n",
	     dir, dentry, dentry->d_name.len, dentry->d_name.name);

	/* open (but not create!) intent? */
	if (nd &&
	    (nd->flags & LOOKUP_OPEN) &&
	    (nd->flags & LOOKUP_CONTINUE) == 0 && /* only open last component */
	    !(nd->intent.open.flags & O_CREAT)) {
		int mode = nd->intent.open.create_mode & ~current->fs->umask;
		return ceph_lookup_open(dir, dentry, nd, mode);
	}

	return ceph_do_lookup(dir->i_sb, dentry, CEPH_STAT_MASK_INODE_ALL, 0);
}

static int ceph_mknod(struct inode *dir, struct dentry *dentry,
			  int mode, dev_t rdev)
{
	struct ceph_client *client = ceph_sb_to_client(dir->i_sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct ceph_mds_request *req;
	struct ceph_mds_request_head *rhead;
	char *path;
	int pathlen;
	u64 pathbase;
	int err;

	dout(5, "dir_mknod in dir %p dentry %p mode 0%o rdev %d\n",
	     dir, dentry, mode, rdev);
	path = ceph_build_dentry_path(dentry, &pathlen, &pathbase);
	if (IS_ERR(path))
		return PTR_ERR(path);
	req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_MKNOD,
				       pathbase, path, 0, 0,
				       dentry, USE_AUTH_MDS);
	kfree(path);
	if (IS_ERR(req)) {
		dout(40, "d_drop %p\n", dentry);
		d_drop(dentry);
		return PTR_ERR(req);
	}
	ceph_mdsc_lease_release(mdsc, dir, 0, CEPH_LOCK_ICONTENT);
	rhead = req->r_request->front.iov_base;
	rhead->args.mknod.mode = cpu_to_le32(mode);
	rhead->args.mknod.rdev = cpu_to_le32(rdev);
	err = ceph_mdsc_do_request(mdsc, req);
	if (!err && req->r_reply_info.trace_numd == 0) {
		/* no trace.  do lookup, in case we are called from create. */
		struct dentry *d;
		d = ceph_do_lookup(dir->i_sb, dentry, CEPH_STAT_MASK_INODE_ALL,
				   0);
		if (d) {
			/* ick.  this is untested... */
			dput(d);
			err = -ESTALE;
			dentry = 0;
		}
	}
	ceph_mdsc_put_request(req);
	if (err) {
		dout(40, "d_drop %p\n", dentry);
		d_drop(dentry);
	}
	return err;
}

static int ceph_create(struct inode *dir, struct dentry *dentry, int mode,
			   struct nameidata *nd)
{
	dout(5, "create in dir %p dentry %p name '%.*s'\n",
	     dir, dentry, dentry->d_name.len, dentry->d_name.name);
	if (nd) {
		BUG_ON((nd->flags & LOOKUP_OPEN) == 0);
		dentry = ceph_lookup_open(dir, dentry, nd, mode);
		/* hrm, what should i do here if we get aliased? */
		if (IS_ERR(dentry))
			return PTR_ERR(dentry);
		return 0;
	}

	/* fall back to mknod */
	return ceph_mknod(dir, dentry, (mode & ~S_IFMT) | S_IFREG, 0);
}

static int ceph_symlink(struct inode *dir, struct dentry *dentry,
			    const char *dest)
{
	struct ceph_client *client = ceph_sb_to_client(dir->i_sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct ceph_mds_request *req;
	char *path;
	int pathlen;
	u64 pathbase;
	int err;

	dout(5, "dir_symlink in dir %p dentry %p to '%s'\n", dir, dentry, dest);
	path = ceph_build_dentry_path(dentry, &pathlen, &pathbase);
	if (IS_ERR(path))
		return PTR_ERR(path);
	req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_SYMLINK,
				       pathbase, path, 0, dest,
				       dentry, USE_AUTH_MDS);
	kfree(path);
	if (IS_ERR(req)) {
		dout(40, "d_drop %p\n", dentry);
		d_drop(dentry);
		return PTR_ERR(req);
	}
	ceph_mdsc_lease_release(mdsc, dir, 0, CEPH_LOCK_ICONTENT);
	err = ceph_mdsc_do_request(mdsc, req);
	ceph_mdsc_put_request(req);
	if (err) {
		dout(40, "d_drop %p\n", dentry);
		d_drop(dentry);
	}
	return err;
}

static int ceph_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	struct ceph_client *client = ceph_sb_to_client(dir->i_sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct ceph_mds_request *req;
	struct ceph_mds_request_head *rhead;
	char *path;
	int pathlen;
	u64 pathbase;
	int err;

	dout(5, "dir_mkdir in dir %p dentry %p mode 0%o\n", dir, dentry, mode);
	path = ceph_build_dentry_path(dentry, &pathlen, &pathbase);
	if (IS_ERR(path))
		return PTR_ERR(path);
	req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_MKDIR,
				       pathbase, path, 0, 0,
				       dentry, USE_AUTH_MDS);
	kfree(path);
	if (IS_ERR(req)) {
		dout(40, "d_drop %p\n", dentry);
		d_drop(dentry);
		return PTR_ERR(req);
	}
	ceph_mdsc_lease_release(mdsc, dir, 0, CEPH_LOCK_ICONTENT);
	rhead = req->r_request->front.iov_base;
	rhead->args.mkdir.mode = cpu_to_le32(mode);
	err = ceph_mdsc_do_request(mdsc, req);
	ceph_mdsc_put_request(req);
	if (err < 0) {
		dout(40, "d_drop %p\n", dentry);
		d_drop(dentry);
	}
	return err;
}

static int ceph_link(struct dentry *old_dentry, struct inode *dir,
			 struct dentry *dentry)
{
	struct ceph_client *client = ceph_sb_to_client(dir->i_sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct ceph_mds_request *req;
	char *oldpath, *path;
	int oldpathlen, pathlen;
	u64 oldpathbase, pathbase;
	int err;

	dout(5, "dir_link in dir %p old_dentry %p dentry %p\n", dir,
	     old_dentry, dentry);
	oldpath = ceph_build_dentry_path(old_dentry, &oldpathlen, &oldpathbase);
	if (IS_ERR(oldpath))
		return PTR_ERR(oldpath);
	path = ceph_build_dentry_path(dentry, &pathlen, &pathbase);
	if (IS_ERR(path)) {
		kfree(oldpath);
		return PTR_ERR(path);
	}
	req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_LINK,
				       pathbase, path,
				       oldpathbase, oldpath,
				       dentry, USE_AUTH_MDS);
	kfree(oldpath);
	kfree(path);
	if (IS_ERR(req)) {
		dout(40, "d_drop %p\n", dentry);
		d_drop(dentry);
		return PTR_ERR(req);
	}

	dget(dentry);                /* to match put_request below */
	req->r_last_dentry = dentry; /* use this dentry in fill_trace */

	ceph_mdsc_lease_release(mdsc, dir, 0, CEPH_LOCK_ICONTENT);
	err = ceph_mdsc_do_request(mdsc, req);
	ceph_mdsc_put_request(req);
	if (err) {
		dout(40, "d_drop %p\n", dentry);
		d_drop(dentry);
	} else if (req->r_reply_info.trace_numd == 0) {
		/* no trace */
		struct inode *inode = old_dentry->d_inode;
		inc_nlink(inode);
		atomic_inc(&inode->i_count);
		dget(dentry);
		d_instantiate(dentry, inode);
	}
	return err;
}

static int ceph_unlink(struct inode *dir, struct dentry *dentry)
{
	struct ceph_client *client = ceph_sb_to_client(dir->i_sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct inode *inode = dentry->d_inode;
	struct ceph_mds_request *req;
	char *path;
	int pathlen;
	u64 pathbase;
	int err;
	int op = ((dentry->d_inode->i_mode & S_IFMT) == S_IFDIR) ?
		CEPH_MDS_OP_RMDIR : CEPH_MDS_OP_UNLINK;

	dout(5, "dir_unlink/rmdir in dir %p dentry %p inode %p\n",
	     dir, dentry, inode);
	path = ceph_build_dentry_path(dentry, &pathlen, &pathbase);
	if (IS_ERR(path))
		return PTR_ERR(path);
	req = ceph_mdsc_create_request(mdsc, op,
				       pathbase, path, 0, 0,
				       dentry, USE_AUTH_MDS);
	kfree(path);
	if (IS_ERR(req))
		return PTR_ERR(req);
	ceph_mdsc_lease_release(mdsc, dir, dentry,
				CEPH_LOCK_DN|CEPH_LOCK_ICONTENT);
	ceph_mdsc_lease_release(mdsc, inode, 0, CEPH_LOCK_ILINK);
	err = ceph_mdsc_do_request(mdsc, req);
	ceph_mdsc_put_request(req);

	if (err == -ENOENT)
		dout(10, "HMMM!\n");
	else if (req->r_reply_info.trace_numd == 0) {
		/* no trace */
		drop_nlink(dentry->d_inode);
		dput(dentry);
	}

	return err;
}

static int ceph_rename(struct inode *old_dir, struct dentry *old_dentry,
			   struct inode *new_dir, struct dentry *new_dentry)
{
	struct ceph_client *client = ceph_sb_to_client(old_dir->i_sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct ceph_mds_request *req;
	char *oldpath, *newpath;
	int oldpathlen, newpathlen;
	u64 oldpathbase, newpathbase;
	int err;

	dout(5, "dir_rename in dir %p dentry %p to dir %p dentry %p\n",
	     old_dir, old_dentry, new_dir, new_dentry);
	oldpath = ceph_build_dentry_path(old_dentry, &oldpathlen, &oldpathbase);
	if (IS_ERR(oldpath))
		return PTR_ERR(oldpath);
	newpath = ceph_build_dentry_path(new_dentry, &newpathlen, &newpathbase);
	if (IS_ERR(newpath)) {
		kfree(oldpath);
		return PTR_ERR(newpath);
	}
	req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_RENAME,
				       oldpathbase, oldpath,
				       newpathbase, newpath,
				       new_dentry, USE_AUTH_MDS);
	kfree(oldpath);
	kfree(newpath);
	if (IS_ERR(req))
		return PTR_ERR(req);
	dget(old_dentry);
	req->r_old_dentry = old_dentry;
	dget(new_dentry);
	req->r_last_dentry = new_dentry;
	ceph_mdsc_lease_release(mdsc, old_dir, old_dentry,
				CEPH_LOCK_DN|CEPH_LOCK_ICONTENT);
	if (new_dentry->d_inode)
		ceph_mdsc_lease_release(mdsc, new_dentry->d_inode, 0,
					CEPH_LOCK_ILINK);
	err = ceph_mdsc_do_request(mdsc, req);
	if (!err && req->r_reply_info.trace_numd == 0) {
		/* no trace */
		if (new_dentry->d_inode)
			dput(new_dentry);
		d_move(old_dentry, new_dentry);
	}
	ceph_mdsc_put_request(req);
	return err;
}



/*
 * check if dentry lease, or parent directory inode lease or cap says
 * this dentry is still valid
 */
static int ceph_dentry_revalidate(struct dentry *dentry, struct nameidata *nd)
{
	struct inode *dir = dentry->d_parent->d_inode;

	dout(10, "d_revalidate %p '%.*s' inode %p\n", dentry,
	     dentry->d_name.len, dentry->d_name.name, dentry->d_inode);

	if (ceph_inode(dir)->i_version == dentry->d_time &&
	    ceph_inode_lease_valid(dir, CEPH_LOCK_ICONTENT)) {
		dout(20, "dentry_revalidate %p have ICONTENT on dir inode %p\n",
		     dentry, dir);
		return 1;
	}
	if (ceph_dentry_lease_valid(dentry)) {
		dout(20, "dentry_revalidate %p lease valid\n", dentry);
		return 1;
	}

	dout(20, "dentry_revalidate %p no lease\n", dentry);
	dout(40, "d_drop %p\n", dentry);
	d_drop(dentry);
	return 0;
}

static void ceph_dentry_release(struct dentry *dentry)
{
	BUG_ON(dentry->d_fsdata);
}

/*
 * reading from a dir
 */
static ssize_t ceph_read_dir(struct file *file, char __user *buf, size_t size,
			     loff_t *ppos)
{
	struct ceph_file_info *cf = file->private_data;
	struct inode *inode = file->f_dentry->d_inode;
	struct ceph_inode_info *ci = ceph_inode(inode);
	int left;

	if (!(ceph_client(inode->i_sb)->mount_args.flags & CEPH_MOUNT_DIRSTAT))
		return -EISDIR;

	if (!cf->dir_info) {
		cf->dir_info = kmalloc(1024, GFP_NOFS);
		if (!cf->dir_info)
			return -ENOMEM;
		cf->dir_info_len = 
			sprintf(cf->dir_info, 
				"entries:   %20lld\n"
				" files:    %20lld\n"
				" subdirs:  %20lld\n"
				"rentries:  %20lld\n"
				" rfiles:   %20lld\n"
				" rsubdirs: %20lld\n"
				"rbytes:    %20lld\n"
				"rctime:    %10ld.%09ld\n",
				ci->i_files + ci->i_subdirs,
				ci->i_files,
				ci->i_subdirs,
				ci->i_rfiles + ci->i_rsubdirs,
				ci->i_rfiles,
				ci->i_rsubdirs,
				ci->i_rbytes,
				(long)ci->i_rctime.tv_sec,
				(long)ci->i_rctime.tv_nsec);
	}

	if (*ppos >= cf->dir_info_len)
		return 0;
	size = min_t(unsigned, size, cf->dir_info_len-*ppos);
	left = copy_to_user(buf, cf->dir_info + *ppos, size);
	if (left == size)
		return -EFAULT;
	*ppos += (size - left);
	return (size - left);
}

const struct file_operations ceph_dir_fops = {
	.read = ceph_read_dir,
	.readdir = ceph_readdir,
	.llseek = ceph_dir_llseek,
	.open = ceph_open,
	.release = ceph_release,
};

const struct inode_operations ceph_dir_iops = {
	.lookup = ceph_lookup,
	.getattr = ceph_getattr,
	.setattr = ceph_setattr,
	.setxattr = ceph_setxattr,
	.getxattr = ceph_getxattr,
	.listxattr = ceph_listxattr,
	.removexattr = ceph_removexattr,
	.mknod = ceph_mknod,
	.symlink = ceph_symlink,
	.mkdir = ceph_mkdir,
	.link = ceph_link,
	.unlink = ceph_unlink,
	.rmdir = ceph_unlink,
	.rename = ceph_rename,
	.create = ceph_create,
};

struct dentry_operations ceph_dentry_ops = {
	.d_revalidate = ceph_dentry_revalidate,
	.d_release = ceph_dentry_release,
};

