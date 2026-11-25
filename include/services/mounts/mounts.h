#ifndef SERVICES_MOUNTS_H
#define SERVICES_MOUNTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MOUNTS_MAX       32
#define MOUNTS_ID_MAX    64
#define MOUNTS_PATH_MAX  512

typedef struct Mount {
    char id[MOUNTS_ID_MAX];
    char root[MOUNTS_PATH_MAX];
} Mount;

void Mounts_init(void);

const char *Mounts_packages_dir(void);
const char *Mounts_mods_dir(void);
const char *Mounts_user_dir(void);

void         Mounts_clear(void);
bool         Mounts_push(const char *id, const char *root);
int32_t      Mounts_count(void);
const Mount *Mounts_get(int32_t index);

int32_t Mounts_find(const char *rel, char *out, size_t cap);
int32_t Mounts_find_below(int32_t below, const char *rel, char *out, size_t cap);
int32_t Mounts_resolve(const char *rel, char *out, size_t cap);

int32_t Mounts_index_of_path(const char *full_path);

bool Mounts_file_exists(const char *path);

void Mounts_log(void);

#endif
