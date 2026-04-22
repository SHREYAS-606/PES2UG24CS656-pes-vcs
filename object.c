// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <errno.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}
// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────>

static const char *object_type_to_string(ObjectType type) {
    switch (type) {
        case OBJ_BLOB:   return "blob";
        case OBJ_TREE:   return "tree";
        case OBJ_COMMIT: return "commit";
        default:         return NULL;
    }
}

static int parse_object_type(const char *type_str, ObjectType *type_out) {
    if (strcmp(type_str, "blob") == 0) {
        *type_out = OBJ_BLOB;
    } else if (strcmp(type_str, "tree") == 0) {
        *type_out = OBJ_TREE;
    } else if (strcmp(type_str, "commit") == 0) {
        *type_out = OBJ_COMMIT;
    } else {
        return -1;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t count) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t written = 0;

    while (written < count) {
        ssize_t n = write(fd, p + written, count - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}
// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str = object_type_to_string(type);
    if (!type_str || !data || !id_out) return -1;

    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    if (header_len < 0 || (size_t)header_len + 1 > sizeof(header)) return -1;

    size_t full_len = (size_t)header_len + 1 + len;
    unsigned char *full_obj = (unsigned char *)malloc(full_len);
    if (!full_obj) return -1;

    memcpy(full_obj, header, (size_t)header_len);
    full_obj[header_len] = '\0';
    memcpy(full_obj + header_len + 1, data, len);

    compute_hash(full_obj, full_len, id_out);

    if (object_exists(id_out)) {
        free(full_obj);
        return 0;
    }

    char final_path[512];
    char shard_dir[512];
    char temp_path[512];
    char hex[HASH_HEX_SIZE + 1];

    object_path(id_out, final_path, sizeof(final_path));
    hash_to_hex(id_out, hex);
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    snprintf(temp_path, sizeof(temp_path), "%s/.tmp-%d", shard_dir, getpid());

    if (mkdir(OBJECTS_DIR, 0755) < 0 && errno != EEXIST) {
        free(full_obj);
        return -1;
    }

    if (mkdir(shard_dir, 0755) < 0 && errno != EEXIST) {
        free(full_obj);
        return -1;
    }

    int fd = open(temp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(full_obj);
        return -1;
    }

    if (write_all(fd, full_obj, full_len) < 0) {
        close(fd);
        unlink(temp_path);
        free(full_obj);
        return -1;
    }

    if (fsync(fd) < 0) {
        close(fd);
        unlink(temp_path);
        free(full_obj);
        return -1;
    }

    if (close(fd) < 0) {
        unlink(temp_path);
        free(full_obj);
        return -1;
    }

    if (rename(temp_path, final_path) < 0) {
        unlink(temp_path);
        free(full_obj);
        return -1;
    }

    int dir_fd = open(shard_dir, O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    free(full_obj);
    return 0;
}
// Read an object from the store.
//
// The caller is responsible for calling free(*data_out).
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    if (!id || !type_out || !data_out || !len_out) return -1;

    char path[512];
    object_path(id, path, sizeof(path));

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }

    long file_size_long = ftell(fp);
    if (file_size_long < 0) {
        fclose(fp);
        return -1;
    }

    size_t file_size = (size_t)file_size_long;
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    unsigned char *buf = (unsigned char *)malloc(file_size);
    if (!buf) {
        fclose(fp);
        return -1;
    }

    if (file_size > 0 && fread(buf, 1, file_size, fp) != file_size) {
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    ObjectID computed;
    compute_hash(buf, file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buf);
        return -1;
    }

    unsigned char *nul = memchr(buf, '\0', file_size);
    if (!nul) {
        free(buf);
        return -1;
    }

    size_t header_len = (size_t)(nul - buf);
    char *header = (char *)malloc(header_len + 1);
    if (!header) {
        free(buf);
        return -1;
    }

    memcpy(header, buf, header_len);
    header[header_len] = '\0';

    char type_str[16];
    size_t parsed_size;
    if (sscanf(header, "%15s %zu", type_str, &parsed_size) != 2) {
        free(header);
        free(buf);
        return -1;
    }

    if (parse_object_type(type_str, type_out) != 0) {
        free(header);
        free(buf);
        return -1;
    }

    size_t data_offset = header_len + 1;
    if (data_offset > file_size || parsed_size != file_size - data_offset) {
        free(header);
        free(buf);
        return -1;
    }
    void *out = malloc(parsed_size);
    if (!out && parsed_size > 0) {
        free(header);
        free(buf);
        return -1;
    }

    if (parsed_size > 0) {
        memcpy(out, buf + data_offset, parsed_size);
    }

    *data_out = out;
    *len_out = parsed_size;

    free(header);
    free(buf);
    return 0;
}
// object module step 1
// object module step 2
