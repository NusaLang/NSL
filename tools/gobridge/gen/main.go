// Generator for `nusa go add`: reads the exported API of the wrapped Go packages and writes
//
//	registry_gen.go   what exists (functions, types, constants) -- consumed by bridge.go
//	manifest.json     the same, for humans and tooling
//	index.ns          the Nusantara module that exposes it (runtime helpers + one namespace per package)
//
// Usage (run inside the wrapper module):  gen -module M [-blank P]... <import path>...
package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

type listedPkg struct {
	ImportPath string
	Name       string
	Dir        string
	GoFiles    []string
	CgoFiles   []string
}

type fnInfo struct {
	Name     string
	In       int
	Variadic bool
}

type pkgInfo struct {
	Alias      string // Go import alias inside registry_gen.go
	Namespace  string // name of the Nusantara namespace
	ImportPath string
	Funcs      []fnInfo
	Types      []string
	Consts     []string
	Vars       []string
}

type multiFlag []string

func (m *multiFlag) String() string     { return strings.Join(*m, ",") }
func (m *multiFlag) Set(s string) error { *m = append(*m, s); return nil }

func main() {
	module := flag.String("module", "", "module path (documentation only)")
	var blanks multiFlag
	flag.Var(&blanks, "blank", "package to import for side effects only (e.g. a database driver); repeatable")
	all := flag.Bool("all", false, "include every public sub-package of each given module path")
	flag.Parse()
	if flag.NArg() == 0 {
		fatal("tidak ada paket yang diberikan")
	}
	paths := expandPaths(flag.Args(), *all)
	explicit := map[string]bool{}
	for _, a := range flag.Args() {
		explicit[a] = true
	}

	var pkgs []*pkgInfo
	usedNS := map[string]int{}
	for i, path := range paths {
		if !explicit[path] {
			// expanded sub-package: skip the ones that are not importable libraries
			if !softListable(path) {
				continue
			}
		}
		lp := goList(path)
		info := &pkgInfo{Alias: "pk" + strconv.Itoa(i+1), ImportPath: lp.ImportPath}
		ns := sanitize(lp.Name)
		if n := usedNS[ns]; n > 0 {
			ns = fmt.Sprintf("%s_%d", ns, n+1)
		}
		usedNS[sanitize(lp.Name)]++
		info.Namespace = ns
		fset := token.NewFileSet()
		for _, f := range append(append([]string{}, lp.GoFiles...), lp.CgoFiles...) {
			file, err := parser.ParseFile(fset, filepath.Join(lp.Dir, f), nil, 0)
			if err != nil {
				fatal("parse %s: %v", f, err)
			}
			collect(file, info)
		}
		sort.Slice(info.Funcs, func(a, b int) bool { return info.Funcs[a].Name < info.Funcs[b].Name })
		sort.Strings(info.Types)
		sort.Strings(info.Consts)
		sort.Strings(info.Vars)
		pkgs = append(pkgs, info)
	}

	writeRegistry("registry_gen.go", pkgs, blanks)
	pruneUntilBuilds("registry_gen.go")
	writeManifestAndIndex(*module, pkgs)
	fmt.Printf("registri: %d paket\n", len(pkgs))
}

func fatal(format string, a ...interface{}) {
	fmt.Fprintf(os.Stderr, "gen: "+format+"\n", a...)
	os.Exit(1)
}

var nonIdent = regexp.MustCompile(`[^A-Za-z0-9_]`)

func sanitize(s string) string {
	s = nonIdent.ReplaceAllString(s, "_")
	if s == "" || (s[0] >= '0' && s[0] <= '9') {
		s = "p_" + s
	}
	return s
}

// expandPaths adds a module's public sub-packages: always with -all, and also when the module
// root itself has no package (mongo-driver, golang.org/x/...), where the root alone is unusable.
func expandPaths(args []string, all bool) []string {
	var out []string
	seen := map[string]bool{}
	add := func(p string) {
		if !seen[p] {
			seen[p] = true
			out = append(out, p)
		}
	}
	for _, a := range args {
		rootOK := exec.Command("go", "list", a).Run() == nil
		if rootOK {
			add(a)
		}
		if all || !rootOK {
			raw, err := exec.Command("go", "list", a+"/...").Output()
			if err != nil {
				if !rootOK {
					fatal("paket %s tidak ditemukan (go list %s/... gagal)", a, a)
				}
				continue
			}
			n := 0
			for _, line := range strings.Fields(string(raw)) {
				if publicPackage(line) && n < 60 {
					add(line)
					n++
				}
			}
		}
	}
	return out
}

func softListable(path string) bool {
	out, err := exec.Command("go", "list", "-json", path).Output()
	if err != nil {
		return false
	}
	var lp listedPkg
	if json.Unmarshal(out, &lp) != nil {
		return false
	}
	return lp.Name != "main" && len(lp.GoFiles)+len(lp.CgoFiles) > 0
}

func publicPackage(p string) bool {
	for _, bad := range []string{"/internal", "/cmd/", "/examples", "/example", "/testdata", "/test/", "/vendor/", "/tools", "/bench", "/_"} {
		if strings.Contains(p+"/", bad) {
			return false
		}
	}
	return true
}

func goList(path string) listedPkg {
	out, err := exec.Command("go", "list", "-json", path).Output()
	if err != nil {
		var stderr string
		if ee, ok := err.(*exec.ExitError); ok {
			stderr = string(ee.Stderr)
		}
		fatal("go list %s: %v\n%s", path, err, stderr)
	}
	var lp listedPkg
	if err := json.Unmarshal(out, &lp); err != nil {
		fatal("go list %s: %v", path, err)
	}
	if lp.Name == "main" {
		fatal("%s adalah package main -- tidak bisa dipakai sebagai pustaka", path)
	}
	return lp
}

func collect(f *ast.File, info *pkgInfo) {
	for _, decl := range f.Decls {
		switch d := decl.(type) {
		case *ast.FuncDecl:
			if d.Recv != nil || !ast.IsExported(d.Name.Name) || d.Type.TypeParams != nil {
				continue
			}
			n := 0
			for _, field := range d.Type.Params.List {
				if len(field.Names) == 0 {
					n++
				} else {
					n += len(field.Names)
				}
			}
			variadic := false
			if len(d.Type.Params.List) > 0 {
				_, variadic = d.Type.Params.List[len(d.Type.Params.List)-1].Type.(*ast.Ellipsis)
			}
			info.Funcs = append(info.Funcs, fnInfo{Name: d.Name.Name, In: n, Variadic: variadic})
		case *ast.GenDecl:
			for _, spec := range d.Specs {
				switch s := spec.(type) {
				case *ast.TypeSpec:
					if !ast.IsExported(s.Name.Name) || s.TypeParams != nil {
						continue
					}
					if _, isIface := s.Type.(*ast.InterfaceType); isIface {
						continue
					}
					info.Types = append(info.Types, s.Name.Name)
				case *ast.ValueSpec:
					for _, n := range s.Names {
						if !ast.IsExported(n.Name) {
							continue
						}
						if d.Tok == token.CONST {
							info.Consts = append(info.Consts, n.Name)
						} else if d.Tok == token.VAR {
							info.Vars = append(info.Vars, n.Name)
						}
					}
				}
			}
		}
	}
}

func writeRegistry(path string, pkgs []*pkgInfo, blanks []string) {
	var b bytes.Buffer
	b.WriteString("// Code generated by `nusa go add`. DO NOT EDIT.\n\npackage main\n\nimport (\n\t\"reflect\"\n")
	for _, p := range pkgs {
		if len(p.Funcs)+len(p.Types)+len(p.Consts)+len(p.Vars) == 0 {
			fmt.Fprintf(&b, "\t_ %q\n", p.ImportPath)
		} else {
			fmt.Fprintf(&b, "\t%s %q\n", p.Alias, p.ImportPath)
		}
	}
	for _, bl := range blanks {
		fmt.Fprintf(&b, "\t_ %q\n", bl)
	}
	b.WriteString(")\n\n")
	// One entry per line: pruneUntilBuilds removes a line the compiler rejects.
	b.WriteString("var funcs = map[string]reflect.Value{\n")
	for _, p := range pkgs {
		for _, f := range p.Funcs {
			fmt.Fprintf(&b, "\t%q: reflect.ValueOf(%s.%s),\n", p.Namespace+"."+f.Name, p.Alias, f.Name)
		}
	}
	b.WriteString("}\n\nvar typs = map[string]reflect.Type{\n")
	for _, p := range pkgs {
		for _, t := range p.Types {
			fmt.Fprintf(&b, "\t%q: reflect.TypeOf((*%s.%s)(nil)).Elem(),\n", p.Namespace+"."+t, p.Alias, t)
		}
	}
	b.WriteString("}\n\nvar consts = map[string]interface{}{\n")
	for _, p := range pkgs {
		for _, c := range p.Consts {
			fmt.Fprintf(&b, "\t%q: interface{}(%s.%s),\n", p.Namespace+"."+c, p.Alias, c)
		}
	}
	b.WriteString("}\n\n// Package variables are read through a closure so each access sees the current value.\nvar vars = map[string]func() reflect.Value{\n")
	for _, p := range pkgs {
		for _, v := range p.Vars {
			fmt.Fprintf(&b, "\t%q: func() reflect.Value { return reflect.ValueOf(&%s.%s).Elem() },\n", p.Namespace+"."+v, p.Alias, v)
		}
	}
	b.WriteString("}\n")
	if err := os.WriteFile(path, b.Bytes(), 0o644); err != nil {
		fatal("tulis %s: %v", path, err)
	}
}

var errLine = regexp.MustCompile(`registry_gen\.go:(\d+):`)

// Some exported names cannot be put in a registry (untyped constants that overflow, names of
// generic-only helpers, ...). Compile, drop the lines the compiler rejects, repeat.
func pruneUntilBuilds(path string) {
	for attempt := 0; attempt < 40; attempt++ {
		cmd := exec.Command("go", "build", "-o", os.DevNull, ".")
		out, err := cmd.CombinedOutput()
		if err == nil {
			return
		}
		bad := map[int]bool{}
		for _, m := range errLine.FindAllStringSubmatch(string(out), -1) {
			n, _ := strconv.Atoi(m[1])
			bad[n] = true
		}
		if len(bad) == 0 {
			fatal("kompilasi jembatan gagal:\n%s", out)
		}
		data, _ := os.ReadFile(path)
		lines := strings.Split(string(data), "\n")
		kept := lines[:0:0]
		dropped := 0
		for i, l := range lines {
			if bad[i+1] && strings.HasPrefix(l, "\t\"") {
				dropped++
				continue
			}
			kept = append(kept, l)
		}
		if dropped == 0 {
			fatal("kompilasi jembatan gagal (tidak bisa dipangkas otomatis):\n%s", out)
		}
		fmt.Printf("  melewati %d nama yang tidak bisa dijembatani\n", dropped)
		_ = os.WriteFile(path, []byte(strings.Join(kept, "\n")), 0o644)
	}
	fatal("terlalu banyak percobaan memangkas registri")
}

func writeManifestAndIndex(module string, pkgs []*pkgInfo) {
	manifest := map[string]interface{}{"modul": module}
	pm := map[string]interface{}{}
	for _, p := range pkgs {
		pm[p.Namespace] = map[string]interface{}{"import": p.ImportPath, "fungsi": p.Funcs, "tipe": p.Types, "konstanta": p.Consts, "variabel": p.Vars}
	}
	manifest["paket"] = pm
	b, _ := json.MarshalIndent(manifest, "", "  ")
	_ = os.WriteFile("manifest.json", b, 0o644)

	var s strings.Builder
	s.WriteString("// GENERATED by `nusa go add` -- jangan diedit manual.\n")
	fmt.Fprintf(&s, "// Jembatan ke modul Go: %s\n", module)
	s.WriteString("// plugin.so di folder ini sudah hasil build: MENJALANKAN modul ini TIDAK butuh Go terpasang.\n\n")
	s.WriteString(nsRuntime)
	s.WriteString("\n")
	for _, p := range pkgs {
		fmt.Fprintf(&s, "// ---- paket %s (%s) ----\nbuat %s = peta_baru();\n", p.Namespace, p.ImportPath, p.Namespace)
		for _, f := range p.Funcs {
			fixed := f.In
			if f.Variadic {
				fixed--
			}
			params := make([]string, 0, f.In)
			for i := 0; i < fixed; i++ {
				params = append(params, "a"+strconv.Itoa(i))
			}
			args := "[" + strings.Join(params, ", ") + "]"
			if f.Variadic {
				params = append(params, "resto")
				args = "_gabung_arg(" + args + ", resto)"
			}
			fmt.Fprintf(&s, "%s[%q] = fungsi(%s) { hasil _panggil(\"\", %q, %s); };\n",
				p.Namespace, f.Name, strings.Join(params, ", "), p.Namespace+"."+f.Name, args)
		}
		for _, t := range p.Types {
			fmt.Fprintf(&s, "%s[%q] = fungsi(data) { hasil _buat(%q, data); };\n", p.Namespace, t, p.Namespace+"."+t)
		}
		for _, c := range p.Consts {
			fmt.Fprintf(&s, "%s[%q] = _konst(%q);\n", p.Namespace, c, p.Namespace+"."+c)
		}
		for _, v := range p.Vars {
			fmt.Fprintf(&s, "%s[%q] = _konst(%q);\n", p.Namespace, v, p.Namespace+"."+v)
		}
		s.WriteString("\n")
	}
	_ = os.WriteFile("index.ns", []byte(s.String()), 0o644)
}

// Runtime helpers of the generated module (brace syntax, so it doesn't depend on layout rules).
const nsRuntime = `buat _p = muat_plugin("./plugin.so");
buat _no_cb = 0;
buat _aktif = benar;

fungsi _urai(teks_json) {
    buat r = json_decode(teks_json);
    jika r["err"] != kosong { lempar r["err"]; }
    hasil _bungkus(r["ok"]);
}

fungsi _bungkus(v) {
    buat t = tipe(v);
    jika t == "larik" {
        buat keluar = [];
        untuk (buat i = 0; i < panjang(v); i = i + 1) { tambah(keluar, _bungkus(v[i])); }
        hasil keluar;
    }
    jika t == "peta" {
        jika v["$h"] != kosong { hasil _objek(v); }
        buat keluar = peta_baru();
        buat kunci = peta_kunci(v);
        untuk (buat i = 0; i < panjang(kunci); i = i + 1) { keluar[kunci[i]] = _bungkus(v[kunci[i]]); }
        hasil keluar;
    }
    hasil v;
}

fungsi _lepas(v) {
    buat t = tipe(v);
    jika t == "fungsi" { hasil _callback(v); }
    jika t == "larik" {
        buat keluar = [];
        untuk (buat i = 0; i < panjang(v); i = i + 1) { tambah(keluar, _lepas(v[i])); }
        hasil keluar;
    }
    jika t == "peta" {
        buat keluar = peta_baru();
        jika v["$h"] != kosong { keluar["$h"] = v["$h"]; hasil keluar; }
        buat kunci = peta_kunci(v);
        untuk (buat i = 0; i < panjang(kunci); i = i + 1) { keluar[kunci[i]] = _lepas(v[kunci[i]]); }
        hasil keluar;
    }
    hasil v;
}

// Penunjuk keluaran buat parameter seperti rows.Scan(...any): jenis "teks", "angka", "boolean",
// "bytes" atau "apa". Go mengisinya, dan pemanggilnya mengembalikan larik nilai yang terisi.
fungsi penunjuk(k) {
    buat p = peta_baru();
    p["$ptr"] = k;
    hasil p;
}

fungsi _gabung_arg(dasar, resto) {
    jika resto != kosong {
        untuk (buat i = 0; i < panjang(resto); i = i + 1) { tambah(dasar, resto[i]); }
    }
    hasil dasar;
}

fungsi _panggil(target, nama, args) {
    buat lepas = [];
    untuk (buat i = 0; i < panjang(args); i = i + 1) { tambah(lepas, _lepas(args[i])); }
    hasil _urai(_p.panggil(target, nama, json_encode(lepas)));
}

fungsi _buat(tipe_go, data) {
    jika data == kosong { hasil _urai(_p.buat(tipe_go, "")); }
    hasil _urai(_p.buat(tipe_go, json_encode(_lepas(data))));
}

fungsi _konst(nama) { hasil _urai(_p.konst(nama)); }

fungsi _metode(h, nama, jumlah) {
    // 100+n: metode variadik -- n argumen tetap lalu satu larik untuk sisanya.
    jika jumlah == 100 { hasil fungsi(r) { hasil _panggil(h, nama, _gabung_arg([], r)); }; }
    jika jumlah == 101 { hasil fungsi(a, r) { hasil _panggil(h, nama, _gabung_arg([a], r)); }; }
    jika jumlah == 102 { hasil fungsi(a, b, r) { hasil _panggil(h, nama, _gabung_arg([a, b], r)); }; }
    jika jumlah == 103 { hasil fungsi(a, b, c, r) { hasil _panggil(h, nama, _gabung_arg([a, b, c], r)); }; }
    jika jumlah == 104 { hasil fungsi(a, b, c, d, r) { hasil _panggil(h, nama, _gabung_arg([a, b, c, d], r)); }; }
    jika jumlah == 0 { hasil fungsi() { hasil _panggil(h, nama, []); }; }
    jika jumlah == 1 { hasil fungsi(a) { hasil _panggil(h, nama, [a]); }; }
    jika jumlah == 2 { hasil fungsi(a, b) { hasil _panggil(h, nama, [a, b]); }; }
    jika jumlah == 3 { hasil fungsi(a, b, c) { hasil _panggil(h, nama, [a, b, c]); }; }
    jika jumlah == 4 { hasil fungsi(a, b, c, d) { hasil _panggil(h, nama, [a, b, c, d]); }; }
    jika jumlah == 5 { hasil fungsi(a, b, c, d, e) { hasil _panggil(h, nama, [a, b, c, d, e]); }; }
    jika jumlah == 6 { hasil fungsi(a, b, c, d, e, f) { hasil _panggil(h, nama, [a, b, c, d, e, f]); }; }
    jika jumlah == 7 { hasil fungsi(a, b, c, d, e, f, g) { hasil _panggil(h, nama, [a, b, c, d, e, f, g]); }; }
    jika jumlah == 8 { hasil fungsi(a, b, c, d, e, f, g, i) { hasil _panggil(h, nama, [a, b, c, d, e, f, g, i]); }; }
    // Lebih dari 8 argumen: satu parameter larik berisi semua argumen.
    hasil fungsi(semua) { hasil _panggil(h, nama, semua); };
}

fungsi _objek(v) {
    buat o = peta_baru();
    buat h = v["$h"];
    o["$h"] = h;
    o["$g"] = pegang(_p.bebas, h);
    o["tipe"] = v["tipe"];
    buat daftar = v["m"];
    buat nama = peta_kunci(daftar);
    untuk (buat i = 0; i < panjang(nama); i = i + 1) { o[nama[i]] = _metode(h, nama[i], daftar[nama[i]]); }
    jika v["f"] != kosong {
        buat kf = peta_kunci(v["f"]);
        untuk (buat i = 0; i < panjang(kf); i = i + 1) { o[kf[i]] = _bungkus(v["f"][kf[i]]); }
    }
    jika v["d"] != kosong { o["nilai"] = _bungkus(v["d"]); }
    o["json"] = fungsi() { hasil _urai(_p.panggil(h, "$json", "[]")); };
    o["teks"] = fungsi() { hasil _urai(_p.panggil(h, "$str", "[]")); };
    o["field"] = fungsi(n) { hasil _urai(_p.field(h, n)); };
    o["set"] = fungsi(n, x) { hasil _urai(_p.set_field(h, n, json_encode(_lepas(x)))); };
    o["bebas"] = fungsi() { _p.bebas(h); };
    hasil o;
}

fungsi _picu(f, a) {
    buat n = panjang(a);
    jika n == 0 { hasil f(); }
    jika n == 1 { hasil f(_bungkus(a[0])); }
    jika n == 2 { hasil f(_bungkus(a[0]), _bungkus(a[1])); }
    jika n == 3 { hasil f(_bungkus(a[0]), _bungkus(a[1]), _bungkus(a[2])); }
    jika n == 4 { hasil f(_bungkus(a[0]), _bungkus(a[1]), _bungkus(a[2]), _bungkus(a[3])); }
    jika n == 5 { hasil f(_bungkus(a[0]), _bungkus(a[1]), _bungkus(a[2]), _bungkus(a[3]), _bungkus(a[4])); }
    hasil kosong;
}

fungsi _dengarkan(nama, f) {
    jalan(fungsi() {
        latar();
        selama _aktif {
            buat e = _p.event(nama, 500);
            jika e != "" {
                buat d = json_decode(e);
                jika tipe(d) == "peta" {
                    // callback sinkron: Go menunggu hasilnya lewat balas()
                    buat r = kosong;
                    coba { r = _picu(f, d["a"]); } tangkap (err) { cetak("kesalahan di callback:", err); }
                    _p.balas(ke_teks(d["$id"]), json_encode({"r": _lepas(r)}));
                } lain {
                    _picu(f, d);
                }
            }
        }
    });
}

fungsi _callback(f) {
    _no_cb = _no_cb + 1;
    buat nama = "cb" + ke_teks(_no_cb);
    _dengarkan(nama, f);
    buat r = peta_baru();
    r["$cb"] = nama;
    hasil r;
}

// Menghentikan semua pendengar callback (biar proses bisa selesai).
fungsi berhenti_dengarkan() { _aktif = salah; }
`
