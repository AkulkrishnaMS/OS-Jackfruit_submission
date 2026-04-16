/*
 * monitor.c - Jackfruit Kernel Memory Monitor (LKM)
 *
 * Creates /dev/container_monitor (major=240).
 * Accepts ioctl IOCTL_REGISTER_CONTAINER from engine.c.
 * Every second checks RSS of each registered PID:
 *   - RSS > soft_limit  → printk WARNING (once per process)
 *   - RSS > hard_limit  → SIGKILL + remove from list
 *
 * Limits are in KILOBYTES (KB).
 *
 * Load:   sudo insmod monitor.ko
 *         sudo mknod /dev/container_monitor c 240 0
 *         sudo chmod 666 /dev/container_monitor
 * Unload: sudo rmmod monitor
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/timer.h>
#include <linux/pid.h>
#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define DEVICE_MAJOR 240

/* ── Per-process tracking node ──────────────────────────────── */
struct monitored_proc {
    int            pid;
    unsigned long  soft_limit;      /* KB */
    unsigned long  hard_limit;      /* KB */
    bool           soft_limit_hit;  /* have we warned already? */
    struct list_head list;
};

static LIST_HEAD(monitor_list);
static DEFINE_MUTEX(monitor_lock);
static struct timer_list monitor_timer;

/* ══════════════════════════════════════════════════════════════
 *  Timer callback — fires every 1 second
 *  Checks RSS of every registered PID.
 * ══════════════════════════════════════════════════════════════ */
static void check_memory(struct timer_list *t)
{
    struct monitored_proc *entry, *tmp;
    struct task_struct    *task;
    unsigned long          rss_kb;

    mutex_lock(&monitor_lock);

    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {

        /* Look up the task by PID */
        rcu_read_lock();
        task = pid_task(find_vpid(entry->pid), PIDTYPE_PID);
        rcu_read_unlock();

        if (!task) {
            /* Process already gone — clean up stale entry */
            printk(KERN_INFO "[Monitor] PID %d gone, removing from list\n",
                   entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* get_mm_rss returns pages; convert to KB */
        if (!task->mm) continue;
        rss_kb = get_mm_rss(task->mm) << (PAGE_SHIFT - 10);

        /* ── HARD LIMIT ──────────────────────────────────────── */
        if (rss_kb > entry->hard_limit) {
            printk(KERN_ALERT
                   "[Monitor] HARD LIMIT: PID %d RSS=%luKB > hard=%luKB — KILLING\n",
                   entry->pid, rss_kb, entry->hard_limit);

            /* Send SIGKILL */
            rcu_read_lock();
            task = pid_task(find_vpid(entry->pid), PIDTYPE_PID);
            if (task)
                kill_pid(find_vpid(entry->pid), SIGKILL, 1);
            rcu_read_unlock();

            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* ── SOFT LIMIT (warn once) ──────────────────────────── */
        if (rss_kb > entry->soft_limit && !entry->soft_limit_hit) {
            printk(KERN_WARNING
                   "[Monitor] SOFT LIMIT: PID %d RSS=%luKB > soft=%luKB — WARNING\n",
                   entry->pid, rss_kb, entry->soft_limit);
            entry->soft_limit_hit = true;
            /* Note: process continues running — just logged */
        }
    }

    mutex_unlock(&monitor_lock);

    /* Re-arm the timer for the next second */
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));
}

/* ══════════════════════════════════════════════════════════════
 *  ioctl handler
 * ══════════════════════════════════════════════════════════════ */
static long mon_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    if (cmd == IOCTL_REGISTER_CONTAINER) {
        struct container_limits lim;
        struct monitored_proc  *new_proc;

        if (copy_from_user(&lim,
                           (struct container_limits __user *)arg,
                           sizeof(lim)))
            return -EFAULT;

        /* Validate */
        if (lim.pid <= 0 || lim.hard_limit == 0) return -EINVAL;

        new_proc = kmalloc(sizeof(*new_proc), GFP_KERNEL);
        if (!new_proc) return -ENOMEM;

        new_proc->pid           = lim.pid;
        new_proc->soft_limit    = lim.soft_limit;   /* KB */
        new_proc->hard_limit    = lim.hard_limit;   /* KB */
        new_proc->soft_limit_hit = false;
        INIT_LIST_HEAD(&new_proc->list);

        mutex_lock(&monitor_lock);
        list_add(&new_proc->list, &monitor_list);
        mutex_unlock(&monitor_lock);

        printk(KERN_INFO
               "[Monitor] Registered PID %d  soft=%luKB  hard=%luKB\n",
               lim.pid, lim.soft_limit, lim.hard_limit);
        return 0;
    }

    return -ENOTTY;   /* unknown ioctl */
}

static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = mon_ioctl,
};

/* ══════════════════════════════════════════════════════════════
 *  Module init / exit
 * ══════════════════════════════════════════════════════════════ */
static int __init monitor_init(void)
{
    int ret = register_chrdev(DEVICE_MAJOR, DEVICE_NAME, &fops);
    if (ret < 0) {
        printk(KERN_ALERT "[Monitor] register_chrdev failed: %d\n", ret);
        return ret;
    }

    timer_setup(&monitor_timer, check_memory, 0);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));

    printk(KERN_INFO
           "[Monitor] Loaded — /dev/%s (major=%d) ready\n",
           DEVICE_NAME, DEVICE_MAJOR);
    printk(KERN_INFO
           "[Monitor] Run: sudo mknod /dev/%s c %d 0 && sudo chmod 666 /dev/%s\n",
           DEVICE_NAME, DEVICE_MAJOR, DEVICE_NAME);
    return 0;
}

static void __exit monitor_exit(void)
{
    struct monitored_proc *entry, *tmp;

    del_timer_sync(&monitor_timer);

    /* Free all list entries */
    mutex_lock(&monitor_lock);
    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&monitor_lock);

    unregister_chrdev(DEVICE_MAJOR, DEVICE_NAME);
    printk(KERN_INFO "[Monitor] Unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jackfruit Team");
MODULE_DESCRIPTION("Container memory monitor — soft/hard limits");
