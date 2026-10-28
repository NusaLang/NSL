<p align="center">
  <img src="docs/hero.svg" width="720" alt="nsl Nusantara" />
</p>

# Nusantara

Bahasa pemrograman modern yang ditulis mandiri secara self-hosted murni dalam Nusantara (`.ns`). Keyword-nya bisa Inggris (`func`, `if`, `return`, ...) atau Indonesia (`fungsi`, `jika`, `kalau`, `hasil`, ...), dengan struktur yang rapi dan fleksibel: mendukung blok berindentasi titik dua (`:`) maupun kurung kurawal (`{ }`). Mengompilasi langsung menjadi satu native binary (x86-64 ELF) tanpa perlu install runtime apa pun. Ekstensi filenya `.ns`.

```python
fungsi fib(n):
    jika n < 2:
        hasil n
    hasil fib(n - 1) + fib(n - 2)

untuk i dalam rentang(10):
    cetak(fib(i))
```

Tiap keyword ada padanan Inggrisnya (`buat`/`let`, `fungsi`/`func`, `jika`/`if`, `hasil`/`return`, dan seterusnya). Dua-duanya boleh dicampur dalam satu file, karena token-nya sama persis di lexer. Sintaks blok fleksibel mendukung gaya indentasi titik dua (`:`) maupun kurung kurawal (`{ }`). Daftar lengkap ada di bagian [Bahasa](#bahasa).

## Kenapa

Awalnya iseng pengen bahasa yang keyword-nya Bahasa Indonesia tapi gak berasa mainan. Pada cabang ini, seluruh toolchain Nusantara telah bermutasi menjadi **self-hosted**:
* **Native AOT Compiler**: Mengompilasi kode `.ns` langsung ke instruksi assembly x86-64 mentah (`as`/`ld`), menghasilkan satu binary native mandiri tanpa dependensi runtime dengan performa mendekati Go.
* **Bytecode VM**: Virtual machine berbasis stack dengan register caching dan constant folding untuk eksekusi bytecode cepat.
* **Tree-walking Interpreter**: Implementasi referensi langsung di atas AST untuk verifikasi semantik bahasa.
* **Murni `.ns`**: Seluruh lexer, parser, interpreter, bytecode VM, dan compiler AOT ditulis dalam bahasa Nusantara sendiri.

## Performa (Benchmark)

Hasil pengujian min-of-N wall-clock (milidetik) pada 5 beban kerja (`fib(20)`, 20k array sum, 10k map ops, 5k string build, 10k OOP calls):

| Bahasa | fib20 | array | map | string | oop |
|---|---:|---:|---:|---:|---:|
| Go | 0.17 | 0.29 | 8.24 | 1.40 | 0.01 |
| **Nusantara (Native AOT)** | **0.33** | **0.59** | 17.60 | 3.45 | **0.50** |
| LuaJIT | 1.55 | 1.41 | 9.98 | 2.56 | 0.27 |
| Node.js (V8) | 1.54 | 11.49 | 41.51 | 5.72 | 6.03 |
| Python 3 | 5.29 | 5.78 | 24.29 | 3.43 | 14.01 |
| Lua | 2.15 | 5.33 | 37.67 | 10.30 | 25.84 |
| Ruby | 2.86 | 5.28 | 92.02 | 12.74 | 23.19 |
| Perl | 14.80 | 6.99 | 18.88 | 1.65 | 15.91 |

Native AOT berada di kisaran ~2x kecepatan Go pada komputasi aritmatika, array, dan OOP, mengungguli Python, Ruby, Lua, dan Node.js.

## Pasang & Jalankan

Build compiler self-hosted:

```bash
make build   # Bootstrap core/main.ns menjadi executable ./nusa
make test    # Jalankan pengujian seluruh backend
make install # Pasang executable ke ~/.local/bin
```

Cara menjalankan program:

```bash
./nusa run main.ns                    # Jalankan via Tree-walking Interpreter
./nusa --vm main.ns                   # Jalankan via Bytecode VM
./nusa build --native main.ns -o app  # Kompilasi langsung ke native binary ELF
./app                                 # Eksekusi binary mandiri tanpa runtime
```

## Bahasa

| Indonesia | Inggris | | Indonesia | Inggris |
|---|---|---|---|---|
| `buat` | `let` / `var` | | `kelas` | `class` |
| `fungsi` | `func` | | `turunan` | `extends` |
| `jika` / `lain` / `kalau` | `if` / `else` / `elif` | | `dalam` | `in` |
| `selama` | `while` | | `bentuk` | `struct` |
| `untuk` | `for` | | `coba` / `tangkap` | `try` / `except` |
| `hasil` | `return` | | `lempar` | `throw` |
| `berhenti` / `lanjut` | `break` / `continue` | | `benar` / `salah` | `true` / `false` |
| `dan` / `atau` / `bukan` | `and` / `or` / `not` | | `kosong` | `null` / `None` |

Tipe dasar: `angka` (`int`/`float`), `teks` (`str`), `boolean` (`bool`), `kosong` (`None`), `larik` (`list`), `peta` (`map`/`dict`). Anotasi tipe bersifat opsional gaya TypeScript:

```python
fungsi luas(p: angka, l: angka): angka:
    hasil p * l
```

Pemrograman Berorientasi Objek (OOP):

```python
kelas Hewan:
    fungsi __init__(ini, nama, umur = 1):
        ini.nama = nama
        ini.umur = umur

    fungsi suara(ini, pesan = "bersuara"):
        cetak(ini.nama, pesan)

kelas Anjing turunan Hewan:
    fungsi suara(ini, pesan = "menggonggong"):
        cetak(ini.nama, pesan)

buat a = Anjing("Rex")
a.suara()           # Rex menggonggong
a.suara("tidur")    # Rex tidur
```

Mendukung alias instance `ini`, `self`, atau `this`, nilai parameter default, penanganan eksepsi (`coba`/`tangkap`/`akhirnya`/`lempar`), serta fungsi anonim lambda.

## Builtin

* `JalankanPerintah` / `runCommand`: menjalankan proses eksternal langsung lewat syscall (fork, execve, pipe, waitpid di Native AOT, aman dari shell injection).
* `baca_file` / `readFile`, `tulis_file` / `writeFile`, `file_ada` / `fileExists`.
* `impor()`: modul lokal `.ns` (di-inline secara statis saat kompilasi Native AOT).
* `gcInfo()` dan `gcForce()`: pemantauan memori runtime.
* Fungsi bawaan koleksi: `panjang` / `len`, `tambah` / `push`, `hapus_akhir` / `pop`, `rentang` / `range`, konversi tipe `ke_teks` / `str`, `ke_angka` / `int`, `float`.

## Struktur

```
core/        Lexer, Parser, AST, Interpreter, Bytecode VM, CLI entry point
compiler/    Native AOT Compiler (x86-64) dan runtime assembly
std/         Pustaka standar (proc, sysmod, i18n, dll.)
net/         Modul jaringan (HTTP, socket)
crypto/      Modul kriptografi dan hashing
media/       Modul pemrosesan media
lib/         Pustaka utilitas dan kompatibilitas
```

## Lisensi

Apache-2.0, lihat [LICENSE](LICENSE) dan [NOTICE](NOTICE).
