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

Otomatis build (`make`) dan pasang ke `~/.local/bin`. Butuh `g++` (atau compiler C++17 lain) — gak ada dependency eksternal, cuma standard library + POSIX socket.

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

- `http_get`/`http_post` (HTTPS beneran, TLS lewat BearSSL divendor), `tcp_konek`/`tcp_kirim`/`tcp_terima`, `http_dengar` buat bikin server
- `json_encode`/`json_decode`, `base64_encode`/`decode`
- `sha256_hex`, `hmac_sha256_hex`, `jwt_buat`/`jwt_verifikasi`
- `qr_baca` — baca QR code dari gambar (PNG/JPEG/BMP/dst)
- `jalankan_perintah` — jalanin proses eksternal, argv-safe (gak lewat shell)
- `impor()` buat modul lokal, `muat_plugin()` buat native plugin (`.so` lewat dlopen — ada plugin bawaan SQLite, HTTP+TLS, WebSocket, crypto lanjutan, forensik gambar BMP)

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
plugins/    plugin native (muat_plugin, .so via dlopen)
tests/      regression/golden test suite
```

Eksekusi lewat bytecode VM dulu (`src/vm.cpp`, dengan JIT buat fungsi/loop aritmatika murni di `src/jit.cpp`), fallback ke tree-walking langsung di AST (`src/interpreter.cpp`) buat konstruksi yang belum didukung VM (kelas, struct, enum, try/catch, fungsi anonim).

## Lisensi

Lihat [NOTICE](NOTICE).
