// `nusa go add <module>` -- build a Nusantara module that exposes a Go module.
#include "gobridge.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include "i18n.hpp"

namespace gobridge {

namespace {

bool fileExists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

bool makeDirs(const std::string& path) {
    std::string cur;
    std::stringstream ss(path);
    std::string part;
    if (!path.empty() && path[0] == '/') cur = "/";
    while (std::getline(ss, part, '/')) {
        if (part.empty()) continue;
        cur += part;
        if (!fileExists(cur) && mkdir(cur.c_str(), 0755) != 0) return false;
        cur += "/";
    }
    return true;
}

bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << content;
    return static_cast<bool>(f);
}

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

// Runs `cmd` inside `dir`, output going straight to the terminal.
int runIn(const std::string& dir, const std::string& cmd) {
    std::string full = "cd " + shellQuote(dir) + " && " + cmd;
    return std::system(full.c_str());
}

void usage() {
    std::cout << i18n::tr(
        "pakai:\n"
        "  nusa go add <modul>[@versi] [opsi]   bangun modul Nusantara dari modul Go\n"
        "\n"
        "opsi:\n"
        "  --pkg <sub>      paket lain di dalam modul (mis. --pkg store/sqlstore); boleh diulang\n"
        "  --semua          ikutkan semua sub-paket publik modul (otomatis kalau root modul bukan paket)\n"
        "  --std <paket>    paket standar Go (mis. --std strings --std time); boleh diulang\n"
        "  --blank <paket>  import untuk efek samping saja, mis. driver database; boleh diulang\n"
        "  --nama <nama>    nama modul Nusantara (default: elemen terakhir path modul)\n"
        "  --keluar <dir>   folder tujuan (default: ./nusantara_modules/<nama>)\n"
        "\n"
        "Go cuma dibutuhkan untuk MEMBANGUN. Folder hasilnya (plugin.so + index.ns) bisa dibagikan\n"
        "dan dipakai di mesin tanpa Go: impor(\"<nama>\") atau `import <nama>`.\n",
        "usage:\n"
        "  nusa go add <module>[@version] [options]   build a Nusantara module from a Go module\n"
        "\n"
        "options:\n"
        "  --pkg <sub>      another package inside the module (e.g. --pkg store/sqlstore); repeatable\n"
        "  --all            include every public sub-package (automatic when the module root is not a package)\n"
        "  --std <pkg>      a Go standard library package (e.g. --std strings --std time); repeatable\n"
        "  --blank <pkg>    import for side effects only, e.g. a database driver; repeatable\n"
        "  --nama <name>    Nusantara module name (default: last element of the module path)\n"
        "  --keluar <dir>   output folder (default: ./nusantara_modules/<name>)\n"
        "\n"
        "Go is only needed to BUILD. The output folder (plugin.so + index.ns) can be shared and used\n"
        "on machines without Go: impor(\"<name>\") or `import <name>`.\n");
}

std::string lastElement(const std::string& path) {
    std::string p = path;
    size_t at = p.find('@');
    if (at != std::string::npos) p = p.substr(0, at);
    while (!p.empty() && p.back() == '/') p.pop_back();
    size_t slash = p.find_last_of('/');
    std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    for (char& c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) c = '_';
    }
    // Go module paths may end in a /vN major-version element.
    if (name.size() >= 2 && name[0] == 'v' && std::isdigit(static_cast<unsigned char>(name[1])) && slash != std::string::npos) {
        std::string rest = p.substr(0, slash);
        size_t s2 = rest.find_last_of('/');
        name = s2 == std::string::npos ? rest : rest.substr(s2 + 1);
    }
    return name;
}

int add(int argc, char** argv) {
    std::string moduleArg;
    std::vector<std::string> subPkgs, stdPkgs, blanks;
    bool allPkgs = false;
    std::string name, outDir;
    for (int i = 0; i < argc; i++) {
        std::string a = argv[i];
        auto value = [&](std::string& dst) {
            if (i + 1 >= argc) {
                std::cerr << "nusantara: error: " << a << " butuh nilai\n";
                return false;
            }
            dst = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--pkg") { if (!value(v)) return 1; subPkgs.push_back(v); }
        else if (a == "--std") { if (!value(v)) return 1; stdPkgs.push_back(v); }
        else if (a == "--blank") { if (!value(v)) return 1; blanks.push_back(v); }
        else if (a == "--semua" || a == "--all") allPkgs = true;
        else if (a == "--nama") { if (!value(name)) return 1; }
        else if (a == "--keluar") { if (!value(outDir)) return 1; }
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "nusantara: error: opsi nggak dikenal: " << a << "\n";
            return 1;
        } else if (moduleArg.empty()) moduleArg = a;
        else { std::cerr << "nusantara: error: argumen berlebih: " << a << "\n"; return 1; }
    }
    if (moduleArg.empty() && stdPkgs.empty()) {
        usage();
        return 1;
    }

    if (std::system("go version >/dev/null 2>&1") != 0) {
        std::cerr << i18n::tr(
            "nusantara: error: toolchain Go nggak ketemu di PATH.\n"
            "  Go dibutuhkan HANYA buat membangun modul ini (sekali, di mesin pembuat).\n"
            "  Hasil build-nya (folder dengan plugin.so) bisa dibagikan dan dijalankan tanpa Go.\n",
            "nusantara: error: no Go toolchain found on PATH.\n"
            "  Go is needed ONLY to build this module (once, on the builder's machine).\n"
            "  The built folder (containing plugin.so) can be shared and run without Go.\n");
        return 1;
    }

    std::string modulePath = moduleArg, version = "latest";
    size_t at = moduleArg.find('@');
    if (at != std::string::npos) {
        modulePath = moduleArg.substr(0, at);
        version = moduleArg.substr(at + 1);
    }
    if (name.empty()) name = !modulePath.empty() ? lastElement(modulePath) : stdPkgs.front();
    if (outDir.empty()) outDir = "nusantara_modules/" + name;

    // Scratch module where the bridge is compiled.
    const char* cacheEnv = std::getenv("XDG_CACHE_HOME");
    const char* homeEnv = std::getenv("HOME");
    std::string cacheRoot = cacheEnv ? std::string(cacheEnv) : (homeEnv ? std::string(homeEnv) + "/.cache" : "/tmp");
    std::string work = cacheRoot + "/nusa-go/" + name;
    if (!makeDirs(work) || !makeDirs(work + "/gen")) {
        std::cerr << "nusantara: error: nggak bisa bikin folder kerja " << work << "\n";
        return 1;
    }
    for (const EmbeddedFile& f : embeddedFiles()) {
        if (!writeFile(work + "/" + f.path, f.content)) {
            std::cerr << "nusantara: error: gagal nulis " << f.path << "\n";
            return 1;
        }
    }
    writeFile(work + "/go.mod", "module nusabridge\n\ngo 1.21\n");
    writeFile(work + "/registry_gen.go",
              "package main\n\nimport \"reflect\"\n\nvar funcs = map[string]reflect.Value{}\nvar typs = map[string]reflect.Type{}\nvar consts = map[string]interface{}{}\nvar vars = map[string]func() reflect.Value{}\n");
    std::remove((work + "/go.sum").c_str());

    // -mod=mod lets every go command record what it needs in go.mod/go.sum itself.
    const std::string env = "GOFLAGS=-mod=mod CGO_ENABLED=1 ";
    auto step = [&](const std::string& what, const std::string& cmd) {
        std::cout << "-- " << what << "\n" << std::flush;
        if (runIn(work, env + cmd) != 0) {
            std::cerr << "nusantara: error: langkah gagal: " << what << "\n";
            return false;
        }
        return true;
    };

    std::vector<std::string> genPkgs;
    if (!modulePath.empty()) {
        if (!step("ambil modul " + modulePath + "@" + version, "go get " + shellQuote(modulePath + "@" + version))) return 1;
        genPkgs.push_back(modulePath);
        for (const std::string& s : subPkgs) genPkgs.push_back(modulePath + "/" + s);
    }
    for (const std::string& s : stdPkgs) genPkgs.push_back(s);
    for (const std::string& b : blanks) {
        if (!step("ambil " + b, "go get " + shellQuote(b))) return 1;
    }

    std::string genCmd = "go run ./gen -module " + shellQuote(modulePath.empty() ? "std" : modulePath);
    if (allPkgs) genCmd += " -all";
    for (const std::string& b : blanks) genCmd += " -blank " + shellQuote(b);
    for (const std::string& p : genPkgs) genCmd += " " + shellQuote(p);
    if (!step("baca API paket Go dan bikin registri", genCmd)) return 1;
    if (!step("rapikan dependensi", "go mod tidy")) return 1;
    if (!step("bangun plugin (go build -buildmode=c-shared)", "go build -buildmode=c-shared -o plugin.so .")) return 1;

    if (!makeDirs(outDir)) {
        std::cerr << "nusantara: error: nggak bisa bikin folder tujuan " << outDir << "\n";
        return 1;
    }
    for (const char* f : {"plugin.so", "index.ns", "manifest.json"}) {
        if (runIn(work, "cp " + shellQuote(f) + " " + shellQuote(outDir.front() == '/' ? outDir : std::string(getcwd(nullptr, 0)) + "/" + outDir) + "/") != 0) {
            std::cerr << "nusantara: error: gagal menyalin " << f << "\n";
            return 1;
        }
    }
    std::cout << i18n::tr("selesai: ", "done: ") << outDir << "\n"
              << i18n::tr("  pakai:  import ", "  use:    import ") << name << "\n";
    return 0;
}

}  // namespace

int runCommand(int argc, char** argv) {
    if (argc < 1 || std::string(argv[0]) == "help" || std::string(argv[0]) == "--help") {
        usage();
        return argc < 1 ? 1 : 0;
    }
    if (std::string(argv[0]) == "add") return add(argc - 1, argv + 1);
    std::cerr << "nusantara: error: perintah 'go " << argv[0] << "' nggak dikenal\n";
    usage();
    return 1;
}

}  // namespace gobridge
