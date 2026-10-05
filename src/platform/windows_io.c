/* Native Windows filesystem adapter. Relative operations use a directory
 * handle, never a check-then-open concatenated pathname. */
#include <windows.h>
#include <winternl.h>
#include <aclapi.h>
#include <wincrypt.h>
#include <glib.h>
#include <sqlite3.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include "platform/io.h"

static int win_error(DWORD code)
{
    switch (code) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: errno = EEXIST; break;
    case ERROR_LOCK_VIOLATION: errno = EAGAIN; break;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: errno = EACCES; break;
    case ERROR_DISK_FULL: errno = ENOSPC; break;
    case ERROR_DIRECTORY: errno = ENOTDIR; break;
    case ERROR_INVALID_PARAMETER: case ERROR_INVALID_NAME: errno = EINVAL; break;
    case ERROR_NOT_SAME_DEVICE: errno = EXDEV; break;
    default: errno = EIO; break;
    }
    return -1;
}

static WCHAR *wide(const char *text)
{
    WCHAR *result = (WCHAR *)g_utf8_to_utf16(text, -1, NULL, NULL, NULL);
    if (result == NULL) errno = EINVAL;
    return result;
}

static HANDLE handle(int fd) { return (HANDLE)_get_osfhandle(fd); }

int ghm_platform_lock(int fd)
{
    OVERLAPPED overlap = {0};
    return LockFileEx(handle(fd), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
        0, 1, 0, &overlap) ? 0 : win_error(GetLastError());
}

char *ghm_windows_ca_bundle(const char *directory)
{
    /* The bundled OpenSSL libraries cannot use MSYS2's build-time CA path.
     * Export the signed-in machine's trusted Windows roots, without disabling
     * verification or shipping somebody else's desktop certificate store. */
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (store == NULL) return NULL;
    GString *pem = g_string_new(NULL);
    PCCERT_CONTEXT cert = NULL;
    while ((cert = CertEnumCertificatesInStore(store, cert)) != NULL) {
        DWORD length = 0;
        if (!CryptBinaryToStringA(cert->pbCertEncoded, cert->cbCertEncoded,
            CRYPT_STRING_BASE64HEADER, NULL, &length)) continue;
        char *encoded = malloc(length);
        if (encoded != NULL && CryptBinaryToStringA(cert->pbCertEncoded, cert->cbCertEncoded,
            CRYPT_STRING_BASE64HEADER, encoded, &length)) g_string_append(pem, encoded);
        free(encoded);
    }
    CertCloseStore(store, 0);
    char *path = g_build_filename(directory, "windows-ca.pem", NULL);
    if (pem->len == 0 || !g_file_set_contents(path, pem->str, (gssize)pem->len, NULL)) {
        g_free(path); path = NULL;
    }
    g_string_free(pem, TRUE); return path;
}

static int descriptor(HANDLE file, int flags)
{
    if (file == INVALID_HANDLE_VALUE) return win_error(GetLastError());
    int fd = _open_osfhandle((intptr_t)file,
        _O_BINARY | _O_NOINHERIT | (flags & (_O_RDONLY | _O_WRONLY | _O_RDWR | _O_APPEND)));
    if (fd < 0) CloseHandle(file);
    return fd;
}

static int is_reparse(HANDLE file)
{
    FILE_ATTRIBUTE_TAG_INFO info;
    if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &info, sizeof(info)))
        return -1;
    return (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

static int check_open(HANDLE file, int flags)
{
    if (file == INVALID_HANDLE_VALUE) return win_error(GetLastError());
    if ((flags & O_NOFOLLOW) && is_reparse(file) != 0) {
        CloseHandle(file); errno = ELOOP; return -1;
    }
    return descriptor(file, flags);
}

int ghm_win_open(const char *path, int flags, ...)
{
    WCHAR *name = wide(path);
    if (name == NULL) return -1;
    DWORD desired = FILE_READ_ATTRIBUTES | READ_CONTROL;
    if (!(flags & O_DIRECTORY)) {
        desired |= (flags & O_RDWR) ? GENERIC_READ | GENERIC_WRITE :
            (flags & O_WRONLY) ? GENERIC_WRITE : GENERIC_READ;
    }
    DWORD creation = (flags & O_CREAT) ? ((flags & O_EXCL) ? CREATE_NEW :
        (flags & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS) :
        (flags & O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING;
    DWORD attributes = FILE_FLAG_BACKUP_SEMANTICS;
    if (flags & O_NOFOLLOW) attributes |= FILE_FLAG_OPEN_REPARSE_POINT;
    if (flags & (O_RDWR | O_WRONLY)) attributes |= FILE_FLAG_WRITE_THROUGH;
    HANDLE file = CreateFileW(name, desired, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, creation, attributes, NULL);
    g_free(name);
    if (file != INVALID_HANDLE_VALUE && (flags & O_DIRECTORY)) {
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(file, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            CloseHandle(file); errno = ENOTDIR; return -1;
        }
    }
    return check_open(file, flags);
}

static HANDLE relative(int parent, const char *name, ACCESS_MASK desired, ULONG disposition,
                       ULONG options, int refuse_reparse)
{
    /* One component only: an absolute name or separator must never override
     * RootDirectory. The editor validates the stronger Windows naming rules. */
    if (name == NULL || name[0] == '\0' || strpbrk(name, "/\\:") != NULL ||
        strcmp(name, "..") == 0) { errno = EINVAL; return INVALID_HANDLE_VALUE; }
    WCHAR *text = wide(name);
    if (text == NULL) return INVALID_HANDLE_VALUE;
    size_t bytes = wcslen(text) * sizeof(WCHAR);
    if (bytes > 65532) { g_free(text); errno = ENAMETOOLONG; return INVALID_HANDLE_VALUE; }
    UNICODE_STRING string = {.Length = (USHORT)bytes, .MaximumLength = (USHORT)(bytes + 2), .Buffer = text};
    OBJECT_ATTRIBUTES attrs = {.Length = sizeof(attrs), .RootDirectory = handle(parent),
        .ObjectName = &string, .Attributes = 0x40 /* OBJ_CASE_INSENSITIVE */};
    IO_STATUS_BLOCK status;
    HANDLE file = INVALID_HANDLE_VALUE;
    NTSTATUS code = NtCreateFile(&file, desired | SYNCHRONIZE | FILE_READ_ATTRIBUTES | READ_CONTROL,
        &attrs, &status, NULL, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, disposition,
        options | 0x20 /* FILE_SYNCHRONOUS_IO_NONALERT */ | 0x200000 /* FILE_OPEN_REPARSE_POINT */,
        NULL, 0);
    g_free(text);
    if (code < 0) { win_error(RtlNtStatusToDosError(code)); return INVALID_HANDLE_VALUE; }
    if (refuse_reparse && is_reparse(file) != 0) {
        CloseHandle(file); errno = ELOOP; return INVALID_HANDLE_VALUE;
    }
    return file;
}

int ghm_win_openat(int parent, const char *name, int flags, ...)
{
    ACCESS_MASK desired = (flags & O_DIRECTORY) ? FILE_LIST_DIRECTORY :
        (flags & O_RDWR) ? GENERIC_READ | GENERIC_WRITE :
        (flags & O_WRONLY) ? GENERIC_WRITE : GENERIC_READ;
    ULONG disposition = (flags & O_CREAT) ? ((flags & O_EXCL) ? 2U :
        (flags & O_TRUNC) ? 5U : 3U) : (flags & O_TRUNC) ? 4U : 1U;
    ULONG options = (flags & O_DIRECTORY) ? 1U : 0x40U; /* directory/non-directory */
    if (flags & (O_RDWR | O_WRONLY)) options |= 2U; /* write-through */
    HANDLE file = relative(parent, name, desired, disposition, options, (flags & O_NOFOLLOW) != 0);
    return file == INVALID_HANDLE_VALUE ? -1 : descriptor(file, flags);
}

static unsigned owner(HANDLE file)
{
    PSID sid = NULL;
    PSECURITY_DESCRIPTOR security = NULL;
    HANDLE token = NULL;
    DWORD needed = 0;
    unsigned result = 1;
    if (GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                        &sid, NULL, NULL, NULL, &security) != ERROR_SUCCESS) return result;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        GetTokenInformation(token, TokenUser, NULL, 0, &needed);
        TOKEN_USER *user = malloc(needed);
        if (user != NULL && GetTokenInformation(token, TokenUser, user, needed, &needed) &&
            EqualSid(sid, user->User.Sid)) result = 0;
        free(user);
        /* Elevated processes can create files owned by their TokenOwner group
         * rather than TokenUser. Accept only this token's actual default owner,
         * not an arbitrary group membership. */
        if (result != 0) {
            needed = 0;
            GetTokenInformation(token, TokenOwner, NULL, 0, &needed);
            TOKEN_OWNER *default_owner = malloc(needed);
            if (default_owner != NULL && GetTokenInformation(token, TokenOwner,
                default_owner, needed, &needed) && EqualSid(sid, default_owner->Owner)) result = 0;
            free(default_owner);
        }
        CloseHandle(token);
    }
    LocalFree(security);
    return result;
}

static int metadata(HANDLE file, struct ghm_win_stat *out)
{
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(file, &info)) return win_error(GetLastError());
    uint64_t ticks = ((uint64_t)info.ftLastWriteTime.dwHighDateTime << 32) | info.ftLastWriteTime.dwLowDateTime;
    *out = (struct ghm_win_stat){
        .st_dev = info.dwVolumeSerialNumber,
        .st_ino = ((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow,
        .st_size = (int64_t)(((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow),
        .st_nlink = info.nNumberOfLinks, .st_uid = owner(file),
        .st_mode = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? S_IFLNK :
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? S_IFDIR : S_IFREG,
        .st_mtim = {.tv_sec = (time_t)(ticks / 10000000U) - 11644473600LL,
                    .tv_nsec = (long)((ticks % 10000000U) * 100U)}
    };
    out->st_mode |= (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0444U : 0666U;
    return 0;
}

int ghm_win_fstat(int fd, struct ghm_win_stat *out) { return metadata(handle(fd), out); }
static int path_stat(const char *path, struct ghm_win_stat *out, int follow)
{
    WCHAR *text = wide(path);
    if (text == NULL) return -1;
    HANDLE file = CreateFileW(text, FILE_READ_ATTRIBUTES | READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT), NULL);
    g_free(text);
    if (file == INVALID_HANDLE_VALUE) return win_error(GetLastError());
    int result = metadata(file, out);
    CloseHandle(file); return result;
}
int ghm_win_stat(const char *path, struct ghm_win_stat *out) { return path_stat(path, out, 1); }
int ghm_win_lstat(const char *path, struct ghm_win_stat *out) { return path_stat(path, out, 0); }
int ghm_win_fstatat(int parent, const char *name, struct ghm_win_stat *out, int flags)
{
    (void)flags;
    HANDLE file = relative(parent, name, FILE_READ_ATTRIBUTES, 1, 0, 0);
    if (file == INVALID_HANDLE_VALUE) return -1;
    int result = metadata(file, out);
    CloseHandle(file); return result;
}

int ghm_win_mkdir(const char *path, unsigned mode)
{
    (void)mode;
    WCHAR *name = wide(path);
    if (name == NULL) return -1;
    BOOL ok = CreateDirectoryW(name, NULL);
    DWORD code = GetLastError(); g_free(name);
    return ok ? 0 : win_error(code);
}
int ghm_win_mkdirat(int parent, const char *name, unsigned mode)
{
    (void)mode;
    HANDLE file = relative(parent, name, FILE_LIST_DIRECTORY, 2, 1, 1);
    if (file == INVALID_HANDLE_VALUE) return -1;
    CloseHandle(file); return 0;
}
int ghm_win_unlink(const char *path)
{
    WCHAR *name = wide(path);
    if (name == NULL) return -1;
    DWORD attrs = GetFileAttributesW(name);
    BOOL ok = (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) &&
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) ? RemoveDirectoryW(name) : DeleteFileW(name);
    DWORD code = GetLastError(); g_free(name);
    return ok ? 0 : win_error(code);
}
int ghm_win_unlinkat(int parent, const char *name, int flags)
{
    (void)flags;
    HANDLE file = relative(parent, name, DELETE, 1, 0, 0);
    if (file == INVALID_HANDLE_VALUE) return -1;
    FILE_DISPOSITION_INFO info = {.DeleteFile = TRUE};
    BOOL ok = SetFileInformationByHandle(file, FileDispositionInfo, &info, sizeof(info));
    DWORD code = GetLastError(); CloseHandle(file);
    return ok ? 0 : win_error(code);
}
int ghm_win_rename(const char *old, const char *next)
{
    WCHAR *a = wide(old), *b = wide(next);
    if (a == NULL || b == NULL) { g_free(a); g_free(b); return -1; }
    BOOL ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    DWORD code = GetLastError(); g_free(a); g_free(b);
    return ok ? 0 : win_error(code);
}
int ghm_win_renameat2(int parent, const char *old, int next_parent, const char *next, unsigned flags)
{
    if (next == NULL || strpbrk(next, "/\\:") != NULL) { errno = EINVAL; return -1; }
    HANDLE file = relative(parent, old, DELETE, 1, 0, 1);
    if (file == INVALID_HANDLE_VALUE) return -1;
    WCHAR *text = wide(next);
    if (text == NULL) { CloseHandle(file); return -1; }
    size_t bytes = wcslen(text) * sizeof(WCHAR);
    size_t size = sizeof(FILE_RENAME_INFORMATION) + bytes;
    FILE_RENAME_INFORMATION *info = calloc(1, size);
    if (info == NULL) { g_free(text); CloseHandle(file); errno = ENOMEM; return -1; }
    info->ReplaceIfExists = (flags & RENAME_NOREPLACE) == 0;
    info->RootDirectory = handle(next_parent);
    info->FileNameLength = (DWORD)bytes;
    memcpy(info->FileName, text, bytes);
    IO_STATUS_BLOCK status;
    NTSTATUS code = NtSetInformationFile(file, &status, info, (ULONG)size, FileRenameInformation);
    free(info); g_free(text); CloseHandle(file);
    return code >= 0 ? 0 : win_error(RtlNtStatusToDosError(code));
}
int ghm_win_renameat(int a, const char *old, int b, const char *next)
{ return ghm_win_renameat2(a, old, b, next, 0); }
int ghm_win_fsync(int fd)
{
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(handle(fd), &info)) return win_error(GetLastError());
    /* Windows does not offer POSIX directory fsync. Writable files are opened
     * write-through and explicitly flushed before publication. */
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    return FlushFileBuffers(handle(fd)) ? 0 : win_error(GetLastError());
}
int ghm_win_fchmod(int fd, unsigned mode)
{
    FILE_BASIC_INFO info;
    if (!GetFileInformationByHandleEx(handle(fd), FileBasicInfo, &info, sizeof(info))) return win_error(GetLastError());
    if (mode & 0222) info.FileAttributes &= ~(DWORD)FILE_ATTRIBUTE_READONLY;
    else info.FileAttributes |= FILE_ATTRIBUTE_READONLY;
    return SetFileInformationByHandle(handle(fd), FileBasicInfo, &info, sizeof(info)) ? 0 : win_error(GetLastError());
}
int ghm_win_access(const char *path, int mode)
{
    struct ghm_win_stat info;
    if (ghm_win_stat(path, &info) != 0) return -1;
    if ((mode & W_OK) && !(info.st_mode & 0222)) { errno = EACCES; return -1; }
    return 0; /* Actual creates/opens still enforce the ACL. */
}
int ghm_win_faccessat(int parent, const char *name, int mode, int flags)
{
    (void)flags;
    if (strcmp(name, ".") == 0) {
        struct ghm_win_stat info;
        return ghm_win_fstat(parent, &info);
    }
    HANDLE file = relative(parent, name, (mode & W_OK) ? GENERIC_WRITE : FILE_READ_ATTRIBUTES, 1, 0, 1);
    if (file == INVALID_HANDLE_VALUE) return -1;
    CloseHandle(file); return 0;
}
char *ghm_win_mkdtemp(char *pattern)
{
    size_t length = strlen(pattern);
    if (length < 6 || strcmp(pattern + length - 6, "XXXXXX") != 0) { errno = EINVAL; return NULL; }
    for (int attempt = 0; attempt < 100; ++attempt) {
        unsigned char random[6]; sqlite3_randomness(6, random);
        for (int i = 0; i < 6; ++i) pattern[length - 6 + (size_t)i] = "abcdefghijklmnopqrstuvwxyz0123456789"[random[i] % 36];
        if (ghm_win_mkdir(pattern, 0700) == 0) return pattern;
        if (errno != EEXIST) return NULL;
    }
    errno = EEXIST; return NULL;
}
char *ghm_win_strndup(const char *text, size_t count)
{
    size_t length = strnlen(text, count);
    char *copy = malloc(length + 1);
    if (copy != NULL) { memcpy(copy, text, length); copy[length] = '\0'; }
    return copy;
}
int ghm_win_alphasort(const struct dirent **a, const struct dirent **b)
{ return strcmp((*a)->d_name, (*b)->d_name); }
int ghm_win_scandir(const char *path, struct dirent ***out, int (*select)(const struct dirent *),
                   int (*compare)(const struct dirent **, const struct dirent **))
{
    GError *error = NULL;
    GDir *directory = g_dir_open(path, 0, &error);
    if (directory == NULL) { g_clear_error(&error); errno = EIO; return -1; }
    struct dirent **list = NULL;
    int count = 0;
    const char *name;
    while ((name = g_dir_read_name(directory)) != NULL) {
        struct dirent *entry = calloc(1, sizeof(*entry));
        if (entry == NULL || strlen(name) >= sizeof(entry->d_name)) { free(entry); goto failed; }
        strcpy(entry->d_name, name);
        if (select != NULL && !select(entry)) { free(entry); continue; }
        struct dirent **grown = realloc(list, ((size_t)count + 1) * sizeof(*list));
        if (grown == NULL) { free(entry); goto failed; }
        list = grown; list[count++] = entry;
    }
    g_dir_close(directory);
    /* Insertion sort avoids casting incompatible function pointer types. */
    if (compare != NULL) for (int i = 1; i < count; ++i) {
        struct dirent *entry = list[i]; int j = i;
        while (j > 0 && compare((const struct dirent **)&list[j - 1], (const struct dirent **)&entry) > 0) {
            list[j] = list[j - 1]; --j;
        }
        list[j] = entry;
    }
    *out = list; return count;
failed:
    g_dir_close(directory);
    for (int i = 0; i < count; ++i) free(list[i]);
    free(list); errno = ENOMEM; return -1;
}
FILE *ghm_win_fopen(const char *path, const char *mode)
{
    WCHAR *a = wide(path), *b = wide(mode);
    FILE *file = a != NULL && b != NULL ? _wfopen(a, b) : NULL;
    g_free(a); g_free(b); return file;
}

ssize_t ghm_win_read(int fd, void *buffer, size_t bytes)
{ return _read(fd, buffer, (unsigned)(bytes > INT_MAX ? INT_MAX : bytes)); }
ssize_t ghm_win_write(int fd, const void *buffer, size_t bytes)
{ return _write(fd, buffer, (unsigned)(bytes > INT_MAX ? INT_MAX : bytes)); }
GhmWinDir *ghm_win_opendir(const char *path)
{
    GError *error = NULL;
    GDir *directory = g_dir_open(path, 0, &error);
    if (directory == NULL) {
        errno = error != NULL && error->code == G_FILE_ERROR_NOENT ? ENOENT : EIO;
        g_clear_error(&error); return NULL;
    }
    GhmWinDir *result = calloc(1, sizeof(*result));
    if (result == NULL) { g_dir_close(directory); errno = ENOMEM; return NULL; }
    result->directory = directory; return result;
}
struct dirent *ghm_win_readdir(GhmWinDir *directory)
{
    const char *name = g_dir_read_name(directory->directory);
    if (name == NULL) return NULL;
    if (strlen(name) >= sizeof(directory->entry.d_name)) { errno = ENAMETOOLONG; return NULL; }
    strcpy(directory->entry.d_name, name); return &directory->entry;
}
int ghm_win_closedir(GhmWinDir *directory)
{ g_dir_close(directory->directory); free(directory); return 0; }
