#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/smp.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>
#include <linux/random.h>
#include <linux/atomic.h>
#include <linux/version.h>
#include <linux/limits.h>

#include "lottery.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Advanced OS Student");
MODULE_DESCRIPTION("Multi-Core Proportional-Share Lottery Scheduler");
MODULE_VERSION("1.0");

#define TIMER_INTERVAL_MS 50
#define LOTTERY_DEVICE_NAME "lottery"

/*
 * Structure representing an active task registered in a CPU's runqueue
 */
struct lottery_task {
    struct list_head list;
    unsigned long pid;
    unsigned long tickets;
    struct task_struct *task;
};

/*
 * Per-CPU runqueue structure
 */
struct lottery_queue {
    int cpu_id;
    spinlock_t lock;
    struct list_head tasks;
    unsigned long total_tickets;
    unsigned int task_count;
    struct lottery_task *current_winner;
};

/* Per-CPU runqueue pointer */
static struct lottery_queue __percpu *percpu_queues;

/* Periodic timer and work item */
static struct hrtimer lottery_timer;
static struct work_struct lottery_work;
static ktime_t timer_interval;

/* Mutex to serialize registrations across user processes */
static DEFINE_MUTEX(registration_lock);

/* Atomic counter to log queue status every 10 ticks (~500ms) */
static atomic_t tick_counter = ATOMIC_INIT(0);

/*
 * Forward declarations
 */
static void lottery_work_func(struct work_struct *work);
static enum hrtimer_restart lottery_timer_tick(struct hrtimer *timer);

/*
 * Unregister a task given its PID.
 * Called from LOTTERY_UNREGISTER and release() fop.
 */
static int unregister_task_by_pid(unsigned long pid)
{
    int cpu;
    struct lottery_task *entry = NULL, *pos, *tmp;
    struct lottery_queue *q = NULL;
    unsigned long flags;
    struct task_struct *next_winner_task = NULL;
    struct task_struct *unreg_task = NULL;

    for_each_online_cpu(cpu) {
        struct lottery_queue *cur_q = per_cpu_ptr(percpu_queues, cpu);

        spin_lock_irqsave(&cur_q->lock, flags);
        list_for_each_entry_safe(pos, tmp, &cur_q->tasks, list) {
            if (pos->pid == pid) {
                entry = pos;
                q = cur_q;

                list_del(&entry->list);
                q->total_tickets -= entry->tickets;
                q->task_count--;

                /* If the unregistered task was the current winner, select a replacement */
                if (q->current_winner == entry) {
                    q->current_winner = NULL;
                    if (q->task_count > 0) {
                        q->current_winner = list_first_entry(&q->tasks, struct lottery_task, list);
                        next_winner_task = q->current_winner->task;
                        get_task_struct(next_winner_task);
                    }
                }
                break;
            }
        }
        spin_unlock_irqrestore(&cur_q->lock, flags);

        if (entry)
            break;
    }

    if (!entry)
        return -ESRCH;

    /* Wake up the replacement winner if one was chosen */
    if (next_winner_task) {
        send_sig_info(SIGCONT, SEND_SIG_PRIV, next_winner_task);
        put_task_struct(next_winner_task);
    }

    unreg_task = entry->task;

    /* Restore CPU affinity to all online CPUs */
    set_cpus_allowed_ptr(unreg_task, cpu_online_mask);

    /* Resume task in case it was stopped */
    send_sig_info(SIGCONT, SEND_SIG_PRIV, unreg_task);

    put_task_struct(unreg_task);
    kfree(entry);

    return 0;
}

/*
 * IOCTL handler for /dev/lottery
 */
static long lottery_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case LOTTERY_REGISTER: {
        struct lottery_struct req;
        struct lottery_task *new_entry;
        struct pid *pid_struct;
        struct task_struct *task;
        int min_cpu;
        unsigned long min_tickets;
        int cpu;
        unsigned long flags;
        struct lottery_queue *target_q;

        if (copy_from_user(&req, (struct lottery_struct __user *)arg, sizeof(req)))
            return -EFAULT;

        if (req.tickets == 0)
            return -EINVAL;

        /* Resolve struct pid to struct task_struct */
        pid_struct = find_get_pid(req.pid);
        if (!pid_struct)
            return -ESRCH;

        task = get_pid_task(pid_struct, PIDTYPE_PID);
        put_pid(pid_struct);
        if (!task)
            return -ESRCH;

        new_entry = kmalloc(sizeof(*new_entry), GFP_KERNEL);
        if (!new_entry) {
            put_task_struct(task);
            return -ENOMEM;
        }

        INIT_LIST_HEAD(&new_entry->list);
        new_entry->pid = req.pid;
        new_entry->tickets = req.tickets;
        new_entry->task = task; /* Retain task reference */

        /* Lock registration mutex to safely evaluate queue loads */
        mutex_lock(&registration_lock);

        min_cpu = -1;
        min_tickets = ULONG_MAX;

        for_each_online_cpu(cpu) {
            struct lottery_queue *q = per_cpu_ptr(percpu_queues, cpu);
            unsigned long tix;

            spin_lock_irqsave(&q->lock, flags);
            tix = q->total_tickets;
            spin_unlock_irqrestore(&q->lock, flags);

            if (tix < min_tickets) {
                min_tickets = tix;
                min_cpu = cpu;
            }
        }

        if (min_cpu == -1)
            min_cpu = cpumask_first(cpu_online_mask);

        target_q = per_cpu_ptr(percpu_queues, min_cpu);

        spin_lock_irqsave(&target_q->lock, flags);
        /* Use list_add_tail for newly allocated unlinked node */
        list_add_tail(&new_entry->list, &target_q->tasks);
        target_q->total_tickets += new_entry->tickets;
        target_q->task_count++;

        if (target_q->task_count == 1) {
            /* First task on this core: make it the running winner */
            target_q->current_winner = new_entry;
            send_sig_info(SIGCONT, SEND_SIG_PRIV, task);
        } else {
            /* Queue already has an active winner: pause this task until selected */
            send_sig_info(SIGSTOP, SEND_SIG_PRIV, task);
        }
        spin_unlock_irqrestore(&target_q->lock, flags);

        /* Bind task to target CPU core */
        set_cpus_allowed_ptr(task, cpumask_of(min_cpu));

        mutex_unlock(&registration_lock);
        return 0;
    }

    case LOTTERY_UNREGISTER: {
        struct lottery_struct req;

        if (copy_from_user(&req, (struct lottery_struct __user *)arg, sizeof(req)))
            return -EFAULT;

        return unregister_task_by_pid(req.pid);
    }

    default:
        return -ENOTTY;
    }
}

/*
 * Release handler: Clean up process if it terminated without unregistering
 */
static int lottery_release(struct inode *inode, struct file *file)
{
    unregister_task_by_pid(current->pid);
    return 0;
}

static const struct file_operations lottery_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = lottery_ioctl,
    .release        = lottery_release,
};

static struct miscdevice lottery_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = LOTTERY_DEVICE_NAME,
    .fops  = &lottery_fops,
};

/*
 * Dynamic Load Balancer:
 * Evaluates per-CPU queues and migrates tasks from heaviest to lightest core.
 */
static void balance_queues(void)
{
    int max_cpu = -1, min_cpu = -1;
    unsigned long max_tickets = 0, min_tickets = ULONG_MAX;
    int cpu;
    struct lottery_queue *src_q, *dst_q;
    struct lottery_queue *first_q, *second_q;
    unsigned long flags1, flags2;
    struct lottery_task *pos, *candidate = NULL;
    struct task_struct *migrated_task = NULL;
    unsigned long migrated_pid = 0, migrated_tickets = 0;
    bool did_migrate = false;

    if (num_online_cpus() < 2)
        return;

    /* Find heaviest (max_cpu) and lightest (min_cpu) cores */
    for_each_online_cpu(cpu) {
        struct lottery_queue *q = per_cpu_ptr(percpu_queues, cpu);
        unsigned long flags;
        unsigned long tix;

        spin_lock_irqsave(&q->lock, flags);
        tix = q->total_tickets;
        spin_unlock_irqrestore(&q->lock, flags);

        if (tix > max_tickets) {
            max_tickets = tix;
            max_cpu = cpu;
        }
        if (tix < min_tickets) {
            min_tickets = tix;
            min_cpu = cpu;
        }
    }

    if (max_cpu == -1 || min_cpu == -1 || max_cpu == min_cpu)
        return;

    /* Rule 1: Hysteresis Threshold (strictly > 50 tickets) */
    if (max_tickets <= min_tickets || (max_tickets - min_tickets) <= 50)
        return;

    src_q = per_cpu_ptr(percpu_queues, max_cpu);
    dst_q = per_cpu_ptr(percpu_queues, min_cpu);

    /*
     * Double Lock Acquisition:
     * Sort locks by CPU ID to prevent AB-BA deadlocks.
     * Use spin_lock_irqsave_nested with SINGLE_DEPTH_NESTING for the second lock.
     */
    if (max_cpu < min_cpu) {
        first_q = src_q;
        second_q = dst_q;
    } else {
        first_q = dst_q;
        second_q = src_q;
    }

    spin_lock_irqsave(&first_q->lock, flags1);
    spin_lock_irqsave_nested(&second_q->lock, flags2, SINGLE_DEPTH_NESTING);

    /* Re-verify hysteresis condition under lock */
    if (src_q->total_tickets > dst_q->total_tickets &&
        (src_q->total_tickets - dst_q->total_tickets) > 50) {

        /*
         * Rule 2: Anti-Thrashing / Overshoot Prevention:
         * Verify dst_new <= src_new + 20
         * where dst_new = dst_q->total_tickets + candidate->tickets
         *       src_new = src_q->total_tickets - candidate->tickets
         */
        list_for_each_entry(pos, &src_q->tasks, list) {
            unsigned long dst_new = dst_q->total_tickets + pos->tickets;
            unsigned long src_new = src_q->total_tickets - pos->tickets;

            if (dst_new <= src_new + 20) {
                candidate = pos;
                break;
            }
        }

        if (candidate) {
            /* Atomically transfer task node from src_q to dst_q */
            list_move_tail(&candidate->list, &dst_q->tasks);
            src_q->total_tickets -= candidate->tickets;
            src_q->task_count--;
            dst_q->total_tickets += candidate->tickets;
            dst_q->task_count++;

            /* If candidate was the running winner on src_q, clear and stop it */
            if (src_q->current_winner == candidate) {
                src_q->current_winner = NULL;
                send_sig_info(SIGSTOP, SEND_SIG_PRIV, candidate->task);
            }

            migrated_task = candidate->task;
            get_task_struct(migrated_task);
            migrated_pid = candidate->pid;
            migrated_tickets = candidate->tickets;
            did_migrate = true;
        }
    }

    /* Release locks in reverse order (LIFO) */
    spin_unlock_irqrestore(&second_q->lock, flags2);
    spin_unlock_irqrestore(&first_q->lock, flags1);

    /* Enforce CPU affinity migration in process context */
    if (did_migrate && migrated_task) {
        set_cpus_allowed_ptr(migrated_task, cpumask_of(min_cpu));
        pr_info("[LOTTERY] Load Balance: Migrated PID %lu (%lu tix) CPU %d -> CPU %d\n",
                migrated_pid, migrated_tickets, max_cpu, min_cpu);
        put_task_struct(migrated_task);
    }
}

/*
 * Quantum Winner Selection & Context Switcher:
 * For each CPU, conducts lottery drawing and uses SIGSTOP/SIGCONT to enforce execution.
 */
static void schedule_lottery(void)
{
    int cpu;

    for_each_online_cpu(cpu) {
        struct lottery_queue *q = per_cpu_ptr(percpu_queues, cpu);
        struct task_struct *to_stop = NULL;
        struct task_struct *to_cont = NULL;
        unsigned long flags;

        spin_lock_irqsave(&q->lock, flags);

        if (q->task_count == 0 || q->total_tickets == 0) {
            q->current_winner = NULL;
        } else if (q->task_count == 1) {
            struct lottery_task *sole = list_first_entry(&q->tasks, struct lottery_task, list);

            if (q->current_winner != sole) {
                if (q->current_winner) {
                    to_stop = q->current_winner->task;
                    get_task_struct(to_stop);
                }
                q->current_winner = sole;
                to_cont = sole->task;
                get_task_struct(to_cont);
            }
        } else {
            /* q->task_count > 1: Proportional lottery winner selection */
            unsigned long rand_ticket;
            unsigned long counter = 0;
            struct lottery_task *pos, *winner = NULL;

            /* Retrieve entropy from CSPRNG */
            get_random_bytes(&rand_ticket, sizeof(rand_ticket));
            rand_ticket = (rand_ticket % q->total_tickets) + 1;

            list_for_each_entry(pos, &q->tasks, list) {
                counter += pos->tickets;
                if (counter >= rand_ticket) {
                    winner = pos;
                    break;
                }
            }

            if (!winner)
                winner = list_first_entry(&q->tasks, struct lottery_task, list);

            if (q->current_winner != winner) {
                if (q->current_winner) {
                    to_stop = q->current_winner->task;
                    get_task_struct(to_stop);
                }
                q->current_winner = winner;
                to_cont = winner->task;
                get_task_struct(to_cont);
            }
        }

        spin_unlock_irqrestore(&q->lock, flags);

        /* Context switch via kernel signaling outside spinlock */
        if (to_stop) {
            send_sig_info(SIGSTOP, SEND_SIG_PRIV, to_stop);
            put_task_struct(to_stop);
        }
        if (to_cont) {
            send_sig_info(SIGCONT, SEND_SIG_PRIV, to_cont);
            put_task_struct(to_cont);
        }
    }
}

/*
 * Periodic Diagnostic Queue Snapshot:
 * Emits queue load status every 10 ticks (~500ms).
 */
static void print_status_snapshot(void)
{
    if (atomic_inc_return(&tick_counter) % 10 == 0) {
        char buf[256];
        int len = 0;
        int cpu;

        buf[0] = '\0';
        for_each_online_cpu(cpu) {
            struct lottery_queue *q = per_cpu_ptr(percpu_queues, cpu);
            unsigned long flags;
            unsigned long tix;
            unsigned int tasks;

            spin_lock_irqsave(&q->lock, flags);
            tix = q->total_tickets;
            tasks = q->task_count;
            spin_unlock_irqrestore(&q->lock, flags);

            len += scnprintf(buf + len, sizeof(buf) - len,
                             "CPU%d: %lu tix (%u tasks) | ",
                             cpu, tix, tasks);
        }

        pr_info("[LOTTERY_STATUS] %s\n", buf);
    }
}

/*
 * Deferred work handler (Process context)
 */
static void lottery_work_func(struct work_struct *work)
{
    balance_queues();
    schedule_lottery();
    print_status_snapshot();
}

/*
 * High-Resolution Timer callback (Interrupt / Atomic context)
 */
static enum hrtimer_restart lottery_timer_tick(struct hrtimer *timer)
{
    schedule_work(&lottery_work);
    hrtimer_forward_now(timer, timer_interval);
    return HRTIMER_RESTART;
}

/*
 * Module Initialization
 */
static int __init lottery_init(void)
{
    int cpu;
    int ret;

    pr_info("lottery: Initializing multi-core lottery scheduler module\n");

    /* 1. Allocate per-CPU runqueues */
    percpu_queues = alloc_percpu(struct lottery_queue);
    if (!percpu_queues) {
        pr_err("lottery: Failed to allocate per-CPU runqueues\n");
        return -ENOMEM;
    }

    for_each_possible_cpu(cpu) {
        struct lottery_queue *q = per_cpu_ptr(percpu_queues, cpu);
        q->cpu_id = cpu;
        spin_lock_init(&q->lock);
        INIT_LIST_HEAD(&q->tasks);
        q->total_tickets = 0;
        q->task_count = 0;
        q->current_winner = NULL;
    }

    /* 2. Register misc character device */
    ret = misc_register(&lottery_dev);
    if (ret) {
        pr_err("lottery: Failed to register misc device /dev/%s (err=%d)\n",
               LOTTERY_DEVICE_NAME, ret);
        free_percpu(percpu_queues);
        return ret;
    }

    /* 3. Setup workqueue and high-resolution timer */
    INIT_WORK(&lottery_work, lottery_work_func);
    timer_interval = ktime_set(0, TIMER_INTERVAL_MS * 1000000L);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
    hrtimer_setup(&lottery_timer, lottery_timer_tick, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#else
    hrtimer_init(&lottery_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    lottery_timer.function = lottery_timer_tick;
#endif

    hrtimer_start(&lottery_timer, timer_interval, HRTIMER_MODE_REL);

    pr_info("lottery: Module loaded successfully with device /dev/%s\n", LOTTERY_DEVICE_NAME);
    return 0;
}

/*
 * Module Exit / Cleanup
 */
static void __exit lottery_exit(void)
{
    int cpu;
    unsigned long flags;

    pr_info("lottery: Cleaning up and unloading module\n");

    /* Stop timer and flush deferred work */
    hrtimer_cancel(&lottery_timer);
    cancel_work_sync(&lottery_work);

    /* Unregister misc device */
    misc_deregister(&lottery_dev);

    /* Clean up all registered tasks across all CPU runqueues */
    if (percpu_queues) {
        for_each_possible_cpu(cpu) {
            struct lottery_queue *q = per_cpu_ptr(percpu_queues, cpu);
            struct lottery_task *entry, *tmp;

            spin_lock_irqsave(&q->lock, flags);
            list_for_each_entry_safe(entry, tmp, &q->tasks, list) {
                list_del(&entry->list);

                /* Restore task state and CPU affinity */
                send_sig_info(SIGCONT, SEND_SIG_PRIV, entry->task);
                set_cpus_allowed_ptr(entry->task, cpu_online_mask);
                put_task_struct(entry->task);
                kfree(entry);
            }
            q->total_tickets = 0;
            q->task_count = 0;
            q->current_winner = NULL;
            spin_unlock_irqrestore(&q->lock, flags);
        }

        free_percpu(percpu_queues);
        percpu_queues = NULL;
    }

    pr_info("lottery: Module unloaded cleanly\n");
}

module_init(lottery_init);
module_exit(lottery_exit);
