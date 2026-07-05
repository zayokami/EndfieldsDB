#ifndef EF_PORT_H
#define EF_PORT_H

#include "endfields.h"

struct ef_io {
    int fd;
    void *map_addr;
    size_t map_size;
    size_t map_capacity;
    enum ef_backend backend;
    int readonly;
#ifdef _WIN32
    void *map_handle;
#endif
};

#if EF_HAS_FILE_IO
enum ef_err ef_port_open_file(const char *filepath, size_t map_size, struct ef_io *io, int *is_new_out);
enum ef_err ef_port_open_file_existing(const char *filepath, struct ef_io *io, int readonly);
enum ef_err ef_port_grow_file(struct ef_io *io, size_t new_map_size);
#endif

enum ef_err ef_port_open_memory(void *buffer, size_t buffer_size, struct ef_io *io);
void ef_port_close(struct ef_io *io);
enum ef_err ef_port_sync(const struct ef_io *io, enum ef_sync_mode mode);

#endif
