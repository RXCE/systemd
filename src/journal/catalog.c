/* SPDX-License-Identifier: LGPL-2.1+ */

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "sd-id128.h"

#include "alloc-util.h"
#include "catalog.h"
#include "conf-files.h"
#include "fd-util.h"
#include "fileio.h"
#include "hashmap.h"
#include "log.h"
#include "mkdir.h"
#include "path-util.h"
#include "siphash24.h"
#include "sparse-endian.h"
#include "strbuf.h"
#include "string-util.h"
#include "strv.h"
#include "util.h"

const char * const catalog_file_dirs[] = {
        "/usr/local/lib/systemd/catalog/",
        "/usr/lib/systemd/catalog/",
        NULL
};

#define CATALOG_SIGNATURE (uint8_t[]) { 'R', 'H', 'H', 'H', 'K', 'S', 'L', 'P' }

typedef struct CatalogHeader {
        uint8_t signature[8];  /* "RHHHKSLP" */
        le32_t compatible_flags;
        le32_t incompatible_flags;
        le64_t header_size;
        le64_t n_items;
        le64_t catalog_item_size;
} CatalogHeader;

typedef struct CatalogItem {
        sd_id128_t id;
        char language[32];
        le64_t offset;
} CatalogItem;

static void catalog_hash_func(const void *p, struct siphash *state) {
        const CatalogItem *i = p;

        siphash24_compress(&i->id, sizeof(i->id), state);
        siphash24_compress(i->language, strlen(i->language), state);
}

static int catalog_compare_func(const void *a, const void *b) {
        const CatalogItem *i = a, *j = b;
        unsigned k;
        int r;

        for (k = 0; k < ELEMENTSOF(j->id.bytes); k++) {
                r = CMP(i->id.bytes[k], j->id.bytes[k]);
                if (r != 0)
                        return r;
        }

        return strcmp(i->language, j->language);
}

const struct hash_ops catalog_hash_ops = {
        .hash = catalog_hash_func,
        .compare = catalog_compare_func
};

static bool next_header(const char **s) {
        const char *e;

        e = strchr(*s, '\n');

        /* Unexpected end */
        if (!e)
                return false;

        /* End of headers */
        if (e == *s)
                return false;

        *s = e + 1;
        return true;
}

static const char *skip_header(const char *s) {
        while (next_header(&s))
                ;
        return s;
}

static char *combine_entries(const char *one, const char *two) {
        const char *b1, *b2;
        size_t l1, l2, n;
        char *dest, *p;

        /* Find split point of headers to body */
        b1 = skip_header(one);
        b2 = skip_header(two);

        l1 = strlen(one);
        l2 = strlen(two);
        dest = new(char, l1 + l2 + 1);
        if (!dest) {
                log_oom();
                return NULL;
        }

        p = dest;

        /* Headers from @one */
        n = b1 - one;
        p = mempcpy(p, one, n);

        /* Headers from @two, these will only be found if not present above */
        n = b2 - two;
        p = mempcpy(p, two, n);

        /* Body from @one */
        n = l1 - (b1 - one);
        if (n > 0) {
                memcpy(p, b1, n);
                p += n;

        /* Body from @two */
        } else {
                n = l2 - (b2 - two);
                memcpy(p, b2, n);
                p += n;
        }

        assert(p - dest <= (ptrdiff_t)(l1 + l2));
        p[0] = '\0';
        return dest;
}

static int finish_item(
                Hashmap *h,
                sd_id128_t id,
                const char *language,
                char *payload, size_t payload_size) {

        _cleanup_free_ CatalogItem *i = NULL;
        _cleanup_free_ char *prev = NULL, *combined = NULL;

        assert(h);
        assert(payload);
        assert(payload_size > 0);

        i = new0(CatalogItem, 1);
        if (!i)
                return log_oom();

        i->id = id;
        if (language) {
                assert(strlen(language) > 1 && strlen(language) < 32);
                strcpy(i->language, language);
        }

        prev = hashmap_get(h, i);
        if (prev) {
                /* Already have such an item, combine them */
                combined = combine_entries(payload, prev);
                if (!combined)
                        return log_oom();

                if (hashmap_update(h, i, combined) < 0)
                        return log_oom();
                combined = NULL;
        } else {
                /* A new item */
                combined = memdup(payload, payload_size + 1);
                if (!combined)
                        return log_oom();

                if (hashmap_put(h, i, combined) < 0)
                        return log_oom();
                i = NULL;
                combined = NULL;
        }

        return 0;
}

int catalog_file_lang(const char* filename, char **lang) {
        char *beg, *end, *_lang;

        end = endswith(filename, ".catalog");
        if (!end)
                return 0;

        beg = end - 1;
        while (beg > filename && !IN_SET(*beg, '.', '/') && end - beg < 32)
                beg--;

        if (*beg != '.' || end <= beg + 1)
                return 0;

        _lang = strndup(beg + 1, end - beg - 1);
        if (!_lang)
                return -ENOMEM;

        *lang = _lang;
        return 1;
}

static int catalog_entry_lang(const char* filename, int line,
                              const char* t, const char* deflang, char **lang) {
        size_t c;

        c = strlen(t);
        if (c == 0) {
                log_error("[%s:%u] Language too short.", filename, line);
                return -EINVAL;
        }
        if (c > 31) {
                log_error("[%s:%u] language too long.", filename, line);
                return -EINVAL;
        }

        if (deflang) {
                if (streq(t, deflang)) {
                        log_warning("[%s:%u] language specified unnecessarily",
                                    filename, line);
                        return 0;
                } else
                        log_warning("[%s:%u] language differs from default for file",
                                    filename, line);
        }

        *lang = strdup(t);
        if (!*lang)
                        return -ENOMEM;

        return 0;
}

int catalog_import_file(Hashmap *h, const char *path) {
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *payload = NULL;
        size_t payload_size = 0, payload_allocated = 0;
        unsigned n = 0;
        sd_id128_t id;
        _cleanup_free_ char *deflang = NULL, *lang = NULL;
        bool got_id = false, empty_line = true;
        int r;

        assert(h);
        assert(path);

        f = fopen(path, "re");
        if (!f)
                return log_error_errno(errno, "Failed to open file %s: %m", path);

        r = catalog_file_lang(path, &deflang);
        if (r < 0)
                log_error_errno(r, "Failed to determine language for file %s: %m", path);
        if (r == 1)
                log_debug("File %s has language %s.", path, deflang);

        for (;;) {
                char line[LINE_MAX];
                size_t line_len;

                if (!fgets(line, sizeof(line), f)) {
                        if (feof(f))
                                break;

                        return log_error_errno(errno, "Failed to read file %s: %m", path);
                }

                n++;

                truncate_nl(line);

                if (line[0] == 0) {
                        empty_line = true;
                        continue;
                }

                if (strchr(COMMENTS "\n", line[0]))
                        continue;

                if (empty_line &&
                    strlen(line) >= 2+1+32 &&
                    line[0] == '-' &&
                    line[1] == '-' &&
                    line[2] == ' ' &&
                    IN_SET(line[2+1+32], ' ', '\0')) {

                        bool with_language;
                        sd_id128_t jd;

                        /* New entry */

                        with_language = line[2+1+32] != '\0';
                        line[2+1+32] = '\0';

                        if (sd_id128_from_string(line + 2 + 1, &jd) >= 0) {

                                if (got_id) {
                                        if (payload_size == 0) {
                                                log_error("[%s:%u] No payload text.", path, n);
                                                return -EINVAL;
                                        }

                                        r = finish_item(h, id, lang ?: deflang, payload, payload_size);
                                        if (r < 0)
                                                return r;

                                        lang = mfree(lang);
                                        payload_size = 0;
                                }

                                if (with_language) {
                                        char *t;

                                        t = strstrip(line + 2 + 1 + 32 + 1);
                                        r = catalog_entry_lang(path, n, t, deflang, &lang);
                                        if (r < 0)
                                                return r;
                                }

                                got_id = true;
                                empty_line = false;
                                id = jd;

                                continue;
                        }
                }

                /* Payload */
                if (!got_id) {
                        log_error("[%s:%u] Got payload before ID.", path, n);
                        return -EINVAL;
                }

                line_len = strlen(line);
                if (!GREEDY_REALLOC(payload, payload_allocated,
                                    payload_size + (empty_line ? 1 : 0) + line_len + 1 + 1))
                        return log_oom();

                if (empty_line)
                        payload[payload_size++] = '\n';
                memcpy(payload + payload_size, line, line_len);
                payload_size += line_len;
                payload[payload_size++] = '\n';
                payload[payload_size] = '\0';

                empty_line = false;
        }

        if (got_id) {
                if (payload_size == 0) {
                        log_error("[%s:%u] No payload text.", path, n);
                        return -EINVAL;
                }

                r = finish_item(h, id, lang ?: deflang, payload, payload_size);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int64_t write_catalog(const char *database, struct strbuf *sb,
                             CatalogItem *items, size_t n) {
        CatalogHeader header;
        _cleanup_fclose_ FILE *w = NULL;
        int r;
        _cleanup_free_ char *d, *p = NULL;
        size_t k;

        d = dirname_malloc(database);
        if (!d)
                return log_oom();

        r = mkdir_p(d, 0775);
        if (r < 0)
                return log_error_errno(r, "Recursive mkdir %s: %m", d);

        r = fopen_temporary(database, &w, &p);
        if (r < 0)
                return log_error_errno(r, "Failed to open database for writing: %s: %m",
                                       database);

        zero(header);
        memcpy(header.signature, CATALOG_SIGNATURE, sizeof(header.signature));
        header.header_size = htole64(ALIGN_TO(sizeof(CatalogHeader), 8));
        header.catalog_item_size = htole64(sizeof(CatalogItem));
        header.n_items = htole64(n);

        r = -EIO;

        k = fwrite(&header, 1, sizeof(header), w);
        if (k != sizeof(header)) {
                log_error("%s: failed to write header.", p);
                goto error;
        }

        k = fwrite(items, 1, n * sizeof(CatalogItem), w);
        if (k != n * sizeof(CatalogItem)) {
                log_error("%s: failed to write database.", p);
                goto error;
        }

        k = fwrite(sb->buf, 1, sb->len, w);
        if (k != sb->len) {
                log_error("%s: failed to write strings.", p);
                goto error;
        }

        r = fflush_and_check(w);
        if (r < 0) {
                log_error_errno(r, "%s: failed to write database: %m", p);
                goto error;
        }

        fchmod(fileno(w), 0644);

        if (rename(p, database) < 0) {
                r = log_error_errno(errno, "rename (%s -> %s) failed: %m", p, database);
                goto error;
        }

        return ftello(w);

error:
        (void) unlink(p);
        return r;
}

int catalog_update(const char* database, const char* root, const char* const* dirs) {
        _cleanup_strv_free_ char **files = NULL;
        char **f;
        _cleanup_(strbuf_cleanupp) struct strbuf *sb = NULL;
        _cleanup_hashmap_free_free_free_ Hashmap *h = NULL;
        _cleanup_free_ CatalogItem *items = NULL;
        ssize_t offset;
        char *payload;
        CatalogItem *i;
        Iterator j;
        unsigned n;
        int r;
        int64_t sz;

        h = hashmap_new(&catalog_hash_ops);
        sb = strbuf_new();
        if (!h || !sb)
                return log_oom();

        r = conf_files_list_strv(&files, ".catalog", root, 0, dirs);
        if (r < 0)
                return log_error_errno(r, "Failed to get catalog files: %m");

        STRV_FOREACH(f, files) {
                log_debug("Reading file '%s'", *f);
                r = catalog_import_file(h, *f);
                if (r < 0)
                        return log_error_errno(r, "Failed to import file '%s': %m", *f);
        }

        if (hashmap_size(h) <= 0) {
                log_info("No items in catalog.");
                return 0;
        } else
                log_debug("Found %u items in catalog.", hashmap_size(h));

        items = new(CatalogItem, hashmap_size(h));
        if (!items)
                return log_oom();

        n = 0;
        HASHMAP_FOREACH_KEY(payload, i, h, j) {
                log_debug("Found " SD_ID128_FORMAT_STR ", language %s",
                          SD_ID128_FORMAT_VAL(i->id),
                          isempty(i->language) ? "C" : i->language);

                offset = strbuf_add_string(sb, payload, strlen(payload));
                if (offset < 0)
                        return log_oom();

                i->offset = htole64((uint64_t) offset);
                items[n++] = *i;
        }

        assert(n == hashmap_size(h));
        qsort_safe(items, n, sizeof(CatalogItem), catalog_compare_func);

        strbuf_complete(sb);

        sz = write_catalog(database, sb, items, n);
        if (sz < 0)
                return log_error_errno(sz, "Failed to write %s: %m", database);

        log_debug("%s: wrote %u items, with %zu bytes of strings, %"PRIi64" total size.",
                  database, n, sb->len, sz);
        return 0;
}

static int open_mmap(const char *database, int *_fd, struct stat *_st, void **_p) {
        const CatalogHeader *h;
        int fd;
        void *p;
        struct stat st;

        assert(_fd);
        assert(_st);
        assert(_p);

        fd = open(database, O_RDONLY|O_CLOEXEC);
        if (fd < 0)
                return -errno;

        if (fstat(fd, &st) < 0) {
                safe_close(fd);
                return -errno;
        }

        if (st.st_size < (off_t) sizeof(CatalogHeader)) {
                safe_close(fd);
                return -EINVAL;
        }

        p = mmap(NULL, PAGE_ALIGN(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
                safe_close(fd);
                return -errno;
        }

        h = p;
        if (memcmp(h->signature, CATALOG_SIGNATURE, sizeof(h->signature)) != 0 ||
            le64toh(h->header_size) < sizeof(CatalogHeader) ||
            le64toh(h->catalog_item_size) < sizeof(CatalogItem) ||
            h->incompatible_flags != 0 ||
            le64toh(h->n_items) <= 0 ||
            st.st_size < (off_t) (le64toh(h->header_size) + le64toh(h->catalog_item_size) * le64toh(h->n_items))) {
                safe_close(fd);
                munmap(p, st.st_size);
                return -EBADMSG;
        }

        *_fd = fd;
        *_st = st;
        *_p = p;

        return 0;
}

static const char *find_id(void *p, sd_id128_t id) {
        CatalogItem *f = NULL, key = { .id = id };
        const CatalogHeader *h = p;
        const char *loc;

        loc = setlocale(LC_MESSAGES, NULL);
        if (loc && loc[0] && !streq(loc, "C") && !streq(loc, "POSIX")) {
                strncpy(key.language, loc, sizeof(key.language));
                key.language[strcspn(key.language, ".@")] = 0;

                f = bsearch(&key, (const uint8_t*) p + le64toh(h->header_size), le64toh(h->n_items), le64toh(h->catalog_item_size), catalog_compare_func);
                if (!f) {
                        char *e;

                        e = strchr(key.language, '_');
                        if (e) {
                                *e = 0;
                                f = bsearch(&key, (const uint8_t*) p + le64toh(h->header_size), le64toh(h->n_items), le64toh(h->catalog_item_size), catalog_compare_func);
                        }
                }
        }

        if (!f) {
                zero(key.language);
                f = bsearch(&key, (const uint8_t*) p + le64toh(h->header_size), le64toh(h->n_items), le64toh(h->catalog_item_size), catalog_compare_func);
        }

        if (!f)
                return NULL;

        return (const char*) p +
                le64toh(h->header_size) +
                le64toh(h->n_items) * le64toh(h->catalog_item_size) +
                le64toh(f->offset);
}

int catalog_get(const char* database, sd_id128_t id, char **_text) {
        _cleanup_close_ int fd = -1;
        void *p = NULL;
        struct stat st = {};
        char *text = NULL;
        int r;
        const char *s;

        assert(_text);

        r = open_mmap(database, &fd, &st, &p);
        if (r < 0)
                return r;

        s = find_id(p, id);
        if (!s) {
                r = -ENOENT;
                goto finish;
        }

        text = strdup(s);
        if (!text) {
                r = -ENOMEM;
                goto finish;
        }

        *_text = text;
        r = 0;

finish:
        if (p)
                munmap(p, st.st_size);

        return r;
}

static char *find_header(const char *s, const char *header) {

        for (;;) {
                const char *v;

                v = startswith(s, header);
                if (v) {
                        v += strspn(v, WHITESPACE);
                        return strndup(v, strcspn(v, NEWLINE));
                }

                if (!next_header(&s))
                        return NULL;
        }
}

static void dump_catalog_entry(FILE *f, sd_id128_t id, const char *s, bool oneline) {
        if (oneline) {
                _cleanup_free_ char *subject = NULL, *defined_by = NULL;

                subject = find_header(s, "Subject:");
                defined_by = find_header(s, "Defined-By:");

                fprintf(f, SD_ID128_FORMAT_STR " %s: %s\n",
                        SD_ID128_FORMAT_VAL(id),
                        strna(defined_by), strna(subject));
        } else
                fprintf(f, "-- " SD_ID128_FORMAT_STR "\n%s\n",
                        SD_ID128_FORMAT_VAL(id), s);
}

int catalog_list(FILE *f, const char *database, bool oneline) {
        _cleanup_close_ int fd = -1;
        void *p = NULL;
        struct stat st;
        const CatalogHeader *h;
        const CatalogItem *items;
        int r;
        unsigned n;
        sd_id128_t last_id;
        bool last_id_set = false;

        r = open_mmap(database, &fd, &st, &p);
        if (r < 0)
                return r;

        h = p;
        items = (const CatalogItem*) ((const uint8_t*) p + le64toh(h->header_size));

        for (n = 0; n < le64toh(h->n_items); n++) {
                const char *s;

                if (last_id_set && sd_id128_equal(last_id, items[n].id))
                        continue;

                assert_se(s = find_id(p, items[n].id));

                dump_catalog_entry(f, items[n].id, s, oneline);

                last_id_set = true;
                last_id = items[n].id;
        }

        munmap(p, st.st_size);

        return 0;
}

int catalog_list_items(FILE *f, const char *database, bool oneline, char **items) {
        char **item;
        int r = 0;

        STRV_FOREACH(item, items) {
                sd_id128_t id;
                int k;
                _cleanup_free_ char *msg = NULL;

                k = sd_id128_from_string(*item, &id);
                if (k < 0) {
                        log_error_errno(k, "Failed to parse id128 '%s': %m", *item);
                        if (r == 0)
                                r = k;
                        continue;
                }

                k = catalog_get(database, id, &msg);
                if (k < 0) {
                        log_full_errno(k == -ENOENT ? LOG_NOTICE : LOG_ERR, k,
                                       "Failed to retrieve catalog entry for '%s': %m", *item);
                        if (r == 0)
                                r = k;
                        continue;
                }

                dump_catalog_entry(f, id, msg, oneline);
        }

        return r;
}
