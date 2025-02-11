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
static char *device_buffer;

/* mmap function */
static int my_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;

	if (size > MEM_SIZE)
		return -EINVAL;

	return remap_pfn_range(vma, vma->vm_start,
			       __pa(device_buffer) >> PAGE_SHIFT, size,
			       vma->vm_page_prot);
}

/* File operations */
static struct file_operations fops = {
	.owner = THIS_MODULE,
	.mmap = my_mmap,
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

	// Allocate memory
	device_buffer = kmalloc(MEM_SIZE, GFP_KERNEL);
  memset(device_buffer, 0, MEM_SIZE);
  __asm__ volatile("csrw 0xc2, %0" ::"rK"(__pa(device_buffer)) : "memory");
  __asm__ volatile("csrw 0xc5, %0" ::"rK"(4096 / 8) : "memory");
  pr_info("handle table at 0x%llx\n", __pa(device_buffer));

	if (!device_buffer) {
		pr_err("Failed to allocate memory\n");
		device_destroy(my_class, dev_num);
		class_destroy(my_class);
		cdev_del(&my_cdev);
		unregister_chrdev_region(dev_num, 1);
		return -ENOMEM;
	}

	pr_info("my_mmap_device initialized\n");
	return 0;
}

/* Module Exit */
static void __exit my_module_exit(void)
{
	kfree(device_buffer);
	device_destroy(my_class, dev_num);
	class_destroy(my_class);
	cdev_del(&my_cdev);
	unregister_chrdev_region(dev_num, 1);
	pr_info("my_mmap_device removed\n");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nick Wanninger");
MODULE_DESCRIPTION("a module to present the handle table to userspace");

module_init(my_module_init);
module_exit(my_module_exit);
