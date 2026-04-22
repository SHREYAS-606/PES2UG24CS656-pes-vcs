// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6... 1699900000 42 README.md
//   100644 f7e8d9c0b1a2... 1699900100 128 src/main.c

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ───────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;

    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;

    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if ((uint64_t)st.st_mtime != index->entries[i].mtime_sec ||
                (uint32_t)st.st_size != index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;

    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            if (strcmp(ent->d_name, ".pes") == 0)
                continue;
            if (strcmp(ent->d_name, "pes") == 0)
                continue;
            if (strstr(ent->d_name, ".o") != NULL)
                continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }

            if (!is_tracked) {
                struct stat st;
                if (stat(ent->d_name, &st) == 0 && S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }

    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── HELPERS ───────────────────────────────────────────────────────────

static int compare_entries(const void *a, const void *b) {
    const IndexEntry *ea = (const IndexEntry *)a;
    const IndexEntry *eb = (const IndexEntry *)b;
    return strcmp(ea->path, eb->path);
}

// ─── TODO: Implement these ─────────────────────────────────────────────

// Load the index from .pes/index.
//
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    if (!index) return -1;

    memset(index, 0, sizeof(*index));

    FILE *fp = fopen(".pes/index", "r");
    if (!fp) {
        if (errno == ENOENT) {
            return 0;
        }
        perror("fopen");
        return -1;
    }

    char line[2048];

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index is full\n");
            fclose(fp);
            return -1;
        }

        IndexEntry entry;
        char hex[65];
        char path_buf[512];
        unsigned int mode;
        unsigned long long mtime;
        unsigned int size;

        memset(&entry, 0, sizeof(entry));
        memset(path_buf, 0, sizeof(path_buf));

        if (sscanf(line, "%o %64s %llu %u %[^\n]",
                   &mode, hex, &mtime, &size, path_buf) != 5) {
            fprintf(stderr, "error: malformed index entry\n");
            fclose(fp);
            return -1;
        }

        entry.mode = (uint32_t)mode;
        entry.mtime_sec = (uint64_t)mtime;
        entry.size = (uint32_t)size;

        if (hex_to_hash(hex, &entry.hash) != 0) {
            fprintf(stderr, "error: invalid hash in index\n");
            fclose(fp);
            return -1;
        }

        strncpy(entry.path, path_buf, sizeof(entry.path) - 1);
        entry.path[sizeof(entry.path) - 1] = '\0';

        index->entries[index->count++] = entry;
    }

    fclose(fp);
    return 0;
}

// Save the index to .pes/index atomically.
//
// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
    if (!index) return -1;

    IndexEntry *sorted = NULL;
    if (index->count > 0) {
        sorted = malloc((size_t)index->count * sizeof(IndexEntry));
        if (!sorted) {
            perror("malloc");
            return -1;
        }

        for (int i = 0; i < index->count; i++) {
            sorted[i] = index->entries[i];
        }

        qsort(sorted, index->count, sizeof(IndexEntry), compare_entries);
    }

    FILE *fp = fopen(".pes/index.tmp", "w");
    if (!fp) {
        perror("fopen");
        free(sorted);
        return -1;
    }

    for (int i = 0; i < index->count; i++) {
        char hex[65];
        hash_to_hex(&sorted[i].hash, hex);

        if (fprintf(fp, "%06o %s %" PRIu64 " %" PRIu32 " %s\n",
                    sorted[i].mode,
                    hex,
                    sorted[i].mtime_sec,
                    sorted[i].size,
                    sorted[i].path) < 0) {
            perror("fprintf");
            fclose(fp);
            free(sorted);
            return -1;
        }
    }

    if (fflush(fp) != 0) {
        perror("fflush");
        fclose(fp);
        free(sorted);
        return -1;
    }

    if (fsync(fileno(fp)) != 0) {
        perror("fsync");
        fclose(fp);
        free(sorted);
        return -1;
    }

    if (fclose(fp) != 0) {
        perror("fclose");
        free(sorted);
        return -1;
    }

    free(sorted);

    if (rename(".pes/index.tmp", ".pes/index") != 0) {
        perror("rename");
        return -1;
    }

    return 0;
}

// Stage a file for the next commit.
//
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    if (!index || !path) return -1;

    struct stat st;
    if (stat(path, &st) != 0) {
        perror(path);
        return -1;
    }

    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "error: '%s' is not a regular file\n", path);
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror(path);
        return -1;
    }

    void *data = NULL;
    if (st.st_size > 0) {
        data = malloc((size_t)st.st_size);
        if (!data) {
            perror("malloc");
            fclose(fp);
            return -1;
        }

        size_t nread = fread(data, 1, (size_t)st.st_size, fp);
        if (nread != (size_t)st.st_size) {
            fprintf(stderr, "error: failed to read '%s'\n", path);
            free(data);
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);

    ObjectID hash;
    if (object_write(OBJ_BLOB, data, (size_t)st.st_size, &hash) != 0) {
        fprintf(stderr, "error: failed to stage '%s'\n", path);
        free(data);
        return -1;
    }

    free(data);

    uint32_t git_mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;

    IndexEntry *entry = index_find(index, path);
    if (entry) {
        entry->mode = git_mode;
        entry->hash = hash;
        entry->mtime_sec = (uint64_t)st.st_mtime;
        entry->size = (uint32_t)st.st_size;
        strncpy(entry->path, path, sizeof(entry->path) - 1);
        entry->path[sizeof(entry->path) - 1] = '\0';
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index is full\n");
            return -1;
        }

        entry = &index->entries[index->count++];
        memset(entry, 0, sizeof(*entry));

        entry->mode = git_mode;
        entry->hash = hash;
        entry->mtime_sec = (uint64_t)st.st_mtime;
        entry->size = (uint32_t)st.st_size;
        strncpy(entry->path, path, sizeof(entry->path) - 1);
        entry->path[sizeof(entry->path) - 1] = '\0';
    }

    return index_save(index);
}
// step 1
// step 2
