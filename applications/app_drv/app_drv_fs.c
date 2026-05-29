/*
 * SD-card auto-mount.
 *
 * mmcsd registers `sd0` asynchronously from its own kernel thread once
 * enumeration succeeds. A standalone fs_mnt thread polls for the device,
 * mounts ELM/FATFS at "/", then suspends indefinitely.
 *
 * Why suspend instead of return:
 *   - INIT_APP_EXPORT runs synchronously on the init/main thread and would
 *     block boot for the entire wait window, delaying msh by ~10 s.
 *   - Letting fs_mnt return triggers defunct cleanup on tidle0 (256 B stack),
 *     which overflows when DFS V2 / FatFs cleanup hooks walk fire
 *     (observed: `[E/kernel.sched] thread:tidle0 stack overflow`). Holding
 *     the thread parked keeps its 2 KB stack reserved but is otherwise free
 *     and cleanly sidesteps the tidle0 budget issue without forcing every
 *     user to bump IDLE_THREAD_STACK_SIZE via menuconfig.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <dfs_fs.h>

#define SD_DEV_NAME      "sd0"
#define SD_MOUNT_POINT   "/"
#define SD_FS_TYPE       "elm"
#define SD_WAIT_TICKS_MS 100
#define SD_WAIT_MAX_MS   10000
#define FS_THREAD_PRIO   25
#define FS_THREAD_STACK  2048

static void fs_mount_thread_entry(void *param)
{
    (void)param;

    rt_device_t dev = RT_NULL;
    for (int waited = 0; waited < SD_WAIT_MAX_MS && dev == RT_NULL; waited += SD_WAIT_TICKS_MS) {
        dev = rt_device_find(SD_DEV_NAME);
        if (dev == RT_NULL) {
            rt_thread_mdelay(SD_WAIT_TICKS_MS);
        }
    }

    if (dev == RT_NULL) {
        rt_kprintf("[FS] %s not present after %dms — card missing?\n",
                   SD_DEV_NAME, SD_WAIT_MAX_MS);
    } else if (dfs_mount(SD_DEV_NAME, SD_MOUNT_POINT, SD_FS_TYPE, 0, 0) == 0) {
        rt_kprintf("[FS] %s mounted at %s as %s\n",
                   SD_DEV_NAME, SD_MOUNT_POINT, SD_FS_TYPE);
    } else {
        rt_kprintf("[FS] mount %s -> %s (%s) failed errno=%d\n",
                   SD_DEV_NAME, SD_MOUNT_POINT, SD_FS_TYPE, rt_get_errno());
    }

    /* Park forever — see file header. */
    for (;;) {
        rt_thread_mdelay(60000);
    }
}

static int fs_auto_mount_init(void)
{
    rt_thread_t t = rt_thread_create("fs_mnt", fs_mount_thread_entry, RT_NULL,
                                     FS_THREAD_STACK, FS_THREAD_PRIO, 10);
    if (t == RT_NULL) {
        rt_kprintf("[FS] fs_mnt thread create failed\n");
        return -RT_ENOMEM;
    }
    rt_thread_startup(t);
    return RT_EOK;
}
INIT_APP_EXPORT(fs_auto_mount_init);
