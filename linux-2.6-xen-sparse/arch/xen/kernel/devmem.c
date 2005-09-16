/*
 *  Originally from linux/drivers/char/mem.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Added devfs support. 
 *    Jan-11-1998, C. Scott Ananian <cananian@alumni.princeton.edu>
 *  Shared /dev/zero mmaping support, Feb 2000, Kanoj Sarcar <kanoj@sgi.com>
 */

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mman.h>
#include <linux/random.h>
#include <linux/init.h>
#include <linux/raw.h>
#include <linux/tty.h>
#include <linux/capability.h>
#include <linux/smp_lock.h>
#include <linux/devfs_fs_kernel.h>
#include <linux/ptrace.h>
#include <linux/device.h>
#include <asm/pgalloc.h>
#include <asm/uaccess.h>
#include <asm/io.h>

static inline int uncached_access(struct file *file)
{
        if (file->f_flags & O_SYNC)
                return 1;
        /* Xen sets correct MTRR type on non-RAM for us. */
        return 0;
}

/*
 * This funcion reads the *physical* memory. The f_pos points directly to the 
 * memory location. 
 */
static ssize_t read_mem(struct file * file, char __user * buf,
			size_t count, loff_t *ppos)
{
	unsigned long i, p = *ppos;
	ssize_t read = -EFAULT;
	void *v;

	if ((v = ioremap(p, count)) == NULL) {
		/*
		 * Some programs (e.g., dmidecode) groove off into weird RAM
		 * areas where no table scan possibly exist (because Xen will
		 * have stomped on them!). These programs get rather upset if
                 * we let them know that Xen failed their access, so we fake
                 * out a read of all zeroes. :-)
		 */
		for (i = 0; i < count; i++)
			if (put_user(0, buf+i))
				return -EFAULT;
		return count;
	}
	if (copy_to_user(buf, v, count))
		goto out;

	read = count;
	*ppos += read;
out:
	iounmap(v);
	return read;
}

static ssize_t write_mem(struct file * file, const char __user * buf, 
			 size_t count, loff_t *ppos)
{
	unsigned long p = *ppos;
	ssize_t written = -EFAULT;
	void *v;

	if ((v = ioremap(p, count)) == NULL)
		return -EFAULT;
	if (copy_to_user(v, buf, count))
		goto out;

	written = count;
	*ppos += written;
out:
	iounmap(v);
	return written;
}

static int mmap_mem(struct file * file, struct vm_area_struct * vma)
{
	if (uncached_access(file))
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	if (direct_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
				   vma->vm_end - vma->vm_start,
				   vma->vm_page_prot, DOMID_IO))
		return -EAGAIN;

	return 0;
}

/*
 * The memory devices use the full 32/64 bits of the offset, and so we cannot
 * check against negative addresses: they are ok. The return value is weird,
 * though, in that case (0).
 *
 * also note that seeking relative to the "end of file" isn't supported:
 * it has no meaning, so it returns -EINVAL.
 */
static loff_t memory_lseek(struct file * file, loff_t offset, int orig)
{
	loff_t ret;

	down(&file->f_dentry->d_inode->i_sem);
	switch (orig) {
		case 0:
			file->f_pos = offset;
			ret = file->f_pos;
			force_successful_syscall_return();
			break;
		case 1:
			file->f_pos += offset;
			ret = file->f_pos;
			force_successful_syscall_return();
			break;
		default:
			ret = -EINVAL;
	}
	up(&file->f_dentry->d_inode->i_sem);
	return ret;
}

static int open_mem(struct inode * inode, struct file * filp)
{
	return capable(CAP_SYS_RAWIO) ? 0 : -EPERM;
}

struct file_operations mem_fops = {
	.llseek		= memory_lseek,
	.read		= read_mem,
	.write		= write_mem,
	.mmap		= mmap_mem,
	.open		= open_mem,
};
