/*
 * newlib 系统调用桩（GCC 移植的一部分，2026-10-09 由 B 新增）
 *
 * 背景：`-specs=nosys.specs` 会为这些符号提供弱桩，链接时逐条警告
 *       "_close is not implemented and will always fail"。
 *       本工程只用 printf（经 usart.c 里的 _write 重定向到串口），
 *       并不使用文件/堆栈检查等系统调用，所以这里给出**明确的空实现**，
 *       把链接警告消掉，也让"为什么没有文件系统"这件事写在代码里。
 *   ★ 不要删：删掉会退回 nosys 的弱桩并重新出现警告。
 */
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

int _close(int file) { (void)file; return -1; }
int _fstat(int file, struct stat *st) { (void)file; st->st_mode = S_IFCHR; return 0; }
int _isatty(int file) { (void)file; return 1; }
off_t _lseek(int file, off_t ptr, int dir) { (void)file; (void)ptr; (void)dir; return 0; }
int _read(int file, char *ptr, int len) { (void)file; (void)ptr; (void)len; return 0; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
