/* tinyplay.c
**
** Copyright 2011, The Android Open Source Project
** Copyright 2026, Artyom <https://github.com/ortom-io>
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are met:
**     * Redistributions of source code must retain the above copyright
**       notice, this list of conditions and the following disclaimer.
**     * Redistributions in binary form must reproduce the above copyright
**       notice, this list of conditions and the following disclaimer in the
**       documentation and/or other materials provided with the distribution.
**     * Neither the name of The Android Open Source Project nor the names of
**       its contributors may be used to endorse or promote products derived
**       from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY The Android Open Source Project ``AS IS'' AND
** ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED. IN NO EVENT SHALL The Android Open Source Project BE LIABLE
** FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
** DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
** SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
** CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
** LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
** OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
** DAMAGE.
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <dirent.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <assert.h>
#include <stdatomic.h>
#include <linux/futex.h>
#include <sys/time.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <alsa/asoundlib.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <sys/inotify.h>
#include <limits.h>
#include <stdalign.h>


/* Wrapper for direct system call */
static long
sys_futex(void *addr1, int op, int val1, const struct timespec *timeout, void *addr2, int val3)
{
    return syscall(SYS_futex, addr1, op, val1, timeout, addr2, val3);
}

/* Fallback definitions for older headers */
#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#endif
#ifndef FUTEX_WAKE
#define FUTEX_WAKE 1
#endif

#define OPTPARSE_IMPLEMENTATION
#include "optparse.h"

#ifndef likely
#define likely(x)      __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
#define unlikely(x)    __builtin_expect(!!(x), 0)
#endif

#ifndef SNDRV_PCM_IOCTL_DELAY
#define SNDRV_PCM_IOCTL_DELAY _IOR('A', 0x21, snd_pcm_sframes_t)
#endif

/* --- CONFIGURATION --- */
#define DEFAULT_SHM_FILE "/mnt/nlp/tinyplay_shm"
#define SHIELD_DIR       "/mnt/nlp/tmp"
#define MY_BUNKER        "tiny_shield"
#define MAX_STATE_ENTRIES 2048

static char g_cpuset_root[128] = "/dev/cpuset";

/* Commands & States */
#define CMD_NONE 0
#define CMD_PAUSE 1
#define CMD_RESUME 2
#define CMD_SEEK 3
#define CMD_EXIT 4
#define CMD_UPDATE_TIME 5
#define STATE_PLAYING 0
#define STATE_PAUSED 1
#define STATE_COMPLETED 2
#define STATE_ERROR 3
#define STATE_DRAINING 4

/* Standard WAV Defines */
#define ID_RIFF 0x46464952
#define ID_WAVE 0x45564157
#define ID_FMT  0x20746d66
#define ID_DATA 0x61746164
#define WAVE_FORMAT_PCM 0x0001
#define WAVE_FORMAT_IEEE_FLOAT 0x0003

/* Priority Defines */
#ifndef IOPRIO_CLASS_RT
#define IOPRIO_CLASS_RT 1
#endif
#ifndef IOPRIO_WHO_PROCESS
#define IOPRIO_WHO_PROCESS 1
#endif
#ifndef PR_SET_TIMERSLACK
#define PR_SET_TIMERSLACK 29
#endif

#ifndef MS_BIND
#define MS_BIND 4096
#endif
#ifndef MS_REC
#define MS_REC 16384
#endif
#ifndef MS_PRIVATE
#define MS_PRIVATE (1<<18)
#endif
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif

/* --- GLOBALS --- */
static int g_pm_qos_fd = -1;
static const char *g_shm_path = DEFAULT_SHM_FILE;
static pid_t g_child_pid = -1;
static bool g_is_cgroup_v2 = false;

/* Fast state cache */
typedef struct {
    char path[128];
    char original_val[256];
    char target_val[256];
    bool is_mount_point;
} state_entry_t;

static state_entry_t g_state_cache[MAX_STATE_ENTRIES];
static int g_state_count = 0;

/* SHM Structure */
struct player_ctrl {
    atomic_uint_least32_t magic;              /* 0 */
    atomic_int_least32_t  command;            /* 4 */
    atomic_uint_least32_t seek_target;        /* 8 */
    atomic_uint_least32_t total_frames;       /* 12 */
    atomic_uint_least32_t current_frame;      /* 16 */
    atomic_int_least32_t  state;              /* 20 */
    atomic_uint_least32_t sample_rate;        /* 24 */
    atomic_uint_least32_t channels;           /* 28 */
    atomic_uint_least32_t bits;               /* 32 */
    atomic_uint_least32_t exact_total_frames; /* 36 */
    atomic_uint_least32_t hw_period_size;     /* 40 */
    atomic_uint_least32_t hw_buffer_size;     /* 44 */
    atomic_int_least32_t  hw_format;          /* 48 */
    atomic_int_least32_t  hw_access;          /* 52 */
    uint8_t _padding[64 - (14 * sizeof(atomic_uint_least32_t))]; /* Remainder: 8 bytes */
} __attribute__((aligned(64)));

_Static_assert(sizeof(struct player_ctrl) == 64, "player_ctrl structure size violation! Check padding.");

static struct player_ctrl *shm = NULL;
volatile atomic_int signal_event = 0;
static volatile atomic_int stop_flag = 0;

/* Standard Structures */
struct riff_wave_header {
    uint32_t riff_id;
    uint32_t riff_sz;
    uint32_t wave_id;
} __attribute__((packed));

struct chunk_header {
    uint32_t id;
    uint32_t sz;
} __attribute__((packed));

struct chunk_fmt {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} __attribute__((packed));

struct ctx {
    snd_pcm_t *pcm;
    int fd;
    int alsa_fd;
    uint8_t *map_start;
    size_t map_size;
    uint8_t *data_start;
    size_t data_size;
    size_t play_offset;
    struct chunk_fmt fmt;
    uint16_t valid_bits_per_sample;
    bool is_float_ext;
    
    /* --- RT THREAD DATA --- */
    /* Written exclusively by the isolated audio core */
    alignas(64) atomic_size_t sync_base_frames;
    
    /* --- BACKGROUND THREADS DATA --- */
    /* Written exclusively by the warmer_thread on standard cores */
    alignas(64) atomic_size_t live_file_size;
};

struct cmd {
    const char *filename;
    const char *filetype;
    unsigned int card;
    unsigned int device;
    int flags;

    unsigned int rate;
    unsigned int channels;
    snd_pcm_format_t format;
    snd_pcm_uframes_t period_size;
    unsigned int period_count;

    unsigned int bits;
    int cpu_core;
    bool is_float;
    const char *shm_file;
    size_t expected_size;
    bool use_mmap;
    long initial_seek;
    char usb_uevent_path[PATH_MAX];
    bool dac_claimed;
    bool use_unbind;
};

/* --- ROBUST I/O HELPERS --- */
static int
sys_write_opt(const char *path, const char *val)
{
    if (!path || !val)
        return 0;
    
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;

    ssize_t len = strlen(val);
    ssize_t ret = write(fd, val, len);
    close(fd);
    
    return ret == len;
}

static void
read_val(const char *path, char *buf, size_t size)
{
    if (!path || !buf || !size)
        return;
    
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t len = read(fd, buf, size - 1);
        if (len > 0) {
            buf[len] = '\0';
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
                buf[--len] = '\0';
        } else {
            buf[0] = '\0';
        }
        close(fd);
    } else {
        buf[0] = '\0';
    }
}

static inline bool
file_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

/* Remove trailing newline and carriage return */
void
trim_newline(char *str)
{
    if (!str || !*str)
        return;
    str[strcspn(str, "\r\n")] = '\0';
}

/* =========================================================================
   USB DAC EXCLUSIVE ACCESS (UNBIND -> REMOVE -> BIND)
   ========================================================================= */
static void
acquire_usb_dac(struct cmd *cmd)
{
    if (!cmd->use_unbind)
        return;

    char temp_path[PATH_MAX];
    char usb_root_path[PATH_MAX];
    char unbind_path[] = "/sys/bus/usb/drivers/snd-usb-audio/unbind";
    char bind_path[]   = "/sys/bus/usb/drivers/snd-usb-audio/bind";

    /* Find USB device root */
    snprintf(temp_path, sizeof(temp_path), "/sys/class/sound/card%u/device/..", cmd->card);
    if (!realpath(temp_path, usb_root_path))
        return;

    /* Extract host ID */
    char *usb_id = strrchr(usb_root_path, '/');
    if (!usb_id)
        return;
    usb_id++;

    /* Save uevent path for restoration */
    snprintf(cmd->usb_uevent_path, sizeof(cmd->usb_uevent_path), "%s/uevent", usb_root_path);

    /* Find audio interfaces */
    char ifaces[8][32];
    int iface_count = 0;
    size_t id_len = strlen(usb_id);

    DIR *d = opendir("/sys/bus/usb/drivers/snd-usb-audio");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) && iface_count < 8) {
            if (strncmp(ent->d_name, usb_id, id_len) == 0 && ent->d_name[id_len] == ':') {
                strncpy(ifaces[iface_count], ent->d_name, sizeof(ifaces[0]) - 1);
                ifaces[iface_count][sizeof(ifaces[0]) - 1] = '\0';
                iface_count++;
            }
        }
        closedir(d);
    }

    if (iface_count == 0)
        return;

    /* Unbind driver immediately */
    for (int i = 0; i < iface_count; i++) {
        sys_write_opt(unbind_path, ifaces[i]);
    }

    /* Send remove uevent */
    sys_write_opt(cmd->usb_uevent_path, "remove");

    /* Rebind driver */
    for (int i = 0; i < iface_count; i++) {
        sys_write_opt(bind_path, ifaces[i]);
    }

    cmd->dac_claimed = true;
}

static void
release_usb_dac(struct cmd *cmd)
{
    if (!cmd->use_unbind)
        return;

    /* Restore uevent state */
    if (cmd->dac_claimed && cmd->usb_uevent_path[0] != '\0') {
        sys_write_opt(cmd->usb_uevent_path, "add");
        cmd->dac_claimed = false;
    }
}

/* =========================================================================
   SYSTEM OPTIMIZATION ENGINE
   ========================================================================= */

bool
is_cpu_in_list(int cpu_idx, const char *list_str)
{
    if (!list_str || strlen(list_str) == 0)
        return false;
    
    const char *ptr = list_str;
    while (*ptr) {
        while (*ptr && !isdigit(*ptr))
            ptr++;
        if (!*ptr)
            break;
        
        char *endptr;
        long start = strtol(ptr, &endptr, 10);
        long end = start;
        if (*endptr == '-')
            end = strtol(endptr + 1, &endptr, 10);
        if (cpu_idx >= start && cpu_idx <= end)
            return true;
        
        ptr = endptr;
    }
    return false;
}

void
register_smart(const char *path, const char *val)
{
    if (!path || !val)
        return;
    if (!file_exists(path))
        return;
    if (g_state_count >= MAX_STATE_ENTRIES) {
        static bool overflow_warned = false;
        if (!overflow_warned) {
            fprintf(stderr, "WARNING: Optimization cache full (%d entries). Some tweaks won't apply!\n", MAX_STATE_ENTRIES);
            overflow_warned = true;
        }
        return;
    }

    /* Check for duplicates */
    for (int i = 0; i < g_state_count; i++) {
        if (strcmp(g_state_cache[i].path, path) == 0)
            return;
    }

    state_entry_t *entry = &g_state_cache[g_state_count++];

    snprintf(entry->path, sizeof(entry->path), "%s", path);
    snprintf(entry->target_val, sizeof(entry->target_val), "%s", val);

    trim_newline(entry->target_val);

    entry->is_mount_point = false;

    /* Read original value for backup */
    read_val(path, entry->original_val, sizeof(entry->original_val));
    trim_newline(entry->original_val);
}

void
register_change(const char *path, const char *target_val, bool is_mount)
{
    if (is_mount) {
        if (g_state_count >= MAX_STATE_ENTRIES)
            return;
        
        state_entry_t *entry = &g_state_cache[g_state_count++];
        
        snprintf(entry->path, sizeof(entry->path), "%s", path);
        snprintf(entry->target_val, sizeof(entry->target_val), "%s", target_val);
        
        entry->is_mount_point = true;
    } else {
        register_smart(path, target_val);
    }
}

void
exclude_cpu_from_list(const char *in_str, char *out_buf, size_t out_size, int cpu_to_remove)
{
    bool cpu_present[256] = { 0 };
    int max_cpu = 0;
    const char *ptr = in_str;

    while (*ptr) {
        while (*ptr && !isdigit(*ptr))
            ptr++;
        if (!*ptr)
            break;
        
        int start = strtol(ptr, (char **)&ptr, 10);
        int end = start;
        if (*ptr == '-') {
            ptr++;
            end = strtol(ptr, (char **)&ptr, 10);
        }
        
        for (int i = start; i <= end; i++) {
            if (i >= 0 && i < 256) {
                cpu_present[i] = true;
                if (i > max_cpu)
                    max_cpu = i;
            }
        }
    }
    
    if (cpu_to_remove >= 0 && cpu_to_remove < 256)
        cpu_present[cpu_to_remove] = false;

    out_buf[0] = '\0';
    bool first = true;
    int i = 0;
    
    while (i <= max_cpu) {
        if (cpu_present[i]) {
            int range_start = i;
            while (i + 1 <= max_cpu && cpu_present[i + 1])
                i++;
            int range_end = i;
            
            if (!first)
                strncat(out_buf, ",", out_size - strlen(out_buf) - 1);
            
            char tmp[32];
            if (range_start == range_end)
                snprintf(tmp, sizeof(tmp), "%d", range_start);
            else
                snprintf(tmp, sizeof(tmp), "%d-%d", range_start, range_end);
            
            strncat(out_buf, tmp, out_size - strlen(out_buf) - 1);
            first = false;
        }
        i++;
    }
}

void
precalc_usb_irqs(int cpu_core)
{
    FILE *fp = fopen("/proc/interrupts", "r");
    if (!fp)
        return;
    
    char line[1024], proc_path[64], core_str[16], hex_str[16];
    
    /* Calculate values */
    snprintf(core_str, sizeof(core_str), "%d", cpu_core);
    snprintf(hex_str, sizeof(hex_str), "%x", (1 << cpu_core));
    
    /* Prepare paths for dummy files */
    char dummy_path[128], dummy_hex[128];
    snprintf(dummy_path, sizeof(dummy_path), "%s/shield_list", SHIELD_DIR);
    snprintf(dummy_hex, sizeof(dummy_hex), "%s/shield_hex", SHIELD_DIR);

    /* Ensure base directories exist */
    mkdir("/mnt/nlp", 0755);
    mkdir(SHIELD_DIR, 0755);

    /* Write list dummy file */
    int fd = open(dummy_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        write(fd, core_str, strlen(core_str));
        close(fd);
    } else {
        perror("Failed to create shield_list dummy file");
    }

    /* Write hex dummy file */
    fd = open(dummy_hex, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        write(fd, hex_str, strlen(hex_str));
        close(fd);
    } else {
        perror("Failed to create shield_hex dummy file");
    }

    /* Scan interrupts and apply rules */
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "xhci") || strstr(line, "dwc3") || strstr(line, "usb") || strstr(line, "ehci")) {
            char *ptr = line;
            while (*ptr == ' ')
                ptr++;
            
            if (!isdigit(*ptr))
                continue;
            
            long irq = strtol(ptr, NULL, 10);
            if (irq <= 0)
                continue;
            
            /* Set smp_affinity_list */
            snprintf(proc_path, sizeof(proc_path), "/proc/irq/%ld/smp_affinity_list", irq);
            register_change(proc_path, core_str, true);

            /* Set smp_affinity */
            snprintf(proc_path, sizeof(proc_path), "/proc/irq/%ld/smp_affinity", irq);
            register_change(proc_path, hex_str, true);
        }
    }
    fclose(fp);
}

void
precalc_sibling_eviction(int cpu_core)
{
    DIR *d;
    struct dirent *dir;
    
    d = opendir(g_cpuset_root);
    if (!d)
        return;

    char path_cpus[128], buf_in[256], buf_out[256];
    const char *cpu_filename = g_is_cgroup_v2 ? "cpuset.cpus" : "cpus";

    while ((dir = readdir(d))) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0)
            continue;
        if (strcmp(dir->d_name, MY_BUNKER) == 0)
            continue;
        if (dir->d_type != DT_DIR && dir->d_type != DT_UNKNOWN)
            continue;

        snprintf(path_cpus, sizeof(path_cpus), "%s/%s/%s", g_cpuset_root, dir->d_name, cpu_filename);
        
        if (!file_exists(path_cpus))
            continue;

        read_val(path_cpus, buf_in, sizeof(buf_in));
        if (strlen(buf_in) > 0) {
            exclude_cpu_from_list(buf_in, buf_out, sizeof(buf_out), cpu_core);
            if (strlen(buf_out) > 0 && strcmp(buf_in, buf_out) != 0) {
                register_change(path_cpus, buf_out, false);
            }
        }
    }
    closedir(d);
}

void
detect_cpuset_path(void)
{
    FILE *fp = fopen("/proc/mounts", "r");
    bool found_v1 = false;
    bool found_v2 = false;
    char v2_mountpoint[128] = { 0 };

    g_is_cgroup_v2 = false;

    if (fp) {
        char line[512];
        char device[128], mountpoint[128], type[32], options[256];
        
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "%127s %127s %31s %255s", device, mountpoint, type, options) == 4) {
                
                /* Priority: Look for Cgroup v1 (cpuset) */
                if (strcmp(type, "cgroup") == 0 && strstr(options, "cpuset")) {
                    strncpy(g_cpuset_root, mountpoint, sizeof(g_cpuset_root) - 1);
                    g_cpuset_root[sizeof(g_cpuset_root) - 1] = '\0';
                    g_is_cgroup_v2 = false;
                    found_v1 = true;
                    break;
                }

                /* Remember Cgroup v2, but keep searching for v1 */
                if (strcmp(type, "cgroup2") == 0 && !found_v2) {
                    strncpy(v2_mountpoint, mountpoint, sizeof(v2_mountpoint) - 1);
                    found_v2 = true;
                }
            }
        }
        fclose(fp);
    }

    if (!found_v1 && found_v2) {
        strncpy(g_cpuset_root, v2_mountpoint, sizeof(g_cpuset_root) - 1);
        g_cpuset_root[sizeof(g_cpuset_root) - 1] = '\0';
        g_is_cgroup_v2 = true;
    } else if (!found_v1 && !found_v2) {
        if (access("/dev/cpuset", F_OK) == 0) {
            strcpy(g_cpuset_root, "/dev/cpuset");
            g_is_cgroup_v2 = false;
        } else if (access("/sys/fs/cgroup/cpuset", F_OK) == 0) {
            strcpy(g_cpuset_root, "/sys/fs/cgroup/cpuset");
            g_is_cgroup_v2 = false;
        } else if (access("/sys/fs/cgroup/cgroup.controllers", F_OK) == 0) {
            strcpy(g_cpuset_root, "/sys/fs/cgroup");
            g_is_cgroup_v2 = true;
        }
    }
}

void
precalc_system_state(int target_core)
{
    g_state_count = 0;
    char path[128], buf[64], cluster_cpus[64] = { 0 };
    
    long num_cpus = sysconf(_SC_NPROCESSORS_CONF);
    if (num_cpus < 1)
        num_cpus = 8;

    /* Determine target cluster */
    bool targeted_mode = (target_core >= 0);
    
    if (targeted_mode) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/related_cpus", target_core);
        if (!file_exists(path))
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/affected_cpus", target_core);
        if (!file_exists(path))
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/core_siblings_list", target_core);
        
        read_val(path, cluster_cpus, sizeof(cluster_cpus));
        trim_newline(cluster_cpus);

        if (strlen(cluster_cpus) == 0)
            snprintf(cluster_cpus, sizeof(cluster_cpus), "%d", target_core);
    }

    /* Iterate over CPU cores */
    for (int i = 0; i < num_cpus; i++) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", i);
        register_smart(path, "1");

        /* Skip if core is not in cluster */
        if (!targeted_mode || !is_cpu_in_list(i, cluster_cpus)) {
            continue;
        }

        /* CPU Performance governor */
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
        register_smart(path, "performance");

        /* Lock CPU frequency */
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", i);
        if (file_exists(path)) {
            read_val(path, buf, sizeof(buf));
            trim_newline(buf);
            
            if (strlen(buf) > 0) {
                snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", i);
                register_smart(path, buf);
                
                char hw_min_path[128];
                char hw_min_val[64];
                snprintf(hw_min_path, sizeof(hw_min_path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_min_freq", i);
                
                if (file_exists(hw_min_path)) {
                    read_val(hw_min_path, hw_min_val, sizeof(hw_min_val));
                    trim_newline(hw_min_val);
                    
                    if (strlen(hw_min_val) > 0 && g_state_count > 0) {
                        state_entry_t *last_entry = &g_state_cache[g_state_count - 1];
                        
                        if (strstr(last_entry->path, "scaling_min_freq")) {
                             snprintf(last_entry->original_val, sizeof(last_entry->original_val), "%s", hw_min_val);
                        }
                    }
                }
            }
        }

        /* Disable C-States */
        for (int state = 0; state < 10; state++) {
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/disable", i, state);
            if (!file_exists(path))
                break;
            register_smart(path, "1");
        }

        /* Core control */
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/core_ctl/min_cpus", i);
        if (file_exists(path)) {
            char path_max[128], max_val[16] = { 0 };
            snprintf(path_max, sizeof(path_max), "/sys/devices/system/cpu/cpu%d/core_ctl/max_cpus", i);
            read_val(path_max, max_val, sizeof(max_val));
            trim_newline(max_val);

            if (strlen(max_val) > 0)
                register_smart(path, max_val);
            else
                register_smart(path, "2");

            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/core_ctl/enable", i);
            register_smart(path, "0");
            
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/core_ctl/busy_down_thres", i);
            register_smart(path, "0");
        }
    }

    /* Global tweaks */
    struct tweak {
        const char *p;
        const char *v;
    };
    
    struct tweak tweaks[] = {
        {"/proc/sys/kernel/sched_energy_aware", "0"},
        {"/proc/sys/kernel/sched_migration_cost_ns", "5000000"},
        {"/proc/sys/kernel/sched_util_clamp_min_rt_default", "1024"},
        {"/proc/vendor_sched/groups/ta/uclamp_min", "1024"},
        {"/proc/vendor_sched/groups/fg/uclamp_min", "1024"},
        {"/sys/module/qcom_lpm/parameters/sleep_disabled", "1"},
        {"/sys/module/lpm_levels/parameters/sleep_disabled", "1"},
        {"/sys/module/mtk_core_ctl/parameters/enable", "0"},
        {"/proc/perfmgr/boost_ctrl/eas_ctrl/perfserv_ta_boost", "1"},
        {"/sys/power/cpuhotplug/enabled", "0"},
        {"/sys/module/exynos_hotplug/parameters/enable", "0"},
        {"/proc/sys/kernel/watchdog", "0"},
        {"/proc/sys/kernel/nmi_watchdog", "0"},
        {"/sys/kernel/debug/tracing/tracing_on", "0"},
        {NULL, NULL}
    };

    for (int k = 0; tweaks[k].p; k++) {
        register_smart(tweaks[k].p, tweaks[k].v);
    }

    /* Devfreq bus and interconnect performance */
    DIR *devfreq_dir = opendir("/sys/class/devfreq");
    if (devfreq_dir) {
        struct dirent *ent;
        char df_path[256];
        
        while ((ent = readdir(devfreq_dir))) {
            if (ent->d_name[0] == '.')
                continue;
            
            snprintf(df_path, sizeof(df_path), "/sys/class/devfreq/%s/governor", ent->d_name);
            if (file_exists(df_path)) {
                register_smart(df_path, "performance");
            }
            
            char df_max_path[256], df_max_val[64];
            snprintf(df_max_path, sizeof(df_max_path), "/sys/class/devfreq/%s/max_freq", ent->d_name);
            if (file_exists(df_max_path)) {
                read_val(df_max_path, df_max_val, sizeof(df_max_val));
                trim_newline(df_max_val);
                if (strlen(df_max_val) > 0) {
                    snprintf(df_path, sizeof(df_path), "/sys/class/devfreq/%s/min_freq", ent->d_name);
                    register_smart(df_path, df_max_val);
                }
            }
        }
        closedir(devfreq_dir);
    }

    /* Disable sleep for storage devices */
    DIR *block_dir = opendir("/sys/block");
    if (block_dir) {
        struct dirent *ent;
        char blk_path[256];
        char sched_val[256];
        
        while ((ent = readdir(block_dir))) {
            if (strstr(ent->d_name, "mmcblk") || strstr(ent->d_name, "sd") || strstr(ent->d_name, "nvme")) {
                snprintf(blk_path, sizeof(blk_path), "/sys/block/%s/device/power/control", ent->d_name);
                if (file_exists(blk_path))
                    register_smart(blk_path, "on");
                
                snprintf(blk_path, sizeof(blk_path), "/sys/block/%s/queue/scheduler", ent->d_name);
                if (file_exists(blk_path)) {
                    read_val(blk_path, sched_val, sizeof(sched_val));
                    if (strstr(sched_val, "none"))
                        register_smart(blk_path, "none");
                    else if (strstr(sched_val, "mq-deadline"))
                        register_smart(blk_path, "mq-deadline");
                    else if (strstr(sched_val, "noop"))
                        register_smart(blk_path, "noop");
                }
            }
        }
        closedir(block_dir);
    }

    /* Vendor-specific power tweaks */
    struct tweak power_tweaks[] = {
        {"/sys/module/cpu_boost/parameters/input_boost_enabled", "1"},
        {"/sys/module/cpu_boost/parameters/input_boost_ms", "2000"},
        {"/sys/module/pcie_aspm/parameters/policy", "performance"},
        {"/sys/module/snd_hda_intel/parameters/power_save", "0"},
        {NULL, NULL}
    };
    
    for (int k = 0; power_tweaks[k].p; k++) {
        register_smart(power_tweaks[k].p, power_tweaks[k].v);
    }

    /* Continuous USB power and autosuspend disable */
    DIR *usb_dir = opendir("/sys/bus/usb/devices");
    if (usb_dir) {
        struct dirent *ent;
        char usb_path[256];
        
        while ((ent = readdir(usb_dir))) {
            if (ent->d_name[0] == '.')
                continue;

            snprintf(usb_path, sizeof(usb_path), "/sys/bus/usb/devices/%s/power/control", ent->d_name);
            if (file_exists(usb_path)) {
                register_smart(usb_path, "on");
            }

            snprintf(usb_path, sizeof(usb_path), "/sys/bus/usb/devices/%s/power/level", ent->d_name);
            if (file_exists(usb_path)) {
                register_smart(usb_path, "on");
            }

            snprintf(usb_path, sizeof(usb_path), "/sys/bus/usb/devices/%s/power/autosuspend", ent->d_name);
            if (file_exists(usb_path)) {
                register_smart(usb_path, "-1");
            }

            snprintf(usb_path, sizeof(usb_path), "/sys/bus/usb/devices/%s/power/pm_qos_no_power_off", ent->d_name);
            if (file_exists(usb_path))
                register_smart(usb_path, "1");

            snprintf(usb_path, sizeof(usb_path), "/sys/bus/usb/devices/%s/power/autosuspend_delay_ms", ent->d_name);
            if (file_exists(usb_path)) {
                register_smart(usb_path, "-1");
            }

            snprintf(usb_path, sizeof(usb_path), "/sys/bus/usb/devices/%s/power/usb2_hardware_lpm", ent->d_name);
            if (file_exists(usb_path))
                register_smart(usb_path, "0");

            snprintf(usb_path, sizeof(usb_path), "/sys/bus/usb/devices/%s/power/usb3_hardware_lpm", ent->d_name);
            if (file_exists(usb_path))
                register_smart(usb_path, "0");

            snprintf(usb_path, sizeof(usb_path), "/sys/bus/usb/devices/%s/power/wakeup", ent->d_name);
            if (file_exists(usb_path))
                register_smart(usb_path, "disabled");
        }
        closedir(usb_dir);
    }

    /* USB controller tweaks */
    struct tweak usb_tweaks[] = {
        {"/sys/module/usbcore/parameters/usbfs_memory_mb", "128"},
        {"/sys/module/usbcore/parameters/autosuspend", "-1"},
        {"/sys/module/dwc3/parameters/disable_lpm", "Y"},
        {"/sys/module/xhci_hcd/parameters/quirks", "262144"},
        {"/sys/module/iommu/parameters/strict", "0"},
        {NULL, NULL}
    };

    for (int k = 0; usb_tweaks[k].p; k++) {
        if (file_exists(usb_tweaks[k].p)) {
            register_smart(usb_tweaks[k].p, usb_tweaks[k].v);
        }
    }

    /* Audio and bus specific tweaks */
    struct tweak extra_audio_tweaks[] = {
        {"/sys/module/snd_usb_audio/parameters/power_save", "0"},
        {"/sys/module/snd_soc_core/parameters/pmdown_time", "0"},
        {"/sys/module/workqueue/parameters/power_efficient", "N"},
        {"/sys/module/workqueue/parameters/power_efficient", "0"},
        {"/sys/module/snd_usb_audio/parameters/implicit_fb", "0"},
        {"/sys/module/snd_usb_audio/parameters/autoclock", "0"},
        {"/sys/kernel/timer_migration", "0"},
        {NULL, NULL}
    };

    for (int k = 0; extra_audio_tweaks[k].p; k++) {
        if (file_exists(extra_audio_tweaks[k].p)) {
            register_smart(extra_audio_tweaks[k].p, extra_audio_tweaks[k].v);
        }
    }

    DIR *pci_dir = opendir("/sys/bus/pci/devices");
    if (pci_dir) {
        struct dirent *ent;
        char pci_path[256];
        
        while ((ent = readdir(pci_dir))) {
            if (ent->d_name[0] == '.')
                continue;
            
            snprintf(pci_path, sizeof(pci_path), "/sys/bus/pci/devices/%s/power/control", ent->d_name);
            if (file_exists(pci_path)) {
                register_smart(pci_path, "on");
            }

            snprintf(pci_path, sizeof(pci_path), "/sys/bus/pci/devices/%s/link/l1_aspm", ent->d_name);
            if (file_exists(pci_path))
                register_smart(pci_path, "0");
            
            snprintf(pci_path, sizeof(pci_path), "/sys/bus/pci/devices/%s/d3cold_allowed", ent->d_name);
            if (file_exists(pci_path))
                register_smart(pci_path, "0");
        }
        closedir(pci_dir);
    }

    /* VM tweaks to reduce background jitter */
    struct tweak vm_tweaks[] = {
        {"/proc/sys/vm/swappiness", "0"},
        {"/proc/sys/vm/compaction_proactiveness", "0"},
        {"/proc/sys/vm/watermark_boost_factor", "0"},
        {"/proc/sys/vm/stat_interval", "120"},
        {"/proc/sys/vm/page-cluster", "0"},
        {NULL, NULL}
    };

    for (int k = 0; vm_tweaks[k].p; k++) {
        if (file_exists(vm_tweaks[k].p)) {
            register_smart(vm_tweaks[k].p, vm_tweaks[k].v);
        }
    }

    /* USB power supply current limits */
    struct tweak usb_current_tweaks[] = {
        {"/sys/class/power_supply/usb/current_max", "1500000"},
        {"/sys/class/power_supply/usb/hw_current_max", "1500000"},
        {"/sys/class/power_supply/main/current_max", "1500000"},
        {"/sys/class/typec/port0/power_role", "source"},
        {NULL, NULL}
    };

    for (int k = 0; usb_current_tweaks[k].p; k++) {
        if (file_exists(usb_current_tweaks[k].p)) {
            register_smart(usb_current_tweaks[k].p, usb_current_tweaks[k].v);
        }
    }

    /* Network polling tweaks */
    struct tweak network_tweaks[] = {
        {"/proc/sys/net/ipv4/tcp_low_latency", "1"},
        {"/proc/sys/net/core/busy_read", "0"},
        {"/proc/sys/net/core/busy_poll", "0"},
        {NULL, NULL}
    };
    
    for (int k = 0; network_tweaks[k].p; k++) {
        register_smart(network_tweaks[k].p, network_tweaks[k].v);
    }

    /* IRQ isolation */
    if (target_core >= 0) {
        precalc_sibling_eviction(target_core);
        precalc_usb_irqs(target_core);
    }
}

void
fast_apply_system_state(void)
{
    if (g_pm_qos_fd < 0) {
        int qos_fd = open("/dev/cpu_dma_latency", O_RDWR | O_CLOEXEC);
        if (qos_fd >= 0) {
            int32_t lat = 0;
            if (write(qos_fd, &lat, sizeof(lat)) < 0) {
                fprintf(stderr, "WARNING: Failed to lock CPU DMA latency: %s\n", strerror(errno));
            }
            g_pm_qos_fd = qos_fd;
        }
    }
    
    char dummy_path[128], dummy_hex[128];
    snprintf(dummy_path, sizeof(dummy_path), "%s/shield_list", SHIELD_DIR);
    snprintf(dummy_hex, sizeof(dummy_hex), "%s/shield_hex", SHIELD_DIR);

    for (int i = 0; i < g_state_count; i++) {
        if (!g_state_cache[i].is_mount_point) {
            sys_write_opt(g_state_cache[i].path, g_state_cache[i].target_val);
        } else {
            umount2(g_state_cache[i].path, MNT_DETACH);
            chmod(g_state_cache[i].path, 0666);
            sys_write_opt(g_state_cache[i].path, g_state_cache[i].target_val);
            
            /* Select appropriate dummy file */
            const char *src = dummy_path;
            if (strstr(g_state_cache[i].path, "smp_affinity") && !strstr(g_state_cache[i].path, "_list")) {
                src = dummy_hex;
            }

            if (mount(src, g_state_cache[i].path, "", MS_BIND, NULL) != 0) {
                perror("mount bind failed");
            }
        }
    }
}

void
fast_revert_system_state(bool full_cleanup, bool is_supervisor)
{
    char path[128];
    char val[64];

    /* Move process to root cgroup */
    snprintf(path, sizeof(path), "%s/cgroup.procs", g_cpuset_root);
    
    if (is_supervisor && g_child_pid > 0) {
        snprintf(val, sizeof(val), "%d", g_child_pid);
        sys_write_opt(path, val);
    } else {
        snprintf(val, sizeof(val), "%d", getpid());
        sys_write_opt(path, val);
    }

    /* Disable exclusive/partition settings */
    snprintf(path, sizeof(path), "%s/%s", g_cpuset_root, MY_BUNKER);
    
    umount2(path, MNT_DETACH);

    if (g_is_cgroup_v2) {
        char path_part[128];
        snprintf(path_part, sizeof(path_part), "%s/cpuset.cpus.partition", path);
        sys_write_opt(path_part, "member");
    } else {
        char path_excl[128];
        snprintf(path_excl, sizeof(path_excl), "%s/cpu_exclusive", path);
        sys_write_opt(path_excl, "0");
    }

    /* Remove isolation group */
    if (rmdir(path) != 0 && errno != ENOENT) {
        char path_mem[128];
        const char *f_mems = g_is_cgroup_v2 ? "cpuset.mems" : "mems";
        
        snprintf(path_mem, sizeof(path_mem), "%s/%s", path, f_mems);
        sys_write_opt(path_mem, "0");
        rmdir(path);
    }

    /* Restore previous system state */
    for (int i = g_state_count - 1; i >= 0; i--) {
        if (g_state_cache[i].is_mount_point) {
            if (is_supervisor || full_cleanup) {
                umount2(g_state_cache[i].path, MNT_DETACH);
            }
        } else {
            sys_write_opt(g_state_cache[i].path, g_state_cache[i].original_val);
        }
    }

    if (full_cleanup && g_pm_qos_fd >= 0) {
        close(g_pm_qos_fd);
        g_pm_qos_fd = -1;
    }
}

void
create_and_enter_bunker(int cpu_core)
{
    char path_bunker[128], path_root[128], buf[64], temp[128];

    const char *f_cpus = g_is_cgroup_v2 ? "cpuset.cpus" : "cpus";
    const char *f_mems = g_is_cgroup_v2 ? "cpuset.mems" : "mems";
    
    snprintf(path_bunker, sizeof(path_bunker), "%s/%s", g_cpuset_root, MY_BUNKER);

    /* Reset isolation group */
    umount2(path_bunker, MNT_DETACH);
    rmdir(path_bunker);

    if (g_is_cgroup_v2) {
        char path_subtree[128];
        snprintf(path_subtree, sizeof(path_subtree), "%s/cgroup.subtree_control", g_cpuset_root);
        sys_write_opt(path_subtree, "+cpuset");
    }

    if (mkdir(path_bunker, 0755) != 0 && errno != EEXIST) {
        perror("Failed to create bunker cgroup");
        return;
    }

    /* Configure MEMS */
    snprintf(path_root, sizeof(path_root), "%s/%s", g_cpuset_root, f_mems);
    snprintf(temp, sizeof(temp), "%s/%s", path_bunker, f_mems);
    read_val(path_root, buf, sizeof(buf));
    sys_write_opt(temp, buf);

    /* Configure CPUS */
    snprintf(temp, sizeof(temp), "%s/%s", path_bunker, f_cpus);
    snprintf(buf, sizeof(buf), "%d", cpu_core);
    sys_write_opt(temp, buf);

    if (g_is_cgroup_v2) {
        char path_part[128];
        snprintf(path_part, sizeof(path_part), "%s/cpuset.cpus.partition", path_bunker);
        
        /* Try "isolated" partition (optimal for real-time audio) */
        if (!sys_write_opt(path_part, "isolated")) {
             if (!sys_write_opt(path_part, "root")) {
                 fprintf(stderr, "Warning: Failed to set Cgroup v2 partition (isolated/root). Real-time perf may suffer.\n");
             } else {
                 printf("Cgroup v2: partition set to 'root' (isolated not supported)\n");
             }
        }
    } else {
        snprintf(temp, sizeof(temp), "%s/cpu_exclusive", path_bunker);
        sys_write_opt(temp, "1");
        
        snprintf(temp, sizeof(temp), "%s/sched_load_balance", path_bunker);
        sys_write_opt(temp, "0");
        
        snprintf(temp, sizeof(temp), "%s/sched_relax_domain_level", path_bunker);
        sys_write_opt(temp, "0");
        
        snprintf(temp, sizeof(temp), "%s/memory_migrate", path_bunker);
        sys_write_opt(temp, "0");
    }

    /* Enter cgroup */
    snprintf(temp, sizeof(temp), "%s/cgroup.procs", path_bunker);
    int fd_tasks = open(temp, O_WRONLY | O_CLOEXEC);
    
    if (fd_tasks < 0 && !g_is_cgroup_v2) {
        snprintf(temp, sizeof(temp), "%s/tasks", path_bunker);
        fd_tasks = open(temp, O_WRONLY | O_CLOEXEC);
    }

    if (fd_tasks >= 0) {
        snprintf(buf, sizeof(buf), "%d", getpid());
        if (write(fd_tasks, buf, strlen(buf)) < 0) {
            fprintf(stderr, "WARNING: Failed to enter bunker cgroup: %s\n", strerror(errno));
        }
        close(fd_tasks);
    } else {
        perror("Failed to enter bunker");
    }

    if (mount(path_bunker, path_bunker, "", MS_BIND, NULL) == 0) {
         mount(path_bunker, path_bunker, "", MS_REMOUNT | MS_BIND | MS_RDONLY, NULL);
    }
}

void
optimize_process(int cpu_core)
{
    mlockall(MCL_CURRENT | MCL_FUTURE);
    
    struct sched_param sp;
    sp.sched_priority = 99;
    
    sched_setscheduler(0, SCHED_FIFO, &sp);
    setpriority(PRIO_PROCESS, 0, -20);
    syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, (IOPRIO_CLASS_RT << 13) | 0);
    prctl(PR_SET_TIMERSLACK, 1UL);

    if (cpu_core >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_core, &cpuset);
        
        sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
        create_and_enter_bunker(cpu_core);
        sched_yield();
    }
}

void
reset_process_priority(void)
{
    munlockall();
    
    struct sched_param sp;
    sp.sched_priority = 0;
    
    sched_setscheduler(0, SCHED_OTHER, &sp);
    setpriority(PRIO_PROCESS, 0, 0);
}

/* =========================================================================
   AUDIO ENGINE & MAIN EXECUTION
   Signals, WAV parsing, SHM management, ALSA loop, main().
   ========================================================================= */

/* --- SIGNALS --- */
/* Player child signal handler */
void
child_signal_handler(int sig)
{
    /* Save current thread errno to correctly return -EINTR for ALSA or Futex */
    int saved_errno = errno; 
    
    /* Notify the main loop to check SHM */
    atomic_store_explicit(&signal_event, 1, memory_order_release);
    
    /* Handle hard exit */
    if (sig == SIGTERM || sig == SIGINT) {
        atomic_store_explicit(&stop_flag, 1, memory_order_release);
        if (shm) {
            /* Wake up watcher_thread on hard exit */
            atomic_store_explicit(&shm->command, CMD_EXIT, memory_order_release);
            sys_futex((int *)&shm->command, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
        }
    }
    
    /* Restore errno */
    errno = saved_errno; 
}

/* Supervisor signal handler */
void
supervisor_signal_handler(int sig)
{
    /* Forward signal to the child.
       Supervisor exits when waitpid() returns. */
    if (g_child_pid > 0) {
        kill(g_child_pid, sig);
    }
}

void
setup_child_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa)); 
    sa.sa_handler = child_signal_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask); 
    sigaction(SIGTERM, &sa, NULL); 
    sigaction(SIGINT, &sa, NULL);
}

static int
parse_wav_headers(struct ctx *ctx)
{
    uint8_t *ptr = ctx->map_start;
    size_t file_size = ctx->map_size;
    
    if (file_size < sizeof(struct riff_wave_header)) {
        fprintf(stderr, "Error: File too small\n");
        return -1;
    }
    
    struct riff_wave_header *riff = (struct riff_wave_header *)ptr;
    if (riff->riff_id != ID_RIFF || riff->wave_id != ID_WAVE) {
        fprintf(stderr, "Error: Not a valid WAV file (Bad RIFF/WAVE signature)\n");
        return -1;
    }
    
    size_t offset = sizeof(struct riff_wave_header);
    bool found_fmt = false;
    bool found_data = false;

    while (offset < file_size) {
        if (offset + sizeof(struct chunk_header) > file_size) {
            break; 
        }

        struct chunk_header *chk = (struct chunk_header *)(ptr + offset);
        uint32_t chunk_id = chk->id;
        uint32_t chunk_size = chk->sz;
        
        offset += sizeof(struct chunk_header);

        if (offset > file_size || chunk_size > file_size - offset) {
            if (chunk_id == ID_DATA) {
                chunk_size = file_size - offset;
                fprintf(stderr, "Warning: Data chunk truncated, playing available data.\n");
            } else {
                fprintf(stderr, "Warning: Corrupted metadata chunk found, stopping scan.\n");
                break;
            }
        }

        if (chunk_id == ID_FMT) {
            if (chunk_size < sizeof(struct chunk_fmt)) {
                fprintf(stderr, "Error: FMT chunk too small (%u)\n", chunk_size);
                return -1;
            }
            memcpy(&ctx->fmt, ptr + offset, sizeof(struct chunk_fmt));
            
            /* Read WAVE_FORMAT_EXTENSIBLE */
            ctx->valid_bits_per_sample = ctx->fmt.bits_per_sample;
            ctx->is_float_ext = false;

            if (ctx->fmt.audio_format == 0xFFFE && chunk_size >= sizeof(struct chunk_fmt) + 24) {
                /* Read ValidBitsPerSample (offset +18 from chunk start) */
                ctx->valid_bits_per_sample = *(uint16_t *)(ptr + offset + sizeof(struct chunk_fmt) + 2);
                /* Read GUID (offset +24 from chunk start). 0x03 = Float, 0x01 = PCM */
                uint8_t *guid = (uint8_t *)(ptr + offset + sizeof(struct chunk_fmt) + 8);
                if (guid[0] == 0x03) { 
                    ctx->is_float_ext = true;
                }
            }
            found_fmt = true;
        } else if (chunk_id == ID_DATA) {
            ctx->data_start = ptr + offset;
            
            if (chunk_size == 0 || chunk_size == 0xFFFFFFFF || chunk_size > file_size - offset) {
                ctx->data_size = file_size - offset; /* Trust the actual file size */
            } else {
                ctx->data_size = chunk_size;
            }
            
            ctx->play_offset = 0;
            found_data = true;
        }
        
        size_t real_advance = chunk_size + (chunk_size % 2);
        
        if (file_size - offset < real_advance) {
            offset = file_size;
        } else {
            offset += real_advance;
        }
        
        if (found_fmt && found_data)
            break;
    }

    if (!found_fmt || !found_data) {
        fprintf(stderr, "Error: Required WAV chunks (FMT/DATA) not found.\n");
        return -1;
    }
    return 0;
}

static int
map_file(struct ctx *ctx, struct cmd *cmd)
{
    /* Open read-only */
    ctx->fd = open(cmd->filename, O_RDONLY | O_CLOEXEC | O_NOATIME);
    if (ctx->fd < 0)
        ctx->fd = open(cmd->filename, O_RDONLY | O_CLOEXEC);
    if (ctx->fd < 0) {
        perror("Failed to open file");
        return -1;
    }

    struct stat sb; 
    if (fstat(ctx->fd, &sb) == -1) {
        perror("fstat failed");
        close(ctx->fd);
        return -1;
    }
    
    atomic_init(&ctx->live_file_size, sb.st_size);

    if (cmd->expected_size > 0) {
        /* Map to expected_size for live streaming. Pages will be mapped upon write. */
        ctx->map_size = cmd->expected_size;
    } else {
        ctx->map_size = sb.st_size;
    }

    int flags = MAP_SHARED;
    if (cmd->expected_size == 0) {
        /* Pre-populate memory for fully available files to prevent page faults */
        flags |= MAP_POPULATE;
    }

    ctx->map_start = mmap(NULL, ctx->map_size, PROT_READ, flags, ctx->fd, 0);

    if (ctx->map_start == MAP_FAILED) {
        perror("mmap failed");
        close(ctx->fd);
        return -1;
    }

#ifdef MADV_HUGEPAGE
    madvise(ctx->map_start, ctx->map_size, MADV_HUGEPAGE);
#endif
    madvise(ctx->map_start, ctx->map_size, MADV_SEQUENTIAL);
    if (cmd->expected_size == 0) {
        madvise(ctx->map_start, ctx->map_size, MADV_WILLNEED);
    }

    return 0;
}

static int
ctx_init(struct ctx *ctx, struct cmd *cmd)
{
    memset(ctx, 0, sizeof(struct ctx)); 
    ctx->fd = -1; 
    ctx->map_start = MAP_FAILED;

    if (map_file(ctx, cmd) != 0) {
        return -1;
    }

    if (cmd->filetype && strcmp(cmd->filetype, "raw") == 0) {
        ctx->data_start = ctx->map_start;
        ctx->data_size = (cmd->expected_size > 0) ? cmd->expected_size : ctx->map_size;
        ctx->play_offset = 0;
        
        if (cmd->bits == 32) {
            cmd->format = SND_PCM_FORMAT_S32_LE;
        } else if (cmd->bits == 24) {
            cmd->format = SND_PCM_FORMAT_S24_3LE;
        } else {
            cmd->format = SND_PCM_FORMAT_S16_LE;
        }
    } else {
        if (parse_wav_headers(ctx) != 0) {
            munmap(ctx->map_start, ctx->map_size);
            close(ctx->fd);
            return -1;
        }

        cmd->channels = ctx->fmt.num_channels;
        cmd->rate = ctx->fmt.sample_rate;
        cmd->bits = ctx->valid_bits_per_sample; /* Use actual bit depth */

        /* Determine container for 24-bit */
        int container_bytes = ctx->fmt.block_align / cmd->channels;

        if (ctx->fmt.audio_format == WAVE_FORMAT_IEEE_FLOAT || ctx->is_float_ext) {
            cmd->format = (cmd->bits == 64) ? SND_PCM_FORMAT_FLOAT64_LE : SND_PCM_FORMAT_FLOAT_LE;
            cmd->is_float = true;
        } else {
            switch (cmd->bits) {
            case 8:
                cmd->format = SND_PCM_FORMAT_U8;
                break;
            case 16:
                cmd->format = SND_PCM_FORMAT_S16_LE;
                break;
            case 24: 
                if (container_bytes == 3) {
                    cmd->format = SND_PCM_FORMAT_S24_3LE;
                } else if (container_bytes == 4) {
                    cmd->format = SND_PCM_FORMAT_S24_LE;
                } else {
                    fprintf(stderr, "Fatal: Unsupported 24-bit container\n");
                    return -1;
                }
                break;
            case 32:
                cmd->format = SND_PCM_FORMAT_S32_LE;
                break;
            default:
                cmd->format = SND_PCM_FORMAT_S16_LE;
                break;
            }
        }
    }

    char dev_name[64];
    snprintf(dev_name, sizeof(dev_name), "hw:%u,%u", cmd->card, cmd->device);

    /* Disable software resampling and mixing */
    int open_mode = SND_PCM_NO_AUTO_RESAMPLE | SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT | SND_PCM_NO_SOFTVOL;

    int err = snd_pcm_open(&ctx->pcm, dev_name, SND_PCM_STREAM_PLAYBACK, open_mode);
    if (err < 0) {
        fprintf(stderr, "Fatal: snd_pcm_open failed for '%s': %s\n", dev_name, snd_strerror(err));
        return -1;
    }

    snd_pcm_hw_params_t *hwparams;
    snd_pcm_hw_params_alloca(&hwparams);
    snd_pcm_hw_params_any(ctx->pcm, hwparams);

    snd_pcm_access_t access = cmd->use_mmap ? SND_PCM_ACCESS_MMAP_INTERLEAVED : SND_PCM_ACCESS_RW_INTERLEAVED;
    if (snd_pcm_hw_params_set_access(ctx->pcm, hwparams, access) < 0) {
        fprintf(stderr, "Warning: MMAP not supported. Falling back to RW_INTERLEAVED.\n");
        cmd->use_mmap = false;
        snd_pcm_hw_params_set_access(ctx->pcm, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED);
    }

    if ((err = snd_pcm_hw_params_set_format(ctx->pcm, hwparams, cmd->format)) < 0) {
        fprintf(stderr, "Fatal: Hardware rejected format %s (%s)\n", snd_pcm_format_name(cmd->format), snd_strerror(err));
        return -1;
    }

    if ((err = snd_pcm_hw_params_set_channels(ctx->pcm, hwparams, cmd->channels)) < 0) {
        fprintf(stderr, "Fatal: Hardware rejected %u channels (%s)\n", cmd->channels, snd_strerror(err));
        return -1;
    }

    /* Disable ALSA software resampler */
    if ((err = snd_pcm_hw_params_set_rate_resample(ctx->pcm, hwparams, 0)) < 0) {
        fprintf(stderr, "Fatal: Cannot disable ALSA software resampler (%s)\n", snd_strerror(err));
        return -1;
    }

    /* Require exact sample rate */
    if ((err = snd_pcm_hw_params_set_rate(ctx->pcm, hwparams, cmd->rate, 0)) < 0) {
        fprintf(stderr, "Fatal: DAC rejected exact rate %u Hz (%s)\n", cmd->rate, snd_strerror(err));
        return -1;
    }
    
    /* Hardware parameters configuration: period size, buffer size, memory alignment */
    int dir = 0;
    snd_pcm_uframes_t req_period_size;
    unsigned int req_periods;

    if (cmd->period_size == 0 || cmd->period_count == 0) {
        /* Disallow fractional periods for DMA ring buffer stability */
        snd_pcm_hw_params_set_periods_integer(ctx->pcm, hwparams);

        /* Query hardware constraints */
        snd_pcm_uframes_t hw_max_buffer, hw_max_period, hw_min_period;
        unsigned int hw_min_periods, hw_max_periods;
        
        if (snd_pcm_hw_params_get_buffer_size_max(hwparams, &hw_max_buffer) < 0)
            hw_max_buffer = 65536;
        
        dir = 0;
        if (snd_pcm_hw_params_get_period_size_max(hwparams, &hw_max_period, &dir) < 0)
            hw_max_period = hw_max_buffer / 2;
        
        dir = 0;
        if (snd_pcm_hw_params_get_period_size_min(hwparams, &hw_min_period, &dir) < 0)
            hw_min_period = 16;
        
        dir = 0;
        if (snd_pcm_hw_params_get_periods_min(hwparams, &hw_min_periods, &dir) < 0)
            hw_min_periods = 2;
        
        dir = 0;
        if (snd_pcm_hw_params_get_periods_max(hwparams, &hw_max_periods, &dir) < 0)
            hw_max_periods = 1024;
        
        if (hw_min_periods < 2)
            hw_min_periods = 2;
        if (hw_min_period == 0)
            hw_min_period = 16;

        /* Calculate safe maximum period to prevent ALSA solver overflow */
        snd_pcm_uframes_t absolute_safe_max_period = hw_max_buffer / hw_min_periods;
        if (hw_max_period > absolute_safe_max_period) {
            hw_max_period = absolute_safe_max_period; 
        }

        /* Force minimum of 3 periods for buffers < 50 ms to prevent XRUNs */
        if (hw_max_buffer < (cmd->rate / 20) && hw_max_periods >= 3) {
            req_period_size = hw_max_buffer / 3; 
        } else {
            req_period_size = hw_max_period;     
        }

        /* Calculate hardware memory alignment (Page -> L1 Cache -> RAM Bus) */
        size_t frame_bytes = (snd_pcm_format_physical_width(cmd->format) / 8) * cmd->channels;
        snd_pcm_uframes_t align_step = 1; 
        
        if (likely(frame_bytes > 0)) {
            long page_size = sysconf(_SC_PAGESIZE);
            if (page_size <= 0)
                page_size = 4096;
            
            long cache_line = 64; 
#ifdef _SC_LEVEL1_DCACHE_LINESIZE
            long sys_cache = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
            if (sys_cache > 0)
                cache_line = sys_cache;
#endif
            long bus_word = 8;

            size_t a = frame_bytes, b = page_size, temp;
            while (b) {
                temp = b;
                b = a % b;
                a = temp;
            }
            snd_pcm_uframes_t optimal_page_block = page_size / a;

            a = frame_bytes;
            b = cache_line;
            while (b) {
                temp = b;
                b = a % b;
                a = temp;
            }
            snd_pcm_uframes_t optimal_cache_block = cache_line / a;

            a = frame_bytes;
            b = bus_word;
            while (b) {
                temp = b;
                b = a % b;
                a = temp;
            }
            snd_pcm_uframes_t optimal_bus_block = bus_word / a;

            align_step = optimal_bus_block; 

            if (req_period_size >= optimal_page_block) {
                align_step = optimal_page_block;
            } else if (req_period_size >= optimal_cache_block) {
                align_step = optimal_cache_block;
            }

            req_period_size = (req_period_size / align_step) * align_step;

            if (req_period_size == 0) {
                req_period_size = align_step;
            }
        }

        if (req_period_size < hw_min_period) {
            snd_pcm_uframes_t aligned_min = ((hw_min_period + align_step - 1) / align_step) * align_step;
            
            if (aligned_min <= hw_max_period) {
                req_period_size = aligned_min;
            } else {
                req_period_size = hw_min_period; 
            }
        }

        req_periods = hw_max_buffer / req_period_size;
        
        if (req_periods > hw_max_periods)
            req_periods = hw_max_periods;
        if (req_periods < hw_min_periods)
            req_periods = hw_min_periods;

    } else {
        /* Manual mode from command line arguments */
        req_period_size = cmd->period_size;
        req_periods = cmd->period_count;
    }

    /* Apply parameters to ALSA driver */
    snd_pcm_uframes_t actual_period = req_period_size;
    dir = 0; /* Nearest */
    if (snd_pcm_hw_params_set_period_size_near(ctx->pcm, hwparams, &actual_period, &dir) < 0) {
        fprintf(stderr, "Error: Driver rejected period size configuration.\n");
    }

    unsigned int actual_periods = req_periods;
    dir = 0; 
    if (snd_pcm_hw_params_set_periods_near(ctx->pcm, hwparams, &actual_periods, &dir) < 0) {
        fprintf(stderr, "Error: Driver rejected period count configuration.\n");
    }

    /* Commit hardware parameters */
    if ((err = snd_pcm_hw_params(ctx->pcm, hwparams)) < 0) {
        fprintf(stderr, "Error: Failed to commit ALSA hw_params (%s)\n", snd_strerror(err));
        return -1; 
    }

    /* Sync actual values back to context */
    dir = 0;
    snd_pcm_hw_params_get_period_size(hwparams, &cmd->period_size, &dir);
    snd_pcm_uframes_t actual_buffer;
    snd_pcm_hw_params_get_buffer_size(hwparams, &actual_buffer); 
    cmd->period_count = actual_buffer / cmd->period_size;

    /* Configure software parameters */
    snd_pcm_sw_params_t *swparams;
    snd_pcm_sw_params_alloca(&swparams);
    snd_pcm_sw_params_current(ctx->pcm, swparams);
    
    /* Wake up only when a full period is available */
    snd_pcm_sw_params_set_avail_min(ctx->pcm, swparams, cmd->period_size);
    
    /* Start playback immediately after filling the first period (using actual_buffer size) */
    snd_pcm_sw_params_set_start_threshold(ctx->pcm, swparams, actual_buffer);
    
    /* Stop on underrun when buffer is completely empty */
    snd_pcm_sw_params_set_stop_threshold(ctx->pcm, swparams, actual_buffer);
    
    /* Disable timestamps for performance */
    snd_pcm_sw_params_set_tstamp_mode(ctx->pcm, swparams, SND_PCM_TSTAMP_NONE);
    
    if ((err = snd_pcm_sw_params(ctx->pcm, swparams)) < 0) {
        fprintf(stderr, "Warning: Failed to set ALSA sw_params (%s)\n", snd_strerror(err));
    }

    /* Prepare buffer for initial start */
    snd_pcm_prepare(ctx->pcm);

    /* Disable internal alsa-lib locks */
    snd_pcm_nonblock(ctx->pcm, 1);

    /* Retrieve direct Kernel FD for lock-free ALSA bypass */
    ctx->alsa_fd = -1;
    if (snd_pcm_type(ctx->pcm) == SND_PCM_TYPE_HW) {
        int count = snd_pcm_poll_descriptors_count(ctx->pcm);
        if (count >= 1 && count < 16) {
            struct pollfd pfds[16];
            if (snd_pcm_poll_descriptors(ctx->pcm, pfds, count) == count) {
                ctx->alsa_fd = pfds[0].fd;
            }
        }
    } else {
        fprintf(stderr, "Warning: PCM is not direct HW. IOCTL bypass disabled.\n");
    }

    return 0;
}

void
init_shm(uint32_t total_frames, struct cmd *cmd)
{
    const char *shm_path = cmd ? cmd->shm_file : g_shm_path;
    int fd = open(shm_path, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0)
        return;

    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
    if (fcntl(fd, F_SETLK, &fl) == -1) {
        fprintf(stderr, "Error: Another instance is running (locked SHM at %s).\n", shm_path);
        close(fd);
        exit(EXIT_FAILURE); 
    }
    
    fchmod(fd, 0666); 
    ftruncate(fd, sizeof(struct player_ctrl));
    
    shm = mmap(NULL, sizeof(struct player_ctrl), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    
    if (shm != MAP_FAILED) {
        /* Verify if SHM was initialized externally */
        uint32_t current_magic = atomic_load_explicit(&shm->magic, memory_order_acquire);
        
        if (current_magic != 0xDEADBEEF) {
            /* Fresh memory, initialize completely */
            atomic_init(&shm->command, CMD_NONE);
            atomic_init(&shm->state, STATE_PAUSED);
            atomic_init(&shm->total_frames, total_frames);
            if (cmd) {
                atomic_init(&shm->sample_rate, cmd->rate);
                atomic_init(&shm->channels, cmd->channels);
                atomic_init(&shm->hw_period_size, cmd->period_size);
                atomic_init(&shm->hw_buffer_size, cmd->period_size * cmd->period_count);
                atomic_init(&shm->hw_format, cmd->format);
                atomic_init(&shm->hw_access, cmd->use_mmap ? 3 : 4); /* 3 = MMAP, 4 = RW */
            }
            atomic_store_explicit(&shm->magic, 0xDEADBEEF, memory_order_release);
        } else {
            /* Memory already initialized, update playback state */
            atomic_store_explicit(&shm->state, STATE_PAUSED, memory_order_release);
            atomic_store_explicit(&shm->total_frames, total_frames, memory_order_release);
            if (cmd) {
                atomic_store_explicit(&shm->sample_rate, cmd->rate, memory_order_release);
                atomic_store_explicit(&shm->channels, cmd->channels, memory_order_release);
                atomic_store_explicit(&shm->hw_period_size, cmd->period_size, memory_order_release);
                atomic_store_explicit(&shm->hw_buffer_size, cmd->period_size * cmd->period_count, memory_order_release);
                atomic_store_explicit(&shm->hw_format, cmd->format, memory_order_release);
                atomic_store_explicit(&shm->hw_access, cmd->use_mmap ? 3 : 4, memory_order_release);
            }
        }
        atomic_thread_fence(memory_order_seq_cst);
    }
}

void
ctx_free(struct ctx *ctx)
{
    if (ctx->pcm)
        snd_pcm_close(ctx->pcm);
    if (ctx->map_start != MAP_FAILED)
        munmap(ctx->map_start, ctx->map_size);
    if (ctx->fd >= 0)
        close(ctx->fd);
}

/* Arguments for the watcher thread */
struct watcher_args {
    pthread_t main_tid;
    struct ctx *ctx;
    struct cmd *cmd;
    int cmd_efd;
};

/* Thread listening to SHM and reacting to state changes */
static void *
shm_watcher_thread(void *arg)
{
    /* Lower thread priority to prevent audio interference */
    struct sched_param sp;
    sp.sched_priority = 0;
    sched_setscheduler(0, SCHED_OTHER, &sp);
    setpriority(PRIO_PROCESS, 0, 19);

    struct watcher_args *wargs = (struct watcher_args *)arg;
    pthread_t main_tid = wargs->main_tid;
    struct ctx *ctx = wargs->ctx;
    
    while (!atomic_load(&stop_flag)) {
        int current_cmd = atomic_load_explicit(&shm->command, memory_order_acquire);
        
        sys_futex((int *)&shm->command, FUTEX_WAIT, current_cmd, NULL, NULL, 0);
        
        if (atomic_load(&stop_flag))
            break;
        int new_cmd = atomic_load_explicit(&shm->command, memory_order_acquire);

        /* Direct kernel bypass for real-time timer synchronization */
        if (new_cmd == CMD_UPDATE_TIME) {
            snd_pcm_sframes_t delay_frames = 0;
            
            if (wargs->ctx->alsa_fd >= 0) {
                if (ioctl(wargs->ctx->alsa_fd, SNDRV_PCM_IOCTL_DELAY, &delay_frames) < 0) {
                    delay_frames = 0;
                }
            }

            /* Filter invalid hardware reports */
            long max_hw_buffer = wargs->cmd->period_size * wargs->cmd->period_count;
            if (delay_frames < 0 || (max_hw_buffer > 0 && delay_frames > max_hw_buffer)) {
                delay_frames = 0;
            }

            /* Lock-free acoustic position calculation */
            size_t base_frames = atomic_load_explicit(&wargs->ctx->sync_base_frames, memory_order_acquire);
            long acoustic = (long)base_frames - delay_frames;
            if (acoustic < 0)
                acoustic = 0;

            atomic_store_explicit(&shm->current_frame, acoustic, memory_order_relaxed);
            
            int expected_cmd = CMD_UPDATE_TIME;
            atomic_compare_exchange_strong_explicit(&shm->command, &expected_cmd, CMD_NONE, memory_order_release, memory_order_relaxed);
            sys_futex((int *)&shm->command, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
            
            continue;
        }

        if (new_cmd == CMD_PAUSE || new_cmd == CMD_SEEK || new_cmd == CMD_EXIT || new_cmd == CMD_RESUME) {
            uint64_t val = 1;
            /* Asynchronous zero-signal wakeup for the RT thread's poll() */
            write(wargs->cmd_efd, &val, sizeof(val)); 
        }
    }
    return NULL;
}

/* RT Memory Prefetcher
   Isolates main audio thread from disk latency (Page Faults). */
static void *
memory_warmer_thread(void *arg)
{
    /* Lower priority to avoid resource contention with audio */
    struct sched_param sp;
    sp.sched_priority = 0;
    sched_setscheduler(0, SCHED_OTHER, &sp);
    setpriority(PRIO_PROCESS, 0, 19); 

    struct ctx *ctx = (struct ctx *)arg;
    const size_t page_size = sysconf(_SC_PAGESIZE);
    size_t warmed_offset = 0;
    
    while (!atomic_load(&stop_flag)) {
        bool is_ffmpeg_done = (shm && atomic_load_explicit(&shm->exact_total_frames, memory_order_acquire) > 0);
        struct stat st;
        
        /* Query physical file size in the background telemetry thread */
        if (fstat(ctx->fd, &st) == 0 && st.st_size > 0) {
            size_t available_size = st.st_size;
            if (available_size > ctx->map_size)
                available_size = ctx->map_size;
            
            /* 1. Page-in the newly decoded file chunks first.
               All page faults are processed on this low-priority thread. */
            if (warmed_offset < available_size) {
                /* Asynchronously ask the kernel to prepare the ENTIRE chunk at once.
                   One syscall instead of thousands to prevent VMA lock contention. */
                madvise(ctx->map_start + warmed_offset, available_size - warmed_offset, MADV_WILLNEED);
                
                while (warmed_offset < available_size && !atomic_load(&stop_flag)) {
                    /* Synchronously force a hardware Page Fault */
                    __asm__ __volatile__ ("" : : "r" (ctx->map_start[warmed_offset]) : "memory");
                    
                    warmed_offset += page_size;
                }
            }
            
            /* 2. Publish the verified and pre-warmed file size to the RT thread.
               Using memory_order_release guarantees memory consistency across cores. */
            atomic_store_explicit(&ctx->live_file_size, available_size, memory_order_release);
            
            /* Clean exit: background decoding is complete and all pages are locked in RAM */
            if (is_ffmpeg_done && warmed_offset >= available_size) {
                break;
            }
        } else if (is_ffmpeg_done) {
            /* Exit if file is complete but fstat failed (safety net) */
            break;
        }
        
        /* Extremely responsive hardware polling.
           Sleep 2ms to prevent CPU saturation on standard cores while 
           keeping the isolated ALSA RT-thread fully fed. */
        usleep(2000); 
    }
    return NULL;
}

/* Smooth volume fade (fade-in or fade-out) */
static void
apply_fade(uint8_t *buffer, size_t frames, struct cmd *cmd, bool is_fade_in)
{
    if (!buffer || frames == 0)
        return;
    int phys_width = snd_pcm_format_physical_width(cmd->format);

    if (cmd->format == SND_PCM_FORMAT_FLOAT_LE) {
        float *samples = (float *)buffer;
        for (size_t i = 0; i < frames; i++) {
            float vol = (float)i / (float)frames;
            if (!is_fade_in) vol = 1.0f - vol;
            vol *= vol;
            for (unsigned int c = 0; c < cmd->channels; c++)
                samples[i * cmd->channels + c] *= vol;
        }
    } else if (cmd->format == SND_PCM_FORMAT_FLOAT64_LE) {
        double *samples = (double *)buffer;
        for (size_t i = 0; i < frames; i++) {
            double vol = (double)i / (double)frames;
            if (!is_fade_in) vol = 1.0 - vol;
            vol *= vol;
            for (unsigned int c = 0; c < cmd->channels; c++)
                samples[i * cmd->channels + c] *= vol;
        }
    } else if (phys_width == 16) {
        int16_t *samples = (int16_t *)buffer;
        for (size_t i = 0; i < frames; i++) {
            uint32_t base_vol = (uint32_t)(((uint64_t)i << 16) / frames);
            uint32_t vol = is_fade_in ? base_vol : 65536 - base_vol;
            uint32_t vol_sq = (uint32_t)(((uint64_t)vol * vol) >> 16);
            for (unsigned int c = 0; c < cmd->channels; c++) 
                samples[i * cmd->channels + c] = (int16_t)(((samples[i * cmd->channels + c] * (int32_t)vol_sq) + 32768) >> 16);
        }
    } else if (phys_width == 32) {
        int32_t *samples = (int32_t *)buffer;
        for (size_t i = 0; i < frames; i++) {
            uint32_t base_vol = (uint32_t)(((uint64_t)i << 16) / frames);
            uint32_t vol = is_fade_in ? base_vol : 65536 - base_vol;
            uint32_t vol_sq = (uint32_t)(((uint64_t)vol * vol) >> 16);
            for (unsigned int c = 0; c < cmd->channels; c++) 
                samples[i * cmd->channels + c] = (int32_t)((((int64_t)samples[i * cmd->channels + c] * vol_sq) + 32768) >> 16);
        }
    } else if (phys_width == 24) {
        uint8_t *samples = (uint8_t *)buffer;
        for (size_t i = 0; i < frames; i++) {
            uint32_t base_vol = (uint32_t)(((uint64_t)i << 16) / frames);
            uint32_t vol = is_fade_in ? base_vol : 65536 - base_vol;
            uint32_t vol_sq = (uint32_t)(((uint64_t)vol * vol) >> 16);
            for (unsigned int c = 0; c < cmd->channels; c++) {
                uint8_t *p = samples + (i * cmd->channels + c) * 3;
                
                int32_t val = p[0] | (p[1] << 8) | (p[2] << 16);
                
                if (val & 0x00800000) {
                    val |= 0xFF000000;
                }

                val = (int32_t)((((int64_t)val * vol_sq) + 32768) >> 16);

                p[0] = val & 0xFF;
                p[1] = (val >> 8) & 0xFF;
                p[2] = (val >> 16) & 0xFF;
            }
        }
    }
}

/* Returns the exact number of bytes physically available in mapped memory */
static size_t
get_available_bytes(struct ctx *ctx, struct cmd *cmd, size_t offset)
{
    size_t header_size = ctx->data_start - ctx->map_start;
    size_t absolute_offset = header_size + offset;

    /* Telemetry Optimization: read atomic size published by background prefetcher.
       For static files (expected_size == 0), this is initialized to final file size.
       For live files, warmer_thread securely updates this value.
       RT thread never leaves Userspace! No VFS locks! */
    size_t live_size = atomic_load_explicit(&ctx->live_file_size, memory_order_acquire);
    
    if (live_size > absolute_offset) {
        size_t available = live_size - absolute_offset;
        
        /* Optional constraint for perfectly valid WAV headers */
        if (ctx->data_size > 0 && available > (ctx->data_size - offset)) {
            available = ctx->data_size - offset;
        }
        return available;
    }
    
    /* If prefetcher hasn't warmed enough data, we return 0. 
       play_sample() will safely wait (usleep) without making blocking syscalls. */
    return 0;
}

/* Safely queries the ALSA hardware delay in frames via direct kernel bypass. */
static long
get_safe_alsa_delay(struct ctx *ctx, long max_hw_buffer)
{
    if (!ctx || ctx->alsa_fd < 0)
        return 0;

    snd_pcm_sframes_t delay_sf = 0;
    if (ioctl(ctx->alsa_fd, SNDRV_PCM_IOCTL_DELAY, &delay_sf) < 0)
        return 0;

    long delay_frames = (long)delay_sf;
    if (delay_frames < 0)
        return 0;

    /* Filter out invalid delays from buggy ALSA drivers */
    if (max_hw_buffer > 0 && delay_frames > max_hw_buffer)
        return 0;

    return delay_frames;
}

/* Waits for ALSA hardware to become ready or for a command to arrive.
   Bypasses alsa-lib locks entirely by polling the kernel FD directly.
   Returns true if interrupted by a command. */
static bool
wait_for_alsa_or_command(struct ctx *ctx, int cmd_efd, int timeout_ms)
{
    if (!ctx || ctx->alsa_fd < 0) {
        usleep(2000);
        return false;
    }

    struct pollfd pfds[2];

    /* ALSA hardware descriptor directly from kernel */
    pfds[0].fd = ctx->alsa_fd;
    pfds[0].events = POLLOUT | POLLERR | POLLNVAL;
    pfds[0].revents = 0;

    /* Command eventfd */
    pfds[1].fd = cmd_efd;
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;

    poll(pfds, 2, timeout_ms);

    /* Interrupted by watcher thread */
    if (pfds[1].revents & POLLIN) {
        uint64_t dummy;
        read(cmd_efd, &dummy, sizeof(dummy));
        return true;
    }

    /* Note: If hardware has an XRUN (POLLERR), poll wakes up instantly.
       We don't call snd_pcm_recover here; instead we let ALSA_WRITE 
       catch it as -EPIPE on the next loop iteration to keep logic centralized. */
    
    return false;
}

/* Lock-free helper to sync time base with UI */
static inline void
update_sync_base(struct ctx *ctx, struct cmd *cmd, size_t silence_frames)
{
    size_t frame_bytes = (snd_pcm_format_physical_width(cmd->format) / 8) * cmd->channels;
    size_t total_real_frames = ctx->play_offset / frame_bytes;
    atomic_store_explicit(&ctx->sync_base_frames, total_real_frames + silence_frames, memory_order_release);
}

/* --- PLAYBACK LOOP (CHILD) --- */
int play_sample(struct ctx *ctx, struct cmd *cmd);

/* Player internal states */
typedef enum {
    SM_PLAYING,
    SM_PAUSING,
    SM_SLEEPING,
    SM_RESUMING,
    SM_DRAINING
} player_sm_state_t;

int
play_sample(struct ctx *ctx, struct cmd *cmd)
{
#define ALSA_WRITE(pcm, buf, size) \
    (cmd->use_mmap ? snd_pcm_mmap_writei(pcm, buf, size) : snd_pcm_writei(pcm, buf, size))

    size_t chunk_frames = cmd->period_size;
    size_t total_buffer_frames = cmd->period_size * cmd->period_count;
    
    /* Hardware constraint safeguard */
    if (chunk_frames == 0 || chunk_frames > total_buffer_frames) {
        chunk_frames = total_buffer_frames / 2; 
    }

    size_t chunk_bytes = snd_pcm_frames_to_bytes(ctx->pcm, chunk_frames);
    size_t period_bytes = snd_pcm_frames_to_bytes(ctx->pcm, cmd->period_size);

    init_shm(snd_pcm_bytes_to_frames(ctx->pcm, ctx->data_size), cmd);

    /* Initial seek logic for settings reload */
    if (cmd->initial_seek > 0) {
        size_t frame_bytes = (snd_pcm_format_physical_width(cmd->format) / 8) * cmd->channels;
        size_t target_bytes = (size_t)cmd->initial_seek * frame_bytes;
        
        size_t avail = get_available_bytes(ctx, cmd, 0);
        if (target_bytes > avail)
            target_bytes = avail;
        target_bytes -= (target_bytes % frame_bytes);
        
        ctx->play_offset = target_bytes;
        update_sync_base(ctx, cmd, 0);
        
        if (shm) {
            atomic_store_explicit(&shm->current_frame, target_bytes / frame_bytes, memory_order_release);
        }
    }

    setup_child_signals();

    /* Notify client that hardware is initializing */
    if (shm)
        atomic_store_explicit(&shm->state, STATE_PAUSED, memory_order_release);

    /* Start background services */
    int cmd_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    atomic_init(&ctx->sync_base_frames, 0);
    pthread_t main_tid = pthread_self(); 
    struct watcher_args wargs = { .main_tid = main_tid, .ctx = ctx, .cmd = cmd, .cmd_efd = cmd_efd };
    
    pthread_t watcher_tid, warmer_tid;
    pthread_create(&watcher_tid, NULL, shm_watcher_thread, &wargs);
    
    if (cmd->expected_size > 0) {
        pthread_create(&warmer_tid, NULL, memory_warmer_thread, ctx);
    }

    int timeout_ms = ((cmd->period_size * 1000) + cmd->rate - 1) / cmd->rate + 20;
    
    player_sm_state_t sm_state = SM_PLAYING;
    struct timespec retry_interval; 
    bool is_hw_error = false; 
    bool track_completed = false; 

    size_t silence_frames_needed = 0;
    size_t silence_frames_written = 0;

    /* Preallocate silence buffer */
    size_t max_period_bytes = snd_pcm_frames_to_bytes(ctx->pcm, cmd->period_size);
    void *silence_buf = NULL;
    if (posix_memalign(&silence_buf, 64, max_period_bytes) != 0) {
        fprintf(stderr, "Fatal: Failed to allocate aligned silence buffer\n");
        return 1;
    }
    snd_pcm_format_set_silence(cmd->format, silence_buf, cmd->period_size * cmd->channels);
    if (!silence_buf) {
        fprintf(stderr, "Fatal: Failed to allocate silence buffer\n");
        ctx_free(ctx);
        return 1;
    }

    /* Calculate dynamic fade duration */
    size_t optimal_fade_frames = (cmd->rate * 50) / 1000; 
    if (optimal_fade_frames < 64)
        optimal_fade_frames = 64;

    /* Preallocate buffer for realtime fades */
    size_t rt_frame_bytes = (snd_pcm_format_physical_width(cmd->format) / 8) * cmd->channels;
    size_t max_rt_frames = optimal_fade_frames * 2; 
    if (cmd->period_size * 2 > max_rt_frames) {
        max_rt_frames = cmd->period_size * 2;
    }
    uint8_t *rt_fade_buf = NULL;
    if (posix_memalign((void **)&rt_fade_buf, 64, max_rt_frames * rt_frame_bytes) != 0) {
        fprintf(stderr, "Fatal: Failed to allocate aligned RT fade buffer\n");
        free(silence_buf);
        return 1;
    }

    /* Lock allocated memory pages */
    mlock(silence_buf, max_period_bytes);
    mlock(rt_fade_buf, max_rt_frames * rt_frame_bytes);

    snd_pcm_format_set_silence(cmd->format, silence_buf, cmd->period_size * cmd->channels);
    snd_pcm_format_set_silence(cmd->format, rt_fade_buf, max_rt_frames * cmd->channels);
    
    if (!rt_fade_buf) {
        fprintf(stderr, "Fatal: Failed to allocate RT fade buffer\n");
        free(silence_buf);
        ctx_free(ctx);
        return 1;
    }

    /* DAC warm-up */
    if (ctx->pcm) {
        uint32_t wakeup_ms = 80;
        size_t wakeup_frames = (cmd->rate * wakeup_ms) / 1000;
        
        if (wakeup_frames > cmd->period_size) {
            wakeup_frames = cmd->period_size;
        }

        snd_pcm_format_set_silence(cmd->format, rt_fade_buf, wakeup_frames * cmd->channels);
        if (ALSA_WRITE(ctx->pcm, rt_fade_buf, wakeup_frames) < 0) {
            /* Ignore initial warmup errors */
        }
    }

    /* Main state machine loop */
    while (!atomic_load(&stop_flag) && !track_completed) {

        /* Process commands */
        if (unlikely(atomic_load_explicit(&signal_event, memory_order_acquire) || 
            (shm && atomic_load_explicit(&shm->command, memory_order_acquire) != CMD_NONE))) {
            
            atomic_store_explicit(&signal_event, 0, memory_order_release);
            
            /* Drain eventfd to prevent spurious poll() wakeups in the future.
               Since cmd_efd is non-blocking, this instantly clears the counter. 
               Called only when a command actually occurs, preserving Zero-Syscall path! */
            uint64_t dump;
            while (read(cmd_efd, &dump, sizeof(dump)) > 0);
            
            if (shm) {
                int c = atomic_load_explicit(&shm->command, memory_order_acquire);
            
                if (c == CMD_EXIT) {
                    if (ctx->pcm && (sm_state == SM_PLAYING || sm_state == SM_DRAINING)) {
                        /* Temporarily switch to blocking mode to ensure writes */
                        snd_pcm_nonblock(ctx->pcm, 0);

                        long delay_frames = get_safe_alsa_delay(ctx, 0);

                        long safety_margin = 1024; 
                        size_t frame_bytes = (snd_pcm_format_physical_width(cmd->format) / 8) * cmd->channels;
                        
                        long remaining_unfaded_frames = delay_frames;
                        size_t read_offset = ctx->play_offset;

                        if (delay_frames > safety_margin) {
                            unsigned long rewind_amount = delay_frames - safety_margin;
                            snd_pcm_sframes_t actually_rewound = snd_pcm_rewind(ctx->pcm, rewind_amount);
                            
                            if (actually_rewound > 0) {
                                remaining_unfaded_frames = delay_frames - actually_rewound;
                                size_t actual_rewind_bytes = actually_rewound * frame_bytes;
                                read_offset = (ctx->play_offset >= actual_rewind_bytes) ? 
                                              (ctx->play_offset - actual_rewind_bytes) : 0;
                            }
                        }

                        size_t fade_frames = optimal_fade_frames;
                        size_t fade_bytes = fade_frames * frame_bytes;

                        /* Apply fade-out */
                        size_t avail = get_available_bytes(ctx, cmd, read_offset);
                        if (avail >= fade_bytes) {
                            memcpy(rt_fade_buf, ctx->data_start + read_offset, fade_bytes);
                            apply_fade(rt_fade_buf, fade_frames, cmd, false);
                        } else {
                            memset(rt_fade_buf, 0, fade_bytes);
                        }
                        ALSA_WRITE(ctx->pcm, rt_fade_buf, fade_frames);

                        /* Write hardware silence to stabilize DAC */
                        size_t silence_frames = optimal_fade_frames;
                        snd_pcm_format_set_silence(cmd->format, rt_fade_buf, silence_frames * cmd->channels);
                        ALSA_WRITE(ctx->pcm, rt_fade_buf, silence_frames);

                        /* Exit timing synchronization */
                        long total_written_frames = (read_offset / frame_bytes) + fade_frames + silence_frames;
                        long max_real_acoustic = (read_offset / frame_bytes) + fade_frames;

                        while (!atomic_load(&stop_flag)) {
                            if (atomic_load_explicit(&signal_event, memory_order_acquire))
                                break;

                            long d_frames = get_safe_alsa_delay(ctx, 0);
                            if (d_frames <= 0)
                                break;

                            long acoustic = total_written_frames - d_frames;
                            
                            /* Clamp silence to prevent timer drift */
                            if (acoustic > max_real_acoustic)
                                acoustic = max_real_acoustic;
                            if (acoustic < 0)
                                acoustic = 0;
                            
                            if (shm)
                                atomic_store_explicit(&shm->current_frame, acoustic, memory_order_relaxed);

                            usleep(2000); /* 2ms tick */
                        }

                        snd_pcm_drop(ctx->pcm);
                    }
                    atomic_store(&stop_flag, 1);
                    break;
                } else if (c == CMD_PAUSE) {
                    /* Transition to pausing if not already sleeping or pausing */
                    /* If RESUMING, state change to PAUSING will occur on the next iteration */
                    if (sm_state != SM_SLEEPING && sm_state != SM_PAUSING) {
                        sm_state = SM_PAUSING;
                    }
                } else if (c == CMD_RESUME) {
                    /* Clear command immediately to prevent loops */
                    int expected_cmd = CMD_RESUME;
                    atomic_compare_exchange_strong_explicit(&shm->command, &expected_cmd, CMD_NONE, memory_order_release, memory_order_relaxed);
                    
                    if (sm_state == SM_SLEEPING) {
                        sm_state = SM_RESUMING;
                    }
                } else if (c == CMD_SEEK) {
                    int expected_cmd = CMD_SEEK;
                    
                    /* Exclusively acquire command via CAS. Concurrent modifications will be caught on the next iteration. */
                    if (atomic_compare_exchange_strong_explicit(&shm->command, &expected_cmd, CMD_NONE, memory_order_release, memory_order_relaxed)) {
                        
                        /* Read target strictly after successful command acquisition */
                        uint32_t target_frame = atomic_load_explicit(&shm->seek_target, memory_order_acquire);
                        uint32_t total_frames = atomic_load_explicit(&shm->total_frames, memory_order_acquire);

                        /* Handle seek to EOF or beyond */
                        if (target_frame >= total_frames) {
                            if (ctx->pcm) {
                                snd_pcm_drop(ctx->pcm); 
                            }
                            track_completed = true; /* Exit immediately */
                            break; 
                        }

                        if (ctx->pcm) {
                            long delay_frames = get_safe_alsa_delay(ctx, 0);
                            long safety_margin = 1024;
                            bool rewound = false;

                            /* Calculate position for normal seek */
                            size_t frame_bytes = (snd_pcm_format_physical_width(cmd->format) / 8) * cmd->channels;
                            size_t target_bytes = (size_t)target_frame * frame_bytes;

                            size_t real_data_size = get_available_bytes(ctx, cmd, 0);
                            if (target_bytes > real_data_size)
                                target_bytes = real_data_size;
                            target_bytes = (target_bytes / frame_bytes) * frame_bytes;

                            /* Fast rewind path */
                            if (delay_frames > safety_margin) {
                                unsigned long rewind_amount = delay_frames - safety_margin;
                                snd_pcm_sframes_t actually_rewound = snd_pcm_rewind(ctx->pcm, rewind_amount);
                                
                                if (actually_rewound > 0) {
                                    rewound = true;
                                    size_t fade_frames = optimal_fade_frames;
                                    size_t fade_bytes = fade_frames * frame_bytes;
                                    
                                    size_t actual_rewind_bytes = actually_rewound * frame_bytes;
                                    size_t old_tail_offset = (ctx->play_offset >= actual_rewind_bytes) ? (ctx->play_offset - actual_rewind_bytes) : 0;

                                    size_t avail_old = get_available_bytes(ctx, cmd, old_tail_offset);
                                    if (avail_old >= fade_bytes) {
                                        memcpy(rt_fade_buf, ctx->data_start + old_tail_offset, fade_bytes);
                                        apply_fade(rt_fade_buf, fade_frames, cmd, false);
                                        ALSA_WRITE(ctx->pcm, rt_fade_buf, fade_frames); 
                                    }

                                    size_t avail_new = get_available_bytes(ctx, cmd, target_bytes);
                                    if (avail_new >= fade_bytes) {
                                        memcpy(rt_fade_buf, ctx->data_start + target_bytes, fade_bytes);
                                        apply_fade(rt_fade_buf, fade_frames, cmd, true);
                                        
                                        snd_pcm_sframes_t written = ALSA_WRITE(ctx->pcm, rt_fade_buf, fade_frames);
                                        long actual_written = (written > 0) ? written : 0;
                                        ctx->play_offset = target_bytes + (actual_written * frame_bytes);
                                        update_sync_base(ctx, cmd, 0);
                                    } else {
                                        ctx->play_offset = target_bytes;
                                        update_sync_base(ctx, cmd, 0);
                                    }
                                }
                            }

                            /* Deep reset path */
                            if (!rewound) {
                                snd_pcm_drop(ctx->pcm);
                                snd_pcm_prepare(ctx->pcm);

                                size_t period_frames = cmd->period_size;
                                snd_pcm_format_set_silence(cmd->format, rt_fade_buf, period_frames * cmd->channels);

                                size_t silence_frames = 512;
                                if (silence_frames > period_frames)
                                    silence_frames = period_frames;
                                size_t audio_frames = period_frames - silence_frames;
                                size_t audio_bytes = audio_frames * frame_bytes;

                                size_t avail_new = get_available_bytes(ctx, cmd, target_bytes);
                                if (audio_bytes > avail_new) {
                                    audio_bytes = avail_new;
                                    audio_bytes -= (audio_bytes % frame_bytes);
                                    audio_frames = audio_bytes / frame_bytes;
                                }

                                if (audio_bytes > 0) {
                                    memcpy(rt_fade_buf + (silence_frames * frame_bytes), ctx->data_start + target_bytes, audio_bytes);
                                    size_t fi_frames = optimal_fade_frames;
                                    if (fi_frames > audio_frames)
                                        fi_frames = audio_frames;
                                    apply_fade(rt_fade_buf + (silence_frames * frame_bytes), fi_frames, cmd, true);
                                }

                                snd_pcm_sframes_t written = 0;
                                if (silence_frames + audio_frames > 0) {
                                    int retries = 0;
                                    do {
                                        written = ALSA_WRITE(ctx->pcm, rt_fade_buf, silence_frames + audio_frames);
                                        if (written == -EAGAIN) {
                                            usleep(2000); 
                                            retries++;
                                        }
                                    } while (written == -EAGAIN && retries < 10);
                                    
                                    if (written == -EPIPE || written == -ESTRPIPE) {
                                        snd_pcm_recover(ctx->pcm, written, 1);
                                        written = 0; 
                                    }
                                }

                                long actual_written_audio = 0;
                                if (written > 0) {
                                    actual_written_audio = written - silence_frames;
                                    if (actual_written_audio < 0)
                                        actual_written_audio = 0;
                                }

                                ctx->play_offset = target_bytes + (actual_written_audio * frame_bytes);
                                update_sync_base(ctx, cmd, 0);
                                silence_frames_needed = 0;
                                silence_frames_written = 0;
                            }
                            
                            atomic_store_explicit(&shm->current_frame, target_frame, memory_order_release);
                            sm_state = SM_PLAYING;
                        } else {
                            /* Device is paused */
                            size_t frame_bytes = (snd_pcm_format_physical_width(cmd->format) / 8) * cmd->channels;
                            size_t target_bytes = (size_t)target_frame * frame_bytes;

                            size_t real_data_size = get_available_bytes(ctx, cmd, 0);
                            if (target_bytes > real_data_size)
                                target_bytes = real_data_size;
                            target_bytes = (target_bytes / frame_bytes) * frame_bytes;

                            ctx->play_offset = target_bytes;
                            update_sync_base(ctx, cmd, 0);
                            atomic_store_explicit(&shm->current_frame, target_frame, memory_order_release);
                            
                            int expected_none = CMD_NONE;
                            atomic_compare_exchange_strong_explicit(&shm->command, &expected_none, CMD_PAUSE, memory_order_release, memory_order_relaxed);
                            atomic_store_explicit(&shm->state, STATE_PAUSED, memory_order_release);
                            sm_state = SM_SLEEPING;
                        }
                    }
                }
            }
        }

        /* State machine execution */
        switch (sm_state) {
        case SM_PLAYING: {
            /* Safety check */
            if (!ctx->pcm) {
                sm_state = SM_RESUMING;
                break;
            }

            /* Read the EOF flag once for the entire state */
            uint32_t is_ffmpeg_done = 0;
            if (shm)
                is_ffmpeg_done = atomic_load_explicit(&shm->exact_total_frames, memory_order_acquire);

            /* Determine available data from the current position */
            size_t available = get_available_bytes(ctx, cmd, ctx->play_offset);
            size_t frame_bytes = (snd_pcm_format_physical_width(cmd->format) / 8) * cmd->channels;

            if (!available) {
                /* Handle EOF or expected file end */
                if (is_ffmpeg_done > 0 || ctx->play_offset >= ctx->data_size) {
                    /* Transition to DRAINING */
                    long max_hw_buffer = cmd->period_size * cmd->period_count;
                    long current_delay = get_safe_alsa_delay(ctx, max_hw_buffer);

                    /* Drain exactly the remaining samples. Provide a 64-sample (~1.5 ms) safety margin if the driver returns 0. */
                    if (current_delay <= 0) {
                        current_delay = 64; 
                    }

                    silence_frames_needed = current_delay;
                    silence_frames_written = 0;
                    if (shm)
                        atomic_store_explicit(&shm->state, STATE_DRAINING, memory_order_release);
                    
                    sm_state = SM_DRAINING;
                    continue;
                } else {
                    /* Wait for ffmpeg. 1ms sleep yields the core to the scheduler,
                       preventing VRM noise (100% CPU busy-wait) and avoiding false 
                       poll() wakes since ALSA is already starving (POLLOUT is true). */
                    usleep(1000);
                    continue;
                }
            }

            /* Calculate write chunk size */
            size_t bytes_to_write = available;
            if (bytes_to_write > chunk_bytes)
                bytes_to_write = chunk_bytes;
            /* Frame alignment */
            bytes_to_write -= (bytes_to_write % frame_bytes);

            void *data_ptr = ctx->data_start + ctx->play_offset;

            __builtin_prefetch(data_ptr, 0, 3);

            /* Handle incomplete periods */
            if (bytes_to_write > 0 && bytes_to_write < chunk_bytes) {
                if (is_ffmpeg_done > 0) {
                    /* EOF reached. Pad the final incomplete chunk with silence to safely flush it to ALSA. */
                    size_t frames_valid = bytes_to_write / frame_bytes;
                    size_t frames_missing = (chunk_bytes - bytes_to_write) / frame_bytes;
                    
                    /* Copy audio data to buffer */
                    memcpy(rt_fade_buf, data_ptr, bytes_to_write);
                    /* Fill the rest of the period with bit-perfect silence */
                    snd_pcm_format_set_silence(cmd->format, rt_fade_buf + bytes_to_write, frames_missing * cmd->channels);
                    
                    data_ptr = rt_fade_buf;
                    /* Send a full period to the hardware */
                    bytes_to_write = chunk_bytes;
                } else {
                    /* Stream is live, ffmpeg just hasn't delivered a full chunk yet.
                       WE MUST NOT PAD! If we pad, we advance play_offset into empty space 
                       and permanently delete upcoming audio data. Yield and wait for ffmpeg. */
                    usleep(1000);
                    continue;
                }
            }

            /* Unified completion condition */
            if (!bytes_to_write) {
                if (is_ffmpeg_done > 0) {
                    long max_hw_buffer = cmd->period_size * cmd->period_count;
                    long current_delay = get_safe_alsa_delay(ctx, max_hw_buffer);
                    
                    /* Drain exactly the number of samples needed to empty the DAC */
                    if (current_delay <= 0) {
                        current_delay = 64; 
                    }
                    
                    silence_frames_needed = current_delay;
                    silence_frames_written = 0;
                    if (shm)
                        atomic_store_explicit(&shm->state, STATE_DRAINING, memory_order_release);
                    
                    sm_state = SM_DRAINING;
                    continue;
                } else {
                    usleep(1000);
                    continue;
                }
            }

            /* Prefetch data */
            __builtin_prefetch(data_ptr + bytes_to_write, 0, 3);

            /* Write to ALSA */
            snd_pcm_sframes_t written_frames = ALSA_WRITE(ctx->pcm, data_ptr, bytes_to_write / frame_bytes);

            if (written_frames < 0) {
                /* Proactive command check: exit ALSA loop immediately if a signal is received */
                int current_cmd = atomic_load_explicit(&shm->command, memory_order_acquire);
                if (current_cmd == CMD_PAUSE || current_cmd == CMD_SEEK || current_cmd == CMD_EXIT ||
                    atomic_load_explicit(&signal_event, memory_order_acquire)) {
                    break;  
                }

                if (written_frames == -EAGAIN) {
                    if (wait_for_alsa_or_command(ctx, cmd_efd, timeout_ms))
                        break;
                    continue;
                }

                if (written_frames == -EINTR) {
                    break;
                }

                if (written_frames == -EPIPE || written_frames == -ESTRPIPE) {
                    if (snd_pcm_recover(ctx->pcm, written_frames, 1) != 0) {
                        is_hw_error = true;
                        atomic_store(&stop_flag, 1);
                        break;
                    }
                    
                    /* XRUN recovery: apply software fade-in to prevent secondary pops */
                    size_t fade_frames = optimal_fade_frames; 
                    if (fade_frames > bytes_to_write / frame_bytes) {
                        fade_frames = bytes_to_write / frame_bytes;
                    }
                    
                    if (fade_frames > 0 && rt_fade_buf) {
                        memcpy(rt_fade_buf, data_ptr, fade_frames * frame_bytes);
                        apply_fade(rt_fade_buf, fade_frames, cmd, true);
                        
                        snd_pcm_sframes_t rec_write = ALSA_WRITE(ctx->pcm, rt_fade_buf, fade_frames);
                        if (rec_write > 0) {
                            ctx->play_offset += rec_write * frame_bytes;
                            update_sync_base(ctx, cmd, 0);
                        }
                    }
                    /* Continue to recalculate available data */
                    continue;
                }

                /* Handle generic hardware errors */
                is_hw_error = true;
                atomic_store(&stop_flag, 1);
                break;
            }

            if (written_frames > 0) {
                /* Update byte offset */
                ctx->play_offset += written_frames * frame_bytes;
                update_sync_base(ctx, cmd, 0);
                
                if (shm) {
                    int current_st = atomic_load_explicit(&shm->state, memory_order_acquire);
                    if (current_st == STATE_PAUSED) {
                        atomic_store_explicit(&shm->state, STATE_PLAYING, memory_order_release);
                    }

                    /* Update time purely in User-Space (Zero-Syscall).
                       Subtract hardware buffer size to approximate acoustic time. */
                    long hw_buffer_frames = cmd->period_size * cmd->period_count;
                    size_t total_real_frames = ctx->play_offset / frame_bytes;
                    long acoustic = (long)total_real_frames - hw_buffer_frames;

                    if (acoustic < 0) acoustic = 0;
                    
                    atomic_store_explicit(&shm->current_frame, acoustic, memory_order_relaxed);
                }
            } else {
                /* Wait if no frames were written (bypassing alsa-lib wait) */
                if (wait_for_alsa_or_command(ctx, cmd_efd, timeout_ms))
                    break;
            }
            break;
        }

        case SM_PAUSING: {
            /* State: SM_PAUSING (Fade-out and cleanup) */
            if (ctx->pcm) {
                snd_pcm_nonblock(ctx->pcm, 0);

                long delay_frames = get_safe_alsa_delay(ctx, 0);

                long safety_margin = 1024;
                size_t frame_bytes = (snd_pcm_format_physical_width(cmd->format) / 8) * cmd->channels;
                size_t read_offset = ctx->play_offset;

                if (delay_frames > safety_margin) {
                    unsigned long rewind_amount = delay_frames - safety_margin;
                    snd_pcm_sframes_t actually_rewound = snd_pcm_rewind(ctx->pcm, rewind_amount);
                    
                    if (actually_rewound > 0) {
                        size_t actual_rewind_bytes = actually_rewound * frame_bytes;
                        read_offset = (ctx->play_offset >= actual_rewind_bytes) ? 
                                      (ctx->play_offset - actual_rewind_bytes) : 0;
                    }
                }

                size_t fade_frames = optimal_fade_frames;
                size_t fade_bytes = fade_frames * frame_bytes;

                size_t avail = get_available_bytes(ctx, cmd, read_offset);
                if (avail >= fade_bytes) {
                    memcpy(rt_fade_buf, ctx->data_start + read_offset, fade_bytes);
                    apply_fade(rt_fade_buf, fade_frames, cmd, false);
                } else {
                    memset(rt_fade_buf, 0, fade_bytes);
                }
                ALSA_WRITE(ctx->pcm, rt_fade_buf, fade_frames);

                size_t silence_frames = optimal_fade_frames;
                snd_pcm_format_set_silence(cmd->format, rt_fade_buf, silence_frames * cmd->channels);
                ALSA_WRITE(ctx->pcm, rt_fade_buf, silence_frames);

                /* Strict synchronization */
                long total_written_frames = (read_offset / frame_bytes) + fade_frames + silence_frames;
                long max_real_acoustic = (read_offset / frame_bytes) + fade_frames;

                while (!atomic_load(&stop_flag)) {
                    int cmd_now = atomic_load_explicit(&shm->command, memory_order_acquire);
                    if (cmd_now == CMD_RESUME || cmd_now == CMD_SEEK || cmd_now == CMD_EXIT || 
                        atomic_load_explicit(&signal_event, memory_order_acquire)) {
                        break;
                    }

                    long d_frames = get_safe_alsa_delay(ctx, 0);
                    if (d_frames <= 0)
                        break; 

                    long acoustic = total_written_frames - d_frames;
                    
                    /* Clamp */
                    if (acoustic > max_real_acoustic)
                        acoustic = max_real_acoustic;
                    if (acoustic < 0)
                        acoustic = 0;
                    
                    if (shm)
                        atomic_store_explicit(&shm->current_frame, acoustic, memory_order_relaxed);

                    usleep(2000); 
                }

                long final_delay = get_safe_alsa_delay(ctx, 0);
                long final_acoustic = total_written_frames - final_delay;
                
                /* Clamp to prevent skipping audio during resume */
                if (final_acoustic > max_real_acoustic)
                    final_acoustic = max_real_acoustic;
                if (final_acoustic < (long)(read_offset / frame_bytes)) {
                    final_acoustic = read_offset / frame_bytes;
                }

                snd_pcm_drop(ctx->pcm);

                ctx->play_offset = final_acoustic * frame_bytes;
                update_sync_base(ctx, cmd, 0);

                if (shm) {
                    atomic_store_explicit(&shm->current_frame, final_acoustic, memory_order_release);
                    atomic_store_explicit(&shm->state, STATE_PAUSED, memory_order_release);
                }

                snd_pcm_close(ctx->pcm);
                ctx->pcm = NULL;
                ctx->alsa_fd = -1;
            }

            release_usb_dac(cmd);

            fast_revert_system_state(false, false);
            reset_process_priority();

            if (shm) {
                atomic_store_explicit(&shm->state, STATE_PAUSED, memory_order_release);
            }
            sm_state = SM_SLEEPING;
            break;
        }

        case SM_SLEEPING: {
            int current_cmd = atomic_load_explicit(&shm->command, memory_order_acquire);
            
            /* Wake up on any active command */
            if (current_cmd != CMD_NONE && current_cmd != CMD_PAUSE)
                break;
            
            /* Sleep natively on eventfd instead of futex. */
            struct pollfd pfd = { .fd = cmd_efd, .events = POLLIN, .revents = 0 };
            poll(&pfd, 1, -1);
            
            /* Drain eventfd immediately if woken by a command to prevent 
               spurious poll() triggers in subsequent states. */
            if (pfd.revents & POLLIN) {
                uint64_t dump;
                while (read(cmd_efd, &dump, sizeof(dump)) > 0);
            }
            break;
        }

        case SM_RESUMING: {
            /* State: SM_RESUMING (Wake up and fade-in) */
            int chk_cmd = atomic_load_explicit(&shm->command, memory_order_acquire);
            if (chk_cmd == CMD_PAUSE) {
                sm_state = SM_PAUSING;
                break;
            }
            if (chk_cmd == CMD_EXIT) {
                atomic_store(&stop_flag, 1);
                break;
            }

            if (!ctx->pcm) {
                /* Re-acquire DAC and update card number */
                acquire_usb_dac(cmd);
                
                fast_apply_system_state();
                optimize_process(cmd->cpu_core);

                char dev_name[64];
                /* Use the updated card number */
                snprintf(dev_name, sizeof(dev_name), "hw:%u,%u", cmd->card, cmd->device);
                
                int open_mode = SND_PCM_NO_AUTO_RESAMPLE | SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT | SND_PCM_NO_SOFTVOL;
                if (snd_pcm_open(&ctx->pcm, dev_name, SND_PCM_STREAM_PLAYBACK, open_mode) == 0) {
                    snd_pcm_hw_params_t *hwp;
                    snd_pcm_hw_params_alloca(&hwp);
                    snd_pcm_hw_params_any(ctx->pcm, hwp);
                    
                    /* Reapply MMAP access configuration */
                    snd_pcm_access_t access = cmd->use_mmap ? SND_PCM_ACCESS_MMAP_INTERLEAVED : SND_PCM_ACCESS_RW_INTERLEAVED;
                    snd_pcm_hw_params_set_access(ctx->pcm, hwp, access);
                    
                    snd_pcm_hw_params_set_format(ctx->pcm, hwp, cmd->format);
                    snd_pcm_hw_params_set_channels(ctx->pcm, hwp, cmd->channels);
                    snd_pcm_hw_params_set_rate_resample(ctx->pcm, hwp, 0);
                    snd_pcm_hw_params_set_rate(ctx->pcm, hwp, cmd->rate, 0);
                    
                    snd_pcm_uframes_t req_buf = cmd->period_size * cmd->period_count;
                    snd_pcm_hw_params_set_buffer_size_near(ctx->pcm, hwp, &req_buf);
                    
                    snd_pcm_uframes_t req_per = cmd->period_size;
                    int dir = 0;
                    snd_pcm_hw_params_set_period_size_near(ctx->pcm, hwp, &req_per, &dir);
                    snd_pcm_hw_params(ctx->pcm, hwp);

                    snd_pcm_sw_params_t *swp;
                    snd_pcm_sw_params_alloca(&swp);
                    snd_pcm_sw_params_current(ctx->pcm, swp);
                    snd_pcm_sw_params_set_avail_min(ctx->pcm, swp, cmd->period_size);
                    snd_pcm_sw_params_set_start_threshold(ctx->pcm, swp, req_buf);
                    
                    /* Restore stop_threshold and tstamp_mode */
                    snd_pcm_sw_params_set_stop_threshold(ctx->pcm, swp, req_buf);
                    snd_pcm_sw_params_set_tstamp_mode(ctx->pcm, swp, SND_PCM_TSTAMP_NONE);
                    
                    snd_pcm_sw_params(ctx->pcm, swp);
                    
                    /* Ensure hardware is in PREPARED state */
                    snd_pcm_prepare(ctx->pcm);

                    snd_pcm_nonblock(ctx->pcm, 1);

                    /* Update FD for Watcher */
                    ctx->alsa_fd = -1;
                    int count = snd_pcm_poll_descriptors_count(ctx->pcm);
                    if (count >= 1 && count < 16) {
                        struct pollfd pfds[16];
                        if (snd_pcm_poll_descriptors(ctx->pcm, pfds, count) == count) {
                            ctx->alsa_fd = pfds[0].fd;
                        }
                    }
                }
            }

            if (ctx->pcm) {
                /* Soft start sequence */
                size_t frame_bytes = (snd_pcm_format_physical_width(cmd->format) / 8) * cmd->channels;
                size_t period_frames = cmd->period_size;
                
                snd_pcm_format_set_silence(cmd->format, rt_fade_buf, period_frames * cmd->channels);

                size_t silence_frames = 512; 
                if (silence_frames > period_frames)
                    silence_frames = period_frames;
                
                size_t audio_frames = period_frames - silence_frames;
                size_t audio_bytes = audio_frames * frame_bytes;

                size_t avail_new = get_available_bytes(ctx, cmd, ctx->play_offset);
                if (audio_bytes > avail_new) {
                    audio_bytes = avail_new;
                    audio_bytes -= (audio_bytes % frame_bytes);
                    audio_frames = audio_bytes / frame_bytes;
                }

                if (audio_bytes > 0) {
                    memcpy(rt_fade_buf + (silence_frames * frame_bytes), ctx->data_start + ctx->play_offset, audio_bytes);
                    size_t fi_frames = optimal_fade_frames;
                    if (fi_frames > audio_frames)
                        fi_frames = audio_frames;
                    apply_fade(rt_fade_buf + (silence_frames * frame_bytes), fi_frames, cmd, true);
                }

                if (silence_frames + audio_frames > 0) {
                    if (ALSA_WRITE(ctx->pcm, rt_fade_buf, silence_frames + audio_frames) < 0) {
                    }
                }

                ctx->play_offset += audio_bytes;
                update_sync_base(ctx, cmd, 0);
                
                /* STATE_PLAYING will be set by SM_PLAYING on the first physical byte */
                sm_state = SM_PLAYING;
            } else {
                if (ctx->pcm) { 
                    snd_pcm_close(ctx->pcm); 
                    ctx->pcm = NULL; 
                }
                
                /* Sleep for 10ms on eventfd, immediately interruptible by Kotlin commands */
                struct pollfd pfd = { .fd = cmd_efd, .events = POLLIN, .revents = 0 };
                poll(&pfd, 1, 10);
                
                /* Drain eventfd if we were woken by a command rather than a timeout */
                if (pfd.revents & POLLIN) {
                    uint64_t dump;
                    while (read(cmd_efd, &dump, sizeof(dump)) > 0);
                }
                
                if (atomic_load(&stop_flag))
                    break;
            }
            break;
        }

        case SM_DRAINING: {
            /* State: SM_DRAINING */
            if (silence_frames_written >= silence_frames_needed) {
                track_completed = true; /* Real audio has left the speakers */
                break;
            }
            
            size_t chunk_f = cmd->period_size;
            
            /* Do not write more silence than the remaining audio in the DAC */
            if (silence_frames_written + chunk_f > silence_frames_needed) {
                chunk_f = silence_frames_needed - silence_frames_written;
            }
            
            if (chunk_f > 0) {
                /* Write pre-allocated silence to flush the buffer */
                int w = ALSA_WRITE(ctx->pcm, silence_buf, chunk_f);
                
                if (w < 0) {
                    int cmd_now = atomic_load_explicit(&shm->command, memory_order_acquire);
                    if (cmd_now == CMD_PAUSE || cmd_now == CMD_SEEK || atomic_load_explicit(&signal_event, memory_order_acquire)) {
                        break; 
                    }
                    
                    if (w == -EAGAIN) { 
                        if (wait_for_alsa_or_command(ctx, cmd_efd, timeout_ms))
                            break;
                        continue; 
                    }

                    if (w == -EINTR)
                        break;
                    
                    /* Final exit condition */
                    if (w == -EPIPE) { 
                        track_completed = true; 
                        break; 
                    }
                    
                    is_hw_error = true;
                    atomic_store(&stop_flag, 1);
                    break;
                } else {
                    silence_frames_written += w;
                    update_sync_base(ctx, cmd, silence_frames_written);
                }
            }
            
            /* Timer synchronization */
            /* Smooth UI timer updates during drain */
            if (shm) {
                long hw_delay = get_safe_alsa_delay(ctx, 0);

                size_t frame_bytes = (snd_pcm_format_physical_width(cmd->format) / 8) * cmd->channels;
                size_t total_real_frames = ctx->play_offset / frame_bytes;

                /* Calculate frames emitted from the speakers */
                long frames_emitted = (total_real_frames + silence_frames_written) - hw_delay;
                
                /* Restrict timer to valid acoustic frames */
                long acoustic = frames_emitted;
                if (acoustic > total_real_frames)
                    acoustic = total_real_frames;
                if (acoustic < 0)
                    acoustic = 0;
                
                atomic_store_explicit(&shm->current_frame, acoustic, memory_order_relaxed);
            }
            break;
        }
        }
    }

    /* Memory cleanup */
    if (silence_buf) {
        free(silence_buf);
    }
    
    /* Free fade buffer */
    if (rt_fade_buf) {
        free(rt_fade_buf);
    }

    printf("\nPlayed %zu bytes. Remains %zu bytes.\n", ctx->play_offset, ctx->data_size > ctx->play_offset ? ctx->data_size - ctx->play_offset : 0);
    
    if (ctx->pcm) {
        /* Drop PCM immediately as DAC is either empty or playback was interrupted */
        snd_pcm_drop(ctx->pcm);
    }
    
    if (shm) {
        if (is_hw_error) {
            atomic_store_explicit(&shm->state, STATE_ERROR, memory_order_release);
        } else {
            atomic_store_explicit(&shm->state, STATE_COMPLETED, memory_order_release);
        }
    }

    /* Graceful shutdown */
    atomic_store(&stop_flag, 1);
    
    /* Wake up watcher thread to prevent deadlocks */
    atomic_store_explicit(&shm->command, CMD_EXIT, memory_order_release);
    sys_futex((int*)&shm->command, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
    
    pthread_join(watcher_tid, NULL);
    if (cmd->expected_size > 0) {
        pthread_join(warmer_tid, NULL);
    }

    if (cmd_efd >= 0)
        close(cmd_efd);
        
    return 0;
}

int
run_player_child(struct cmd *cmd)
{
    struct ctx ctx;

    /* Apply system tweaks */
    fast_apply_system_state();

    /* Initialize file, WAV, and PCM contexts */
    if (ctx_init(&ctx, cmd) < 0) {
        /* ctx_init handles its own cleanup on failure */
        return 1; 
    }

    /* Apply process optimizations */
    optimize_process(cmd->cpu_core);
    
    printf("playing '%s': %u ch, %u hz, %u-bit ", cmd->filename, cmd->channels,
            cmd->rate, snd_pcm_format_physical_width(cmd->format));
            
    if (cmd->format == SND_PCM_FORMAT_FLOAT_LE) {
        printf("floating-point PCM\n");
    } else {
        printf("signed PCM\n");
    }

    /* Start the main playback loop */
    int res = play_sample(&ctx, cmd);

    /* Release resources */
    ctx_free(&ctx);
    
    return res;
}

void
cmd_init(struct cmd *cmd)
{
    memset(cmd, 0, sizeof(struct cmd));
    cmd->flags = 0;
    cmd->use_mmap = false;
    cmd->initial_seek = 0;

    /* 0 indicates auto-detection */
    cmd->period_size = 0; 
    cmd->period_count = 0;

    cmd->channels = 2;
    cmd->rate = 48000;
    cmd->bits = 16;
    cmd->cpu_core = -1;
    cmd->shm_file = DEFAULT_SHM_FILE;
    cmd->expected_size = 0;

    cmd->usb_uevent_path[0] = '\0';
    cmd->dac_claimed = false;
    cmd->use_unbind = false; 
}

void
print_usage(const char *argv0)
{
    fprintf(stderr, "usage: %s file.wav [options]\n", argv0);
    fprintf(stderr, "options:\n");
    fprintf(stderr, "-D | --card   <card number>    The card to receive the audio\n");
    fprintf(stderr, "-d | --device <device number>  The device to receive the audio\n");
    fprintf(stderr, "-p | --period-size <size>      The size of the PCM's period\n");
    fprintf(stderr, "-n | --period-count <count>    The number of PCM periods\n");
    fprintf(stderr, "-i | --file-type <file-type>   The type of file to read (raw or wav)\n");
    fprintf(stderr, "-c | --channels <count>        The amount of channels per frame\n");
    fprintf(stderr, "-r | --rate <rate>             The amount of frames per second\n");
    fprintf(stderr, "-b | --bits <bit-count>        The number of bits in one sample\n");
    fprintf(stderr, "-f | --float                   The frames are in floating-point PCM\n");
    fprintf(stderr, "-M | --mmap                    Use memory mapped IO to play audio\n");
    fprintf(stderr, "-C | --cpu-core <core>         Isolate player on specific CPU core\n");
    fprintf(stderr, "-S | --shm <path>              Path to SHM control file\n");
    fprintf(stderr, "-E | --expected-size <bytes>   Pre-truncate file to this size for instant streaming\n");
    fprintf(stderr, "-s | --start-frame <frame>     Start playback exactly from this frame\n");
    fprintf(stderr, "-U | --use-unbind              Enable exclusive USB DAC access (unbind/bind trick)\n");
}

int
main(int argc, char **argv)
{
    struct cmd cmd;
    struct optparse opts;

    /* Initialization */
    detect_cpuset_path();
    cmd_init(&cmd);
    optparse_init(&opts, argv);

    /* Parse arguments */
    struct optparse_long long_options[] = {
        { "card", 'D', OPTPARSE_REQUIRED },
        { "device", 'd', OPTPARSE_REQUIRED },
        { "period-size", 'p', OPTPARSE_REQUIRED },
        { "period-count", 'n', OPTPARSE_REQUIRED },
        { "file-type", 'i', OPTPARSE_REQUIRED },
        { "channels", 'c', OPTPARSE_REQUIRED },
        { "rate", 'r', OPTPARSE_REQUIRED },
        { "bits", 'b', OPTPARSE_REQUIRED },
        { "float", 'f', OPTPARSE_NONE },
        { "mmap", 'M', OPTPARSE_NONE },
        { "cpu-core", 'C', OPTPARSE_REQUIRED },
        { "shm", 'S', OPTPARSE_REQUIRED },
        { "expected-size", 'E', OPTPARSE_REQUIRED },
        { "start-frame", 's', OPTPARSE_REQUIRED },
        { "use-unbind", 'U', OPTPARSE_NONE },
        { "help", 'h', OPTPARSE_NONE },
        { 0, 0, 0 }
    };

    int c;
    while ((c = optparse_long(&opts, long_options, NULL)) != -1) {
        switch (c) {
        case 'D':
            sscanf(opts.optarg, "%u", &cmd.card);
            break;
        case 'd':
            sscanf(opts.optarg, "%u", &cmd.device);
            break;
        case 'p':
            sscanf(opts.optarg, "%lu", &cmd.period_size);
            break;
        case 'n':
            sscanf(opts.optarg, "%u", &cmd.period_count);
            break;
        case 'c':
            sscanf(opts.optarg, "%u", &cmd.channels);
            break;
        case 'r':
            sscanf(opts.optarg, "%u", &cmd.rate);
            break;
        case 'i':
            cmd.filetype = opts.optarg;
            break;
        case 'b':
            sscanf(opts.optarg, "%u", &cmd.bits);
            break;
        case 'f':
            cmd.is_float = true;
            break;
        case 'M':
            cmd.use_mmap = true;
            break;
        case 'C':
            sscanf(opts.optarg, "%d", &cmd.cpu_core);
            break;
        case 'S':
            cmd.shm_file = opts.optarg;
            break;
        case 'E':
            sscanf(opts.optarg, "%zu", &cmd.expected_size);
            break;
        case 's':
            sscanf(opts.optarg, "%ld", &cmd.initial_seek);
            break;
        case 'U':
            cmd.use_unbind = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        }
    }

    cmd.filename = optparse_arg(&opts);
    if (!cmd.filename) {
        print_usage(argv[0]);
        return 1;
    }

    /* Acquire USB DAC */
    acquire_usb_dac(&cmd); 

    /* Prepare system state and record current values */
    precalc_system_state(cmd.cpu_core);

    /* Hardware capabilities are checked inside ctx_init */

    /* Fork process */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        return 1;
    }

    if (pid > 0) {
        /* --- SUPERVISOR (ROOT) --- */
        g_child_pid = pid;
        
        /* Supervisor process name */
        const char *DaemonName = "tplayd";

        /* Change name for top/htop by modifying argv[0] */
        strncpy(argv[0], DaemonName, strlen(argv[0]));
        
        /* Clear remainder of the old name */
        for (size_t i = strlen(DaemonName); i < strlen(argv[0]); i++) {
            argv[0][i] = '\0';
        }
        
        /* Change thread name for ps */
        prctl(PR_SET_NAME, DaemonName, 0, 0, 0);

        /* Setup signals: catch TERM/INT and forward to child */
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = supervisor_signal_handler;
        
        /* Omit SA_RESTART to allow waitpid interruption */
        sigaction(SIGINT, &sa, NULL); 
        sigaction(SIGTERM, &sa, NULL);

        int status = 0;
        
        /* Protect waitpid against premature EINTR exits */
        int ret;
        do {
            ret = waitpid(pid, &status, 0);
        } while (ret == -1 && errno == EINTR);

        /* --- CLEANUP ZONE --- */
        /* This code executes unconditionally upon child termination */

        /* Release USB DAC */
        release_usb_dac(&cmd); 
        
        fast_revert_system_state(true, true); 
        unlink(g_shm_path);

        /* Return correct exit code */
        if (WIFSIGNALED(status))
            return 128 + WTERMSIG(status);
        if (WIFEXITED(status))
            return WEXITSTATUS(status);
        
        return 1;

    } else {
        /* --- WORKER (PLAYER) --- */
        prctl(PR_SET_NAME, "tinyplay", 0, 0, 0);
        
        /* Start core logic. System cleanup is handled by the parent supervisor. */
        return run_player_child(&cmd);
    }
}