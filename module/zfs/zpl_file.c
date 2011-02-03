/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2011, Lawrence Livermore National Security, LLC.
 */


#include <sys/zfs_vfsops.h>
#include <sys/zfs_vnops.h>
#include <sys/zfs_znode.h>
#include <sys/zpl.h>


static int
zpl_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct dentry *dentry = filp->f_path.dentry;
	cred_t *cr;
	int error;

	cr = (cred_t *)get_current_cred();
	error = -zfs_readdir(dentry->d_inode, dirent, filldir,
	    &filp->f_pos, cr);
	put_cred(cr);
	ASSERT3S(error, <=, 0);

	return (error);
}

static int
zpl_fsync(struct file *filp, struct dentry *dentry, int datasync)
{
	cred_t *cr;
	int error;

	cr = (cred_t *)get_current_cred();
	error = -zfs_fsync(filp->f_path.dentry->d_inode, datasync, cr);
	put_cred(cr);
	ASSERT3S(error, <=, 0);

	return (error);
}

ssize_t
zpl_read_common(struct inode *ip, const char *buf, size_t len, loff_t pos,
     uio_seg_t segment, int flags, cred_t *cr)
{
	int error;
	struct iovec iov;
	uio_t uio;

	iov.iov_base = (void *)buf;
	iov.iov_len = len;

	uio.uio_iov = &iov;
	uio.uio_resid = len;
	uio.uio_iovcnt = 1;
	uio.uio_loffset = pos;
	uio.uio_limit = MAXOFFSET_T;
	uio.uio_segflg = segment;

	error = -zfs_read(ip, &uio, flags, cr);
	if (error < 0)
		return (error);

	return (len - uio.uio_resid);
}

static ssize_t
zpl_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
	cred_t *cr;
	ssize_t read;

	cr = (cred_t *)get_current_cred();
	read = zpl_read_common(filp->f_mapping->host, buf, len, *ppos,
	    UIO_USERSPACE, filp->f_flags, cr);
	put_cred(cr);

	if (read < 0)
		return (read);

	*ppos += read;
	return (read);
}

ssize_t
zpl_write_common(struct inode *ip, const char *buf, size_t len, loff_t pos,
    uio_seg_t segment, int flags, cred_t *cr)
{
	int error;
	struct iovec iov;
	uio_t uio;

	iov.iov_base = (void *)buf;
	iov.iov_len = len;

	uio.uio_iov = &iov;
	uio.uio_resid = len,
	uio.uio_iovcnt = 1;
	uio.uio_loffset = pos;
	uio.uio_limit = MAXOFFSET_T;
	uio.uio_segflg = segment;

	error = -zfs_write(ip, &uio, flags, cr);
	if (error < 0)
		return (error);

	return (len - uio.uio_resid);
}

static ssize_t
zpl_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{
	cred_t *cr;
	ssize_t wrote;

	cr = (cred_t *)get_current_cred();
	wrote = zpl_write_common(filp->f_mapping->host, buf, len, *ppos,
	    UIO_USERSPACE, filp->f_flags, cr);
	put_cred(cr);

	if (wrote < 0)
		return (wrote);

	*ppos += wrote;
	return (wrote);
}

/*
 * It's worth taking a moment to describe how mmap is implemented
 * for zfs because it differs considerably from other Linux filesystems.
 * However, this issue is handled the same way under OpenSolaris.
 *
 * The issue is that by design zfs bypasses the Linux page cache and
 * leaves all caching up to the ARC.  This has been shown to work
 * well for the common read(2)/write(2) case.  However, mmap(2)
 * is problem because it relies on being tightly integrated with the
 * page cache.  To handle this we cache mmap'ed files twice, once in
 * the ARC and a second time in the page cache.  The code is careful
 * to keep both copies synchronized.
 *
 * When a file with an mmap'ed region is written to using write(2)
 * both the data in the ARC and existing pages in the page cache
 * are updated.  For a read(2) data will be read first from the page
 * cache then the ARC if needed.  Neither a write(2) or read(2) will
 * will ever result in new pages being added to the page cache.
 *
 * New pages are added to the page cache only via .readpage() which
 * is called when the vfs needs to read a page off disk to back the
 * virtual memory region.  These pages may be modified without
 * notifying the ARC and will be written out periodically via
 * .writepage().  This will occur due to either a sync or the usual
 * page aging behavior.  Note because a read(2) of a mmap'ed file
 * will always check the page cache first even when the ARC is out
 * of date correct data will still be returned.
 *
 * While this implementation ensures correct behavior it does have
 * have some drawbacks.  The most obvious of which is that it
 * increases the required memory footprint when access mmap'ed
 * files.  It also adds additional complexity to the code keeping
 * both caches synchronized.
 *
 * Longer term it may be possible to cleanly resolve this wart by
 * mapping page cache pages directly on to the ARC buffers.  The
 * Linux address space operations are flexible enough to allow
 * selection of which pages back a particular index.  The trick
 * would be working out the details of which subsystem is in
 * charge, the ARC, the page cache, or both.  It may also prove
 * helpful to move the ARC buffers to a scatter-gather lists
 * rather than a vmalloc'ed region.
 */
static int
zpl_mmap(struct file *filp, struct vm_area_struct *vma)
{
	znode_t *zp = ITOZ(filp->f_mapping->host);
	int error;

	error = generic_file_mmap(filp, vma);
	if (error)
		return (error);

	mutex_enter(&zp->z_lock);
	zp->z_is_mapped = 1;
	mutex_exit(&zp->z_lock);

	return (error);
}

/*
 * Populate a page with data for the Linux page cache.  This function is
 * only used to support mmap(2).  There will be an identical copy of the
 * data in the ARC which is kept up to date via .write() and .writepage().
 *
 * Current this function relies on zpl_read_common() and the O_DIRECT
 * flag to read in a page.  This works but the more correct way is to
 * update zfs_fillpage() to be Linux friendly and use that interface.
 */
static int
zpl_readpage(struct file *filp, struct page *pp)
{
	struct inode *ip;
	loff_t off, i_size;
	size_t len, wrote;
	cred_t *cr;
	void *pb;
	int error = 0;

	ASSERT(PageLocked(pp));
	ip = pp->mapping->host;
	off = page_offset(pp);
	i_size = i_size_read(ip);
	ASSERT3S(off, <, i_size);

	cr = (cred_t *)get_current_cred();
	len = MIN(PAGE_CACHE_SIZE, i_size - off);

	pb = kmap(pp);

	/* O_DIRECT is passed to bypass the page cache and avoid deadlock. */
	wrote = zpl_read_common(ip, pb, len, off, UIO_SYSSPACE, O_DIRECT, cr);
	if (wrote != len)
		error = -EIO;

	if (!error && (len < PAGE_CACHE_SIZE))
		memset(pb + len, 0, PAGE_CACHE_SIZE - len);

	kunmap(pp);
	put_cred(cr);

	if (error) {
		SetPageError(pp);
		ClearPageUptodate(pp);
	} else {
		ClearPageError(pp);
		SetPageUptodate(pp);
		flush_dcache_page(pp);
	}

	unlock_page(pp);

	return (error);
}

/*
 * Write out dirty pages to the ARC, this function is only required to
 * support mmap(2).  Mapped pages may be dirtied by memory operations
 * which never call .write().  These dirty pages are kept in sync with
 * the ARC buffers via this hook.
 *
 * Currently this function relies on zpl_write_common() and the O_DIRECT
 * flag to push out the page.  This works but the more correct way is
 * to update zfs_putapage() to be Linux friendly and use that interface.
 */
static int
zpl_writepage(struct page *pp, struct writeback_control *wbc)
{
	struct inode *ip;
	loff_t off, i_size;
	size_t len, read;
	cred_t *cr;
	void *pb;
	int error = 0;

	ASSERT(PageLocked(pp));
	ip = pp->mapping->host;
	off = page_offset(pp);
	i_size = i_size_read(ip);

	cr = (cred_t *)get_current_cred();
	len = MIN(PAGE_CACHE_SIZE, i_size - off);

	pb = kmap(pp);

	/* O_DIRECT is passed to bypass the page cache and avoid deadlock. */
	read = zpl_write_common(ip, pb, len, off, UIO_SYSSPACE, O_DIRECT, cr);
	if (read != len)
		error = -EIO;

	kunmap(pp);
	put_cred(cr);

	if (error) {
		SetPageError(pp);
		ClearPageUptodate(pp);
	} else {
		ClearPageError(pp);
		SetPageUptodate(pp);
	}

	unlock_page(pp);

	return (error);
}

const struct address_space_operations zpl_address_space_operations = {
	.readpage	= zpl_readpage,
	.writepage	= zpl_writepage,
};

const struct file_operations zpl_file_operations = {
	.open		= generic_file_open,
	.llseek		= generic_file_llseek,
	.read		= zpl_read,
	.write		= zpl_write,
	.readdir	= zpl_readdir,
	.mmap		= zpl_mmap,
	.fsync		= zpl_fsync,
};

const struct file_operations zpl_dir_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= generic_read_dir,
	.readdir	= zpl_readdir,
	.fsync		= zpl_fsync,
};
