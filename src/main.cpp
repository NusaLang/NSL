// Nusantara (.ns) toolchain -- native C++ port.
// Usage: nusantara run <file.ns>
//        nusantara            (drops into a REPL, like plain `python`)

#include <algorithm>
#include <cctype>
#include <map>
#include <chrono>
#include <cstdio>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <set>
#include <functional>
#include <thread>

#include <csignal>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "base64.hpp"
#include "gc.hpp"
#include "gil.hpp"
#include "i18n.hpp"
#include "interpreter.hpp"
#include "js_runtime.hpp"
#include "json.hpp"
#include "lexer.hpp"
#include "net.hpp"
#include "parser.hpp"
#include "plugin.hpp"
#include "proc.hpp"
#include "sha256.hpp"
#include "sysplugin.hpp"
#include "typechecker.hpp"
#include "vm.hpp"

#define NUSA_VERSION "0.0.1"

namespace {

// Reports the four exception types the lexer/parser/interpreter throw,
// shared by both the file runner and the REPL.
void reportError(const std::exception& e) {
    std::cerr << "nusantara: error: " << e.what() << '\n';
}

// Force-closes any socket a script forgot to tcp_tutup(), on every return path.
struct NetCleanupGuard {
    ~NetCleanupGuard() { net::closeAllTracked(); }
};

void waitForGoroutines() {
    while (GC::liveGoroutines.load() > 0) {
        GilRelease release;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Directory a relative impor() path resolves against, regardless of cwd.
std::string dirOf(const std::string& path) {
    size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? "." : path.substr(0, pos);
}

// .js files dispatch to a separate engine (QuickJS, js_runtime.hpp/cpp).
bool hasExtension(const std::string& path, const std::string& ext) {
    return path.size() >= ext.size() && path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
}

int runFile(const std::string& path) {
    if (hasExtension(path, ".js")) {
        return jsrt::runJsFile(path);
    }
    std::ifstream file(path);
    if (!file) {
        std::cerr << "nusantara: error: " << i18n::tr("gagal buka file '", "could not open file '") << path << "'\n";
        return 1;
    }
    std::ostringstream buf;
    buf << file.rdbuf();
    std::string source = buf.str();

    NetCleanupGuard netGuard;
    try {
        Lexer lexer(source);
        std::vector<Token> tokens = lexer.tokenize();

        Parser parser(std::move(tokens));
        std::unique_ptr<Program> program = parser.parse();

        TypeChecker checker;
        checker.check(*program);
        
        // compute transitive hash
        std::set<std::string> visited;
        std::vector<std::string> deps = {path};
        visited.insert(path);
        
        std::function<void(const Expr*)> findDeps = [&](const Expr* expr) {
            if (!expr) return;
            if (expr->kind == ExprKind::Call) {
                auto* n = static_cast<const CallExpr*>(expr);
                if (n->callee->kind == ExprKind::Identifier) {
                    auto* id = static_cast<const IdentifierExpr*>(n->callee.get());
                    if ((id->name == "impor" || id->name == "import") && n->args.size() == 1 && n->args[0]->kind == ExprKind::Literal) {
                        auto* lit = static_cast<const LiteralExpr*>(n->args[0].get());
                        if (lit->litKind == LiteralExpr::Kind::String) {
                            std::string depPath = dirOf(path) + "/" + lit->str;
                            if (visited.insert(depPath).second) {
                                deps.push_back(depPath);
                                std::ifstream f(depPath);
                                if (f) {
                                    std::ostringstream b; b << f.rdbuf();
                                    Lexer lex(b.str());
                                    try {
                                        Parser p(lex.tokenize());
                                        auto subProg = p.parse();
                                        for (const auto& s : subProg->statements) {
                                            if (s->kind == StmtKind::ExprStmt) findDeps(static_cast<const ExprStmtNode*>(s.get())->expr.get());
                                            else if (s->kind == StmtKind::Let) findDeps(static_cast<const LetStmt*>(s.get())->value.get());
                                        }
                                    } catch (...) {}
                                }
                            }
                        }
                    }
                }
                findDeps(n->callee.get());
                for (auto& a : n->args) findDeps(a.get());
            } else if (expr->kind == ExprKind::Binary) {
                auto* n = static_cast<const BinaryExpr*>(expr);
                findDeps(n->left.get()); findDeps(n->right.get());
            } else if (expr->kind == ExprKind::Unary) {
                findDeps(static_cast<const UnaryExpr*>(expr)->operand.get());
            } else if (expr->kind == ExprKind::ArrayLit) {
                for (auto& a : static_cast<const ArrayLitExpr*>(expr)->elements) findDeps(a.get());
            } else if (expr->kind == ExprKind::Index) {
                auto* n = static_cast<const IndexExpr*>(expr);
                findDeps(n->target.get()); findDeps(n->index.get());
            }
        };
        for (const auto& s : program->statements) {
            if (s->kind == StmtKind::ExprStmt) findDeps(static_cast<const ExprStmtNode*>(s.get())->expr.get());
            else if (s->kind == StmtKind::Let) findDeps(static_cast<const LetStmt*>(s.get())->value.get());
            else if (s->kind == StmtKind::Block) {
                for (const auto& bs : static_cast<const BlockStmt*>(s.get())->statements) {
                    if (bs->kind == StmtKind::ExprStmt) findDeps(static_cast<const ExprStmtNode*>(bs.get())->expr.get());
                    else if (bs->kind == StmtKind::Let) findDeps(static_cast<const LetStmt*>(bs.get())->value.get());
                }
            }
        }
        
        std::string combinedHash = "";
        for (const auto& d : deps) {
            struct stat st;
            if (stat(d.c_str(), &st) == 0) {
                combinedHash += d + ":" + std::to_string(st.st_mtime) + ":" + std::to_string(st.st_size) + "|";
            }
        }
        // quick hash string
        size_t h = std::hash<std::string>{}(combinedHash);
        std::string finalHash = std::to_string(h);
        std::string cacheDir = dirOf(path) + "/.nusa-cache";
        size_t lastSlash = path.find_last_of('/');
        std::string fname = (lastSlash == std::string::npos) ? path : path.substr(lastSlash + 1);
        std::string cachePath = cacheDir + "/_" + fname + ".bin";
        
        std::unique_ptr<VmProgram> vmProgram = vmDeserialize(cachePath, finalHash);
        if (vmProgram) {
            // Cache hit: bytecode came from disk without the native-code JIT
            // attachment (vmSerialize never writes it) -- re-derive it here.
            vmAttachNativeFunctions(*vmProgram, *program);
            Interpreter interpreter(dirOf(path));
            try {
                int rc = vmRun(*vmProgram, &interpreter);
                waitForGoroutines();
                return rc;
            } catch (const VmRuntimeError& e) {
                std::cerr << "nusa --vm: error: " << e.what() << "\n";
                return 1;
            } catch (const RuntimeError& e) {
                reportError(e);
                return 1;
            }
        }

        try {
            vmProgram = vmCompile(*program);
            mkdir(cacheDir.c_str(), 0755);
            vmSerialize(*vmProgram, cachePath, finalHash);
            
            Interpreter interpreter(dirOf(path));
            try {
                int rc = vmRun(*vmProgram, &interpreter);
                waitForGoroutines();
                return rc;
            } catch (const VmRuntimeError& e) {
                std::cerr << "nusa --vm: error: " << e.what() << "\n";
                return 1;
            } catch (const RuntimeError& e) {
                reportError(e);
                return 1;
            }
        } catch (const VmCompileError& e) {
            // fallback to tree-walking
        }

        Interpreter interpreter(dirOf(path));
        int rc = 0;
        try {
            interpreter.run(*program);
        } catch (const RuntimeError& e) {
            reportError(e);
            rc = 1;
        }
        waitForGoroutines();
        return rc;
    } catch (const LexError& e) {
        reportError(e);
        return 1;
    } catch (const ParseError& e) {
        reportError(e);
        return 1;
    } catch (const TypeError& e) {
        reportError(e);
        return 1;
    }
}

int runVmFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << "nusantara: error: " << i18n::tr("gagal buka file '", "could not open file '") << path << "'\n";
        return 1;
    }
    std::ostringstream buf;
    buf << file.rdbuf();
    std::string source = buf.str();

    try {
        Lexer lexer(source);
        std::vector<Token> tokens = lexer.tokenize();
        Parser parser(std::move(tokens));
        std::unique_ptr<Program> program = parser.parse();
        std::unique_ptr<VmProgram> vmProgram = vmCompile(*program);
        Interpreter interpreter(dirOf(path));
        try {
            int rc = vmRun(*vmProgram, &interpreter);
            waitForGoroutines();
            return rc;
        } catch (const VmRuntimeError& e) {
            std::cerr << "nusa --vm: error: " << e.what() << "\n";
            return 1;
        } catch (const RuntimeError& e) {
            reportError(e);
            return 1;
        }
    } catch (const LexError& e) {
        reportError(e);
        return 1;
    } catch (const ParseError& e) {
        reportError(e);
        return 1;
    } catch (const VmCompileError& e) {
        std::cerr << "nusantara: error: " << e.what() << "\n";
        return 1;
    }
}

int runEval(const std::string& source) {
    NetCleanupGuard netGuard;
    try {
        Lexer lexer(source);
        std::vector<Token> tokens = lexer.tokenize();

        Parser parser(std::move(tokens));
        std::unique_ptr<Program> program = parser.parse();

        TypeChecker checker;
        checker.check(*program);

        Interpreter interpreter(".");
        int rc = 0;
        try {
            interpreter.run(*program);
        } catch (const RuntimeError& e) {
            reportError(e);
            rc = 1;
        }
        waitForGoroutines();
        return rc;
    } catch (const LexError& e) {
        reportError(e);
        return 1;
    } catch (const ParseError& e) {
        reportError(e);
        return 1;
    } catch (const TypeError& e) {
        reportError(e);
        return 1;
    }
}

int runCheck(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << "nusantara: error: " << i18n::tr("gagal buka file '", "could not open file '") << path << "'\n";
        return 1;
    }
    std::ostringstream buf;
    buf << file.rdbuf();
    std::string source = buf.str();

    try {
        Lexer lexer(source);
        std::vector<Token> tokens = lexer.tokenize();

        Parser parser(std::move(tokens));
        std::unique_ptr<Program> program = parser.parse();

        TypeChecker checker;
        checker.check(*program);
    } catch (const LexError& e) {
        reportError(e);
        return 1;
    } catch (const ParseError& e) {
        reportError(e);
        return 1;
    } catch (const TypeError& e) {
        reportError(e);
        return 1;
    }
    std::cout << path << i18n::tr(": sintaks oke\n", ": syntax ok\n");
    return 0;
}

long long fileMtime(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return -1;
    return static_cast<long long>(st.st_mtime);
}

// Live-reload: run now, then poll mtime and re-run on change. stat()
// polling, no inotify/FSEvents dependency.
int runWatch(const std::string& path) {
    std::cout << i18n::tr("Nusantara -- mode 'watch': ngawasin '", "Nusantara -- 'watch' mode: watching '") << path
               << i18n::tr("'. Simpan file buat langsung dijalanin ulang. Ctrl+C buat keluar.\n",
                            "'. Save the file to re-run it immediately. Ctrl+C to quit.\n");
    std::cout.flush();
    auto runOnce = [&]() {
        std::cout << i18n::tr("\n=== jalanin ulang: ", "\n=== re-running: ") << path << " ===\n";
        runFile(path);
        // stdout is fully buffered when redirected -- flush explicitly.
        std::cout.flush();
    };
    long long lastMtime = fileMtime(path);
    runOnce();
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        long long now = fileMtime(path);
        if (now == -1) continue;  // file momentarily missing (editor doing an atomic save)
        if (now != lastMtime) {
            lastMtime = now;
            runOnce();
        }
    }
}

std::string rtrim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

// One interpreter for the whole session (let/fn from earlier lines stay
// visible). Lines buffer until braces balance -- naive counting, doesn't
// look inside strings/comments.
int runRepl() {
    std::cout << i18n::tr("Nusantara (.ns) REPL -- Ctrl+D untuk keluar\n",
                           "Nusantara (.ns) REPL -- Ctrl+D to exit\n");
    Interpreter interpreter;
    std::string buffer;
    int braceDepth = 0;
    std::string line;
    // fn values point back into their FnDeclStmt -- keep every parsed
    // Program alive for the session so earlier functions stay callable.
    std::vector<std::unique_ptr<Program>> history;

    while (true) {
        std::cout << (braceDepth > 0 ? "...       " : "nusantara> ");
        if (!std::getline(std::cin, line)) {
            std::cout << '\n';
            break;  // EOF (Ctrl+D)
        }
        for (char c : line) {
            if (c == '{') braceDepth++;
            else if (c == '}') braceDepth--;
        }
        buffer += line;
        buffer += '\n';
        if (braceDepth > 0) continue;  // still inside an open block

        NetCleanupGuard netGuard;
        std::string source = buffer;
        buffer.clear();
        braceDepth = 0;

        std::string trimmed = rtrim(source);
        if (trimmed.empty()) continue;

        bool looksLikeBareExpr = trimmed.back() != ';' && trimmed.back() != '}';
        if (looksLikeBareExpr) {
            // Try it as a single expression first, so typing `1 + 1`
            // auto-prints a result the way Python's REPL does.
            try {
                Lexer lexer(source);
                Parser parser(lexer.tokenize());
                ExprPtr expr = parser.parseSingleExpression();
                Value result = interpreter.evalGlobal(expr.get());
                if (result.type != ValueType::Null) {
                    std::cout << result.stringify() << '\n';
                }
                continue;
            } catch (...) {
                // Not a bare expression -- fall through and try it as
                // statement(s) instead (better error message from there).
            }
        }

        try {
            Lexer lexer(source);
            Parser parser(lexer.tokenize());
            history.push_back(parser.parse());
            interpreter.run(*history.back());
        } catch (const LexError& e) {
            reportError(e);
        } catch (const ParseError& e) {
            reportError(e);
        } catch (const RuntimeError& e) {
            reportError(e);
        }
    }
    waitForGoroutines();
    return 0;
}

bool ensureDir(const std::string& path) {
    if (mkdir(path.c_str(), 0755) == 0) return true;
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool ensureDirRecursive(const std::string& path) {
    if (path.empty() || path == "/" || path == ".") return true;
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    size_t pos = path.find_last_of('/');
    if (pos != std::string::npos && !ensureDirRecursive(path.substr(0, pos))) return false;
    return ensureDir(path);
}

// Recursively copies src into dst, skipping .git -- used by `nusa get`.
bool copyTreeSkippingGit(const std::string& src, const std::string& dst) {
    if (!ensureDir(dst)) return false;
    DIR* d = opendir(src.c_str());
    if (!d) return false;
    struct dirent* entry;
    bool ok = true;
    while (ok && (entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (name == ".git") continue;
        std::string srcPath = src + "/" + name;
        std::string dstPath = dst + "/" + name;
        struct stat st{};
        if (lstat(srcPath.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            ok = copyTreeSkippingGit(srcPath, dstPath);
        } else if (S_ISREG(st.st_mode)) {
            std::ifstream in(srcPath, std::ios::binary);
            std::ofstream out(dstPath, std::ios::binary);
            if (!in || !out) {
                ok = false;
            } else {
                out << in.rdbuf();
            }
        }
        // symlinks/devices/etc. inside a cloned repo: skip silently,
        // not something a Nusantara package needs.
    }
    closedir(d);
    return ok;
}

void removeTree(const std::string& path) {
    DIR* d = opendir(path.c_str());
    if (!d) {
        unlink(path.c_str());
        return;
    }
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        std::string full = path + "/" + name;
        struct stat st{};
        if (lstat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            removeTree(full);
        } else {
            unlink(full.c_str());
        }
    }
    closedir(d);
    rmdir(path.c_str());
}

// go.sum-style integrity hash, sorted + NUL-separated so ordering can't
// produce a colliding hash across different file sets.
std::string hashPackageFiles(const Value& files) {
    std::vector<std::pair<std::string, std::string>> entries;
    for (const Value& entry : *files.array()) {
        if (entry.type != ValueType::Map) continue;
        auto nameIt = entry.map()->find("nama");
        auto contentIt = entry.map()->find("isi");
        if (nameIt == entry.map()->end() || contentIt == entry.map()->end()) continue;
        entries.emplace_back(nameIt->second.str(), contentIt->second.str());
    }
    std::sort(entries.begin(), entries.end());
    std::string combined;
    for (auto& [nama, isi] : entries) {
        combined += nama;
        combined += '\0';
        combined += isi;
        combined += '\0';
    }
    return sha256::hexDigest(combined);
}
bool readTextFile(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream buf;
    buf << file.rdbuf();
    out = buf.str();
    return true;
}
std::unordered_map<std::string, std::string> loadManifestDeps(const std::string& path) {
    std::unordered_map<std::string, std::string> deps;
    std::string text;
    if (!readTextFile(path, text)) return deps;
    try {
        Value root = json::decode(text);
        if (root.type != ValueType::Map) return deps;
        auto depsIt = root.map()->find("deps");
        if (depsIt == root.map()->end() || depsIt->second.type != ValueType::Map) return deps;
        for (const auto& [nama, v] : *depsIt->second.map()) {
            if (v.type == ValueType::String) deps[nama] = v.str();
        }
    } catch (const std::exception&) {
        // corrupt nusa.json -- treat as empty rather than blocking `nusa get`
    }
    return deps;
}
struct LockEntry {
    std::string versi;
    std::string sha256;
};
std::unordered_map<std::string, LockEntry> loadLockFile(const std::string& path) {
    std::unordered_map<std::string, LockEntry> lock;
    std::string text;
    if (!readTextFile(path, text)) return lock;
    try {
        Value root = json::decode(text);
        if (root.type != ValueType::Map) return lock;
        for (const auto& [nama, v] : *root.map()) {
            if (v.type != ValueType::Map) continue;
            auto verIt = v.map()->find("versi");
            auto shaIt = v.map()->find("sha256");
            if (verIt == v.map()->end() || shaIt == v.map()->end()) continue;
            lock[nama] = {verIt->second.str(), shaIt->second.str()};
        }
    } catch (const std::exception&) {
        // corrupt nusa-lock.json -- treat as empty; worst case, nusa install
        // re-fetches everything and rewrites a fresh lock
    }
    return lock;
}
std::string jsonStringEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}
// Hand-rolled (not json::encode) so entries come out sorted + indented.
void saveManifestDeps(const std::string& path, const std::unordered_map<std::string, std::string>& deps) {
    std::vector<std::string> names;
    names.reserve(deps.size());
    for (const auto& [nama, _] : deps) names.push_back(nama);
    std::sort(names.begin(), names.end());
    std::ostringstream out;
    out << "{\n  \"deps\": {";
    for (size_t i = 0; i < names.size(); i++) {
        out << (i ? ",\n" : "\n") << "    \"" << jsonStringEscape(names[i]) << "\": \""
            << jsonStringEscape(deps.at(names[i])) << "\"";
    }
    out << (names.empty() ? "" : "\n  ") << "}\n}\n";
    std::ofstream f(path, std::ios::binary);
    f << out.str();
}
void saveLockFile(const std::string& path, const std::unordered_map<std::string, LockEntry>& lock) {
    std::vector<std::string> names;
    names.reserve(lock.size());
    for (const auto& [nama, _] : lock) names.push_back(nama);
    std::sort(names.begin(), names.end());
    std::ostringstream out;
    out << "{";
    for (size_t i = 0; i < names.size(); i++) {
        const LockEntry& e = lock.at(names[i]);
        out << (i ? ",\n" : "\n") << "  \"" << jsonStringEscape(names[i]) << "\": { \"versi\": \""
            << jsonStringEscape(e.versi) << "\", \"sha256\": \"" << jsonStringEscape(e.sha256) << "\" }";
    }
    out << (names.empty() ? "" : "\n") << "}\n";
    std::ofstream f(path, std::ios::binary);
    f << out.str();
}

bool collectFilesRecursive(const std::string& baseDir, const std::string& relPrefix,
                            std::vector<Value>& files, std::string& err);

// git:/git@/http(s):// forms, anything ending .git, or a bare host/path
// (github.com/user/repo -- segment before the first "/" must contain a dot).
bool isGitSpec(const std::string& s) {
    if (s.rfind("git:", 0) == 0) return true;
    if (s.rfind("git@", 0) == 0) return true;
    if (s.rfind("https://", 0) == 0) return true;
    if (s.rfind("http://", 0) == 0) return true;
    std::string beforeRef = s.substr(0, s.find('#'));
    if (beforeRef.size() > 4 && beforeRef.compare(beforeRef.size() - 4, 4, ".git") == 0) return true;
    size_t slash = beforeRef.find('/');
    return slash != std::string::npos && beforeRef.find('.') < slash;
}

// git clone --depth 1 (via proc::run, no shell) into a temp dir, copy the
// tree minus .git/ into nusantara_modules/<nama>/. Shared by runGetGit and
// runInstallAll.
bool installPackageFromGit(const std::string& spec, bool global, std::string& outNama,
                            std::string& outPkgDir, std::string& err) {
    std::string rest = spec.rfind("git:", 0) == 0 ? spec.substr(4) : spec;
    std::string ref;
    size_t hash = rest.find('#');
    if (hash != std::string::npos) {
        ref = rest.substr(hash + 1);
        rest = rest.substr(0, hash);
    }
    while (!rest.empty() && rest.back() == '/') rest.pop_back();
    if (rest.empty()) {
        err = i18n::tr("git: butuh url repo, mis. git:github.com/user/repo",
            "git: needs a repo url, e.g. git:github.com/user/repo");
        return false;
    }

    std::string cloneUrl = rest;
    if (cloneUrl.rfind("https://", 0) != 0 && cloneUrl.rfind("http://", 0) != 0 &&
        cloneUrl.rfind("git@", 0) != 0) {
        cloneUrl = "https://" + cloneUrl;
    }

    size_t lastSlash = rest.find_last_of('/');
    std::string nama = lastSlash == std::string::npos ? rest : rest.substr(lastSlash + 1);
    if (nama.size() > 4 && nama.compare(nama.size() - 4, 4, ".git") == 0) {
        nama = nama.substr(0, nama.size() - 4);
    }
    if (nama.empty()) {
        err = i18n::tr("nggak bisa nentuin nama paket dari '", "can't determine package name from '") + rest + "'";
        return false;
    }

    char tmpl[] = "/tmp/nusa-get-XXXXXX";
    char* tmpDir = mkdtemp(tmpl);
    if (!tmpDir) {
        err = i18n::tr("gagal bikin folder sementara", "failed to create temp folder");
        return false;
    }
    std::string cloneDir = std::string(tmpDir) + "/repo";

    std::vector<std::string> args{"clone", "--depth", "1", "--quiet"};
    if (!ref.empty()) {
        args.push_back("--branch");
        args.push_back(ref);
    }
    args.push_back(cloneUrl);
    args.push_back(cloneDir);

    std::cout << "nusa get: git clone " << cloneUrl
              << (ref.empty() ? "" : (i18n::tr(" (cabang/tag: ", " (branch/tag: ") + ref + ")")) << "...\n";
    proc::ExecResult result;
    try {
        result = proc::run("git", args);
    } catch (const std::exception& e) {
        removeTree(tmpDir);
        err = std::string(i18n::tr("gagal jalanin git -- ", "failed to run git -- ")) + e.what();
        return false;
    }
    if (result.exitCode != 0) {
        removeTree(tmpDir);
        err = i18n::tr("git clone gagal -- ", "git clone failed -- ") + result.err;
        return false;
    }

    std::string modulesRoot = "nusantara_modules";
    if (global) {
        modulesRoot = sysplugin::globalModulesDir();
        if (modulesRoot.empty()) {
            removeTree(tmpDir);
            err = i18n::tr("-g gagal -- nggak bisa nemuin lokasi binary nusa (/proc/self/exe)",
                "-g failed -- can't find the nusa binary location (/proc/self/exe)");
            return false;
        }
    }
    if (!ensureDir(modulesRoot)) {
        removeTree(tmpDir);
        err = i18n::tr("gagal bikin folder ", "failed to create folder ") + modulesRoot;
        return false;
    }
    std::string pkgDir = modulesRoot + "/" + nama;
    bool ok = copyTreeSkippingGit(cloneDir, pkgDir);
    removeTree(tmpDir);
    if (!ok) {
        err = i18n::tr("gagal nyalin file hasil clone ke ", "failed to copy cloned files to ") + pkgDir;
        return false;
    }
    outNama = nama;
    outPkgDir = pkgDir;
    return true;
}

int runGetGit(const std::string& spec, bool global) {
    std::string nama, pkgDir, err;
    if (!installPackageFromGit(spec, global, nama, pkgDir, err)) {
        std::cerr << "nusa get: " << err << "\n";
        return 1;
    }
    if (!global) {
        auto files = std::make_shared<std::vector<Value>>();
        std::string collectErr;
        if (collectFilesRecursive(pkgDir, "", *files, collectErr)) {
            Value filesVal = Value::fromArray(files);
            auto deps = loadManifestDeps("nusa.json");
            deps[nama] = spec;
            saveManifestDeps("nusa.json", deps);
            auto lock = loadLockFile("nusa-lock.json");
            lock[nama] = {spec, hashPackageFiles(filesVal)};
            saveLockFile("nusa-lock.json", lock);
        }
    }
    std::cout << "nusa get: " << nama << i18n::tr(" (git) ke-install di ", " (git) installed to ") << pkgDir << "/"
              << (global ? " (global)" : "") << "\n";
    return 0;
}

int runGet(int argc, char** argv) {
    std::vector<std::string> positional;
    bool global = false;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-g" || a == "--global") {
            global = true;
        } else {
            positional.push_back(a);
        }
    }
    if (positional.empty()) {
        std::cerr << i18n::tr(
            "Usage: nusa get <host/path>[#ref] [-g]\n"
            "  mis. nusa get github.com/user/repo\n"
            "  -g   install ke folder global (sebelah binary nusa), bukan ./nusantara_modules\n",
            "Usage: nusa get <host/path>[#ref] [-g]\n"
            "  e.g. nusa get github.com/user/repo\n"
            "  -g   install to the global folder (next to the nusa binary), not ./nusantara_modules\n");
        return 1;
    }
    std::string nama = positional[0];
    if (isGitSpec(nama)) {
        return runGetGit(nama, global);
    }
    std::cerr << i18n::tr("nusa get: '", "nusa get: '") << nama
               << i18n::tr("' bukan path git -- pake host/path (mis. github.com/user/repo) atau URL git lengkap\n",
                           "' isn't a git path -- use host/path (e.g. github.com/user/repo) or a full git URL\n");
    return 1;
}
// `nusa install`, no args: reproduces nusa.json's deps (go mod download-style).
int runInstallAll() {
    auto deps = loadManifestDeps("nusa.json");
    if (deps.empty()) {
        std::cout << i18n::tr("nusa install: nggak ada nusa.json (atau kosong) -- nggak ada yang diinstal\n",
                               "nusa install: no nusa.json (or it's empty) -- nothing to install\n");
        return 0;
    }
    auto lock = loadLockFile("nusa-lock.json");
    bool anyFailed = false;
    for (const auto& [nama, versi] : deps) {
        std::string pkgDir = "nusantara_modules/" + nama;
        struct stat st {};
        auto lockIt = lock.find(nama);
        if (stat(pkgDir.c_str(), &st) == 0 && lockIt != lock.end() && lockIt->second.versi == versi) {
            std::cout << "nusa install: " << nama << "@" << versi
                       << i18n::tr(" udah ada, lewatin\n", " already present, skipping\n");
            continue;
        }
        if (isGitSpec(versi)) {
            std::string gotNama, gotPkgDir, gitErr;
            if (!installPackageFromGit(versi, false, gotNama, gotPkgDir, gitErr)) {
                std::cerr << "nusa install: " << nama << ": " << gitErr << "\n";
                anyFailed = true;
                continue;
            }
            auto files = std::make_shared<std::vector<Value>>();
            std::string collectErr;
            if (collectFilesRecursive(gotPkgDir, "", *files, collectErr)) {
                lock[nama] = {versi, hashPackageFiles(Value::fromArray(files))};
            }
            std::cout << "nusa install: " << nama << " (git) " << i18n::tr("ke-install\n", "installed\n");
            continue;
        }
        std::cerr << "nusa install: " << nama << ": " << i18n::tr("bukan git spec ('", "not a git spec ('")
                   << versi << i18n::tr("') -- edit nusa.json, isi dependency ini dengan path/URL git\n",
                                         "') -- edit nusa.json and give this dependency a git path/URL\n");
        anyFailed = true;
    }
    saveLockFile("nusa-lock.json", lock);
    return anyFailed ? 1 : 0;
}

int runExec(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: nusa x <nama_paket> [argumen...]\n"
                  << "       nusa exec <nama_paket> [argumen...]\n";
        return 1;
    }
    std::string pkg = argv[2];

    std::string scriptPath = "nusantara_modules/" + pkg + "/index.ns";
    struct stat st {};
    if (stat(scriptPath.c_str(), &st) != 0) {
        scriptPath = "nusantara_modules/" + pkg + "/main.ns";
    }

    if (stat(scriptPath.c_str(), &st) != 0) {
        std::cout << "[nusa x] Mengunduh paket '" << pkg << "'...\n";
        char* getArgv[] = { (char*)"nusa", (char*)"get", (char*)pkg.c_str() };
        int getRc = runGet(3, getArgv);
        if (getRc != 0) {
            std::cerr << "nusa x: error: gagal mengunduh paket '" << pkg << "'\n";
            return getRc;
        }
        scriptPath = "nusantara_modules/" + pkg + "/index.ns";
        if (stat(scriptPath.c_str(), &st) != 0) {
            scriptPath = "nusantara_modules/" + pkg + "/main.ns";
        }
    }

    return runFile(scriptPath);
}

bool collectFilesRecursive(const std::string& baseDir, const std::string& relPrefix,
                            std::vector<Value>& files, std::string& err) {
    DIR* d = opendir(baseDir.c_str());
    if (!d) {
        err = "gagal buka folder '" + baseDir + "'";
        return false;
    }
    struct dirent* entry;
    bool ok = true;
    while (ok && (entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        std::string fullPath = baseDir + "/" + name;
        std::string relPath = relPrefix.empty() ? name : relPrefix + "/" + name;
        struct stat st{};
        if (lstat(fullPath.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            ok = collectFilesRecursive(fullPath, relPath, files, err);
        } else if (S_ISREG(st.st_mode)) {
            std::ifstream in(fullPath, std::ios::binary);
            std::ostringstream buf;
            buf << in.rdbuf();
            Value fileEntry = Value::newMap();
            (*fileEntry.map())["nama"] = Value::fromString(relPath);
            (*fileEntry.map())["isi"] = Value::fromString(buf.str());
            files.push_back(fileEntry);
        }
    }
    closedir(d);
    return ok;
}

void printUsage() {
    if (i18n::isEn()) {
        std::cerr <<
            "nusa " NUSA_VERSION " -- Nusantara (.ns) toolchain\n"
            "\n"
            "Usage: nusa [options] [file.ns]\n"
            "       nusa <command> [args]\n"
            "\n"
            "Options:\n"
            "  (no arguments)                 start an interactive REPL\n"
            "  <file.ns>                      run a file (same as 'nusa run <file.ns>')\n"
            "  <file.js>                      run a real JavaScript file via the embedded QuickJS engine\n"
            "  -e, --eval=<code>              eval a code snippet directly, no file\n"
            "  -c, --check <file.ns>          lex/parse/typecheck only, no execution\n"
            "  -v, --version                  print nusa's version\n"
            "  -h, --help                     show this\n"
            "\n"
            "Commands:\n"
            "  run <file.ns>                  run a file once\n"
            "  watch <file.ns>                run it, then auto re-run on every save\n"
            "  repl                           same as nusa with no arguments\n"
            "  get <host/path>[#ref] [-g]     install a package via git clone (e.g. github.com/user/repo)\n"
            "  install                        install every dep listed in ./nusa.json (like `go mod download`)\n"
            "  install <host/path>[#ref] [-g] same as `get` (also updates nusa.json/nusa-lock.json)\n"
            "  set lang <ind|en>              switch CLI/runtime error message language\n"
            "\n"
            "  -g above: install/update to the global folder (next to the nusa binary),\n"
            "  not ./nusantara_modules cwd -- see the README's 'Package manager' section.\n"
            "  Language can also be set for one run via env var NUSA_LANG=ind|en.\n"
            "\n"
            "Examples:\n"
            "  nusa main.ns\n"
            "  nusa -e 'cetak(1 + 1)'\n"
            "  nusa -c modul.ns\n"
            "  nusa watch main.ns\n"
            "  nusa get github.com/user/repo\n"
            "  nusa app.js\n";
        return;
    }
    std::cerr <<
        "nusa " NUSA_VERSION " -- Nusantara (.ns) toolchain\n"
        "\n"
        "Usage: nusa [options] [file.ns]\n"
        "       nusa <command> [args]\n"
        "\n"
        "Options:\n"
        "  (tanpa argumen)                mulai REPL interaktif\n"
        "  <file.ns>                      jalanin file (sama kayak 'nusa run <file.ns>')\n"
        "  <file.js>                      jalanin file JavaScript asli lewat engine QuickJS bawaan\n"
        "  -e, --eval=<kode>               eval satu potong kode langsung, tanpa file\n"
        "  -c, --check <file.ns>          cek lexer/parser/tipe doang, nggak dieksekusi\n"
        "  -v, --version                  cetak versi nusa\n"
        "  -h, --help                     tampilin ini\n"
        "\n"
        "Commands:\n"
        "  run <file.ns>                  jalanin file sekali\n"
        "  watch <file.ns>                jalanin, terus auto re-run tiap file disimpan\n"
        "  build [file.ns]                rakit bundel produksi next-ns (WASM/HTML/JS obfuscated)\n"
        "  dev [file.ns]                  jalankan server dev next-ns dengan live-reload\n"
        "  deploy [file.ns]               rakit & publikasikan server produksi next-ns\n"
        "  repl                           sama kayak nusa tanpa argumen\n"
        "  get <host/path>[#ref] [-g]     install paket lewat git clone (mis. github.com/user/repo)\n"
        "  install                        install semua dep di ./nusa.json (kayak `go mod download`)\n"
        "  install <host/path>[#ref] [-g] sama kayak `get` (ikut update nusa.json/nusa-lock.json)\n"
        "  set lang <ind|en>              ganti bahasa pesan CLI/error runtime\n"
        "\n"
        "  -g di atas: install/update ke folder global (sebelah binary nusa), bukan\n"
        "  ./nusantara_modules cwd -- lihat README bagian 'Package manager'.\n"
        "  Bahasa juga bisa di-set buat satu kali run lewat env var NUSA_LANG=ind|en.\n"
        "\n"
        "Contoh:\n"
        "  nusa main.ns\n"
        "  nusa -e 'cetak(1 + 1)'\n"
        "  nusa -c modul.ns\n"
        "  nusa watch main.ns\n"
        "  nusa get github.com/user/repo\n";
}

std::string langConfigPath() {
    std::string exe = sysplugin::selfExePath();
    if (exe.empty()) return "";
    return sysplugin::dirNameOf(exe) + "/.nusa_lang";
}

void loadLangFromFile() {
    std::string path = langConfigPath();
    if (path.empty()) return;
    std::ifstream in(path);
    if (!in) return;
    std::string val;
    in >> val;
    if (val == "en") i18n::current() = i18n::Lang::EN;
    else if (val == "ind") i18n::current() = i18n::Lang::ID;
}

void loadLangFromEnv() {
    const char* env = std::getenv("NUSA_LANG");
    if (!env) return;
    std::string val = env;
    for (auto& c : val) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (val == "en" || val == "english") i18n::current() = i18n::Lang::EN;
    else if (val == "ind" || val == "id" || val == "indonesia") i18n::current() = i18n::Lang::ID;
}

int runSetLang(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << i18n::tr("Usage: nusa set lang <ind|en>\n", "Usage: nusa set lang <ind|en>\n");
        return 1;
    }
    std::string val = argv[3];
    for (auto& c : val) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string toWrite;
    if (val == "ind" || val == "id") {
        toWrite = "ind";
    } else if (val == "en" || val == "english") {
        toWrite = "en";
    } else {
        std::cerr << i18n::tr("nusa set lang: bahasa nggak dikenal '", "nusa set lang: unknown language '") << val
                   << i18n::tr("' (pakai 'ind' atau 'en')\n", "' (use 'ind' or 'en')\n");
        return 1;
    }
    std::string path = langConfigPath();
    if (path.empty()) {
        std::cerr << i18n::tr("nusa set lang: nggak bisa nemuin lokasi binary sendiri (/proc/self/exe)\n",
                                "nusa set lang: can't find own binary location (/proc/self/exe)\n");
        return 1;
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        std::cerr << i18n::tr("nusa set lang: gagal nulis ", "nusa set lang: failed to write ") << path << "\n";
        return 1;
    }
    out << toWrite;
    out.close();
    if (toWrite == "en") {
        std::cout << "CLI/runtime error language set to English.\n";
    } else {
        std::cout << "Bahasa CLI/error runtime di-set ke Indonesia.\n";
    }
    return 0;
}

std::string escapeJsString(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '`') out += "\\`";
        else if (c == '$') out += "\\$";
        else out += c;
    }
    return out;
}

// ── Modular .next build ─────────────────────────────────────────────────
// Chunk format: non-executing part pusher. Runner menggabungkan semua part
// secara berurutan -> satu kali _nusa_eval.
static std::string emitChunkScript(const std::string& rawSource) {
    std::ostringstream hexList;
    hexList << "\"";
    char buf[8];
    for (size_t i = 0; i < rawSource.size(); i++) {
        if (i) hexList << ",";
        std::snprintf(buf, sizeof(buf), "0x%02x", static_cast<unsigned char>(rawSource[i]));
        hexList << buf;
    }
    hexList << "\"";
    std::ostringstream out;
    out << "(window.__NUSA_PARTS=window.__NUSA_PARTS||[]).push(" << hexList.str() << ");\n";
    return out.str();
}

static const char* NUSA_RUNNER_JS =
"(async function(){"
"if(typeof NusantaraWasm!=='function')return;"
"try{"
"const w=await NusantaraWasm();"
"const bytes=[];"
"const parts=window.__NUSA_PARTS||[];"
"for(let i=0;i<parts.length;i++){const hs=parts[i].split(',');for(let j=0;j<hs.length;j++)bytes.push(parseInt(hs[j],16));}"
"const src=new TextDecoder('utf-8').decode(new Uint8Array(bytes));"
"const ptr=w.allocateUTF8(src);"
"const out=w._nusa_eval(ptr);"
"const html=w.UTF8ToString(out);"
"w._free(ptr);"
"const app=document.getElementById('app');if(app)app.innerHTML=html;"
"if(window.__NUSA_ANIM)__NUSA_ANIM(app);"
"}catch(e){console.error(e)}"
"if(location.protocol!=='file:'){try{"
"const es=new EventSource('/dev-sse');"
"es.onmessage=function(ev){try{if(JSON.parse(ev.data).op==='reload')location.reload()}catch(_){}}"
";}catch(_){}}"
"})();";

static const char* NUSA_ANIM_JS =
"(function(){"
"function reveal(el,delay){el.style.opacity='0';el.style.transform='translateY(16px)';"
"setTimeout(function(){el.style.transition='opacity .55s ease,transform .55s ease';"
"el.style.opacity='1';el.style.transform='none';},delay);}"
"function stagger(sel,step){var c=document.querySelectorAll(sel+' > *');"
"for(var i=0;i<c.length;i++)reveal(c[i],i*step);}"
"window.__NUSA_ANIM=function(app){"
"if(!app)return;"
"stagger('[data-fade]',90);"
"stagger('[data-stagger]',110);"
"var th=app.querySelector('[data-thread]');"
"if(th){var kids=th.children;var d=350;"
"for(var k=0;k<kids.length;k++){(function(el,dd){el.style.opacity='0';"
"setTimeout(function(){el.style.transition='opacity .4s ease,transform .4s ease';"
"el.style.opacity='1';el.style.transform='none';},dd);})(kids[k],d);d+=520;}}"
"if('IntersectionObserver' in window){"
"var io=new IntersectionObserver(function(es){es.forEach(function(en){"
"if(en.isIntersecting){en.target.classList.add('scroll-animated');io.unobserve(en.target);}});},{threshold:.12});"
"document.querySelectorAll('.scroll-animate').forEach(function(x){io.observe(x);});}"
"else{document.querySelectorAll('.scroll-animate').forEach(function(x){x.classList.add('scroll-animated');});}"
"};})();";

static std::string nextPageHtml(const std::string& title,
                                const std::string& description,
                                const std::vector<std::string>& chunks,
                                bool devReload) {
    std::ostringstream html;
    html << "<!DOCTYPE html><html lang=\"id\"><head><meta charset=\"UTF-8\">"
         << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
         << "<title>" << title << "</title>"
         << "<meta name=\"description\" content=\"" << description << "\">"
         << "<link rel=\"preconnect\" href=\"https://fonts.googleapis.com\">"
         << "<link rel=\"preconnect\" href=\"https://fonts.gstatic.com\" crossorigin>"
         << "<link href=\"https://fonts.googleapis.com/css2?family=Bricolage+Grotesque:opsz,wght@12..96,400..800&family=Hanken+Grotesk:wght@300..800&family=Space+Mono:wght@400;700&display=swap\" rel=\"stylesheet\">"
         << "<script src=\"https://cdn.tailwindcss.com\"></script>"
         << "<script>tailwind.config={theme:{extend:{colors:{kertas:'#070B14','kertas-deep':'#0E1626',panel:'#14203A',tinta:'#EDF1F8',chrome:'#C7D0DC',volt:'#2F6BFF',stabilo:'#2F6BFF',lunas:'#2F6BFF',pensil:'#8A98AE'},fontFamily:{display:['var(--font-bricolage)','system-ui','sans-serif'],body:['var(--font-hanken)','system-ui','sans-serif'],mono:['var(--font-space-mono)','ui-monospace','monospace']}}}}</script>"
         << "<style>"
         << ":root{--font-bricolage:'Bricolage Grotesque';--font-hanken:'Hanken Grotesk';--font-space-mono:'Space Mono';"
         << "--kertas:#070B14;--kertas-deep:#0E1626;--panel:#14203A;--tinta:#EDF1F8;--tinta-soft:#C7D0DC;--chrome:#C7D0DC;"
         << "--stabilo:#2F6BFF;--volt:#2F6BFF;--volt-bright:#5B8DFF;--lunas:#2F6BFF;--pensil:#8A98AE;--garis:rgba(150,172,214,0.16);}"
         << "*{box-sizing:border-box;margin:0;padding:0}"
         << "html{scroll-behavior:smooth}"
         << "body{background:var(--kertas);color:var(--tinta);font-family:var(--font-hanken),system-ui,sans-serif;overflow-x:hidden;-webkit-font-smoothing:antialiased}"
         << "::selection{background:var(--volt);color:#fff}"
         << "#app{min-height:100vh}"
         << ".mark-stabilo{background:linear-gradient(180deg,transparent 56%,var(--volt) 56%,var(--volt) 96%,transparent 96%);padding:0 0.08em}"
         << ".bubble{position:relative;max-width:30rem;padding:0.7rem 0.95rem;border-radius:18px;font-size:0.98rem;line-height:1.5;word-wrap:break-word}"
         << ".bubble-in{background:var(--panel);color:var(--tinta);border:1px solid var(--garis);border-bottom-left-radius:5px}"
         << ".bubble-out{background:var(--volt);color:#fff;border-bottom-right-radius:5px;margin-left:auto}"
         << ".bubble-meta{font-family:var(--font-space-mono),monospace;font-size:0.62rem;letter-spacing:0.04em;color:var(--pensil);margin-top:0.3rem}"
         << ".bubble-out .bubble-meta{color:rgba(255,255,255,0.75)}"
         << "@keyframes bubbleIn{from{opacity:0;transform:translateY(10px) scale(0.97)}to{opacity:1;transform:translateY(0) scale(1)}}"
         << ".bubble-enter{animation:bubbleIn 0.4s cubic-bezier(0.34,1.4,0.64,1) both}"
         << ".cyber-card{background:var(--kertas-deep);border:1px solid var(--garis);border-radius:14px;position:relative;transition:transform 0.25s ease,box-shadow 0.25s ease,border-color 0.25s ease;text-decoration:none}"
         << ".cyber-card:hover{border-color:var(--volt);transform:translateY(-3px);box-shadow:0 14px 34px rgba(0,0,0,0.45),0 0 0 1px rgba(47,107,255,0.25)}"
         << ".btn-primary{position:relative;display:inline-flex;align-items:center;gap:0.5rem;padding:0.8rem 1.6rem;background:var(--volt);border:1px solid var(--volt);color:#fff;font-family:var(--font-space-mono),monospace;font-size:0.8rem;font-weight:700;letter-spacing:0.06em;text-transform:uppercase;cursor:pointer;border-radius:999px;text-decoration:none;transition:all 0.18s ease;box-shadow:0 8px 22px rgba(47,107,255,0.35)}"
         << ".btn-primary:hover{background:var(--volt-bright);border-color:var(--volt-bright);transform:translateY(-2px)}"
         << ".btn-outline{position:relative;display:inline-flex;align-items:center;gap:0.5rem;padding:0.8rem 1.6rem;background:transparent;border:1.5px solid var(--chrome);color:var(--tinta);font-family:var(--font-space-mono),monospace;font-size:0.8rem;font-weight:700;letter-spacing:0.06em;text-transform:uppercase;cursor:pointer;border-radius:999px;text-decoration:none;transition:all 0.18s ease}"
         << ".btn-outline:hover{background:var(--volt);border-color:var(--volt);color:#fff}"
         << ".section-heading{font-family:var(--font-bricolage),system-ui,sans-serif;font-size:clamp(2rem,5vw,3.2rem);font-weight:800;color:var(--tinta);letter-spacing:-0.02em;line-height:1.02}"
         << ".eyebrow{font-family:var(--font-space-mono),monospace;font-size:0.7rem;letter-spacing:0.18em;text-transform:uppercase;color:var(--lunas)}"
         << ".scroll-animate{opacity:0;transform:translateY(18px);transition:opacity .7s ease,transform .7s ease}.scroll-animate.scroll-animated{opacity:1;transform:none}@keyframes nusaFadeUp{from{opacity:0;transform:translateY(16px)}to{opacity:1;transform:none}}.nusa-hidden{opacity:0}[data-orbs]{position:relative}[data-orbs]::before{content:'';position:fixed;inset:0;z-index:-1;pointer-events:none;background:radial-gradient(640px 520px at 12% 8%,rgba(47,107,255,.20),transparent 62%),radial-gradient(720px 540px at 88% 92%,rgba(91,141,255,.16),transparent 62%)}@media(max-width:640px){[data-orbs]::before{background:radial-gradient(420px 380px at 20% 6%,rgba(47,107,255,.22),transparent 60%),radial-gradient(460px 380px at 85% 94%,rgba(91,141,255,.18),transparent 60%)}}img,svg{max-width:100%}input,button,textarea{font-family:inherit}"
         << "</style>"
         << "</head><body class=\"scanlines\"><div id=\"app\"></div>"
         << "<script src=\"/.next/static/nusantara.js\"></script>";
    for (const auto& c : chunks) {
        html << "<script src=\"" << c << "\"></script>";
    }
    html << "<script>" << NUSA_RUNNER_JS << "</script>";
    if (devReload) {
        html << "<script>try{const es=new EventSource('/dev-sse');es.onmessage=function(e){try{if(JSON.parse(e.data).op==='reload')location.reload()}catch(_){{}}}catch(_){}</script>";
    }
    html << "</body></html>";
    return html.str();
}

// Pemetaan rute -> file HTML di .next/server/pages
static std::string routeSlugForPath(const std::string& reqPath) {
    std::string seg = reqPath;
    if (!seg.empty() && seg[0] == '/') seg = seg.substr(1);
    auto slash = seg.find('/');
    if (slash != std::string::npos) seg = seg.substr(0, slash);
    return seg; // "" = home
}

static std::string servePageRoute(const std::string& reqPath) {
    std::string slug = routeSlugForPath(reqPath);
    std::string pageName = slug.empty() ? "index" : slug;
    std::ifstream f("dist/.next/server/pages/" + pageName + ".html", std::ios::binary);
    if (!f) {
        std::ifstream idx("dist/.next/server/pages/index.html", std::ios::binary);
        if (!idx) return "";
        std::ostringstream b;
        b << idx.rdbuf();
        return b.str();
    }
    std::ostringstream b;
    b << f.rdbuf();
    return b.str();
}

int runBuild(int argc, char** argv) {
    std::string entryPath = "main.ns";
    if (argc >= 3) {
        std::string arg2 = argv[2];
        if (arg2 != "build" && arg2 != "dev" && arg2 != "deploy") {
            entryPath = arg2;
        }
    }

    auto ensureDir = [](const std::string& dir) -> bool {
        // buat rekursif ringkas
        std::string cur;
        std::istringstream ss(dir);
        std::string seg;
        while (std::getline(ss, seg, '/')) {
            if (seg.empty()) { cur += "/"; continue; }
            if (!cur.empty() && cur.back() != '/') cur += "/";
            cur += seg;
            mkdir(cur.c_str(), 0755);
        }
        struct stat st2 {};
        return stat(dir.c_str(), &st2) == 0 && S_ISDIR(st2.st_mode);
    };
    auto copyIfExist = [](const std::string& s, const std::string& d) {
        std::ifstream in(s, std::ios::binary);
        if (!in) return false;
        std::ofstream out(d, std::ios::binary);
        out << in.rdbuf();
        return true;
    };

    std::string source;
    bool isNextNsApp = false;

    struct stat st {};
    if (stat("app", &st) == 0 && S_ISDIR(st.st_mode)) {
        isNextNsApp = true;
        ensureDir("dist/.next/static/chunks");
        ensureDir("dist/.next/server/pages");

        // ── rute: root + app/<slug>/page.ns ──
        std::vector<std::pair<std::string, std::string>> routes;
        routes.push_back({"", "app/page.ns"});
        if (DIR* dp = opendir("app")) {
            while (struct dirent* ent = readdir(dp)) {
                std::string name = ent->d_name;
                if (name == "." || name == ".." || name.empty()) continue;
                std::string pp = "app/" + name + "/page.ns";
                struct stat pst {};
                if (stat(pp.c_str(), &pst) == 0 && S_ISREG(pst.st_mode)) routes.push_back({name, pp});
            }
            closedir(dp);
        }

        // ── SHARED chunk ──
        std::ostringstream sharedBuf;
        sharedBuf << "buat Tautan = fungsi(props) { buat c = props[\"children\"]; jika (c == kosong) { c = \"\"; } hasil ( <a href={props[\"href\"]} kelas={props[\"kelas\"]}>{c}</a> ); };\n"
               << "buat Kepala = fungsi(props) { hasil props[\"children\"]; };\n"
               << "buat Gambar = fungsi(props) { hasil ( <img src={props[\"src\"]} alt={props[\"alt\"]} kelas={props[\"kelas\"]} /> ); };\n"
               << "buat Form = fungsi(props) { hasil ( <form action={props[\"action\"]} kelas={props[\"kelas\"]}>{props[\"children\"]}</form> ); };\n"
               << "buat useState = fungsi(v) { buat s = [v]; s[1] = fungsi(nv) { s[0] = nv; }; hasil s; };\n"
               << "buat useEffect = fungsi(fn, deps) { coba { fn(); } tangkap(e){} };\n\n";
        for (const char* f : {"app/layout.ns", "app/loading.ns", "app/error.ns", "app/not-found.ns"}) {
            std::ifstream lf(f);
            if (lf) sharedBuf << lf.rdbuf() << "\n";
        }
        // helper opsional app/_lib/*.ns
        if (DIR* ldp = opendir("app/_lib")) {
            std::vector<std::string> libs;
            while (struct dirent* le = readdir(ldp)) {
                std::string ln = le->d_name;
                if (ln.size() > 3 && ln.substr(ln.size()-3) == ".ns") libs.push_back(ln);
            }
            closedir(ldp);
            for (const auto& ln : libs) {
                std::ifstream lf("app/_lib/" + ln);
                if (lf) sharedBuf << lf.rdbuf() << "\n";
            }
        }

        static const char* NUSA_GLUE =
            "\ncoba {\n"
            "    buat _content = kosong;\n"
            "    jika (tipe(Page) == \"fungsi\") {\n"
            "        _content = Page();\n"
            "    } lain jika (tipe(NotFound) == \"fungsi\") {\n"
            "        _content = NotFound();\n"
            "    }\n"
            "    jika (tipe(Layout) == \"fungsi\") {\n"
            "        buat _props = peta_baru();\n"
            "        _props[\"children\"] = [_content];\n"
            "        cetak(Layout(_props));\n"
            "    } lain {\n"
            "        cetak(_content);\n"
            "    }\n"
            "} tangkap (_err) {\n"
            "    jika (tipe(Error) == \"fungsi\") {\n"
            "        buat _errProps = peta_baru();\n"
            "        _errProps[\"error\"] = _err;\n"
            "        cetak(Error(_errProps));\n"
            "    } lain {\n"
            "        cetak(_err);\n"
            "    }\n"
            "}\n";

        // ── route chunks ──
        std::map<std::string, std::string> routeSources;
        for (const auto& route : routes) {
            const std::string& slug = route.first;
            std::ostringstream pb;
            std::ifstream pf(route.second);
            if (pf) pb << pf.rdbuf() << "\n";
            else if (slug.empty()) {
                std::ifstream ix("app/index.ns");
                if (ix) pb << ix.rdbuf() << "\n";
            }
            pb << NUSA_GLUE;
            routeSources[slug.empty() ? "index" : slug] = pb.str();
        }

        // ── tulis .next ──
        copyIfExist("nusantara.js",   "dist/.next/static/nusantara.js");
        copyIfExist("nusantara.wasm", "dist/.next/static/nusantara.wasm");
        {
            std::ofstream sh("dist/.next/static/chunks/_shared.js");
            sh << emitChunkScript(sharedBuf.str());
        }
        for (const auto& [slug, code] : routeSources) {
            std::ofstream pc("dist/.next/static/chunks/page-" + slug + ".js");
            pc << emitChunkScript(code);
        }
        for (const auto& [slug, _code] : routeSources) {
            std::vector<std::string> chunks = { "/.next/static/chunks/_shared.js",
                                                "/.next/static/chunks/page-" + slug + ".js" };
            std::ofstream ph("dist/.next/server/pages/" + slug + ".html");
            ph << nextPageHtml("next-ns App",
                               "Aplikasi Next-NS bertenaga WebAssembly & TailwindCSS",
                               chunks, true);
        }
        // kompatibilitas: dist/index.html = home
        {
            std::vector<std::string> hc = { "/.next/static/chunks/_shared.js",
                                            "/.next/static/chunks/page-index.js" };
            std::ofstream io("dist/index.html");
            io << nextPageHtml("next-ns App",
                               "Aplikasi Next-NS bertenaga WebAssembly & TailwindCSS",
                               hc, false);
        }
        std::cout << "next-ns build: app/ -> dist/.next (" << routeSources.size()
                  << " rute, modular Next.js-style)\n";
        return 0;
    }

    // ── mode single-file biasa ──
    std::ifstream file(entryPath);
    if (!file) {
        std::cerr << "nusa build: error: " << i18n::tr("gagal buka file '", "could not open file '") << entryPath << "'\n";
        return 1;
    }
    std::ostringstream buf;
    buf << file.rdbuf();
    source = buf.str();

    ensureDir("dist");
    copyIfExist("nusantara.js",   "dist/nusantara.js");
    copyIfExist("nusantara.wasm", "dist/nusantara.wasm");
    {
        std::ofstream htmlOut("dist/index.html");
        std::vector<std::string> single = { "/.next/static/chunks/app.js" };
        htmlOut << nextPageHtml("Nusantara App", "", single, false);
    }
    {
        std::ofstream js("dist/.next/static/chunks/app.js");
        js << emitChunkScript(source);
    }
    return 0;
}
int runDeploy(int argc, char** argv) {
    std::cout << "=== next-ns Production Deploy ===\n";
    int rc = runBuild(argc, argv);
    if (rc != 0) {
        std::cerr << "nusa deploy: error: gagal merakit bundel produksi\n";
        return rc;
    }

    int port = 3000;
    const char* envPort = std::getenv("PORT");
    if (envPort) port = std::atoi(envPort);

    std::cout << "next-ns deploy: mempublikasikan server produksi di http://localhost:" << port << "\n";
    std::cout.flush();

    auto serveHandler = [](const net::HttpRequestIn& req) -> net::HttpResponseOut {
        net::HttpResponseOut resp;
        std::string fileRel;
        if (req.path.rfind("/.next/", 0) == 0) fileRel = req.path;
        else if (req.path == "/") fileRel = "/index.html";
        else fileRel = req.path;
        std::string filePath = "dist" + fileRel;

        std::ifstream file(filePath, std::ios::binary);
        if (!file) {
            std::string shell = servePageRoute(req.path);
            if (!shell.empty()) {
                resp.status = 200;
                resp.contentType = "text/html";
                resp.body = shell;
                return resp;
            }
            resp.status = 404;
            resp.contentType = "text/plain";
            resp.body = "404 Not Found";
            return resp;
        }
        std::ostringstream buf;
        buf << file.rdbuf();
        resp.status = 200;
        resp.body = buf.str();

        if (filePath.find(".html") != std::string::npos) resp.contentType = "text/html";
        else if (filePath.find(".js") != std::string::npos) resp.contentType = "application/javascript";
        else if (filePath.find(".wasm") != std::string::npos) resp.contentType = "application/wasm";
        else if (filePath.find(".css") != std::string::npos) resp.contentType = "text/css";
        else resp.contentType = "text/plain";

        return resp;
    };

    try {
        net::httpServe(port, serveHandler);
    } catch (const std::exception& e) {
        std::cerr << "nusa deploy error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

int runDev(int argc, char** argv) {
    std::string entryPath = "main.ns";
    if (argc >= 3) {
        std::string arg2 = argv[2];
        if (arg2 != "dev") entryPath = arg2;
    }
    int initialPort = 3000;
    const char* envPort = std::getenv("PORT");
    if (envPort) initialPort = std::atoi(envPort);

    int rc = runBuild(argc, argv);
    if (rc != 0) return rc;

    std::thread([argc, argv, entryPath]() {
        long long lastMtime = fileMtime(entryPath);
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            long long now = fileMtime(entryPath);
            if (now != -1 && now != lastMtime) {
                lastMtime = now;
                std::cout << "\n=== [nusa dev] File " << entryPath << " berubah, merakit ulang... ===\n";
                runBuild(argc, argv);
                std::cout.flush();
            }
        }
    }).detach();

    auto serveHandler = [](const net::HttpRequestIn& req) -> net::HttpResponseOut {
        net::HttpResponseOut resp;
        if (req.path == "/dev-sse") {
            resp.status = 200;
            resp.contentType = "text/event-stream";
            resp.body = "data: {\"op\":\"connected\"}\n\n";
            return resp;
        }

        std::string fileRel;
        if (req.path.rfind("/.next/", 0) == 0) fileRel = req.path;
        else if (req.path == "/") fileRel = "/index.html";
        else fileRel = req.path;
        std::string filePath = "dist" + fileRel;

        bool isStaticAsset = req.path.rfind("/.next/", 0) == 0;
        std::ifstream file(filePath, std::ios::binary);
        if (!file && !isStaticAsset) {
            std::string shell = servePageRoute(req.path);
            if (!shell.empty()) {
                resp.status = 200;
                resp.contentType = "text/html";
                resp.body = shell;
                return resp;
            }
            resp.status = 404;
            resp.contentType = "text/plain";
            resp.body = "404 Not Found";
            return resp;
        }
        std::ostringstream buf;
        buf << file.rdbuf();
        resp.status = 200;
        resp.body = buf.str();

        if (filePath.find(".html") != std::string::npos) resp.contentType = "text/html";
        else if (filePath.find(".js") != std::string::npos) resp.contentType = "application/javascript";
        else if (filePath.find(".wasm") != std::string::npos) resp.contentType = "application/wasm";
        else if (filePath.find(".css") != std::string::npos) resp.contentType = "text/css";
        else resp.contentType = "text/plain";

        return resp;
    };

    int ports[] = {initialPort, 8080, 8081, 3001, 3002, 8888};
    for (int p : ports) {
        try {
            std::cout << i18n::tr("Nusantara -- mode 'dev': server berjalan di http://localhost:", "Nusantara -- 'dev' mode: server running at http://localhost:") << p << "\n";
            std::cout << i18n::tr("Mengawasi '", "Watching '") << entryPath << i18n::tr("' untuk live reload (Ctrl+C untuk keluar)...\n", "' for live reload (Ctrl+C to quit)...\n");
            std::cout.flush();
            net::httpServe(p, serveHandler);
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "Port " << p << " tidak tersedia, mencoba port berikutnya...\n";
        }
    }
    std::cerr << "nusa dev error: Tidak dapat menemukan port yang terbuka.\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(__GLIBC__)
    // Caps glibc's per-thread malloc arenas -- unbounded goroutines would
    // otherwise blow up VmSize (64 MB virtual/arena).
    mallopt(M_ARENA_MAX, 2);
#endif

    loadLangFromFile();
    loadLangFromEnv();

    // A send() to a peer that already closed the connection raises SIGPIPE,
    // which by default kills the whole process -- ignore it, every call
    // site already checks for the resulting EPIPE/-1.
    std::signal(SIGPIPE, SIG_IGN);

    // Main thread registers with GC's per-thread safepoint registry too.
    Interpreter::registerCurrentThread();
    GIL::instance().lock();

    if (argc < 2) {
        return runRepl();
    }
    std::string command = argv[1];
    if (command == "-v" || command == "--version") {
        std::cout << "nusa " NUSA_VERSION "\n";
        return 0;
    }
    if (command == "-h" || command == "--help") {
        printUsage();
        return 0;
    }
    if (command == "-e" || command == "--eval") {
        if (argc < 3) {
            std::cerr << i18n::tr("nusa: -e/--eval butuh argumen kode\n", "nusa: -e/--eval needs a code argument\n");
            return 1;
        }
        return runEval(argv[2]);
    }
    if (command.rfind("--eval=", 0) == 0) {
        return runEval(command.substr(7));
    }
    if (command == "-c" || command == "--check") {
        if (argc < 3) {
            std::cerr << i18n::tr("nusa: -c/--check butuh path file\n", "nusa: -c/--check needs a file path\n");
            return 1;
        }
        return runCheck(argv[2]);
    }
    if (command == "--vm") {
        if (argc < 3) {
            std::cerr << "nusa: --vm butuh path file\n";
            return 1;
        }
        return runVmFile(argv[2]);
    }
    if (command == "repl") {
        return runRepl();
    }
    if (command == "run") {
        if (argc < 3) {
            printUsage();
            return 1;
        }
        return runFile(argv[2]);
    }
    if (command == "watch" || command == "--watch") {
        if (argc < 3) {
            printUsage();
            return 1;
        }
        return runWatch(argv[2]);
    }
    if (command == "build") {
        return runBuild(argc, argv);
    }
    if (command == "dev") {
        return runDev(argc, argv);
    }
    if (command == "deploy") {
        return runDeploy(argc, argv);
    }
    if (command == "x" || command == "exec") {
        return runExec(argc, argv);
    }
    if (command == "get" || command == "install") {
        bool hasPackageArg = false;
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a != "-g" && a != "--global") { hasPackageArg = true; break; }
        }
        if (command == "install" && !hasPackageArg) {
            return runInstallAll();
        }
        return runGet(argc, argv);
    }
    if (command == "set") {
        if (argc < 3 || std::string(argv[2]) != "lang") {
            std::cerr << i18n::tr("Usage: nusa set lang <ind|en>\n", "Usage: nusa set lang <ind|en>\n");
            return 1;
        }
        return runSetLang(argc, argv);
    }
    // Shorthand: `nusa main.ns` runs a file directly, no `run` needed.
    if (argc == 2) {
        return runFile(command);
    }
    std::cerr << i18n::tr("nusantara: perintah nggak dikenal '", "nusantara: unknown command '") << command << "'\n";
    printUsage();
    return 1;
}
