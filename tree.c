// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include "pes.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// forward declaration because pes.h may not declare it
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── Mode Constants ──────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = (size_t)(space - ptr);
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = (uint32_t)strtol(mode_str, NULL, 8);

        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = (size_t)(null_byte - ptr);
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }

    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = (size_t)tree->count * 296;
    uint8_t *buffer = malloc(max_size > 0 ? max_size : 1);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];

        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += (size_t)written + 1;

        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── Helpers ─────────────────────────────────────────────────────────────

static int load_index_local(Index *index_out) {
    FILE *fp = fopen(INDEX_FILE, "r");
    if (!fp) {
        index_out->count = 0;
        return 0;
    }

    index_out->count = 0;
    char line[1024];

    while (fgets(line, sizeof(line), fp)) {
        if (index_out->count >= MAX_INDEX_ENTRIES) {
            fclose(fp);
            return -1;
        }

        IndexEntry *entry = &index_out->entries[index_out->count];

        char hash_hex[HASH_HEX_SIZE + 1];
        unsigned long mode_ul;
        unsigned long long mtime_ull;
        unsigned int size_u;
        char path_buf[512];

        int matched = sscanf(line, "%lo %64s %llu %u %511[^\n]",
                             &mode_ul, hash_hex, &mtime_ull, &size_u, path_buf);
        if (matched != 5) {
            fclose(fp);
            return -1;
        }

        entry->mode = (uint32_t)mode_ul;
        entry->mtime_sec = (uint64_t)mtime_ull;
        entry->size = (uint32_t)size_u;

        if (hex_to_hash(hash_hex, &entry->hash) != 0) {
            fclose(fp);
            return -1;
        }

        strncpy(entry->path, path_buf, sizeof(entry->path) - 1);
        entry->path[sizeof(entry->path) - 1] = '\0';

        index_out->count++;
    }

    fclose(fp);
    return 0;
}

static int tree_has_name(const Tree *tree, const char *name) {
    for (int i = 0; i < tree->count; i++) {
        if (strcmp(tree->entries[i].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int build_tree_recursive(const Index *index, const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    size_t prefix_len = strlen(prefix);

    for (int i = 0; i < index->count; i++) {
        const IndexEntry *ie = &index->entries[i];

        if (strncmp(ie->path, prefix, prefix_len) != 0) {
            continue;
        }

        const char *rest = ie->path + prefix_len;
        if (*rest == '\0') {
            continue;
        }

        const char *slash = strchr(rest, '/');

        if (slash == NULL) {
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            if (strlen(rest) >= sizeof(tree.entries[tree.count].name)) return -1;
            if (tree_has_name(&tree, rest)) continue;

            tree.entries[tree.count].mode = ie->mode;
            tree.entries[tree.count].hash = ie->hash;
            strcpy(tree.entries[tree.count].name, rest);
            tree.count++;
        } else {
            size_t dir_len = (size_t)(slash - rest);
            if (dir_len == 0 || dir_len >= sizeof(tree.entries[0].name)) return -1;

            char dirname[256];
            memcpy(dirname, rest, dir_len);
            dirname[dir_len] = '\0';

            if (tree_has_name(&tree, dirname)) {
                continue;
            }

            char child_prefix[768];
            int n = snprintf(child_prefix, sizeof(child_prefix), "%s%s/", prefix, dirname);
            if (n < 0 || (size_t)n >= sizeof(child_prefix)) return -1;

            ObjectID subtree_id;
            if (build_tree_recursive(index, child_prefix, &subtree_id) != 0) {
                return -1;
            }

            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            tree.entries[tree.count].mode = MODE_DIR;
            tree.entries[tree.count].hash = subtree_id;
            strcpy(tree.entries[tree.count].name, dirname);
            tree.count++;
        }
    }

    void *serialized = NULL;
    size_t serialized_len = 0;

    if (tree_serialize(&tree, &serialized, &serialized_len) != 0) {
        return -1;
    }

    if (object_write(OBJ_TREE, serialized, serialized_len, id_out) != 0) {
        free(serialized);
        return -1;
    }

    free(serialized);
    return 0;
}

// ─── TODO: Implement these ───────────────────────────────────────────────

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// Returns 0 on success, -1 on error.
int tree_from_index(ObjectID *id_out) {
    if (!id_out) return -1;

    Index index;
    if (load_index_local(&index) != 0) {
        return -1;
    }

    return build_tree_recursive(&index, "", id_out);
}
// step 1
