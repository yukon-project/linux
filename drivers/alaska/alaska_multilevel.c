#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "alaska"
#define MEM_SIZE PAGE_SIZE

static dev_t dev_num;
static struct cdev my_cdev;
static struct class *my_class;

#define CSR_HTBASE 0xc2
#define CSR_HTDUMP 0xc3
#define CSR_HTINVAL 0xc4

#define write_csr(reg, val)                                                \
	({                                                                 \
		asm volatile("csrw %0, %1" ::"i"(reg), "rK"((uint64_t)val) \
			     : "memory");                                  \
	})

#define read_csr(csr, val)                 \
	__asm__ __volatile__("csrr %0, %1" \
			     : "=r"(val)   \
			     : "n"(csr)    \
			     : /* clobbers: none */);

/**
 * the handle table is a two-level table with each level being 2mb in total (or
 * 18 bits worth of 8 byte values). The application indexes into it using
 * "handle ids", which are 32 bits currently, and select a given 8 byte value in
 * the two-level handle table.
 *
 * the funky thing is that the userspace must be able to access the leaf nodes of
 * the handle table in a contiguous manner, so we need to manage the handle table
 * sparsely, but map it contiguously.
 *
 * We also have to worry about the fact that the hardware wants the *physical addresses*
 * of the handle table, and therefore we have to keep track of two different 'tables'.
 *
 * One table is the physical handle table (pht) which is the base address of the
 * handle table in physical memory. The other is the virtual handle table (vht)
 * which is the base address of the handle table in virtual memory that the kernel can touch.
 *
 * We name the handle table levels as follows:
 *   - ht0: The top level handle table. Each entry of this points to a 2mb page of ht1.
 *          CSR 0xc2 points to ht0.
 *   - ht1: The leaf level of the handle table. Each ht1 is a 2mb page, which is managed
 *          entirely by the userspace application in a virtually contiguous manner.
 *          The kernel hides the fact these are many pages.
 *
 * Because of how the hardware works, the handle table must not be sparse. That is,
 * if entry N is non-zero in the ht0, there can be no entry M in the ht0 where M < N
 * that is zero. This is because instead of adding checking in the hardware, we assume
 * the kernel will fill out the 0xc5 CSR with the number of total entries in the handle
 * table (ie: the maxiumum handle id that is permitted).
 *
 * The whole interface to manage a `struct alaska_handle_table` is simply to allocate/initialize
 * it (which sets 0xc2), and to "get" the address of a certain ht1. There's also a function to
 * deallocate one.
 */

// HT_SIZE: how many bytes in an ht1 or ht0
#define HT_SIZE_ORDER (21)
#define HT_ALLOC_ORDER (HT_SIZE_ORDER - 12)
#define HT_SIZE (1 << HT_SIZE_ORDER) // 2mb
// HT_ENTRIES: how many entries in an ht1 or ht0, which is 2mb / 8 bytes
#define HT_ENTRIES (HT_SIZE / sizeof(void *))

struct alaska_handle_table {
	int length; // how many ht1 entries are in the table.
	void **ht0; // This is a virtual poitner to the top level ht0
};

// Allocate a block of memory for an ht0 or ht1 (they are the same size)
static void *alaska_allocate_ht(void)
{
	struct page *newpage = NULL;
	void *p;
	newpage = alloc_pages(GFP_KERNEL, HT_ALLOC_ORDER);
	if (newpage == NULL) {
		pr_err("failed to allocate ht block\n");
		return NULL;
	}

	p = page_address(newpage);
	memset(p, 0, HT_SIZE);
	printk("allocate new ht at v=%lx, p=%lx\n", (unsigned long)p, __pa(p));
	return p;
}

static void *alaska_get_ht1(struct alaska_handle_table *ht, unsigned index)
{
	if (index > HT_ENTRIES) {
		printk("index %u is greater than max entries %u\n", index,
		       HT_ENTRIES);
		return NULL;
	}
	unsigned int i;

	if (index >= ht->length) {
		// allocate an entry for it.
		for (i = ht->length; i <= index; i++) {
			if (ht->ht0[i] == NULL) {
				ht->ht0[i] = __pa(alaska_allocate_ht());
			}
		}
		ht->length = index + 1;
		__asm__ volatile("csrw 0xc5, %0" ::"rK"(ht->length * HT_ENTRIES)
				 : "memory");
	}

	// for (i = 0; i < ht->length; i++) {
	//   printk("ht0[%2d]: %zx\n", i, ht->ht0[i]);
	// }

	return ht->ht0[index];
}

static void alaska_handle_table_init(struct alaska_handle_table *ht)
{
	ht->length = 0;
	ht->ht0 = alaska_allocate_ht();
	__asm__ volatile("csrw 0xc2, %0" ::"rK"(__pa(ht->ht0)) : "memory");
	__asm__ volatile("csrw 0xc5, %0" ::"rK"(ht->length * HT_ENTRIES)
			 : "memory");
	// Initialize 16 entries
	alaska_get_ht1(ht, 16);
}

// Currently, we only support a single handle table in the system.
// Later, we will figure out how to have multiple :)
static struct alaska_handle_table *the_ht = NULL;

static vm_fault_t alaska_vma_fault(struct vm_fault *vmf)
{
	void *ht1;
	unsigned long byte_off = vmf->pgoff * PAGE_SIZE;
	unsigned long index = byte_off / HT_SIZE;
	unsigned long bytes_into_ht1 = byte_off % HT_SIZE;
	unsigned long pfn = 0;
	unsigned long map_address = vmf->address & ~0xfff;
	struct page *page;

	// printk("fault address:        0x%lx\n", vmf->address);
	// printk("map address:          0x%lx\n", map_address);
	// printk("fault pgoff:          0x%lx\n", vmf->pgoff);
	// printk("fault index:          %lx\n", index);
	// printk("fault byte_off:       0x%lx\n", byte_off);
	// printk("fault bytes_into_ht1: %lx\n", bytes_into_ht1);
	// printk("vma start:            0x%lx\n", vmf->vma->vm_start);
	// printk("vma end:              0x%lx\n", vmf->vma->vm_end);

	if (the_ht == NULL) {
		the_ht =
			kmalloc(sizeof(struct alaska_handle_table), GFP_KERNEL);
		alaska_handle_table_init(the_ht);
	}

	if (index >= HT_ENTRIES) {
		printk("Invalid fault. Index is too high.\n");
		return VM_FAULT_SIGBUS;
	}

	ht1 = alaska_get_ht1(the_ht, index);

	page = pfn_to_page((unsigned long)(ht1 + bytes_into_ht1) >> PAGE_SHIFT);
	pfn = page_to_pfn(page);

	// printk("remap_pfn_range %16zx %16zx %16zx %x", map_address, pfn, 4096, vmf->vma->vm_page_prot);
	int err = remap_pfn_range(vmf->vma, map_address, pfn, 4096,
				  vmf->vma->vm_page_prot);
	if (err) {
		printk("remap_pfn_range failed\n");
		return err;
	}

	return VM_FAULT_NOPAGE;
}

static struct vm_operations_struct alaska_vm_ops = {
	.fault = alaska_vma_fault,
};

// this is the primary function which is called by the userspace application to
// access the handle table. It allows the application to map
static int alaska_handle_table_mmap(struct file *filp,
				    struct vm_area_struct *vma)
{
	printk("alaska mmap %zx - %zx\n", vma->vm_start, vma->vm_end);
	unsigned long region_size = vma->vm_end - vma->vm_start;
	unsigned long entries = region_size / HT_SIZE;
	unsigned long i;
	vma->vm_ops = &alaska_vm_ops;
	return 0;
}

static struct page *alaska_dump_page = NULL;

// This function is called when the runtime calls read() on our device file.
// The main goal of this is to
static ssize_t alaska_read(struct file *filp, char __user *buf, size_t len,
			   loff_t *offset)
{
	int order = 2;
	ssize_t bytes_read = 0;
	void *alaska_dump_page_virtual, *alaska_dump_page_physical;
	unsigned long htbase;
	volatile int dump_reg = 0;

	read_csr(CSR_HTBASE, htbase);
	if (htbase == 0) {
		printk("handle table not initialized\n");
		return -EINVAL;
	}

	if (alaska_dump_page == NULL) {
		// allocate 2 pages for the dump page
		// they are physically contiguous
		alaska_dump_page = alloc_pages(GFP_KERNEL, order);
		if (alaska_dump_page == NULL) {
			printk("failed to allocate page\n");
			return -ENOMEM;
		}
	}

	alaska_dump_page_virtual = page_address(alaska_dump_page);
	alaska_dump_page_physical = __pa(alaska_dump_page_virtual);

	write_csr(CSR_HTDUMP, (uint64_t)alaska_dump_page_physical);
	do {
		read_csr(CSR_HTDUMP, dump_reg);
	} while (dump_reg);

	// TODO: dump

	if (len > order * 4096) {
		len = order * 4096;
	}

	// Copy data from kernel space to user space
	if (copy_to_user(buf, alaska_dump_page_virtual, len))
		return -EFAULT;

	// Update the offset and the number of bytes read
	bytes_read = len;
	printk("read %zu\n", bytes_read);

	return bytes_read;
}

/* File operations */
static struct file_operations fops = {
	.owner = THIS_MODULE,
	.mmap = alaska_handle_table_mmap,
	.read = alaska_read,
};

/* Module Init */
static int __init my_module_init(void)
{
	// Allocate a device number dynamically
	if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0) {
		pr_err("Failed to allocate char device region\n");
		return -1;
	}

	// Initialize character device
	cdev_init(&my_cdev, &fops);
	if (cdev_add(&my_cdev, dev_num, 1) < 0) {
		pr_err("Failed to add cdev\n");
		unregister_chrdev_region(dev_num, 1);
		return -1;
	}

	// Create device class
	my_class = class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(my_class)) {
		pr_err("Failed to create class\n");
		cdev_del(&my_cdev);
		unregister_chrdev_region(dev_num, 1);
		return PTR_ERR(my_class);
	}

	// Create device file in /dev
	if (device_create(my_class, NULL, dev_num, NULL, DEVICE_NAME) == NULL) {
		pr_err("Failed to create device\n");
		class_destroy(my_class);
		cdev_del(&my_cdev);
		unregister_chrdev_region(dev_num, 1);
		return -1;
	}

	pr_info("alaska_handle_table_mmap_device initialized\n");
	return 0;
}

/* Module Exit */
static void __exit my_module_exit(void)
{
	/* kfree(device_buffer); */
	device_destroy(my_class, dev_num);
	class_destroy(my_class);
	cdev_del(&my_cdev);
	unregister_chrdev_region(dev_num, 1);
	pr_info("alaska_handle_table_mmap_device removed\n");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nick Wanninger");
MODULE_DESCRIPTION("a module to present the handle table to userspace");

module_init(my_module_init);
module_exit(my_module_exit);
