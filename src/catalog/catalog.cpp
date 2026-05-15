/**
 * @file catalog.cpp
 * @brief Catalog implementation — with persistence
 */
#include "catalog/catalog.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>

namespace minidb {

static bool read_exact(FILE* f, void* out, size_t size) {
    return size == 0 || fread(out, size, 1, f) == 1;
}

static bool read_string(FILE* f, String* out) {
    if (!out) return false;
    u32 len = 0;
    if (!read_exact(f, &len, sizeof(len))) return false;
    static constexpr u32 kMaxCatalogStringLen = 1 << 20;
    if (len > kMaxCatalogStringLen) return false;
    char* buf = new char[len + 1];
    if (!read_exact(f, buf, len)) {
        delete[] buf;
        return false;
    }
    buf[len] = '\0';
    *out = String(buf, len);
    delete[] buf;
    return true;
}

static bool is_parser_artifact_name(const String& name) {
    return name.empty() || name == "unexpected token" || name == "unexpected keyword";
}

static Schema sanitize_schema(const Schema& schema) {
    Schema clean;
    for (u32 i = 0; i < schema.column_count(); i++) {
        const Column& col = schema.get_column(i);
        if (is_parser_artifact_name(col.name)) continue;
        clean.add_column(col);
    }
    return clean;
}

Catalog::Catalog() : next_table_id_(10), next_index_id_(1000) {}

u32 Catalog::create_table(const String& name, const Schema& schema) {
    Schema clean_schema = sanitize_schema(schema);
    if (is_parser_artifact_name(name) || clean_schema.column_count() == 0 ||
        name_to_table_id_.contains(name)) {
        return 0;  // already exists
    }

    u32 table_id = next_table_id_++;

    TableEntry entry;
    entry.table_id = table_id;
    entry.table_name = name;
    entry.first_page_id = kNullPageId;
    entry.num_pages = 0;
    entry.num_tuples = 0;
    entry.stat_num_tuples = 0;
    entry.stat_num_pages = 0;
    entry.schema = clean_schema;

    table_entries_[table_id] = entry;
    name_to_table_id_[name] = table_id;

    return table_id;
}

TableEntry* Catalog::get_table(const String& name) {
    auto* tid = name_to_table_id_.find(name);
    if (!tid) return nullptr;
    return table_entries_.find(*tid);
}

TableEntry* Catalog::get_table(u32 table_id) {
    return table_entries_.find(table_id);
}

bool Catalog::drop_table(const String& name) {
    auto* tid = name_to_table_id_.find(name);
    if (!tid) return false;

    drop_indexes_for_table(*tid);
    table_entries_.erase(*tid);
    name_to_table_id_.erase(name);
    return true;
}

void Catalog::drop_indexes_for_table(u32 table_id) {
    Vector<String> to_remove;
    for (auto it = index_entries_.begin(); it; it = index_entries_.next(it)) {
        if (it->value.table_id == table_id) {
            to_remove.push_back(it->value.index_name);
        }
    }
    for (u32 i = 0; i < to_remove.size(); i++) {
        auto* iid = name_to_index_id_.find(to_remove[i]);
        if (!iid) continue;
        index_entries_.erase(*iid);
        name_to_index_id_.erase(to_remove[i]);
    }
}

u32 Catalog::create_index(const String& name, u32 table_id,
                          const Vector<u32>& key_columns, bool unique) {
    TableEntry* table = get_table(table_id);
    if (is_parser_artifact_name(name) || !table || key_columns.empty() ||
        name_to_index_id_.contains(name)) {
        return 0;
    }
    for (u32 i = 0; i < key_columns.size(); i++) {
        if (key_columns[i] >= table->schema.column_count()) return 0;
    }

    u32 index_id = next_index_id_++;

    IndexEntry entry;
    entry.index_id = index_id;
    entry.index_name = name;
    entry.table_id = table_id;
    entry.key_columns = key_columns;
    entry.root_page_id = kNullPageId;
    entry.is_unique = unique;

    index_entries_[index_id] = entry;
    name_to_index_id_[name] = index_id;

    return index_id;
}

IndexEntry* Catalog::get_index(const String& name) {
    auto* iid = name_to_index_id_.find(name);
    if (!iid) return nullptr;
    return index_entries_.find(*iid);
}

IndexEntry* Catalog::get_index(u32 index_id) {
    return index_entries_.find(index_id);
}

Vector<IndexEntry*> Catalog::get_indexes(u32 table_id) {
    Vector<IndexEntry*> result;
    for (auto it = index_entries_.begin(); it; it = index_entries_.next(it)) {
        if (it->value.table_id == table_id) {
            result.push_back(&it->value);
        }
    }
    return result;
}

bool Catalog::drop_index(const String& name) {
    auto* iid = name_to_index_id_.find(name);
    if (!iid) return false;
    index_entries_.erase(*iid);
    name_to_index_id_.erase(name);
    return true;
}

bool Catalog::is_column_indexed(u32 table_id, u32 col_idx) const {
    for (auto it = const_cast<HashMap<u32, IndexEntry>&>(index_entries_).begin();
         it; it = const_cast<HashMap<u32, IndexEntry>&>(index_entries_).next(it)) {
        if (it->value.table_id != table_id) continue;
        for (u32 k = 0; k < it->value.key_columns.size(); k++) {
            if (it->value.key_columns[k] == col_idx) return true;
        }
    }
    // Primary key is also an index
    TableEntry* te = const_cast<Catalog*>(this)->get_table(table_id);
    if (te) {
        for (u32 i = 0; i < te->schema.column_count(); i++) {
            if (te->schema.get_column(i).is_primary && i == col_idx) return true;
        }
    }
    return false;
}

bool Catalog::any_column_indexed(u32 table_id, const Vector<u32>& col_indices) const {
    for (u32 i = 0; i < col_indices.size(); i++) {
        if (is_column_indexed(table_id, col_indices[i])) return true;
    }
    return false;
}

// ============================================================
// Persistence: serialize all table and index metadata to file
// ============================================================

void Catalog::save(const String& path) {
    String tmp_path = path + ".tmp";
    FILE* f = fopen(tmp_path.c_str(), "wb");
    if (!f) return;

    // magic + version
    fwrite("MDBC", 4, 1, f);
    u32 version = 1;
    fwrite(&version, 4, 1, f);

    // next IDs
    fwrite(&next_table_id_, 4, 1, f);
    fwrite(&next_index_id_, 4, 1, f);

    // tables
    u32 table_count = 0;
    for (auto it = table_entries_.begin(); it; it = table_entries_.next(it)) table_count++;
    fwrite(&table_count, 4, 1, f);

    for (auto it = table_entries_.begin(); it; it = table_entries_.next(it)) {
        TableEntry& e = it->value;
        fwrite(&e.table_id, 4, 1, f);
        u32 name_len = e.table_name.size();
        fwrite(&name_len, 4, 1, f);
        fwrite(e.table_name.c_str(), name_len, 1, f);
        fwrite(&e.first_page_id, 8, 1, f);
        fwrite(&e.num_pages, 4, 1, f);
        fwrite(&e.num_tuples, 8, 1, f);
        u32 schema_size = e.schema.serialized_size();
        byte* buf = new byte[schema_size];
        byte* end = e.schema.serialize(buf);
        u32 actual_size = static_cast<u32>(end - buf);
        fwrite(&actual_size, 4, 1, f);
        fwrite(buf, actual_size, 1, f);
        delete[] buf;
    }

    // indexes
    u32 index_count = 0;
    for (auto it = index_entries_.begin(); it; it = index_entries_.next(it)) index_count++;
    fwrite(&index_count, 4, 1, f);

    for (auto it = index_entries_.begin(); it; it = index_entries_.next(it)) {
        IndexEntry& e = it->value;
        fwrite(&e.index_id, 4, 1, f);
        u32 name_len = e.index_name.size();
        fwrite(&name_len, 4, 1, f);
        fwrite(e.index_name.c_str(), name_len, 1, f);
        fwrite(&e.table_id, 4, 1, f);
        u32 num_keys = e.key_columns.size();
        fwrite(&num_keys, 4, 1, f);
        for (u32 k = 0; k < num_keys; k++) {
            fwrite(&e.key_columns[k], 4, 1, f);
        }
        fwrite(&e.root_page_id, 8, 1, f);
        u8 uniq = e.is_unique ? 1 : 0;
        fwrite(&uniq, 1, 1, f);
    }

    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);
    fclose(f);
    if (rename(tmp_path.c_str(), path.c_str()) == 0) {
        const char* raw = path.c_str();
        const char* slash = nullptr;
        for (const char* p = raw; *p; p++) {
            if (*p == '/') slash = p;
        }
        if (slash) {
            String dir(raw, static_cast<u32>(slash - raw));
            int dir_fd = open(dir.c_str(), O_RDONLY);
            if (dir_fd >= 0) {
                fsync(dir_fd);
                close(dir_fd);
            }
        }
    } else {
        unlink(tmp_path.c_str());
    }
}

void Catalog::load(const String& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;

    char magic[4];
    if (fread(magic, 4, 1, f) != 1 || memcmp(magic, "MDBC", 4) != 0) {
        fclose(f);
        return;
    }

    u32 version;
    if (!read_exact(f, &version, 4) ||
        !read_exact(f, &next_table_id_, 4) ||
        !read_exact(f, &next_index_id_, 4)) {
        fclose(f);
        return;
    }

    // tables
    u32 table_count;
    if (!read_exact(f, &table_count, 4) || table_count > 1000000) {
        fclose(f);
        return;
    }
    for (u32 i = 0; i < table_count; i++) {
        TableEntry e;
        if (!read_exact(f, &e.table_id, 4) ||
            !read_string(f, &e.table_name) ||
            !read_exact(f, &e.first_page_id, 8) ||
            !read_exact(f, &e.num_pages, 4) ||
            !read_exact(f, &e.num_tuples, 8)) {
            break;
        }
        e.stat_num_pages = e.num_pages;
        e.stat_num_tuples = e.num_tuples;
        u32 schema_size;
        if (!read_exact(f, &schema_size, 4) || schema_size > (1u << 20)) break;
        byte* schema_buf = new byte[schema_size];
        if (!read_exact(f, schema_buf, schema_size)) {
            delete[] schema_buf;
            break;
        }
        e.schema = sanitize_schema(Schema::deserialize(schema_buf, schema_size));
        delete[] schema_buf;

        if (is_parser_artifact_name(e.table_name) || e.schema.column_count() == 0) {
            continue;
        }
        table_entries_[e.table_id] = e;
        name_to_table_id_[e.table_name] = e.table_id;
    }

    // indexes
    u32 index_count;
    if (!read_exact(f, &index_count, 4) || index_count > 1000000) {
        fclose(f);
        return;
    }
    for (u32 i = 0; i < index_count; i++) {
        IndexEntry e;
        if (!read_exact(f, &e.index_id, 4) ||
            !read_string(f, &e.index_name) ||
            !read_exact(f, &e.table_id, 4)) {
            break;
        }
        u32 num_keys;
        if (!read_exact(f, &num_keys, 4) || num_keys > 1024) break;
        e.key_columns.resize(num_keys);
        for (u32 k = 0; k < num_keys; k++) {
            if (!read_exact(f, &e.key_columns[k], 4)) {
                num_keys = k;
                break;
            }
        }
        if (e.key_columns.size() != num_keys) break;
        if (!read_exact(f, &e.root_page_id, 8)) break;
        u8 uniq;
        if (!read_exact(f, &uniq, 1)) break;
        e.is_unique = (uniq != 0);
        TableEntry* table = get_table(e.table_id);
        if (is_parser_artifact_name(e.index_name) || !table) continue;
        bool valid_keys = e.key_columns.size() > 0;
        for (u32 k = 0; k < e.key_columns.size(); k++) {
            if (e.key_columns[k] >= table->schema.column_count()) {
                valid_keys = false;
                break;
            }
        }
        if (!valid_keys) continue;

        index_entries_[e.index_id] = e;
        name_to_index_id_[e.index_name] = e.index_id;
    }

    fclose(f);
}

void Catalog::for_each_table(void (*callback)(TableEntry&, void*), void* ctx) {
    for (auto it = table_entries_.begin(); it; it = table_entries_.next(it)) {
        callback(it->value, ctx);
    }
}

void Catalog::for_each_index(void (*callback)(IndexEntry&, void*), void* ctx) {
    for (auto it = index_entries_.begin(); it; it = index_entries_.next(it)) {
        callback(it->value, ctx);
    }
}

} // namespace minidb
