#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/string.h>

#define PROC_NAME "pagecache_test"
#define INPUT_SIZE 512

static ssize_t pagecache_test_write(struct file *file,
                                    const char __user *buffer,
                                    size_t count,
                                    loff_t *pos)
{
    char input[INPUT_SIZE];
    int pid_number;
    unsigned long user_address;
    char filename[256];

    struct pid *pid_struct;
    struct task_struct *task;
    struct mm_struct *mm;
    struct page *user_page = NULL;
    struct page *cache_page = NULL;
    struct file *filp = NULL;
    struct address_space *mapping;

    unsigned long user_pfn;
    unsigned long cache_pfn;
    unsigned long user_phys;
    unsigned long cache_phys;

    void *user_kaddr;
    void *cache_kaddr;
    int cmp_result;
    long ret;
    int i;

    /* Do not allow input exceeding our buffer */
    if (count >= sizeof(input))
        return -EINVAL;

    /* Copy input string from user space */
    if (copy_from_user(input, buffer, count))
        return -EFAULT;

    input[count] = '\0';

    /* Parse PID, user-space virtual address, and filename */
    if (sscanf(input, "%d %lx %255s", &pid_number, &user_address, filename) != 3) {
        pr_err("pagecache_test: invalid input\n");
        pr_info("Usage: echo \"<PID> <USER_VIRTUAL_ADDRESS> <FILE_NAME>\" > /proc/%s\n",
                PROC_NAME);
        return -EINVAL;
    }

    /* Print experiment banner and input parameters */
    pr_info("========================================\n");
    pr_info("Page Cache vs User Buffer Experiment\n");
    pr_info("========================================\n");
    pr_info("PID:              %d\n", pid_number);
    pr_info("User virtual addr: 0x%lx\n", user_address);
    pr_info("File:             %s\n", filename);

    /* 1. Find process task structure from PID */
    pid_struct = find_get_pid(pid_number);
    if (!pid_struct) {
        pr_err("pagecache_test: Could not find pid_struct for PID %d\n", pid_number);
        return -ESRCH;
    }

    task = get_pid_task(pid_struct, PIDTYPE_PID);
    put_pid(pid_struct);

    if (!task) {
        pr_err("pagecache_test: Could not find task_struct for PID %d\n", pid_number);
        return -ESRCH;
    }

    /* 2. Get process memory descriptor (mm_struct) */
    mm = get_task_mm(task);
    if (!mm) {
        pr_err("pagecache_test: Process PID %d has no active mm_struct\n", pid_number);
        put_task_struct(task);
        return -EINVAL;
    }

    /* 3. Walk page tables and retrieve physical page backing user buffer */
    mmap_read_lock(mm);
    ret = get_user_pages_remote(mm,
                                user_address,
                                1,
                                FOLL_GET,
                                &user_page,
                                NULL);
    mmap_read_unlock(mm);

    /* Clean up mm and task structs as they are no longer needed */
    mmput(mm);
    put_task_struct(task);

    if (ret != 1) {
        pr_err("pagecache_test: get_user_pages_remote() failed with error code: %ld\n", ret);
        return -EFAULT;
    }

    /* 4. Open file and look up page-cache page at offset 0 (page index 0) */
    filp = filp_open(filename, O_RDONLY | O_LARGEFILE, 0);
    if (IS_ERR(filp)) {
        pr_err("pagecache_test: filp_open failed for %s\n", filename);
        put_page(user_page);
        return PTR_ERR(filp);
    }

    mapping = file_inode(filp)->i_mapping;
    cache_page = find_get_page(mapping, 0);
    if (!cache_page) {
        pr_err("pagecache_test: page is NOT in page cache\n");
        filp_close(filp, NULL);
        put_page(user_page);
        return -ENOENT;
    }

    /* 5. Obtain PFNs and calculate physical addresses */
    user_pfn = page_to_pfn(user_page);
    user_phys = user_pfn << PAGE_SHIFT;

    cache_pfn = page_to_pfn(cache_page);
    cache_phys = cache_pfn << PAGE_SHIFT;

    /* 6. Temporarily map both pages into kernel address space */
    user_kaddr = kmap_local_page(user_page);
    cache_kaddr = kmap_local_page(cache_page);

    /* 7. Print USER BUFFER details and first 16 bytes */
    pr_info("\nUSER BUFFER:\n");
    pr_info("  virtual address = 0x%lx\n", user_address);
    pr_info("  PFN              = %lu\n", user_pfn);
    pr_info("  physical address = 0x%lx\n", user_phys);
    pr_info("  first 16 bytes   =\n");
    for (i = 0; i < 16; i++) {
        pr_info(" %02x\n", ((unsigned char *)user_kaddr)[i]);
    }
    pr_info("\n");

    /* 8. Print PAGE CACHE details and first 16 bytes */
    pr_info("\nPAGE CACHE:\n");
    pr_info("  file offset      = 0\n");
    pr_info("  PFN              = %lu\n", cache_pfn);
    pr_info("  physical address = 0x%lx\n", cache_phys);
    pr_info("  first 16 bytes   =\n");
    for (i = 0; i < 16; i++) {
        pr_info(" %02x\n", ((unsigned char *)cache_kaddr)[i]);
    }
    pr_info("\n");

    /* 9. Compare the entire PAGE_SIZE bytes */
    cmp_result = memcmp(user_kaddr, cache_kaddr, PAGE_SIZE);

    pr_info("\nmemcmp(PAGE_SIZE) = %d\n", cmp_result);
    pr_info("========================================\n");

    /* 10. Unmap kernel mappings in reverse order (LIFO) */
    kunmap_local(cache_kaddr);
    kunmap_local(user_kaddr);

    /* 11. Release page references and close the file */
    put_page(cache_page);
    filp_close(filp, NULL);
    put_page(user_page);

    return count;
}

static const struct proc_ops pagecache_test_proc_ops = {
    .proc_write = pagecache_test_write,
};

static int __init pagecache_test_init(void)
{
    if (proc_create(PROC_NAME, 0666, NULL, &pagecache_test_proc_ops) == NULL) {
        pr_err("pagecache_test: failed to create /proc/%s\n", PROC_NAME);
        return -ENOMEM;
    }

    pr_info("pagecache_test: loaded\n");
    pr_info("Use /proc/pagecache_test to run the experiment\n");

    return 0;
}

static void __exit pagecache_test_exit(void)
{
    remove_proc_entry(PROC_NAME, NULL);

    pr_info("pagecache_test: unloaded\n");
}

module_init(pagecache_test_init);
module_exit(pagecache_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Advanced OS");
MODULE_DESCRIPTION("Proving the Physical-Memory Cost of read()");
