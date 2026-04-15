### 1. Team Information
```bash
Jashruth K A - PES2UG24AM069
Krati Patel - PES2UG24AM077
```

### 2. Build, Load, and Run Instructions
```bash
# 1. BUILD
make clean
make

# 2. LOAD KERNEL MODULE
sudo insmod monitor.ko

# Verify control device
ls -l /dev/container_monitor

# 3. PREPARE ROOT FILESYSTEM (First time only)
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Copy workload programs into rootfs
cp cpu_hog memory_hog io_pulse rootfs-base/

# Create per-container writable copies
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
cp -a ./rootfs-base ./rootfs-low
cp -a ./rootfs-base ./rootfs-high

# Create logs directory
mkdir -p logs

# 4. START SUPERVISOR (Terminal 1)
sudo ./engine supervisor ./rootfs-base

# 5. CONTAINER OPERATIONS (Terminal 2)

# Start two containers
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96

# List tracked containers
sudo ./engine ps

# View logs
sudo ./engine logs alpha

# Run memory test (soft + hard limit demonstration)
sudo ./engine start mem ./rootfs-alpha ./memory_hog --soft-mib 20 --hard-mib 40

# Check kernel logs for soft/hard limit events
sudo dmesg | grep -E "SOFT|HARD"

# Run scheduling experiment (different nice values)
sudo ./engine start highprio ./rootfs-high ./cpu_hog --nice -10
sudo ./engine start lowprio ./rootfs-low ./cpu_hog --nice 10

# Check CPU allocation
ps -eo pid,ni,pcpu,comm | grep cpu_hog

# Stop containers
sudo ./engine stop alpha
sudo ./engine stop beta
sudo ./engine stop mem
sudo ./engine stop highprio
sudo ./engine stop lowprio

# Verify no zombie processes
ps aux | grep defunct

# 6. CLEANUP
# In Terminal 1: Press Ctrl+C to stop supervisor

# Unload kernel module
sudo rmmod monitor

# Verify cleanup
dmesg | tail -10
```

### 3. Demo with Screenshots
## Screenshot 1: Multi-container supervision
What it shows: Two containers (alpha and beta) running under one supervisor process
!(./Demo/OSJP-1.jpeg)

## Screenshot 2: Metadata tracking
What it shows: Output of `engine ps` command showing container IDs, PIDs, STATE, EXIT code, and memory limits
!(./Demo/OSJP-2.png)

## Screenshot 3: Bounded-buffer logging
 What it shows: Log file contents captured through logging pipeline (producer-consumer buffer)
!(./Demo/OSJP-3.png)

## Screenshot 4: CLI and IPC
What it shows: CLI command `engine start test123` and supervisor responding with "IPC Working"
!(./Demo/OSJP-4.png)

## Screenshot 5: Soft-limit warning
What it shows: `dmesg` output showing soft-limit warning when container exceeds 20 MiB
!(./Demo/OSJP-5.jpeg)

## Screenshot 6: Hard-limit enforcement
What it shows: `dmesg` output showing container killed after exceeding 40 MiB hard limit
!(./Demo/OSJP-6.jpeg)

## Screenshot 7: Scheduling experiment
What it shows: `ps` output with `R<+` (high priority) and `RN+` (low priority) CPU usage difference
!(./Demo/OSJP-7.png)

## Screenshot 8: Clean teardown
What it shows: `ps aux | grep defunct` showing no zombie processes after shutdown
!(./Demo/OSJP-8.jpeg)

### 4. Engineering Analysis
## 1. Isolation Mechanisms
Linux namespaces virtualize system resources. My runtime uses:
CLONE_NEWPID: Container sees PID 1, host sees real PID
CLONE_NEWUTS: Each container gets independent hostname
CLONE_NEWNS + chroot(): Each container sees only its rootfs
Still shared: Network, kernel memory, CPU - one kernel bug crashes all containers.

## 2. Supervisor & Process Lifecycle
Every process has a parent. Dead children become zombies until wait() is called.
My implementation:
Supervisor stays alive as parent of all containers
SIGCHLD handler with waitpid(-1, WNOHANG) reaps immediately → no zombies
SIGTERM to supervisor shuts down containers first

## 3. IPC, Threads & Sync
Without locks, concurrent access corrupts shared data.
My implementation:
Pipes: Container output → producer thread
UNIX socket: CLI → supervisor commands
Bounded buffer: Mutex protects head/tail. Condition variables block when empty/full

## 4. Memory Management
RSS = physical RAM used. Kernel tracks this. Soft limit warns, hard limit kills. Enforcement must be in kernel (user-space can't be trusted).
My implementation:
Timer every 1 sec → get_mm_rss() on monitored task
Soft limit: log warning once
Hard limit: send_sig(SIGKILL)
ioctl for supervisor to register PIDs

## 5. Scheduling Behavior
CFS assigns weight based on nice value. Lower nice = higher weight = more CPU time.
My implementation:
Two cpu_hog containers with different nice values
ps shows R<+ (high priority) vs RN+ (low priority)
High priority gets 90-98% CPU, low gets 6-83% CPU

### 5.  Design Decisions & Tradeoffs
```bash
| Subsystem | My Choice | Tradeoff | Justification |
|-----------|-----------|----------|----------------|
| Namespace Isolation | `chroot()` | Less secure (.. escape possible) | Simpler; security not required |
| Supervisor | Single-threaded | No request parallelism | Control channel low traffic |
| IPC Control | UNIX datagram socket | Connection overhead per command | Simple, reliable, no network stack |
| Logging | Bounded buffer (size 16) | May drop logs if full | Prevents container blocking on writes |
| Thread Sync | Mutex + condition vars | More complex than spinlocks | Threads block on I/O; condition vars sleep |
| Kernel Monitor | Timer-based (1 sec) | Misses short spikes | Simpler than event-driven |
| Scheduling Experiment | `nice` values on `cpu_hog` | Only affects CPU-bound tasks | Perfect for demonstrating CFS weighting |
```

### 6. Scheduler Experiment Results
## Raw Data (from Screenshot #7)
```bash
root    7106 98.1%  R<+  /cpu_hog   (high priority)
root    7588 90.9%  R<+  /cpu_hog   (high priority)
root    7093 83.9%  RN+  /cpu_hog   (low priority)
root    7579 6.5%   RN+  /cpu_hog   (low priority)
```

### Comparison Table
```bash
| Priority | STAT Flag | CPU % Range | Average CPU |
|----------|-----------|-------------|-------------|
| High (nice < 0) | `R<+` | 90.9% - 98.1% | 94.5% |
| Low (nice > 0) | `RN+` | 6.5% - 83.9% | 45.2% |
```

### What This Shows
1. **Higher priority gets more CPU** - High priority containers (nice -10) got 94.5% average CPU vs 45.2% for low priority
2. **`R<+` means high priority** - The `<` flag indicates nice < 0, `N` indicates nice > 0
3. **No starvation** - Low priority tasks still run, just get less CPU
4. **Linux CFS behavior confirmed** - CPU time proportional to task weight (weight = 1024 / 1.25^nice)

---

## 👨‍💻 Project By
- Jashruth K A  
- Krati Patel