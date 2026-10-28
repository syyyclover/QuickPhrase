/* Real bundle filesystem/model transactions under allocation failure. */
#include "storage.h"
#include "test_fail_alloc.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int io_fault, io_writes;
ssize_t __real_write(int, const void *, size_t);
int __real_fsync(int);
int __real_close(int);
ssize_t __wrap_write(int fd, const void *data, size_t size)
{
    ++io_writes;
    if (io_fault == 1 && io_writes == 1) return __real_write(fd, data, size / 2);
    if (io_fault == 1 || (io_fault == 4 && io_writes >= 2)) { errno = ENOSPC; return -1; }
    return __real_write(fd, data, size);
}
int __wrap_fsync(int fd)
{ if (io_fault == 2) { errno = EIO; return -1; } return __real_fsync(fd); }
int __wrap_close(int fd)
{ int result = __real_close(fd); if (io_fault == 3) { errno = EIO; return -1; } return result; }

static int files(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    assert(d);
    while ((e = readdir(d))) if (strncmp(e->d_name, "imported_", 9) == 0) ++n;
    closedir(d);
    return n;
}

int main(void)
{
    char dir[] = "/tmp/qp-portable-fail-XXXXXX";
    wchar_t wdir[600], source[600], bundle[600];
    assert(mkdtemp(dir));
    qp_utf8_to_wide(dir, wdir, 600);
    storage_set_data_dir(wdir);
    swprintf(source, 600, L"%ls/source.csv", wdir);
    swprintf(bundle, 600, L"%ls/shared.json", wdir);
    assert(storage_write_text(source, "key,value\na,one\n"));
    AppData *sender = appdata_create();
    assert(sender);
    sender->tables = dtref_create(L"one", source);
    dtref_list_append(&sender->tables, dtref_create(L"two", source));
    assert(sender->tables && sender->tables->next);
    fa_inject(0); assert(storage_export(sender, bundle));
    size_t allocations = fa_calls;
    fa_disable();
    char *expected = storage_read_text(bundle);
    size_t base = fa_live;
    for (size_t i = 1; i <= allocations; ++i) {
        fa_inject(i); int ok = storage_export(sender, bundle); fa_disable();
        assert(fa_hit && !ok);
        char *actual = storage_read_text(bundle);
        assert(actual && !strcmp(expected, actual));
        free(actual); assert(fa_live == base);
    }
    AppData *receiver = appdata_create();
    int added, updated;
    assert(receiver);
    fa_inject(0); assert(storage_import(receiver, bundle, QP_IMPORT_MERGE, &added, &updated));
    allocations = fa_calls;
    fa_disable();
    base = fa_live;
    int original_files = files(dir);
    DataTableRef *before = receiver->tables;
    for (size_t i = 1; i <= allocations; ++i) {
        fa_inject(i);
        int ok = storage_import(receiver, bundle, QP_IMPORT_REPLACE, &added, &updated);
        fa_disable();
        assert(fa_hit && !ok && added == 0 && updated == 0);
        assert(receiver->tables == before && files(dir) == original_files);
        assert(fa_live == base);
    }
    for (io_fault = 1; io_fault <= 4; ++io_fault) {
        io_writes = 0;
        assert(!storage_import(receiver, bundle, QP_IMPORT_REPLACE, &added, &updated));
        assert(receiver->tables == before && files(dir) == original_files);
        assert(added == 0 && updated == 0 && fa_live == base);
    }
    io_fault = 0;
    /* Roll back an earlier table when the next payload is invalid. */
    assert(storage_write_text(bundle,
        "{\"dataTables\":[{\"id\":\"one\",\"path\":\"../../one.csv\",\"contentHex\":\"610a\"},"
        "{\"id\":\"two\",\"path\":\"/etc/two.csv\",\"contentHex\":\"x0\"}]}"));
    assert(!storage_import(receiver, bundle, QP_IMPORT_REPLACE, &added, &updated));
    assert(receiver->tables == before && files(dir) == original_files);
    free(expected); appdata_free(sender); appdata_free(receiver);
    assert(!fa_live);
    /* Remove only this test's private directory contents. */
    DIR *d = opendir(dir); struct dirent *e; char path[1200];
    assert(d);
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name); assert(!unlink(path));
    }
    closedir(d); assert(!rmdir(dir));
    storage_set_data_dir(NULL);
    puts("portable failures: allocation rollback, original export/model/files preserved");
    return 0;
}
