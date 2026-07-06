#if defined(_MSC_VER)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#endif

#include "ef_port.h"
#include "ef_config.h"

#include <string.h>

#if EF_HAS_FILE_IO

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#endif /* EF_HAS_FILE_IO */

#if EF_HAS_FILE_IO && defined(_WIN32)

static enum ef_err ef_port_sync_win32(const struct ef_io *io, enum ef_sync_mode mode)
{
    HANDLE file_handle;

    if (!FlushViewOfFile(io->map_addr, io->map_size)) {
        return EF_ERR_IO;
    }

    /* EF_SYNC_ASYNC: dirty pages reach the OS file cache (like MS_ASYNC). */
    if (mode == EF_SYNC_ASYNC) {
        return EF_OK;
    }

    /* EF_SYNC_FULL: flush OS cache and device buffers (like MS_SYNC). */
    if (io->fd < 0) {
        return EF_ERR_IO;
    }

    file_handle = (HANDLE)_get_osfhandle(io->fd);
    if (file_handle == INVALID_HANDLE_VALUE) {
        return EF_ERR_IO;
    }

    if (!FlushFileBuffers(file_handle)) {
        return EF_ERR_IO;
    }

    return EF_OK;
}

static void ef_port_flush_win32(const struct ef_io *io)
{
    HANDLE file_handle;

    if (io->map_addr != NULL) {
        FlushViewOfFile(io->map_addr, io->map_size);
    }

    if (io->fd < 0) {
        return;
    }

    file_handle = (HANDLE)_get_osfhandle(io->fd);
    if (file_handle != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(file_handle);
    }
}

#endif /* EF_HAS_FILE_IO && _WIN32 */

const char *ef_platform_name(void)
{
#if !EF_HAS_FILE_IO
    return "embedded";
#elif defined(_WIN32)
    return "win32";
#elif defined(__linux__)
    return "linux";
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__unix__)
    return "unix";
#else
    return "posix";
#endif
}

#if EF_HAS_FILE_IO

enum ef_err ef_port_open_file(const char *filepath, size_t map_size, struct ef_io *io, int *is_new_out)
{
#ifdef _WIN32
    struct _stat64 st;
    HANDLE os_handle;
    HANDLE map_handle;
#else
    struct stat st;
#endif

    if (io == NULL || is_new_out == NULL || filepath == NULL) {
        return EF_ERR_NULL_ARG;
    }

    memset(io, 0, sizeof(*io));
    io->fd = -1;
    io->backend = EF_BACKEND_FILE;

#ifdef _WIN32
    io->fd = _open(filepath, _O_RDWR | _O_CREAT | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (io->fd < 0) {
        return EF_ERR_IO;
    }

    if (_fstat64(io->fd, &st) != 0) {
        ef_port_close(io);
        return EF_ERR_IO;
    }

    *is_new_out = (st.st_size == 0);

    if (*is_new_out) {
        if (_chsize_s(io->fd, (__int64)map_size) != 0) {
            ef_port_close(io);
            return EF_ERR_IO;
        }
    } else {
        map_size = (size_t)st.st_size;
    }

    os_handle = (HANDLE)_get_osfhandle(io->fd);
    if (os_handle == INVALID_HANDLE_VALUE) {
        ef_port_close(io);
        return EF_ERR_IO;
    }

    map_handle = CreateFileMappingA(
        os_handle,
        NULL,
        PAGE_READWRITE,
        (DWORD)((map_size >> 32) & 0xFFFFFFFFu),
        (DWORD)(map_size & 0xFFFFFFFFu),
        NULL);
    if (map_handle == NULL) {
        ef_port_close(io);
        return EF_ERR_MMAP;
    }

    io->map_addr = MapViewOfFile(map_handle, FILE_MAP_WRITE, 0, 0, map_size);
    if (io->map_addr == NULL) {
        CloseHandle(map_handle);
        ef_port_close(io);
        return EF_ERR_MMAP;
    }

    io->map_handle = map_handle;
#else
    io->fd = open(filepath, O_RDWR | O_CREAT, 0644);
    if (io->fd < 0) {
        return EF_ERR_IO;
    }

    if (fstat(io->fd, &st) != 0) {
        ef_port_close(io);
        return EF_ERR_IO;
    }

    *is_new_out = (st.st_size == 0);

    if (*is_new_out) {
        if (ftruncate(io->fd, (off_t)map_size) != 0) {
            ef_port_close(io);
            return EF_ERR_IO;
        }
    } else {
        map_size = (size_t)st.st_size;
    }

    io->map_addr = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, io->fd, 0);
    if (io->map_addr == MAP_FAILED) {
        io->map_addr = NULL;
        ef_port_close(io);
        return EF_ERR_MMAP;
    }
#endif

    io->map_size = map_size;
    io->map_capacity = map_size;
    io->readonly = 0;
    return EF_OK;
}

static enum ef_err ef_port_unmap_file(struct ef_io *io)
{
    if (io == NULL || io->map_addr == NULL) {
        return EF_OK;
    }

#ifdef _WIN32
    if (!io->readonly) {
        ef_port_flush_win32(io);
    }
    UnmapViewOfFile(io->map_addr);
    if (io->map_handle != NULL) {
        CloseHandle((HANDLE)io->map_handle);
        io->map_handle = NULL;
    }
#else
    if (!io->readonly && io->map_addr != MAP_FAILED) {
        msync(io->map_addr, io->map_size, MS_SYNC);
    }
    munmap(io->map_addr, io->map_size);
#endif

    io->map_addr = NULL;
    return EF_OK;
}

static enum ef_err ef_port_map_file(struct ef_io *io, size_t map_size, int readonly)
{
#ifdef _WIN32
    HANDLE os_handle;
    HANDLE map_handle;
    DWORD protect = readonly ? PAGE_READONLY : PAGE_READWRITE;
    DWORD access = readonly ? FILE_MAP_READ : FILE_MAP_WRITE;

    os_handle = (HANDLE)_get_osfhandle(io->fd);
    if (os_handle == INVALID_HANDLE_VALUE) {
        return EF_ERR_IO;
    }

    map_handle = CreateFileMappingA(
        os_handle,
        NULL,
        protect,
        (DWORD)((map_size >> 32) & 0xFFFFFFFFu),
        (DWORD)(map_size & 0xFFFFFFFFu),
        NULL);
    if (map_handle == NULL) {
        return EF_ERR_MMAP;
    }

    io->map_addr = MapViewOfFile(map_handle, access, 0, 0, map_size);
    if (io->map_addr == NULL) {
        CloseHandle(map_handle);
        return EF_ERR_MMAP;
    }

    io->map_handle = map_handle;
#else
    int prot = readonly ? PROT_READ : (PROT_READ | PROT_WRITE);
    io->map_addr = mmap(NULL, map_size, prot, MAP_SHARED, io->fd, 0);
    if (io->map_addr == MAP_FAILED) {
        io->map_addr = NULL;
        return EF_ERR_MMAP;
    }
#endif

    io->map_size = map_size;
    io->readonly = readonly;
    return EF_OK;
}

enum ef_err ef_port_open_file_existing(const char *filepath, struct ef_io *io, int readonly)
{
#ifdef _WIN32
    struct _stat64 st;
#else
    struct stat st;
#endif
    size_t map_size;

    if (io == NULL || filepath == NULL) {
        return EF_ERR_NULL_ARG;
    }

    memset(io, 0, sizeof(*io));
    io->fd = -1;
    io->backend = EF_BACKEND_FILE;

#ifdef _WIN32
    io->fd = _open(filepath, _O_BINARY | (readonly ? _O_RDONLY : _O_RDWR));
#else
    io->fd = open(filepath, readonly ? O_RDONLY : O_RDWR);
#endif
    if (io->fd < 0) {
        return EF_ERR_IO;
    }

#ifdef _WIN32
    if (_fstat64(io->fd, &st) != 0) {
        ef_port_close(io);
        return EF_ERR_IO;
    }
#else
    if (fstat(io->fd, &st) != 0) {
        ef_port_close(io);
        return EF_ERR_IO;
    }
#endif

    if (st.st_size == 0) {
        ef_port_close(io);
        return EF_ERR_FILE_SIZE;
    }

    map_size = (size_t)st.st_size;
    if (ef_port_map_file(io, map_size, readonly) != EF_OK) {
        ef_port_close(io);
        return EF_ERR_MMAP;
    }

    io->map_capacity = map_size;
    return EF_OK;
}

enum ef_err ef_port_grow_file(struct ef_io *io, size_t new_map_size)
{
    if (io == NULL || io->backend != EF_BACKEND_FILE) {
        return EF_ERR_NULL_ARG;
    }
    if (io->readonly) {
        return EF_ERR_READONLY;
    }
    if (new_map_size <= io->map_size) {
        return EF_ERR_GROW;
    }

    if (ef_port_unmap_file(io) != EF_OK) {
        return EF_ERR_IO;
    }

#ifdef _WIN32
    if (_chsize_s(io->fd, (__int64)new_map_size) != 0) {
        return EF_ERR_IO;
    }
#else
    if (ftruncate(io->fd, (off_t)new_map_size) != 0) {
        return EF_ERR_IO;
    }
#endif

    if (ef_port_map_file(io, new_map_size, 0) != EF_OK) {
        return EF_ERR_MMAP;
    }

    io->map_capacity = new_map_size;
    return EF_OK;
}

#endif /* EF_HAS_FILE_IO - helpers continued above */

enum ef_err ef_port_open_memory(void *buffer, size_t buffer_size, struct ef_io *io)
{
    if (io == NULL || buffer == NULL || buffer_size == 0) {
        return EF_ERR_NULL_ARG;
    }

    memset(io, 0, sizeof(*io));
    io->fd = -1;
    io->map_addr = buffer;
    io->map_size = buffer_size;
    io->map_capacity = buffer_size;
    io->backend = EF_BACKEND_MEMORY;
    return EF_OK;
}

void ef_port_close(struct ef_io *io)
{
    if (io == NULL) {
        return;
    }

#if EF_HAS_FILE_IO
    if (io->backend == EF_BACKEND_FILE) {
#ifdef _WIN32
        if (!io->readonly) {
            ef_port_flush_win32(io);
        }
        if (io->map_addr != NULL) {
            UnmapViewOfFile(io->map_addr);
        }
        if (io->map_handle != NULL) {
            CloseHandle((HANDLE)io->map_handle);
        }
        if (io->fd >= 0) {
            _close(io->fd);
        }
#else
        if (io->map_addr != NULL && io->map_addr != MAP_FAILED) {
            if (!io->readonly) {
                msync(io->map_addr, io->map_size, MS_SYNC);
            }
            munmap(io->map_addr, io->map_size);
        }
        if (io->fd >= 0) {
            close(io->fd);
        }
#endif
    }
#endif

    memset(io, 0, sizeof(*io));
    io->fd = -1;
}

enum ef_err ef_port_sync(const struct ef_io *io, enum ef_sync_mode mode)
{
    if (io == NULL || io->map_addr == NULL) {
        return EF_ERR_NULL_ARG;
    }

    if (io->backend == EF_BACKEND_MEMORY) {
        return EF_OK;
    }

    if (io->readonly) {
        return EF_OK;
    }

#if EF_HAS_FILE_IO
#ifdef _WIN32
    return ef_port_sync_win32(io, mode);
#else
    if (msync(io->map_addr, io->map_size, mode == EF_SYNC_ASYNC ? MS_ASYNC : MS_SYNC) != 0) {
        return EF_ERR_IO;
    }
    return EF_OK;
#endif
#else
    (void)mode;
    return EF_OK;
#endif
}
