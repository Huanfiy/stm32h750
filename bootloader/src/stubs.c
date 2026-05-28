/* Minimal newlib syscall stubs for the bootloader. The bootloader does no heap
 * allocation, no exit, no file I/O. Anything that pulls in newlib's reentrant
 * stubs is reduced to either a no-op or a fault. */

#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

extern int _end;

void *_sbrk(int incr)
{
    /* Bootloader never calls malloc; any sbrk request is a bug. */
    (void)incr;
    errno = ENOMEM;
    return (void *)-1;
}

int _close(int file)        { (void)file; return -1; }
int _fstat(int file, struct stat *st) { (void)file; (void)st; return 0; }
int _isatty(int file)       { (void)file; return 1; }
int _lseek(int file, int ptr, int dir) { (void)file; (void)ptr; (void)dir; return 0; }
int _read(int file, char *ptr, int len) { (void)file; (void)ptr; (void)len; return 0; }
int _write(int file, char *ptr, int len) { (void)file; (void)ptr; return len; }
int _getpid(void)           { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
void _exit(int rc)          { (void)rc; while (1) { } }

void Error_Handler(void)
{
    __asm volatile("cpsid i");
    while (1)
    {
        __asm volatile("nop");
    }
}

/* The RT-Thread-patched startup_stm32h750xx.s branches to `entry` instead of
 * `main`. The bootloader is not an RT-Thread image, so provide a tiny
 * trampoline. */
extern int main(void);
int entry(void)
{
    return main();
}
