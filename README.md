<p align="center">
  <img src="docs/hero.svg" width="720" alt="nsl Nusantara" />
</p>

# Nusantara

Bahasa pemrograman dengan keyword Bahasa Indonesia. Ditulis native di C++17, dikompilasi jadi satu binary, dan gak perlu install runtime apa pun buat menjalankannya. Ekstensi filenya `.ns`.

```
fungsi fib(n) {
    jika n < 2 { hasil n; }
    hasil fib(n - 1) + fib(n - 2);
}

untuk (buat i = 0; i < 10; i = i + 1) {
    cetak(fib(i));
}
```

Tiap keyword ada padanan Inggrisnya (`buat`/`let`, `fungsi`/`func`, `jika`/`if`, dan seterusnya). Dua-duanya boleh dicampur dalam satu file, karena token-nya sama persis di lexer. Daftar lengkap ada di bagian [Bahasa](#bahasa).

## Kenapa

Awalnya iseng pengen bahasa yang keyword-nya Bahasa Indonesia tapi gak berasa mainan. Jadi dikompilasi ke native binary beneran (bukan di-interpret runtime lain seperti Python), punya garbage collector sendiri, ada goroutine dan channel, dan sintaksnya dijaga simpel kayak Python/JS biar gampang dipelajari.

## Pasang

```bash
./install.sh
```

Skrip ini mengambil binary jadi (Linux x86-64/ARM64, Android/Termux) lalu memasangnya ke `~/.local/bin`, tanpa compiler. Platform lain build dari source (butuh `g++`).

Windows: `install.sh` lewat Git Bash/MSYS, atau PowerShell native: `.\install.ps1`.

Build manual:

```bash
make
./nusantara run examples/hello.ns
```

Dipakai begini:

```bash
nusa run main.ns        # jalankan file
nusa                    # REPL
nusa watch main.ns      # jalankan ulang tiap file disimpan
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

Tipe: `angka` (double), `teks`, `boolean`, `kosong`, `larik` (array), `peta` (map). Anotasi tipe bersifat opsional, gaya TypeScript, dan dicek sebelum program jalan:

```
fungsi luas(p: angka, l: angka): angka {
    hasil p * l;
}
```

OOP:

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

Ada juga `bentuk` (struct data polos), `jenis` (enum), exception (`coba`/`tangkap`/`akhirnya`/`lempar`, boleh melempar nilai apa saja), template string (`` `halo ${nama}` ``), dan fungsi anonim (`fungsi(x) { hasil x*2; }`).

## Concurrency

Goroutine (`jalan`) dan channel (`kanal`) berjalan di atas satu GIL yang otomatis lepas saat menunggu I/O. Jadi berguna buat kerjaan network dan background, walau bukan paralel CPU murni.

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

Memori dikelola GC mark-sweep otomatis, tanpa alokasi atau pembebasan manual.

## Builtin

- `http_get` / `http_post` (HTTPS sungguhan, TLS lewat BearSSL yang divendor), `tcp_konek` / `tcp_kirim` / `tcp_terima`, `http_dengar` untuk bikin server
- `json_encode` / `json_decode`, `base64_encode` / `decode`
- `sha256_hex`, `hmac_sha256_hex`, `jwt_buat` / `jwt_verifikasi`
- `qr_baca`: baca QR code dari gambar (PNG, JPEG, BMP, dan lainnya)
- `jalankan_perintah`: jalankan proses eksternal, argv-safe (tidak lewat shell)
- `impor()` untuk modul lokal, `muat_plugin()` untuk plugin native (`.so` lewat dlopen). Plugin bawaan: SQLite, HTTP+TLS, WebSocket, crypto lanjutan, forensik gambar BMP

Daftar lengkap dan signature-nya ada di `src/interpreter.cpp` (cari `callBuiltin`).

## Package manager

Murni git, tanpa registry terpisah. Paket cuma repo git biasa:

```bash
nusa get github.com/user/repo
nusa get github.com/user/repo#dev   # branch atau tag tertentu
```

`impor("nama")` mencari `nusantara_modules/nama/index.ns` relatif ke folder kerja, mirip `node_modules`.

## Struktur

```
include/    header (.hpp)
src/        implementasi (.cpp) + main.cpp (CLI)
examples/   contoh program
plugins/    plugin native (muat_plugin, .so lewat dlopen)
tests/      regression/golden test suite
```

Eksekusi lewat bytecode VM dulu (`src/vm.cpp`, dengan JIT untuk fungsi dan loop aritmatika murni di `src/jit.cpp`). Konstruksi yang belum didukung VM (kelas, struct, enum, try/catch, fungsi anonim) jatuh ke tree-walking langsung di AST (`src/interpreter.cpp`).

## Lisensi

Apache-2.0, lihat [LICENSE](LICENSE). Kode pihak ketiga di `third_party/` memakai lisensinya masing-masing, daftarnya ada di [NOTICE](NOTICE).
