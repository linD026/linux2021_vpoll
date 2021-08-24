#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/ftrace.h>
#include <linux/kallsyms.h>

#include "trace_time.h"

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("Synthesize events for select/poll/epoll");

#define NAME "vpoll"

#define VPOLL_IOC_MAGIC '^'
#define VPOLL_IO_ADDEVENTS _IO(VPOLL_IOC_MAGIC, 1 /*MMM*/)
#define VPOLL_IO_DELEVENTS _IO(VPOLL_IOC_MAGIC, 2 /*NNN*/)
#define EPOLLALLMASK ((__force __poll_t)0x0fffffff)

static int major = -1;
static struct cdev vpoll_cdev;
static struct class *vpoll_class = NULL;

static struct trace_time tt_do_epoll_wait;

struct vpoll_data {
    wait_queue_head_t wqh;
    __poll_t events;
};

struct ftrace_hook {
    const char *name;
    void *func, *orig;
    unsigned long address;
    struct ftrace_ops ops;
};

static int hook_resolve_addr(struct ftrace_hook *hook)
{
    hook->address = kallsyms_lookup_name(hook->name);
    if (!hook->address) {
        printk("unresolved symbol: %s\n", hook->name);
        return -ENOENT;
    }
    *((unsigned long *)hook->orig) = hook->address;
    return 0;
}

/*
 * This function is the callback function for ftrace.
 * @ip - This is the instruction pointer of the function that is being traced.
 * @parent_ip - This is the instruction pointer of the function that called the
 *              function being traced.
 * @op - This is a pointer to ftrace_ops that was used to register the cakkback
 *       . This can be used to pass data to the callback via the private pointer.
 * @regs - If the FTRACE_OPS_FL_SAVE_REGS or FTRACE_OPS_FL_SAVE_REGS_IF_SUPPORTED
 *         flags are set in the ftrace_ops structure, then this will pointing to
 *         the pt_regs structure like it would be if an breakpoint was placed at
 *         the start of the function where ftrace was tracing. Otherwise it either
 *         contains garbage, or NULL.
 *
 * The `hook->func` is `ower own function`.
 * */
static void notrace hook_ftrace_thunk(unsigned long ip, unsigned long parent_ip,
                                      struct ftrace_ops *ops,
                                      struct pt_regs *regs)
{
    struct ftrace_hook *hook = container_of(ops, struct ftrace_hook, ops);
    if (!within_module(parent_ip, THIS_MODULE))
        regs->ip = (unsigned long)hook->func;
}

static int hook_install(struct ftrace_hook *hook)
{
    int err = hook_resolve_addr(hook);
    if (err)
        return err;

    hook->ops.func = hook_ftrace_thunk;
    hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_RECURSION_SAFE |
                      FTRACE_OPS_FL_IPMODIFY;

    // ste hook->address into filter hash
    err = ftrace_set_filter_ip(&hook->ops, hook->address, 0, 0);
    if (err) {
        printk("ftrace_set_filter_ip() failed: %d\n", err);
        return err;
    }

    err = register_ftrace_function(&hook->ops);
    if (err) {
        printk("register_ftrace_function() failed: %d\n", err);
        ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
        return err;
    }
    return 0;
}

void hook_remove(struct ftrace_hook *hook)
{
    int err = unregister_ftrace_function(&hook->ops);
    if (err)
        printk("unregister_ftrace_function() failed: %d\n", err);
    err = ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
    if (err)
        printk("ftrace_set_filter_ip() failed: %d\n", err);
}

typedef int (*do_epoll_wait_t)(int epfd, struct epoll_event __user *events,
                               int maxevents, struct timespec64 *to);
static struct ftrace_hook hook;
static do_epoll_wait_t real_do_epoll_wait;

static int hook_do_epoll_wait(int epfd, struct epoll_event __user *events,
                              int maxevents, struct timespec64 *to)
{
    int ret;

    if (events->data == 123456789) {
        tt_do_epoll_wait = TRACE_TIME_INIT("do_epoll_wait");
        TRACE_TIME_START(tt_do_epoll_wait);
        ret = real_do_epoll_wait(epfd, events, maxevents, to);
        TRACE_TIME_END(tt_do_epoll_wait);
        TRACE_CALC(tt_do_epoll_wait);
        TRACE_PRINT(tt_do_epoll_wait);
    } else
        ret = real_do_epoll_wait(epfd, events, maxevents, to);

    return ret;
}

static void init_hook(void)
{
    real_do_epoll_wait = (do_epoll_wait_t)kallsyms_lookup_name("do_epoll_wait");
    hook.name = "do_epoll_wait";
    hook.func = hook_do_epoll_wait;
    hook.orig = &real_do_epoll_wait;
    hook_install(&hook);
}

static int vpoll_open(struct inode *inode, struct file *file)
{
    struct vpoll_data *vpoll_data =
            kmalloc(sizeof(struct vpoll_data), GFP_KERNEL);
    if (!vpoll_data)
        return -ENOMEM;
    vpoll_data->events = 0;
    init_waitqueue_head(&vpoll_data->wqh);
    file->private_data = vpoll_data;
    return 0;
}

static int vpoll_release(struct inode *inode, struct file *file)
{
    struct vpoll_data *vpoll_data = file->private_data;
    kfree(vpoll_data);
    return 0;
}

static long vpoll_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct vpoll_data *vpoll_data = file->private_data;
    __poll_t events = arg & EPOLLALLMASK;
    long res = 0;

    spin_lock_irq(&vpoll_data->wqh.lock);
    switch (cmd) {
    case VPOLL_IO_ADDEVENTS:
        vpoll_data->events |= events;
        break;
    case VPOLL_IO_DELEVENTS:
        vpoll_data->events &= ~events;
        break;
    default:
        res = -EINVAL;
    }
    if (res >= 0) {
        res = vpoll_data->events;
        if (waitqueue_active(&vpoll_data->wqh))
            /*WWW*/ wake_up_locked_poll(&vpoll_data->wqh, vpoll_data->events);
        // wake_up_poll(&vpoll_data->wqh, vpoll_data->events);
    }
    spin_unlock_irq(&vpoll_data->wqh.lock);
    return res;
}

static __poll_t vpoll_poll(struct file *file, struct poll_table_struct *wait)
{
    struct vpoll_data *vpoll_data = file->private_data;

    poll_wait(file, &vpoll_data->wqh, wait);

    return READ_ONCE(vpoll_data->events);
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = vpoll_open,
    .release = vpoll_release,
    .unlocked_ioctl = vpoll_ioctl,
    .poll = vpoll_poll,
};

static char *vpoll_devnode(struct device *dev, umode_t *mode)
{
    if (!mode)
        return NULL;

    *mode = 0666;
    return NULL;
}

static int __init vpoll_init(void)
{
    int ret;
    struct device *dev;

    if ((ret = alloc_chrdev_region(&major, 0, 1, NAME)) < 0)
        return ret;
    vpoll_class = class_create(THIS_MODULE, NAME);
    if (IS_ERR(vpoll_class)) {
        ret = PTR_ERR(vpoll_class);
        goto error_unregister_chrdev_region;
    }
    // method (callback)
    vpoll_class->devnode = vpoll_devnode;
    dev = device_create(vpoll_class, NULL, major, NULL, NAME);
    if (IS_ERR(dev)) {
        ret = PTR_ERR(dev);
        goto error_class_destroy;
    }
    cdev_init(&vpoll_cdev, &fops);
    if ((ret = cdev_add(&vpoll_cdev, major, 1)) < 0)
        goto error_device_destroy;

    init_hook();

    printk(KERN_INFO NAME ": loaded\n");
    return 0;

error_device_destroy:
    device_destroy(vpoll_class, major);
error_class_destroy:
    class_destroy(vpoll_class);
error_unregister_chrdev_region:
    unregister_chrdev_region(major, 1);
    return ret;
}

static void __exit vpoll_exit(void)
{
    hook_remove(&hook);
    device_destroy(vpoll_class, major);
    cdev_del(&vpoll_cdev);
    class_destroy(vpoll_class);
    unregister_chrdev_region(major, 1);
    printk(KERN_INFO NAME ": unloaded\n");
}

module_init(vpoll_init);
module_exit(vpoll_exit);
