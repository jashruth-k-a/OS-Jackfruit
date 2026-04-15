#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <sys/types.h>
#endif

#define MONITOR_ID_LEN 64

struct monitor_register_req {
    pid_t         pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char          container_id[MONITOR_ID_LEN];
};

struct monitor_unregister_req {
    pid_t pid;
    char  container_id[MONITOR_ID_LEN];
};

#define MONITOR_MAGIC      0xCE
#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct monitor_register_req)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_unregister_req)

#endif /* MONITOR_IOCTL_H */
