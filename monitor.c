#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/uaccess.h>
#include <linux/signal.h>
#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("student");
MODULE_DESCRIPTION("Container RSS monitor");

#define DEVICE_NAME        "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ── per-container tracking node ── */
struct monitored_entry {
    struct list_head  node;
    pid_t             pid;
    unsigned long     soft_limit_bytes;
    unsigned long     hard_limit_bytes;
    bool              soft_warned;
    char              container_id[MONITOR_ID_LEN];
};

static LIST_HEAD(monitored_list);
static DEFINE_SPINLOCK(list_lock);

static struct timer_list monitor_timer;
static dev_t             dev_num;
static struct class     *dev_class;
static struct cdev       c_dev;

/* ── helpers ── */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    long pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);
    return pages * PAGE_SIZE;
}

static void kill_pid_by_pid(pid_t pid)
{
    struct task_struct *task;
    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task) {
        get_task_struct(task);
        rcu_read_unlock();
        send_sig(SIGKILL, task, 1);
        put_task_struct(task);
    } else {
        rcu_read_unlock();
    }
}

/* ── timer callback — runs in softirq context ── */
static void timer_callback(struct timer_list *t)
{
    struct monitored_entry *e, *tmp;

    spin_lock(&list_lock);
    list_for_each_entry_safe(e, tmp, &monitored_list, node) {
        long rss = get_rss_bytes(e->pid);

        if (rss < 0) {
            /* process already gone */
            pr_info("[container_monitor] %s (pid %d) gone, removing\n",
                    e->container_id, e->pid);
            list_del(&e->node);
            kfree(e);
            continue;
        }

        if (e->hard_limit_bytes && (unsigned long)rss > e->hard_limit_bytes) {
            pr_warn("[container_monitor] HARD limit: %s pid=%d rss=%ld > %lu — SIGKILL\n",
                    e->container_id, e->pid, rss, e->hard_limit_bytes);
            kill_pid_by_pid(e->pid);
        } else if (e->soft_limit_bytes &&
                   (unsigned long)rss > e->soft_limit_bytes &&
                   !e->soft_warned) {
            pr_warn("[container_monitor] SOFT limit: %s pid=%d rss=%ld > %lu\n",
                    e->container_id, e->pid, rss, e->soft_limit_bytes);
            e->soft_warned = true;
        }
    }
    spin_unlock(&list_lock);

    /* re-arm */
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ── ioctl handler ── */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {

    case MONITOR_REGISTER: {
        struct monitor_register_req req;
        struct monitored_entry *entry;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;

        entry = kzalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid              = req.pid;
        entry->soft_limit_bytes = req.soft_limit_bytes;
        entry->hard_limit_bytes = req.hard_limit_bytes;
        entry->soft_warned      = false;
        strncpy(entry->container_id, req.container_id, MONITOR_ID_LEN - 1);

        spin_lock(&list_lock);
        list_add_tail(&entry->node, &monitored_list);
        spin_unlock(&list_lock);

        pr_info("[container_monitor] registered %s pid=%d soft=%lu hard=%lu\n",
                entry->container_id, entry->pid,
                entry->soft_limit_bytes, entry->hard_limit_bytes);
        return 0;
    }

    case MONITOR_UNREGISTER: {
        struct monitor_unregister_req req;
        struct monitored_entry *e, *tmp;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;

        spin_lock(&list_lock);
        list_for_each_entry_safe(e, tmp, &monitored_list, node) {
            if (e->pid == req.pid) {
                list_del(&e->node);
                kfree(e);
                break;
            }
        }
        spin_unlock(&list_lock);
        pr_info("[container_monitor] unregistered pid=%d\n", req.pid);
        return 0;
    }

    default:
        return -ENOTTY;
    }
}

static const struct file_operations fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ── module init / exit ── */
static int __init monitor_init(void)
{
    int rc;

    rc = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (rc < 0) return rc;

    dev_class = class_create(DEVICE_NAME);
    if (IS_ERR(dev_class)) {
        rc = PTR_ERR(dev_class);
        goto err_class;
    }

    if (!device_create(dev_class, NULL, dev_num, NULL, DEVICE_NAME)) {
        rc = -ENOMEM;
        goto err_device;
    }

    cdev_init(&c_dev, &fops);
    rc = cdev_add(&c_dev, dev_num, 1);
    if (rc < 0) goto err_cdev;

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    pr_info("[container_monitor] Loaded.\n");
    return 0;

err_cdev:   device_destroy(dev_class, dev_num);
err_device: class_destroy(dev_class);
err_class:  unregister_chrdev_region(dev_num, 1);
    return rc;
}

static void __exit monitor_exit(void)
{
    struct monitored_entry *e, *tmp;

    del_timer_sync(&monitor_timer);

    spin_lock(&list_lock);
    list_for_each_entry_safe(e, tmp, &monitored_list, node) {
        list_del(&e->node);
        kfree(e);
    }
    spin_unlock(&list_lock);

    cdev_del(&c_dev);
    device_destroy(dev_class, dev_num);
    class_destroy(dev_class);
    unregister_chrdev_region(dev_num, 1);

    pr_info("[container_monitor] Unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
