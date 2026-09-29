# 🏝️ Nusantara (`.ns`)

Bahasa pemrograman dengan keyword Bahasa Indonesia. Ditulis native di C++17, compile jadi satu binary, gak perlu install runtime apa pun buat jalanin.

```
fungsi fib(n) {
    jika n < 2 { hasil n; }
    hasil fib(n - 1) + fib(n - 2);
}

untuk (buat i = 0; i < 10; i = i + 1) {
    cetak(fib(i));
}
```

Tiap keyword ada padanan Inggrisnya (`buat`/`let`, `fungsi`/`func`, `jika`/`if`, dst) dan dua-duanya bisa dicampur bebas dalam satu file — token-nya sama persis di lexer. Daftar lengkap keyword ada di bagian "Bahasa" di bawah.

## Kenapa

Awalnya iseng pengen bahasa yang keyword-nya Bahasa Indonesia tapi gak berasa mainan — jadi dikompilasi ke native binary beneran (bukan di-interpret sama runtime lain kayak Python), ada garbage collector sendiri, ada goroutine/channel, dan sintaksnya dijaga simpel kayak Python/JS biar gampang dipelajari.

## Build & install

```bash
./install.sh
```

Ambil binary jadi (Linux x86-64/ARM64, Android/Termux) dan pasang ke `~/.local/bin` -- gak perlu compiler apa pun. Platform lain fallback build dari source (butuh `g++`).

Windows: `install.sh` lewat Git Bash/MSYS, atau PowerShell native: `.\install.ps1`.

Manual:

```bash
make
./nusantara run examples/hello.ns
```

```bash
nusa run main.ns       # jalanin file
nusa                    # REPL
nusa watch main.ns      # auto re-run tiap disimpen
```

## Bahasa

| Indonesia | Inggris | | Indonesia | Inggris |
|---|---|---|---|---|
| `buat` | `let` | | `kelas` | `class` |
| `fungsi` | `func` | | `turunan` | `extends` |
| `jika` / `lain` | `if` / `else` | | `bentuk` | `struct` |
| `selama` | `while` | | `jenis` | `enum` |
| `untuk` | `for` | | `coba` / `tangkap` | `try` / `catch` |
| `hasil` | `return` | | `lempar` | `throw` |
| `berhenti` / `lanjut` | `break` / `continue` | | `benar` / `salah` | `true` / `false` |

Tipe: `angka` (double), `teks`, `boolean`, `kosong`, `larik` (array), `peta` (map). Anotasi tipe opsional gaya TypeScript, dicek sebelum program jalan:

```
fungsi luas(p: angka, l: angka): angka {
    hasil p * l;
}
```

Ada OOP juga:

```
kelas Hewan {
    fungsi konstruktor(nama) { ini.nama = nama; }
    fungsi bicara() { cetak(ini.nama + " bersuara"); }
}

kelas Anjing turunan Hewan {
    fungsi bicara() { cetak(ini.nama + " menggonggong"); }
}

buat a = Anjing("Rex");
a.bicara();   // "Rex menggonggong"
```

Plus `bentuk` (struct data polos), `jenis` (enum), exception handling (`coba`/`tangkap`/`akhirnya`/`lempar` — boleh lempar nilai apa aja, bukan cuma teks), template string (`` `halo ${nama}` ``), dan fungsi anonim (`fungsi(x) { hasil x*2; }`).

## Concurrency

Goroutine (`jalan`) dan channel (`kanal`), jalan di atas satu GIL yang otomatis lepas pas nunggu I/O — jadi beneran nyata buat kerjaan network/background, walau bukan paralel CPU murni:

```
buat c = kanal_baru();
untuk (buat i = 0; i < 5; i = i + 1) {
    jalan(fungsi(n) { kanal_kirim(c, n * n); }, i);
}
buat total = 0;
untuk (buat i = 0; i < 5; i = i + 1) {
    total = total + kanal_terima(c);
}
cetak(total);   // 30
```

Memori dikelola GC mark-sweep otomatis, gak ada alokasi/pembebasan manual.

## Builtin yang lumayan lengkap

- `http_get`/`http_post` (HTTPS beneran, TLS 1.2/1.3 ditulis sendiri di `src/tls*.cpp`, tanpa pustaka luar), `tcp_konek`/`tcp_kirim`/`tcp_terima`, `http_dengar` buat bikin server
- `json_encode`/`json_decode`, `base64_encode`/`decode`
- `sha256_hex`, `hmac_sha256_hex`, `jwt_buat`/`jwt_verifikasi`
- `qr_baca` — baca QR code dari gambar (PNG/JPEG/BMP/dst); dikerjakan plugin `qr`
- `jalankan_perintah` — jalanin proses eksternal, argv-safe (gak lewat shell)
- `impor()` buat modul lokal, `muat_plugin()` buat native plugin (`.so` lewat dlopen — `http`, `ws`, `crypto`, `audio`, `gambar` sudah tertanam di binary, jadi `muat_plugin("http")` gak butuh file `.so`; `sqlite`, `js` (QuickJS buat file `.js`) dan `qr` berupa `.so` yang ikut terbangun otomatis oleh `make`)

Daftar lengkap + signature-nya ada di `src/interpreter.cpp` (cari `callBuiltin`).

## Package manager

Pure git, gak ada registry terpisah — paket ya cuma repo git biasa:

```bash
nusa get github.com/user/repo
nusa get github.com/user/repo#dev   # branch/tag spesifik
```

`impor("nama")` nyari `nusantara_modules/nama/index.ns` relatif ke folder kerja, sama kayak `node_modules`.

## Struktur

```
include/    header (.hpp)
src/        implementasi (.cpp) + main.cpp (CLI)
examples/   contoh program
plugins/    plugin native (muat_plugin, .so via dlopen); kode C vendor (SQLite, QuickJS, quirc) cuma ada di sini
tools/      jembatan modul Go (tools/gobridge) dan skrip generator
tests/      regression/golden test suite
```

Eksekusi lewat bytecode VM (`src/vm.cpp`, dengan JIT buat fungsi/loop aritmatika murni di `src/jit.cpp`). Tree-walker (`src/interpreter.cpp`) tinggal jadi cadangan buat kelas yang dideklarasi di dalam fungsi/blok dan JSX; `# nusa:tree-walker` di 10 baris pertama atau `NUSA_NO_VM=1` memaksanya.

## Sintaks gaya Python

Blok berindentasi dengan `:`, parameter default (`def f(a, b=1)`), literal peta `{"k": v}`, `;` gak wajib (baris baru mengakhiri pernyataan, juga di gaya kurung kurawal), `def`/`class`/`if`/`elif`/`for x in ...`/`while`/`try`/`except`/`return`/`import`, `range()`, slice `a[1:3]`, `True/False/None`. Kata kunci Indonesia tetap jalan dan sengaja dipendekkan (`kem` = return, dst).

## Modul Go

```bash
nusa go add github.com/user/modul --pkg sub/paket --std strings --blank github.com/mattn/go-sqlite3
```

Membangun `nusantara_modules/<nama>/` (plugin.so + index.ns) dari API Go lewat refleksi, lalu `import <nama>`. Go cuma dibutuhkan buat MEMBANGUN; hasilnya jalan di mesin tanpa Go. Kalau root modul bukan paket (mis. `mongo-driver`) atau kamu pakai `--semua`, semua sub-paket publiknya ikut.

- Fungsi/metode variadik: argumen sisa satu larik dan boleh dilewat (`db.Exec(sql)`, `db.Exec(sql, [a, b])`).
- `penunjuk("teks"|"angka"|"boolean"|"bytes"|"apa")` buat parameter keluaran (`rows.Scan`, `yaml.Unmarshal`); hasil isinya dikembalikan.
- `bytes_dari("teks")` buat parameter `[]byte`/`any` yang butuh bytes (kunci HMAC).
- Fungsi NSL bisa jadi callback Go: yang punya nilai balik atau menerima objek (`sort`, `strings.Map`, `http.HandlerFunc`, `gin`) ditunggu Go sampai selesai; handler event (`AddEventHandler`) jalan di goroutine latar. Untuk parameter `http.Handler`, `io.Writer`, `fmt.Stringer` cukup kirim fungsi.
- Field objek Go (`r.URL.Path`, `resp.Body`) bisa diakses langsung, objek Go dibebaskan otomatis begitu jadi sampah.
- Belum didukung: fungsi generik (`samber/lo`), channel Go, dan antarmuka kustom yang diimplementasikan dari NSL.

Sudah dicoba bangun & pakai: uuid, cast, jwt, decimal, yaml, toml, zerolog, goquery, goldmark, go-qrcode, imaging, go-redis, mysql/pq driver, gorilla/mux, gin, resty, viper, mongo-driver, golang.org/x/{text,net}, go-sqlite3 (`database/sql`), hypermeow (WhatsApp).

## Tanpa pustaka luar

Inti (`nusa`) gak butuh OpenSSL, libcurl, atau kode C vendor: TLS 1.2/1.3 (X25519/P-256/P-384, AES-GCM, ChaCha20-Poly1305, verifikasi X.509) ditulis sendiri di `src/tls*.cpp` dan dipakai bareng oleh `http_get`, plugin `http` (dengan proxy `https_proxy`/`no_proxy`) dan plugin `ws`. Satu perintah `make` sudah membangun binary dan plugin `.so` yang tersisa (SQLite/QuickJS/quirc divendor di foldernya masing-masing); butuh compiler C++17 dan `cc`.

## Lisensi

Lihat [NOTICE](NOTICE).
