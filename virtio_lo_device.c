// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022  Panasonic Automotive Systems, Co., Ltd.
 */

#include <linux/version.h>
#include <linux/atomic.h>
#include <linux/eventfd.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/dma-mapping.h>

#include <uapi/linux/virtio_config.h>

#include "virtio_lo_device.h"
#include "virtio_lo_driver.h"
#include "virtio_lo.h"

/*
 * eventfd_signal() lost its count argument in 6.8, and the change was
 * backported to the 6.6 stable series in 6.6.160.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0) ||                    \
	(LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 160) &&             \
	 LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0))
#define virtio_lo_eventfd_signal(ctx) eventfd_signal(ctx)
#else
#define virtio_lo_eventfd_signal(ctx) eventfd_signal(ctx, 1)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
static inline void vm_flags_set(struct vm_area_struct *vma, vm_flags_t flags)
{
	vma->vm_flags |= flags;
}
#endif

#define VIRTIO_LO_DEAD_PAGES_MAX 1024

struct virtio_lo_dead_range {
	struct list_head list;
	unsigned long start_pfn;
	unsigned long end_pfn;
};

static void virtio_lo_add_pdev(struct work_struct *work);

static void vl_device_parent_release(struct device *dev)
{
}

static struct device vl_device_parent = {
	.init_name = "virtio-lo-bus",
	.release = vl_device_parent_release,
};

static atomic_t vilo_device_id;
static struct workqueue_struct *vilo_wq;

/* Structure allocated on each open call to handle all virtual devices
 * provided by userspace program */
struct virtio_lo_owner {
	atomic_t lastidx;
	spinlock_t lock;
	struct list_head devlist;
	struct address_space *mapping;
	struct rw_semaphore revoke_lock;
	struct list_head dead_ranges;
	spinlock_t dead_pages_lock;
	struct list_head dead_pages;
	unsigned int nr_dead_pages;
	unsigned int live_devs;
	bool mappings_disabled;
};

/* should be called with owner->lock held */
static struct virtio_lo_device *
virtio_owner_getdev_unlocked(struct virtio_lo_owner *owner, unsigned idx)
{
	struct virtio_lo_device *ret = NULL, *dev;
	list_for_each_entry (dev, &owner->devlist, devlist) {
		if (dev->idx == idx) {
			ret = dev;
			break;
		}
	}
	return ret;
}
static inline struct virtio_lo_device *
virtio_owner_getdev(struct virtio_lo_owner *owner, unsigned idx)
{
	unsigned long flags;
	struct virtio_lo_device *ret;

	spin_lock_irqsave(&owner->lock, flags);
	ret = virtio_owner_getdev_unlocked(owner, idx);
	spin_unlock_irqrestore(&owner->lock, flags);
	return ret;
}

static bool virtio_lo_owner_pfn_live(struct virtio_lo_owner *owner,
				     unsigned long pfn)
{
	struct virtio_lo_dead_range *range;

	if (owner->mappings_disabled || owner->live_devs == 0)
		return false;

	list_for_each_entry(range, &owner->dead_ranges, list) {
		if (pfn >= range->start_pfn && pfn < range->end_pfn)
			return false;
	}
	return true;
}

static vm_fault_t virtio_lo_dead_fault(struct vm_fault *vmf)
{
	struct virtio_lo_owner *owner = vmf->vma->vm_file->private_data;
	struct page *page = alloc_page(GFP_KERNEL | __GFP_ZERO);

	if (!page)
		return VM_FAULT_OOM;

	spin_lock(&owner->dead_pages_lock);
	if (owner->nr_dead_pages >= VIRTIO_LO_DEAD_PAGES_MAX) {
		spin_unlock(&owner->dead_pages_lock);
		__free_page(page);
		return VM_FAULT_SIGBUS;
	}
	owner->nr_dead_pages++;
	list_add(&page->lru, &owner->dead_pages);
	spin_unlock(&owner->dead_pages_lock);

	return vmf_insert_pfn(vmf->vma, vmf->address, page_to_pfn(page));
}

static vm_fault_t virtio_lo_misc_legacy_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct virtio_lo_owner *owner = vma->vm_file->private_data;
	unsigned long addr;
	vm_fault_t ret;

	down_read(&owner->revoke_lock);
	if (!virtio_lo_owner_pfn_live(owner, vmf->pgoff)) {
		up_read(&owner->revoke_lock);
		return virtio_lo_dead_fault(vmf);
	}
	ret = vmf_insert_pfn(vma, vmf->address, vmf->pgoff);

	for (addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
		unsigned long pfn = vma->vm_pgoff +
				    ((addr - vma->vm_start) >> PAGE_SHIFT);

		if (addr != vmf->address &&
		    virtio_lo_owner_pfn_live(owner, pfn))
			vmf_insert_pfn(vma, addr, pfn);
	}
	up_read(&owner->revoke_lock);
	return ret;
}

static const struct vm_operations_struct virtio_lo_misc_legacy_vm_ops = {
	.fault = virtio_lo_misc_legacy_fault,
};

static int virtio_lo_revoke_range(struct virtio_lo_owner *owner,
				  u64 start, u64 end)
{
	struct virtio_lo_dead_range *range;

	if (start >= end)
		return 0;

	range = kmalloc(sizeof(*range), GFP_KERNEL);
	if (!range) {
		dev_err(&vl_device_parent,
			"cannot record revoked PFN range 0x%llx-0x%llx\n",
			start, end);
		return -ENOMEM;
	}
	range->start_pfn = PFN_DOWN(start);
	range->end_pfn = PFN_UP(end);
	list_add(&range->list, &owner->dead_ranges);
	return 1;
}

void virtio_lo_device_revoke(struct virtio_lo_device *dev)
{
	struct virtio_lo_owner *owner = dev->owner;
	struct virtio_lo_dead_range *range;
	int added = 0;
	bool allocation_failed = false;
	unsigned int i;

	if (!owner)
		return;

	down_write(&owner->revoke_lock);
	if (dev->revoked) {
		up_write(&owner->revoke_lock);
		return;
	}
	dev->revoked = true;
	if (dev->live) {
		dev->live = false;
		owner->live_devs--;
	}

	if (owner->mappings_disabled) {
		up_write(&owner->revoke_lock);
		return;
	}

	for (i = 0; dev->queues && i < dev->nqueues; i++) {
		struct virtio_lo_vq_info *q = &dev->queues[i];
		u64 start, end;
		int ret;

		if (!q->size)
			continue;
		start = min3(q->desc, q->avail, q->used);
		end = max3(q->desc + 16ull * q->size,
			   q->avail + 2ull * q->size + 6,
			   q->used + 8ull * q->size + 6);
		ret = virtio_lo_revoke_range(owner, start, end);
		if (ret < 0) {
			allocation_failed = true;
			break;
		}
		added += ret;
	}

	if (allocation_failed) {
		owner->mappings_disabled = true;
		downgrade_write(&owner->revoke_lock);
		unmap_mapping_range(owner->mapping, 0, 0, 1);
		up_read(&owner->revoke_lock);
		return;
	}

	if (added > 0) {
		int remaining = added;

		downgrade_write(&owner->revoke_lock);
		list_for_each_entry(range, &owner->dead_ranges, list) {
			if (remaining-- == 0)
				break;
			unmap_mapping_range(
				owner->mapping,
				(loff_t)range->start_pfn << PAGE_SHIFT,
				(loff_t)(range->end_pfn - range->start_pfn)
					<< PAGE_SHIFT,
				1);
		}
		up_read(&owner->revoke_lock);
	} else {
		up_write(&owner->revoke_lock);
	}
}

static int virtio_lo_misc_device_open(struct inode *inode, struct file *file)
{
	struct virtio_lo_owner *owner;

	owner = kzalloc(sizeof(*owner), GFP_KERNEL);
	if (!owner)
		return -ENOMEM;
	atomic_set(&owner->lastidx, 0);
	spin_lock_init(&owner->lock);
	INIT_LIST_HEAD(&owner->devlist);
	owner->mapping = file->f_mapping;
	init_rwsem(&owner->revoke_lock);
	INIT_LIST_HEAD(&owner->dead_ranges);
	spin_lock_init(&owner->dead_pages_lock);
	INIT_LIST_HEAD(&owner->dead_pages);
	file->private_data = owner;
	return 0;
}

static void virtio_lo_device_release(struct virtio_lo_device *dev)
{
	unsigned long i;

	dev->status = 0;
	dev->device_features = 0;

	platform_device_unregister(dev->pdev);
	virtio_lo_device_revoke(dev);

	kfree(dev->config);
	dev->config = NULL;

	if (dev->config_kick) {
		eventfd_ctx_put(dev->config_kick);
	}

	for (i = 0; i < dev->nqueues; i++) {
		if (dev->queues[i].device_kick) {
			eventfd_ctx_put(dev->queues[i].device_kick);
		}
	}
	kfree(dev->queues);
	kfree(dev);
	dev_notice(&vl_device_parent, "device released\n");
}

static int virtio_lo_misc_device_release(struct inode *inode, struct file *file)
{
	if (file->private_data) {
		struct virtio_lo_owner *owner = file->private_data;
		unsigned long flags;
		spin_lock_irqsave(&owner->lock, flags);
		while (!list_empty(&owner->devlist)) {
			struct virtio_lo_device *dev =
				list_first_entry(&owner->devlist,
						 struct virtio_lo_device,
						 devlist);
			list_del(&dev->devlist);
			spin_unlock_irqrestore(&owner->lock, flags);
			virtio_lo_device_release(dev);
			spin_lock_irqsave(&owner->lock, flags);
		}
		spin_unlock_irqrestore(&owner->lock, flags);
		while (!list_empty(&owner->dead_ranges)) {
			struct virtio_lo_dead_range *range =
				list_first_entry(&owner->dead_ranges,
						 struct virtio_lo_dead_range,
						 list);

			list_del(&range->list);
			kfree(range);
		}
		while (!list_empty(&owner->dead_pages)) {
			struct page *page = list_first_entry(
				&owner->dead_pages, struct page, lru);

			list_del(&page->lru);
			__free_page(page);
		}
		kfree(owner);
	}
	dev_notice(&vl_device_parent, "misc device released\n");
	return 0;
}

static int virtio_lo_misc_device_mmap(struct file *file,
				      struct vm_area_struct *vma)
{
	struct virtio_lo_owner *owner = file->private_data;
	bool live;

	if ((vma->vm_flags & (VM_SHARED | VM_MAYWRITE)) == VM_MAYWRITE)
		return -EINVAL;

	down_read(&owner->revoke_lock);
	live = owner->live_devs > 0 && !owner->mappings_disabled;
	up_read(&owner->revoke_lock);
	if (!live)
		return -ENODEV;

	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_ops = &virtio_lo_misc_legacy_vm_ops;
	return 0;
}

static void virtio_lo_add_pdev(struct work_struct *work)
{
	struct virtio_lo_device *dev =
		container_of(work, struct virtio_lo_device, init_work);
	unsigned id = atomic_fetch_add(1, &vilo_device_id);
	char dev_name[32];
	snprintf(dev_name, sizeof(dev_name), "virtio-lo-%04x%04x", dev->device_id, dev->vendor_id);
	if (!is_name_in_virtio_lo_table(dev_name))
		dev_warn(&vl_device_parent, "Device name '%s' is not supported. Please ensure it is added to virtio_lo_id_table", dev_name);

	dev->pdev = platform_device_register_data(&vl_device_parent, dev_name, id, &dev, sizeof(dev));

	if (dma_set_mask(&dev->pdev->dev, DMA_BIT_MASK(64))) {
		dev_notice(&dev->pdev->dev, "Failed to set DMA mask to 64-bits, DMA mask: 0x%llx\n", *dev->pdev->dev.dma_mask);
	} else {
		dev_notice(&dev->pdev->dev, "Success to set DMA mask to 64-bits, DMA mask: 0x%llx\n", *dev->pdev->dev.dma_mask);
	}

	complete_all(&dev->init_done);
}

static long vilo_ioctl_adddev(struct virtio_lo_owner *owner,
			      struct virtio_lo_devinfo __user *info)
{
	struct virtio_lo_devinfo di;
	struct virtio_lo_device *dev;
	struct virtio_lo_qinfo *qi;
	bool mappings_disabled;
	unsigned i;
	long ret = 0;
	unsigned long flags;

	down_read(&owner->revoke_lock);
	mappings_disabled = owner->mappings_disabled;
	up_read(&owner->revoke_lock);
	if (mappings_disabled) {
		dev_err(&vl_device_parent,
			"cannot add device after PFN mappings were disabled\n");
		return -EIO;
	}

	if (copy_from_user(&di, info, sizeof(di))) {
		return -EFAULT;
	}

	dev = kcalloc(1, sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		return -ENOMEM;
	}
	dev->owner = owner;

	spin_lock_init(&dev->config_lock);
	spin_lock_init(&dev->status_lock);

	dev->device_id = di.device_id;
	dev->vendor_id = di.vendor_id;
	dev->nqueues = di.nqueues;
	dev->features = dev->device_features = di.features;

	dev->config_size = di.config_size;
	dev->config = kmalloc(dev->config_size, GFP_KERNEL);
	if (!dev->config) {
		ret = -ENOMEM;
		goto err_dev;
	}
	dev->config_kick = eventfd_ctx_fdget(di.config_kick);

	if (copy_from_user(dev->config, di.config, di.config_size)) {
		ret = -EFAULT;
		goto err_conf;
	}

	qi = kcalloc(dev->nqueues, sizeof(*qi), GFP_KERNEL);
	if (!qi) {
		ret = -ENOMEM;
		goto err_conf;
	}
	if (copy_from_user(qi, di.qinfo, dev->nqueues * sizeof(*qi))) {
		ret = -EFAULT;
		goto err_qi;
	}
	dev->queues = kcalloc(dev->nqueues, sizeof(*dev->queues), GFP_KERNEL);
	if (!dev->queues) {
		ret = -ENOMEM;
		goto err_qi;
	}

	for (i = 0; i < dev->nqueues; i++) {
		dev->queues[i].maxsize = qi[i].size;
		if (qi[i].kickfd != -1) {
			dev->queues[i].device_kick =
				eventfd_ctx_fdget(qi[i].kickfd);
		}
	}

	dev->idx = atomic_fetch_add(1, &owner->lastidx);

	/* everything is OK, create driver part platform device */
	init_completion(&dev->init_done);

	INIT_WORK(&dev->init_work, virtio_lo_add_pdev);

	queue_work(vilo_wq, &dev->init_work);

	wait_for_completion(&dev->init_done);

	if (!(dev->status & VIRTIO_CONFIG_S_DRIVER_OK)) {
		dev_err(&vl_device_parent,
			   "virtio lo device initialization failed\n");
		ret = -ENOENT;
		goto err_queues;
	}

	for (i = 0; i < dev->nqueues; i++) {
		qi[i].size = dev->queues[i].size;
		qi[i].desc = dev->queues[i].desc;
		qi[i].avail = dev->queues[i].avail;
		qi[i].used = dev->queues[i].used;
	}
	if (copy_to_user(di.qinfo, qi, dev->nqueues * sizeof(*qi))) {
		ret = -EFAULT;
	}
	if (copy_to_user(&info->idx, &dev->idx, sizeof(dev->idx))) {
		ret = -EFAULT;
	}
	if (copy_to_user(&info->features, &dev->features,
			 sizeof(dev->features))) {
		ret = -EFAULT;
	}
	if (copy_to_user(di.config, dev->config, dev->config_size)) {
		ret = -EFAULT;
	}
	spin_lock_irqsave(&owner->lock, flags);
	list_add(&dev->devlist, &owner->devlist);
	spin_unlock_irqrestore(&owner->lock, flags);

	down_write(&owner->revoke_lock);
	dev->live = true;
	owner->live_devs++;
	up_write(&owner->revoke_lock);

	kfree(qi);
	return ret;
err_queues:
	kfree(dev->queues);
err_qi:
	kfree(qi);
err_conf:
	kfree(dev->config);
err_dev:
	kfree(dev);
	return ret;
}

static long vilo_ioctl_deldev(struct virtio_lo_owner *owner, unsigned idx)
{
	unsigned long flags;
	long ret = -ENOENT;
	struct virtio_lo_device *dev;

	spin_lock_irqsave(&owner->lock, flags);

	dev = virtio_owner_getdev_unlocked(owner, idx);
	if (dev)
		list_del(&dev->devlist);
	spin_unlock_irqrestore(&owner->lock, flags);

	/*
	 * Release outside the spinlock: virtio_lo_device_release() calls
	 * platform_device_unregister() (-> virtio_gpu_remove -> drm_dev_unplug
	 * -> synchronize_srcu()), which can sleep and must not run in atomic
	 * context. The release path below does the same for this reason.
	 */
	if (dev) {
		virtio_lo_device_release(dev);
		ret = 0;
	}
	return ret;
}

static long vilo_ioctl_getconf(struct virtio_lo_owner *owner,
			       struct virtio_lo_config __user *conf)
{
	struct virtio_lo_config c;
	struct virtio_lo_device *dev;
	void *mem;
	long ret = 0;
	if (copy_from_user(&c, conf, sizeof(c)))
		return -EFAULT;
	dev = virtio_owner_getdev(owner, c.idx);
	if (!dev) {
		return -ENOENT;
	}
	if (c.offset >= dev->config_size ||
	    c.offset + c.len > dev->config_size) {
		return -EINVAL;
	}
	mem = kmalloc(c.len, GFP_KERNEL);
	if (!mem) {
		return -ENOMEM;
	}
	virtio_lo_config_get(dev, c.offset, mem, c.len);
	if (copy_to_user(c.config, mem, c.len)) {
		ret = -EFAULT;
	}
	kfree(mem);

	return ret;
}

static long vilo_ioctl_setconf(struct virtio_lo_owner *owner,
			       const struct virtio_lo_config __user *conf)
{
	struct virtio_lo_config c;
	struct virtio_lo_device *dev;
	void *mem;
	long ret;
	if (copy_from_user(&c, conf, sizeof(c)))
		return -EFAULT;
	dev = virtio_owner_getdev(owner, c.idx);
	if (!dev) {
		return -ENOENT;
	}
	if (c.offset >= dev->config_size ||
	    c.offset + c.len > dev->config_size) {
		return -EINVAL;
	}

	mem = kmalloc(c.len, GFP_KERNEL);
	if (!mem) {
		return -ENOMEM;
	}
	if (copy_from_user(mem, c.config, c.len)) {
		ret = -EFAULT;
	} else {
		virtio_lo_config_set(dev, c.offset, mem, c.len);
		virtio_lo_config_driver(dev->pdev);
		ret = 0;
	}
	kfree(mem);
	return ret;
}

static long vilo_ioctl_kick(struct virtio_lo_owner *owner,
			    const struct virtio_lo_kick __user *kick)
{
	struct virtio_lo_kick k;
	struct virtio_lo_device *dev;
	if (copy_from_user(&k, kick, sizeof(k)))
		return -EFAULT;
	dev = virtio_owner_getdev(owner, k.idx);
	if (!dev) {
		return -ENOENT;
	}
	if (k.qidx >= dev->nqueues) {
		return -EINVAL;
	}
	virtio_lo_kick_driver(dev->pdev, k.qidx);

	return 0;
}

void virtio_lo_kick_device(struct virtio_lo_device *dev, int qidx)
{
	if (qidx >= 0 && qidx < dev->nqueues) {
		if (dev->queues[qidx].device_kick) {
			virtio_lo_eventfd_signal(dev->queues[qidx].device_kick);
		}
	} else {
		unsigned i;
		for (i = 0; i < dev->nqueues; i++) {
			if (dev->queues[i].device_kick) {
				virtio_lo_eventfd_signal(dev->queues[i].device_kick);
			}
		}
	}
}

void virtio_lo_set_queue(struct virtio_lo_device *dev, unsigned qidx, u32 size,
			 u64 desc, u64 avail, u64 used)
{
	struct virtio_lo_vq_info *info = &dev->queues[qidx];
	dev_notice(&vl_device_parent,
		   "setting queue addr %u size %u\n"
		   "\tdesc  %016llx\n"
		   "\tavail %016llx\n"
		   "\tused  %016llx\n",
		   qidx, size, desc, avail, used);
	info->size = size;
	info->desc = desc;
	info->avail = avail;
	info->used = used;
}

void virtio_lo_config_device(struct virtio_lo_device *dev)
{
	if (dev->config_kick) {
		virtio_lo_eventfd_signal(dev->config_kick);
	}
}

void virtio_lo_config_get(struct virtio_lo_device *dev, unsigned offset,
			  void *buf, unsigned len)
{
	unsigned long flags;
	spin_lock_irqsave(&dev->config_lock, flags);
	memcpy(buf, dev->config + offset, len);
	spin_unlock_irqrestore(&dev->config_lock, flags);
}

unsigned virtio_lo_config_generation(struct virtio_lo_device *dev)
{
	unsigned long flags;
	unsigned ret;
	spin_lock_irqsave(&dev->config_lock, flags);
	ret = dev->generation;
	spin_unlock_irqrestore(&dev->config_lock, flags);
	return ret;
}

void virtio_lo_config_set(struct virtio_lo_device *dev, unsigned offset,
			  const void *buf, unsigned len)
{
	unsigned long flags;
	spin_lock_irqsave(&dev->config_lock, flags);
	memcpy(dev->config + offset, buf, len);
	dev->generation++;
	spin_unlock_irqrestore(&dev->config_lock, flags);
}

static long virtio_lo_misc_device_ioctl(struct file *file, unsigned int cmd,
					unsigned long arg)
{
	long ret;
	struct virtio_lo_owner *owner =
		(struct virtio_lo_owner *)file->private_data;
	void __user *argp = (void __user *)arg;

	if (!owner) {
		return -ENOTTY;
	}

	if (_IOC_TYPE(cmd) != VIRTIO_LOIO) {
		return -ENOTTY;
	}

	switch (cmd) {
	case VIRTIO_LO_ADDDEV:
		ret = vilo_ioctl_adddev(owner, argp);
		break;
	case VIRTIO_LO_DELDEV:
		ret = vilo_ioctl_deldev(owner, arg);
		break;
	case VIRTIO_LO_GCONF:
		ret = vilo_ioctl_getconf(owner, argp);
		break;
	case VIRTIO_LO_SCONF:
		ret = vilo_ioctl_setconf(owner, argp);
		break;
	case VIRTIO_LO_KICK:
		ret = vilo_ioctl_kick(owner, argp);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static struct file_operations virtio_lo_misc_device_fops = {
	.owner = THIS_MODULE,
	.open = virtio_lo_misc_device_open,
	.unlocked_ioctl = virtio_lo_misc_device_ioctl,
	.mmap = virtio_lo_misc_device_mmap,
	.release = virtio_lo_misc_device_release
};

static struct miscdevice virtio_lo_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "virtio-lo",
	.fops = &virtio_lo_misc_device_fops
};

int __init virtio_lo_device_init(void)
{
	vilo_wq = create_singlethread_workqueue("virtio-lo");
	if (!vilo_wq) {
		return -ENOMEM;
	}
	if (device_register(&vl_device_parent)) {
		destroy_workqueue(vilo_wq);
	}

	return misc_register(&virtio_lo_misc_device);
}

void __exit virtio_lo_device_exit(void)
{
	flush_workqueue(vilo_wq);
	destroy_workqueue(vilo_wq);
	device_unregister(&vl_device_parent);
	misc_deregister(&virtio_lo_misc_device);
}
