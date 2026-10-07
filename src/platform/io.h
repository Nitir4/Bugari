#ifndef GHM_PLATFORM_IO_H
#define GHM_PLATFORM_IO_H

/* Include after the system headers in callers. Linux keeps its native APIs;
 * Windows implements the subset used by our checked filesystem operations. */
#ifdef _WIN32
#include <glib.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>

#define GHM_DIRECTORY 0x100000
#define GHM_NOFOLLOW 0x200000
#ifndef O_DIRECTORY
#define O_DIRECTORY GHM_DIRECTORY
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW GHM_NOFOLLOW
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif
#ifndef W_OK
#define W_OK 2
#define X_OK 1
#define F_OK 0
#endif
#ifndef S_IFLNK
#define S_IFLNK 0120000
#endif
#ifndef S_ISLNK
#define S_ISLNK(mode) (((mode) & S_IFMT) == S_IFLNK)
#endif

struct ghm_win_stat {
    uint64_t st_dev, st_ino;
    int64_t st_size;
    unsigned st_mode, st_nlink, st_uid;
    struct timespec st_mtim;
};
int ghm_win_stat(const char *, struct ghm_win_stat *);
int ghm_win_lstat(const char *, struct ghm_win_stat *);
int ghm_win_fstat(int, struct ghm_win_stat *);
int ghm_win_fstatat(int, const char *, struct ghm_win_stat *, int);
int ghm_win_open(const char *, int, ...);
int ghm_win_openat(int, const char *, int, ...);
int ghm_win_mkdir(const char *, unsigned);
int ghm_win_mkdirat(int, const char *, unsigned);
int ghm_win_unlink(const char *);
int ghm_win_unlinkat(int, const char *, int);
int ghm_win_rename(const char *, const char *);
int ghm_win_renameat(int, const char *, int, const char *);
int ghm_win_renameat2(int, const char *, int, const char *, unsigned);
int ghm_win_fsync(int);
int ghm_win_fchmod(int, unsigned);
int ghm_win_access(const char *, int);
int ghm_win_faccessat(int, const char *, int, int);
char *ghm_win_mkdtemp(char *);
char *ghm_win_strndup(const char *, size_t);
int ghm_win_scandir(const char *, struct dirent ***, int (*)(const struct dirent *),
                   int (*)(const struct dirent **, const struct dirent **));
int ghm_win_alphasort(const struct dirent **, const struct dirent **);
FILE *ghm_win_fopen(const char *, const char *);
ssize_t ghm_win_read(int, void *, size_t);
ssize_t ghm_win_write(int, const void *, size_t);
typedef struct { GDir *directory; struct dirent entry; } GhmWinDir;
GhmWinDir *ghm_win_opendir(const char *);
struct dirent *ghm_win_readdir(GhmWinDir *);
int ghm_win_closedir(GhmWinDir *);
/* UTF-8 paths, file identities and no-follow behavior must not use CRT stat. */
#define stat ghm_win_stat
#define lstat ghm_win_lstat
#define fstat ghm_win_fstat
#define fstatat ghm_win_fstatat
#define open ghm_win_open
#define openat ghm_win_openat
#define mkdir ghm_win_mkdir
#define mkdirat ghm_win_mkdirat
#define unlink ghm_win_unlink
#define unlinkat ghm_win_unlinkat
#define rename ghm_win_rename
#define renameat ghm_win_renameat
#define renameat2 ghm_win_renameat2
#define fsync ghm_win_fsync
#define fchmod ghm_win_fchmod
#define access ghm_win_access
#define faccessat ghm_win_faccessat
#define mkdtemp ghm_win_mkdtemp
#define strndup ghm_win_strndup
#define scandir ghm_win_scandir
#define alphasort ghm_win_alphasort
#define fopen ghm_win_fopen
#define read ghm_win_read
#define write ghm_win_write
#define DIR GhmWinDir
#define opendir ghm_win_opendir
#define readdir ghm_win_readdir
#define closedir ghm_win_closedir
#define geteuid() 0U
int ghm_platform_lock(int);
char *ghm_windows_ca_bundle(const char *);
#else
#include <sys/file.h>
static inline int ghm_platform_lock(int fd) { return flock(fd, LOCK_EX | LOCK_NB); }
#endif
#endif
