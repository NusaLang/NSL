// Plugin SQLite -- db_buka/db_jalankan/db_query/db_tutup. SQLite
// di-vendor sebagai satu file amalgamation (vendor/sqlite3.c, public
// domain), dikompile langsung jadi bagian .so ini, bukan link ke
// libsqlite3 sistem -- binary nusa inti tetep gak nyentuh SQLite.

#include "plugin_abi.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "base64.hpp"
#include "json.hpp"
#include "value.hpp"
#include "vendor/sqlite3.h"

namespace {

// db_buka() balikin ID kecil (bukan pointer sqlite3* mentah, gak ada
// risiko presisi double). Konkurensi ke satu koneksi diserahin ke
// SQLITE_THREADSAFE=1 (serialized mode).
std::mutex g_tableMu;
std::unordered_map<int, sqlite3*> g_handles;
int g_nextHandle = 1;

int registerHandle(sqlite3* db) {
    std::lock_guard<std::mutex> lock(g_tableMu);
    int id = g_nextHandle++;
    g_handles[id] = db;
    return id;
}

sqlite3* lookupHandle(int id) {
    std::lock_guard<std::mutex> lock(g_tableMu);
    auto it = g_handles.find(id);
    return it == g_handles.end() ? nullptr : it->second;
}

void dropHandle(int id) {
    std::lock_guard<std::mutex> lock(g_tableMu);
    g_handles.erase(id);
}

NsValue nsString(const std::string& s) {
    NsValue v{};
    v.type = NS_STRING;
    v.str = strdup(s.c_str());
    return v;
}

NsValue nsNull() {
    NsValue v{};
    v.type = NS_NULL;
    return v;
}

NsValue nsNumber(double n) {
    NsValue v{};
    v.type = NS_NUMBER;
    v.number = n;
    return v;
}

// {"ok":false,"error":"..."} -- envelope dipakai bareng sama
// db_jalankan/db_query biar sisi .ns bisa nge-cek `.ok` yang sama buat
// keduanya.
NsValue errorEnvelope(const std::string& msg) {
    Value m = Value::newMap();
    (*m.map())["ok"] = Value::fromBool(false);
    (*m.map())["error"] = Value::fromString(msg);
    return nsString(json::encode(m));
}

// Bind satu larik parameter (Value array, hasil json::decode atas
// argumen ke-3 yang opsional) ke placeholder `?1, ?2, ...` -- posisi
// SQLite 1-based, larik Nusantara 0-based.
bool bindParams(sqlite3_stmt* stmt, const Value& params, std::string& errOut) {
    if (params.type != ValueType::Array) {
        errOut = "parameter query harus larik (json_encode([...]))";
        return false;
    }
    for (size_t i = 0; i < params.array()->size(); i++) {
        int idx = static_cast<int>(i) + 1;
        const Value& p = (*params.array())[i];
        int rc = SQLITE_OK;
        switch (p.type) {
            case ValueType::Null:
                rc = sqlite3_bind_null(stmt, idx);
                break;
            case ValueType::Bool:
                rc = sqlite3_bind_int(stmt, idx, p.boolean() ? 1 : 0);
                break;
            case ValueType::Number:
                rc = sqlite3_bind_double(stmt, idx, p.number);
                break;
            case ValueType::String:
                rc = sqlite3_bind_text(stmt, idx, p.str().data(), static_cast<int>(p.str().size()),
                                        SQLITE_TRANSIENT);
                break;
            default:
                errOut = "parameter ke-" + std::to_string(i) + " harus null/boolean/angka/teks";
                return false;
        }
        if (rc != SQLITE_OK) {
            errOut = "gagal bind parameter ke-" + std::to_string(i);
            return false;
        }
    }
    return true;
}

// Satu kolom hasil SELECT -> Value, sesuai tipe runtime SQLite-nya
// (dynamic typing per-cell, bukan per-kolom). BLOB di-base64-encode
// jadi teks (SQLite nggak punya notion "tipe blob" di JSON -- caller
// yang tau kolom mana blob bisa base64_decode() sendiri).
Value columnValue(sqlite3_stmt* stmt, int col) {
    switch (sqlite3_column_type(stmt, col)) {
        case SQLITE_INTEGER:
            return Value::fromNumber(static_cast<double>(sqlite3_column_int64(stmt, col)));
        case SQLITE_FLOAT:
            return Value::fromNumber(sqlite3_column_double(stmt, col));
        case SQLITE_NULL:
            return Value::null();
        case SQLITE_BLOB: {
            const char* data = static_cast<const char*>(sqlite3_column_blob(stmt, col));
            int n = sqlite3_column_bytes(stmt, col);
            return Value::fromString(base64::encode(std::string(data, static_cast<size_t>(n))));
        }
        default: {  // SQLITE_TEXT
            const unsigned char* text = sqlite3_column_text(stmt, col);
            return Value::fromString(text ? std::string(reinterpret_cast<const char*>(text)) : std::string());
        }
    }
}

// Argumen ke-3 opsional (JSON larik parameter). Balikin larik kosong
// kalau nggak dikasih; nullptr `errOut` kalau JSON-nya invalid.
bool decodeOptionalParams(int argc, const NsValue* argv, Value& outParams, std::string& errOut) {
    if (argc < 3) {
        outParams = Value::newArray();
        return true;
    }
    if (argv[2].type != NS_STRING) {
        errOut = "argumen parameter (ke-3) harus teks JSON";
        return false;
    }
    try {
        outParams = json::decode(argv[2].str ? argv[2].str : "[]");
    } catch (const std::exception& e) {
        errOut = std::string("parameter JSON invalid: ") + e.what();
        return false;
    }
    return true;
}

NsValue dbBuka(int argc, const NsValue* argv) {
    if (argc != 1 || argv[0].type != NS_STRING) return nsNull();
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(argv[0].str, &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close_v2(db);
        return nsNull();
    }
    // busy_timeout: kalau ada goroutine lain lagi nulis ke koneksi
    // terpisah ke file yang sama, tunggu sampai 5 detik alih-alih
    // langsung gagal SQLITE_BUSY -- basic tapi penting buat kasus
    // multi-goroutine yang jadi alasan modul ini dibikin.
    sqlite3_busy_timeout(db, 5000);
    return nsNumber(registerHandle(db));
}

NsValue dbTutup(int argc, const NsValue* argv) {
    if (argc != 1 || argv[0].type != NS_NUMBER) return NsValue{NS_BOOL, 0, 0, nullptr};
    int handle = static_cast<int>(argv[0].number);
    sqlite3* db = lookupHandle(handle);
    if (!db) return NsValue{NS_BOOL, 0, 0, nullptr};
    dropHandle(handle);
    sqlite3_close_v2(db);
    return NsValue{NS_BOOL, 1, 0, nullptr};
}

NsValue dbJalankan(int argc, const NsValue* argv) {
    if (argc < 2 || argv[0].type != NS_NUMBER || argv[1].type != NS_STRING) {
        return errorEnvelope("db_jalankan(handle, sql, [param]): argumen nggak valid");
    }
    sqlite3* db = lookupHandle(static_cast<int>(argv[0].number));
    if (!db) return errorEnvelope("handle database nggak valid (udah ditutup?)");

    Value params;
    std::string err;
    if (!decodeOptionalParams(argc, argv, params, err)) return errorEnvelope(err);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, argv[1].str, -1, &stmt, nullptr) != SQLITE_OK) {
        return errorEnvelope(sqlite3_errmsg(db));
    }
    if (!bindParams(stmt, params, err)) {
        sqlite3_finalize(stmt);
        return errorEnvelope(err);
    }

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        return errorEnvelope(sqlite3_errmsg(db));
    }

    Value out = Value::newMap();
    (*out.map())["ok"] = Value::fromBool(true);
    (*out.map())["rows_affected"] = Value::fromNumber(sqlite3_changes(db));
    (*out.map())["last_insert_id"] = Value::fromNumber(static_cast<double>(sqlite3_last_insert_rowid(db)));
    return nsString(json::encode(out));
}

NsValue dbQuery(int argc, const NsValue* argv) {
    if (argc < 2 || argv[0].type != NS_NUMBER || argv[1].type != NS_STRING) {
        return errorEnvelope("db_query(handle, sql, [param]): argumen nggak valid");
    }
    sqlite3* db = lookupHandle(static_cast<int>(argv[0].number));
    if (!db) return errorEnvelope("handle database nggak valid (udah ditutup?)");

    Value params;
    std::string err;
    if (!decodeOptionalParams(argc, argv, params, err)) return errorEnvelope(err);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, argv[1].str, -1, &stmt, nullptr) != SQLITE_OK) {
        return errorEnvelope(sqlite3_errmsg(db));
    }
    if (!bindParams(stmt, params, err)) {
        sqlite3_finalize(stmt);
        return errorEnvelope(err);
    }

    Value rows = Value::newArray();
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        Value row = Value::newMap();
        int nCols = sqlite3_column_count(stmt);
        for (int c = 0; c < nCols; c++) {
            std::string colName = sqlite3_column_name(stmt, c);
            (*row.map())[colName] = columnValue(stmt, c);
        }
        rows.array()->push_back(row);
    }
    bool ok = (rc == SQLITE_DONE);
    std::string errMsg = ok ? "" : sqlite3_errmsg(db);
    sqlite3_finalize(stmt);
    if (!ok) return errorEnvelope(errMsg);

    Value out = Value::newMap();
    (*out.map())["ok"] = Value::fromBool(true);
    (*out.map())["rows"] = rows;
    return nsString(json::encode(out));
}

}  // namespace

extern "C" void ns_plugin_init(void* registry, NsRegisterFn reg) {
    reg(registry, "db_buka", dbBuka);
    reg(registry, "db_tutup", dbTutup);
    reg(registry, "db_jalankan", dbJalankan);
    reg(registry, "db_query", dbQuery);
}
