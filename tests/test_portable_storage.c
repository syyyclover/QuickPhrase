#include "storage.h"
#include "table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static void raw_encodings(const wchar_t *dir)
{
    static const unsigned char utf16[] = {
        0xff,0xfe,'k',0,',',0,'v',0,'\n',0,'A',0,',',0,'B',0,'\n',0
    };
    static const unsigned char gbk[] = {'k','\t','v','\n','A','\t',0xd6,0xd0,'\n'};
    const unsigned char *raw[] = {utf16, gbk};
    const size_t sizes[] = {sizeof(utf16), sizeof(gbk)};
    const int encodings[] = {QP_ENC_UTF16, QP_ENC_GBK};
    wchar_t source[600], bundle[600];
    swprintf(source, 600, L"%ls/encoded.dat", dir);
    swprintf(bundle, 600, L"%ls/encoded.json", dir);
    char *path = qp_wide_to_utf8_alloc(source);
    assert(path);
    for (int i = 0; i < 2; ++i) {
        FILE *f = fopen(path, "wb");
        assert(f && fwrite(raw[i], 1, sizes[i], f) == sizes[i]);
        assert(!fclose(f));
        AppData *sender = appdata_create(), *receiver = appdata_create();
        assert(sender && receiver);
        sender->tables = dtref_create(L"encoded", source);
        assert(sender->tables);
        sender->tables->encoding = encodings[i];
        sender->tables->delimiter = i ? QP_DELIM_TAB : QP_DELIM_COMMA;
        sender->tables->has_header = 1;
        assert(storage_export(sender, bundle));
        assert(storage_import(receiver, bundle, QP_IMPORT_REPLACE, NULL, NULL));
        assert(receiver->tables && receiver->tables->encoding == encodings[i]);
        assert(receiver->tables->delimiter == sender->tables->delimiter);
        assert(receiver->tables->has_header == 1);
        size_t size;
        char *actual = storage_read_bytes(receiver->tables->path, &size);
        assert(actual && size == sizes[i] && !memcmp(actual, raw[i], size));
        free(actual); appdata_free(receiver); appdata_free(sender);
    }
    free(path);
}

int main(int argc, char **argv)
{
    wchar_t *dir = qp_utf8_to_wide_alloc(argc > 1 ? argv[1] : "/tmp/qp-portable-storage-test");
    wchar_t source[600], bundle[600], err[256];
    AppData *sender = appdata_create(), *receiver = appdata_create();
    int added, updated;
    assert(dir && sender && receiver);
    storage_set_data_dir(dir);
    assert(storage_ensure_dir());
    swprintf(source, 600, L"%ls/source.csv", dir);
    swprintf(bundle, 600, L"%ls/shared.json", dir);
    assert(storage_write_text(source, "key,value\r\na,\"hello, world\"\r\n"));
    sender->tables = dtref_create(L"portable", source);
    assert(sender->tables);
    sender->tables->encoding = QP_ENC_UTF8;
    sender->tables->delimiter = QP_DELIM_COMMA;
    sender->tables->has_header = 1;
    assert(storage_export(sender, bundle));
    char *json = storage_read_text(bundle);
    char *path8 = qp_wide_to_utf8_alloc(source);
    assert(json && path8 && !strstr(json, path8));
    assert(storage_write_text(source, "key,value\na,changed\n"));
    assert(storage_import(receiver, bundle, QP_IMPORT_MERGE, &added, &updated));
    assert(receiver->tables && wcscmp(receiver->tables->path, source));
    qp_table_set_refs(receiver->tables);
    QpTable *table = qp_table_get(receiver->tables->id, err, 256);
    assert(table && qp_table_rows(table) == 1);
    assert(!wcscmp(qp_table_cell(table, 0, 1), L"hello, world"));
    assert(storage_save(receiver));
    AppData *loaded = storage_load(NULL);
    assert(loaded && loaded->tables);
    qp_table_set_refs(loaded->tables);
    table = qp_table_get(loaded->tables->id, err, 256);
    assert(table && !wcscmp(qp_table_cell(table, 0, 1), L"hello, world"));
    /* A failed export must retain the previous complete bundle. */
    qp_strlcpy(sender->tables->path, L"missing-table-for-portable-regression.csv", QP_PATH_MAX);
    assert(!storage_export(sender, bundle));
    char *after = storage_read_text(bundle);
    assert(after && !strcmp(after, json));
    free(after); free(json); free(path8);
    /* A malformed embedded table must not replace the live model. */
    assert(storage_write_text(bundle,
        "{\"templates\":[],\"dataTables\":[{\"id\":\"bad\",\"path\":\"../escape.csv\",\"contentHex\":\"0g\"}]}"));
    DataTableRef *before = receiver->tables;
    assert(!storage_import(receiver, bundle, QP_IMPORT_REPLACE, &added, &updated));
    assert(receiver->tables == before && added == 0 && updated == 0);
    appdata_free(loaded); appdata_free(receiver); appdata_free(sender);
    raw_encodings(dir);
    storage_set_data_dir(NULL); free(dir);
    puts("portable storage: moved bundle, snapshot lookup, reload, failed export/import passed");
    return 0;
}
