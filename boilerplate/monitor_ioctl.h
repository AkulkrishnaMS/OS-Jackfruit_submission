#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

/*
 * monitor_ioctl.h — shared between engine.c (user-space) and monitor.c (kernel)
 *
 * ALL LIMITS ARE IN KILOBYTES (KB).
 *
 * Examples:
 *   soft_limit = 20480   means 20 MB
 *   hard_limit = 40960   means 40 MB
 */

#ifdef __KERNEL__
  #include <linux/ioctl.h>
#else
  #include <sys/ioctl.h>
#endif

/* Payload sent from engine.c → monitor.c via ioctl */
struct container_limits {
    int           pid;
    unsigned long soft_limit;   /* KB — log warning when exceeded */
    unsigned long hard_limit;   /* KB — SIGKILL when exceeded     */
};

/* ioctl command */
#define IOCTL_REGISTER_CONTAINER _IOW('m', 1, struct container_limits)

#endif /* MONITOR_IOCTL_H */
