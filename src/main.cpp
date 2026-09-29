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
#include "sysmod.hpp"
#include "pystd.hpp"
#include "json.hpp"
#include "lexer.hpp"
#include "net.hpp"
#include "parser.hpp"
#include "plugin.hpp"
#include "proc.hpp"
#include "sha256.hpp"
#include "gobridge.hpp"
#include "sysplugin.hpp"
#include "typechecker.hpp"
#include "vm.hpp"

#define NUSA_VERSION "0.0.1"

int runUpdate(const std::string& currentVersion);  // src/update.cpp

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
    while (GC::liveGoroutines.load() - GC::daemonGoroutines.load() > 0) {
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

// Escape hatch to the tree-walking interpreter (the reference implementation,
// and the only engine with Environment garbage collection): a `nusa:tree-walker`
// comment in the first lines of a script, or NUSA_NO_VM=1 for everything.
bool wantsTreeWalker(const std::string& source) {
    if (std::getenv("NUSA_NO_VM")) return true;
    size_t pos = 0;
    for (int line = 0; line < 10 && pos < source.size(); line++) {
        size_t eol = source.find('\n', pos);
        std::string text = source.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        if ((text.rfind("//", 0) == 0 || text.rfind("#", 0) == 0) && text.find("nusa:tree-walker") != std::string::npos) return true;
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return false;
}

int runFile(const std::string& path) {
    if (hasExtension(path, ".js")) {
        try {
            return static_cast<int>(sysmod::call("js", "js_jalan", {Value::fromString(path)}).number);
        } catch (const std::exception& e) {
            std::cerr << "nusantara: error: " << e.what() << '\n';
            return 1;
        }
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
        
        // Keyed on the CONTENT of every source file (an mtime has one-second granularity, so a
        // quick edit kept the stale bytecode) and on this build, so a new compiler never reuses
        // bytecode from an older one.
        std::string combinedHash = std::string(__DATE__ " " __TIME__ " " NUSA_VERSION "|");
        for (const auto& d : deps) {
            std::ifstream depFile(d, std::ios::binary);
            if (!depFile) continue;
            std::ostringstream depBuf;
            depBuf << depFile.rdbuf();
            combinedHash += d + ":" + std::to_string(std::hash<std::string>{}(depBuf.str())) + ":" + std::to_string(depBuf.str().size()) + "|";
        }
        // quick hash string
        size_t h = std::hash<std::string>{}(combinedHash);
        std::string finalHash = std::to_string(h);
        std::string cacheDir = dirOf(path) + "/.nusa-cache";
        size_t lastSlash = path.find_last_of('/');
        std::string fname = (lastSlash == std::string::npos) ? path : path.substr(lastSlash + 1);
        std::string cachePath = cacheDir + "/_" + fname + ".bin";
        
        std::unique_ptr<VmProgram> vmProgram = wantsTreeWalker(source) ? nullptr : vmDeserialize(cachePath, finalHash);
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
            if (wantsTreeWalker(source)) throw VmCompileError("dipaksa tree-walker (nusa:tree-walker / NUSA_NO_VM)");
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
            // Fall back to the (much slower) tree-walking interpreter.
            if (std::getenv("NUSA_VM_DEBUG")) std::cerr << "[vm] fallback ke tree-walker: " << e.what() << "\n";
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

void printUsage(std::ostream& out = std::cerr) {
    if (i18n::isEn()) {
        out <<
            "Nusantara is a Python-style programming language (Indonesian or English keywords)\n"
            "that runs on its own bytecode VM.\n"
            "\n"
            "Usage:\n"
            "\n"
            "        nusa [options] [file.ns] [arguments]\n"
            "        nusa <command> [arguments]\n"
            "\n"
            "The commands are:\n"
            "\n"
            "        run       run a program\n"
            "        watch     run a program again every time it is saved\n"
            "        get       add a package (git clone)\n"
            "        install   install the dependencies listed in nusa.json\n"
            "        go        build a Nusantara module from a Go module (nusa go add)\n"
            "        update    update nusa itself to the latest release\n"
            "        set       change a setting (nusa set lang ind|en)\n"
            "        version   print the version\n"
            "\n"
            "Options:\n"
            "\n"
            "        -e code   run a snippet of code\n"
            "        -c file   check syntax and types only, do not run\n"
            "        -v        print the version and exit\n"
            "        -h        print this help and exit\n"
            "\n"
            "With no arguments, nusa starts an interactive prompt. Files ending in .js run on\n"
            "the embedded QuickJS engine.\n"
            "\n"
            "Environment: NUSA_LANG=ind|en (message language), NUSA_NO_VM=1 (tree-walking interpreter).\n";
        return;
    }
    out <<
        "Nusantara adalah bahasa pemrograman bergaya Python (keyword Indonesia atau Inggris)\n"
        "yang berjalan di bytecode VM sendiri.\n"
        "\n"
        "Pemakaian:\n"
        "\n"
        "        nusa [opsi] [file.ns] [argumen]\n"
        "        nusa <perintah> [argumen]\n"
        "\n"
        "Perintah:\n"
        "\n"
        "        run       jalankan program\n"
        "        watch     jalankan ulang program setiap kali disimpan\n"
        "        get       tambah paket (git clone)\n"
        "        install   pasang dependensi yang tercatat di nusa.json\n"
        "        go        bangun modul Nusantara dari modul Go (nusa go add)\n"
        "        update    perbarui nusa ke rilis terbaru\n"
        "        set       ubah pengaturan (nusa set lang ind|en)\n"
        "        version   cetak versi\n"
        "\n"
        "Opsi:\n"
        "\n"
        "        -e kode   jalankan potongan kode\n"
        "        -c file   cek sintaks dan tipe saja, tidak dijalankan\n"
        "        -v        cetak versi lalu keluar\n"
        "        -h        cetak bantuan ini lalu keluar\n"
        "\n"
        "Tanpa argumen, nusa membuka prompt interaktif. File berakhiran .js dijalankan oleh\n"
        "mesin QuickJS bawaan.\n"
        "\n"
        "Lingkungan: NUSA_LANG=ind|en (bahasa pesan), NUSA_NO_VM=1 (interpreter tree-walking).\n";
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
    if (command == "-h" || command == "--help" || command == "help") {
        printUsage(std::cout);
        return 0;
    }
    if (command == "version") {
        std::cout << "nusa " NUSA_VERSION "\n";
        return 0;
    }
    if (command == "update") {
        return runUpdate("nusa " NUSA_VERSION);
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
        pystd::setArgv(std::vector<std::string>(argv + 2, argv + argc));
        return runFile(argv[2]);
    }
    if (command == "watch" || command == "--watch") {
        if (argc < 3) {
            printUsage();
            return 1;
        }
        return runWatch(argv[2]);
    }
    if (command == "go") {
        return gobridge::runCommand(argc - 2, argv + 2);
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
    struct stat scriptStat;
    if (argc == 2 || (stat(command.c_str(), &scriptStat) == 0 && S_ISREG(scriptStat.st_mode))) {
        pystd::setArgv(std::vector<std::string>(argv + 1, argv + argc));
        return runFile(command);
    }
    std::cerr << i18n::tr("nusantara: perintah nggak dikenal '", "nusantara: unknown command '") << command << "'\n";
    printUsage();
    return 1;
}
