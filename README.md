# 🏝️ Nusantara (`.ns`)

Bahasa pemrograman yang ditulis native di C++17, jalan di bytecode VM sendiri, dan bisa dipakai dengan **gaya Python** atau **keyword Bahasa Indonesia** (atau dicampur). Satu binary, tanpa runtime lain, tanpa pustaka luar (TLS-nya pun ditulis sendiri), dan bisa memakai **modul Go** apa saja.

```python
def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

for i in range(10):
    print(fib(i))
```

Versi Indonesianya persis sama artinya (token-nya identik di lexer):

```
fungsi fib(n) {
    jika n < 2 { hasil n; }
    hasil fib(n - 1) + fib(n - 2);
}

untuk (buat i = 0; i < 10; i = i + 1) {
    cetak(fib(i));
}
```

Isi:
[Build & install](#build--install) · [CLI](#cli) · [Bahasa](#bahasa) · [Sintaks gaya Python](#sintaks-gaya-python) · [OOP](#oop) · [Concurrency](#concurrency) · [Builtin](#builtin) · [Modul & plugin](#modul--plugin) · [Modul Go](#modul-go) · [Tanpa pustaka luar](#tanpa-pustaka-luar) · [Memori & performa](#memori--performa) · [Struktur repo](#struktur-repo) · [Tes](#tes) · [Batasan](#batasan)

## Build & install

```bash
./install.sh          # ambil binary jadi (Linux x86-64/ARM64, Android/Termux) ke ~/.local/bin
```

Platform lain fallback build dari source (butuh `g++`). Windows: `install.sh` lewat Git Bash/MSYS, atau PowerShell native `.\install.ps1`.

Dari source, **satu perintah** membangun semuanya:

```bash
make            # binary ./nusantara + plugin .so yang tersisa (sqlite, js, qr)
make opt        # sama, ke build-opt/nusa (dipakai tes suite)
./nusantara run examples/hello.ns
```

Butuh compiler C++17 dan `cc`. Go **tidak** dibutuhkan kecuali kamu membangun modul Go (`nusa go add`).

## CLI

```bash
nusa                          # REPL
nusa main.ns                  # jalanin file  (= nusa run main.ns)
nusa app.js                   # jalanin JavaScript lewat QuickJS (plugin js)
nusa -e 'print(1 + 1)'        # eval satu potong kode
nusa -c main.ns               # cek lexer/parser/tipe saja
nusa watch main.ns            # auto re-run tiap file disimpan
nusa get github.com/u/repo    # install paket (git clone); `install` baca ./nusa.json
nusa go add <modul-go> ...    # bangun modul Nusantara dari modul Go (lihat "Modul Go")
nusa set lang en              # bahasa pesan error: ind | en (atau env NUSA_LANG)
nusa build | dev | deploy     # toolchain web next-ns
```

## Bahasa

Keyword Indonesia dan Inggris/Python **setara dan bisa dicampur bebas**:

| Indonesia | Singkat | Inggris / Python | | Indonesia | Singkat | Inggris / Python |
|---|---|---|---|---|---|---|
| `buat` | | `let` | | `kelas` | | `class` |
| `fungsi` | `fung` | `func`, `function`, `def` | | `turunan` | | `extends` (atau `class A(B):`) |
| `jika` / `lain` | `jk` | `if` / `else`, `elif` | | `bentuk` | | `struct` |
| `selama` | `slm` | `while` | | `jenis` | | `enum` |
| `untuk` | | `for` | | `coba` / `tangkap` / `akhirnya` | `akhir` | `try` / `except`,`catch` / `finally` |
| `hasil` | `kem` | `return` | | `lempar` | | `throw`, `raise` |
| `berhenti` / `lanjut` | `henti` | `break` / `continue` | | `benar` / `salah` / `kosong` | | `True` / `False` / `None` |
| `ini` / `induk` | | `self` / `super` | | `bukan` | | `not` |

Nama builtin juga punya alias Python: `print`=`cetak`, `len`/`length`=`panjang`, `append`/`push`=`tambah`, `str`=`ke_teks`, `int`/`float`=`ke_angka`, `range`=`rentang`, `keys`=`peta_kunci`, `new_map`=`peta_baru`, dst.

Tipe: `angka` (double), `teks`, `boolean`, `kosong`, `larik`, `peta`. Anotasi tipe opsional dicek sebelum program jalan:

```python
def luas(p: angka, l: angka) -> angka:
    return p * l
```

## Sintaks gaya Python

- **Blok berindentasi dengan `:`** — `if x:`, `for i in range(n):`, `while c:`, `def f():`, `class A:`, `try:`/`except e:`/`finally:`. Gaya kurung kurawal `{ }` tetap jalan; satu file memakai salah satunya.
- **`;` tidak wajib.** Baris baru mengakhiri pernyataan, juga di gaya kurung kurawal. Dua pernyataan dalam satu baris tetap butuh `;`.
- **Deklarasi implisit:** `x = 1` cukup, tanpa `buat`. Nama baru di dalam blok tetap terlihat setelahnya (seperti Python); `global x` / `nonlocal x` untuk menulis ke luar fungsi.
- **Parameter default:** `def f(a, b=1, c=[])` — juga di metode, konstruktor, dan lambda. Nilai default dievaluasi tiap panggilan (bukan sekali seperti Python). Argumen yang dilewat bernilai `None`.
- **Literal peta:** `{"nama": "Rex", "umur": 3}`, boleh bersarang dan multi-baris dengan koma penutup. **Peta menjaga urutan sisip** seperti `dict` Python: `keys(m)`, iterasi, dan `json_encode` mengikuti urutan kunci ditambahkan.
- `for x in larik:`, `for i in range(a, b, step):`, slice `a[1:3]`, `a[-1]`, `a[::2]`, `lambda a, b=2: a * b`.
- `import modul`, `from modul import nama as alias`, `import a.b` (paket dengan `__init__`/`index.ns`).
- Metode bawaan gaya Python di teks/larik/peta (`"a,b".split(",")`, `xs.append(v)`, `m.keys()`, dst).
- `and` / `or` / `not`, `in` / `not in` (teks, larik, peta), `is` / `is not` (kesamaan nilai, mis. `x is None`).
- Ekspresi kondisional `a if cond else b` (bersarang boleh), f-string `f"halo {nama}, {x + 1}"` (`{{` `}}` untuk kurung literal; tanpa format spec), string kutip tunggal `'...'` dan triple-quote `"""..."""` multi-baris.

## OOP

```python
class Hewan:
    def __init__(self, nama, suara="..."):
        self.nama = nama
        self.suara = suara
    def bicara(self):
        print(self.nama + " " + self.suara)

class Anjing(Hewan):
    def bicara(self):
        print(self.nama + " menggonggong")

Anjing("Rex").bicara()
```

Gaya Indonesia: `kelas`, `fungsi konstruktor(...)`, `ini.x`, `turunan`, `induk`. Plus `bentuk` (struct data polos), `jenis` (enum), exception (`coba`/`tangkap`/`akhirnya`/`lempar` — boleh lempar nilai apa saja), template string `` `halo ${nama}` ``, dan fungsi anonim.

## Concurrency

Goroutine (`jalan`) dan channel (`kanal_baru`, `kanal_kirim`, `kanal_terima`, `pilih_kanal`), `wg_*` (WaitGroup), di atas satu GIL yang dilepas otomatis saat menunggu I/O atau memanggil plugin — cocok untuk kerjaan network/background, bukan paralel CPU murni.

```python
c = channel()
for i in range(5):
    go(lambda n: chan_send(c, n * n), i)
total = 0
for i in range(5):
    total = total + chan_recv(c)
print(total)   # 30
```

`latar()` di dalam goroutine menandainya sebagai pekerja latar: proses boleh selesai walau goroutine itu masih jalan (dipakai pendengar event modul Go).

## Builtin

- Jaringan: `http_get`/`http_post` (HTTPS beneran), `tcp_konek`/`tcp_kirim`/`tcp_terima`, `http_dengar` (server), `email_kirim`
- Data: `json_encode`/`json_decode`, `base64_encode`/`decode`, `sha256_hex`, `hmac_sha256_hex`, `jwt_buat`/`jwt_verifikasi`
- Sistem: `baca_file`/`tulis_file`/`file_ada`, `jalankan_perintah` (argv-safe, tanpa shell), `proc_stream_*`, `ambil_env`, `waktu`, `tidur`
- `qr_baca` (plugin `qr`), `impor()`, `muat_plugin()`, `gc_paksa()`

Daftar lengkap + signature ada di `src/interpreter.cpp` (cari `builtinNames`/`callBuiltin`).

## Modul & plugin

- `import nama` / `impor("nama")` mencari `nusantara_modules/nama/index.ns` (atau `nama.ns`) relatif ke folder kerja, seperti `node_modules`. `nusa get` memasangnya dari git.
- **Modul sistem `muat_plugin("nama")`:**
  - **Tertanam di binary** (tanpa file `.so`): `http` (klien HTTP/1.1 + TLS, redirect, streaming, proxy), `ws` (WebSocket ws/wss), `crypto`, `audio`, `gambar`.
  - **Plugin `.so`** yang ikut terbangun oleh `make`: `sqlite`, `js` (QuickJS), `qr` (quirc + stb_image). Kode C vendor hanya ada di `plugins/<nama>/vendor/`.
  - Path eksplisit `muat_plugin("./x.so")` memuat plugin buatanmu (ABI di `include/plugin_abi.h`, contoh `plugins/hello`).

```python
h = muat_plugin("http")
r = json_decode(h.minta("GET", "https://example.com", "/", "{}", ""))
print(r["status"], r["header"]["content-type"], len(r["tubuh"]))
```

## Modul Go

Pakai pustaka Go apa saja tanpa menulis binding. Go dibutuhkan **hanya untuk membangun**; folder hasilnya (`plugin.so` + `index.ns`) bisa dibagikan dan dijalankan di mesin tanpa Go.

```bash
nusa go add github.com/google/uuid
nusa go add github.com/polymorfa/hypermeow --pkg types --pkg store/sqlstore --blank github.com/mattn/go-sqlite3
nusa go add --std strings --std net/http --std sort --nama gostd
nusa go add github.com/samber/lo          # fungsi generik ikut
nusa go add go.mongodb.org/mongo-driver   # root bukan paket -> semua sub-paket publik ikut
```

Opsi: `--pkg <sub>` paket lain di modul (boleh diulang) · `--std <paket>` paket standar Go · `--blank <paket>` import untuk efek samping (driver DB) · `--semua` semua sub-paket publik · `--nama <nama>` nama modul · `--keluar <dir>` folder tujuan (default `./nusantara_modules/<nama>`).

Lalu dipakai seperti modul biasa:

```python
import uuid
print(uuid.uuid.New().String())

import util                       # go-sqlite3 lewat database/sql
db = util.sql.Open("sqlite3", "file:t.db")
db.Exec("create table u(id integer primary key, nama text)")          # argumen variadik boleh dilewat
db.Exec("insert into u(nama) values(?)", ["budi"])
rows = db.Query("select id, nama from u", [])
while rows.Next():
    print(rows.Scan([util.penunjuk("angka"), util.penunjuk("teks")]))   # [1, "budi"]
```

**Apa yang didukung**

| Go | Di Nusantara |
|---|---|
| fungsi, metode, konstanta, variabel paket, tipe struct (`Nama(data)`) | dipanggil langsung: `paket.Fungsi(a, b)`, `obj.Metode()` |
| angka/teks/boolean/larik/peta/struct (JSON) | nilai biasa; `int64` > 2^53 dikirim sebagai teks |
| pointer, interface, objek berstatus | objek dengan metode dan field-nya: `r.URL.Path`, `resp.Body`, `obj.field("X")`, `obj.set("X", v)`, `obj.json()`, `obj.teks()`; dibebaskan otomatis saat jadi sampah |
| fungsi/metode variadik | argumen sisa satu larik, boleh dilewat: `db.Exec(sql)` / `db.Exec(sql, [a, b])` |
| parameter keluaran (`rows.Scan(&x)`, `yaml.Unmarshal(d, &v)`) | `penunjuk("teks"\|"angka"\|"boolean"\|"bytes"\|"apa")`; hasil isinya dikembalikan |
| `[]byte` / `[N]byte` | teks (bytes mentah, biner aman); `bytes_dari("teks")` untuk parameter `[]byte`/`any` |
| fungsi generik (`lo.Map`, `slices.*`) | diinstansiasi dengan `any` (kendala numerik: `float64`/`int`); kendala `Ordered` juga punya varian `_teks` (`Max_teks`) |
| callback (`func(...)`) | kirim fungsi NSL. Yang punya nilai balik atau menerima objek/pointer (`sort`, `strings.Map`, `http.HandlerFunc`, gin) ditunggu Go sampai selesai (maks 60 dtk); handler event (`AddEventHandler`) jalan di goroutine latar |
| interface (`sort.Interface`, `http.Handler`, `io.Writer`, `fmt.Stringer`, `error`, interface milik modul) | kirim **peta berisi fungsi** bernama metode: `sort.Sort({"Len": f, "Less": g, "Swap": h})`; interface satu-metode cukup satu fungsi; `error` cukup teks |
| channel | `chan_go("int", 10)` membuat; objek channel punya `Kirim(v, ms)`, `Terima(ms)`, `TerimaOk(ms)`, `Tutup()`, `Panjang()` |
| data untuk parameter interface (`jwt.Claims`) | dipetakan otomatis ke tipe konkret yang cocok (`jwt.MapClaims`) |

```python
import gostd
data = [5, 2, 9, 1]
def tukar(i, j):
    data[i], data[j] = data[j], data[i]      # atau pakai variabel bantu
gostd.sort.Sort({"Len": lambda: len(data), "Less": lambda i, j: data[i] < data[j], "Swap": tukar})

def handler(w, r):
    w.Write("halo dari NSL: " + r.URL.Path)
srv = gostd.httptest.NewServer(handler)      # server Go, handler NSL
print(gostd.io.ReadAll(gostd.http.Get(srv.URL + "/tes").Body))
```

Framework web Go dipakai dengan handler NSL:

```python
import gin
g = gin.gin
r = g.Default()
def kuadrat(c):
    n = int(c.Query("n"))
    c.JSON(200, {"n": n, "kuadrat": n * n})
r.GET("/kuadrat", kuadrat)          # handler tunggal tidak perlu dibungkus larik
def layani():
    latar()                         # pekerja latar: proses boleh selesai walau server masih jalan
    r.Run(":8080")
jalan(layani)
tidur(60000)                        # curl "localhost:8080/kuadrat?n=12" -> {"kuadrat":144,"n":12}
```

Sudah dicoba dibangun: uuid, cast, jwt, decimal, yaml, toml, zerolog, samber/lo, goquery, goldmark, go-qrcode, imaging, go-redis, mysql/pq, gorilla/mux, gin, resty, viper, mongo-driver, golang.org/x/{text,net}, go-sqlite3, hypermeow (WhatsApp).

**Batasan modul Go:** tipe generik (`Set[T]`) tidak dibungkus (hanya fungsinya); antarmuka yang butuh mengisi buffer keluaran (`io.Reader.Read(p)`) tidak bisa diimplementasikan dari NSL; callback yang tidak dibalas dalam 60 detik dilanjutkan dengan nilai kosong; paket yang butuh cgo/library C sistem harus tersedia di mesin pembangun.

## Tanpa pustaka luar

Inti tidak butuh OpenSSL, libcurl, atau kode C vendor:

- **TLS 1.2/1.3 ditulis sendiri** (`src/tls*.cpp`): X25519/P-256/P-384, AES-GCM, ChaCha20-Poly1305, RSA/ECDSA, verifikasi rantai X.509 + nama host + trust store (sistem atau bundel bawaan). Dipakai `http_get`/`http_post`, plugin `http`, dan `ws`. Tidak ada mode "matikan verifikasi".
- Klien HTTP di plugin `http` juga tertanam: HTTP/1.1, redirect, chunked, streaming, timeout, dan proxy via `https_proxy`/`http_proxy`/`no_proxy`.
- Vendor C (SQLite, QuickJS, quirc) hanya ada sebagai plugin `.so` opsional di `plugins/*/vendor/`.

## Memori & performa

- **Bytecode VM** (`src/vm.cpp`) untuk semua konstruksi umum, dengan JIT untuk fungsi/loop aritmatika murni (`src/jit.cpp`). Kelas mendekati kecepatan Python. Tree-walker (`src/interpreter.cpp`) tinggal cadangan untuk kelas yang dideklarasi di dalam fungsi/blok dan JSX; paksa dengan komentar `# nusa:tree-walker` di 10 baris pertama atau env `NUSA_NO_VM=1`; `NUSA_VM_DEBUG=1` untuk debug VM.
- **GC mark-sweep** untuk lingkungan/closure, dan pelacak siklus untuk objek: siklus `a.o = b; b.o = a`, larik yang berisi dirinya, dan peta yang menunjuk dirinya sendiri dibebaskan (diuji jutaan iterasi dengan RSS datar). Ambang koleksi mengikuti ukuran heap yang hidup, jadi heap besar (modul Go dengan ribuan closure) tidak di-mark ulang terus-menerus.
- Objek Go dibebaskan otomatis; sesi WhatsApp live 10 menit dan soak `ws`/`wss` 5 menit tidak menunjukkan pertumbuhan memori.
- Kalau curiga bocor: jalankan `valgrind --leak-check=full nusa skrip.ns` (`definitely lost` harus 0) atau ukur RSS pada loop panjang.

## Struktur repo

```
include/        header (.hpp): value, gc, vm, parser, tls*, plugin_abi.h, ordered_map.hpp ...
src/            interpreter, VM, JIT, lexer/layout/parser (sintaks Python), typechecker, GC
                tls*.cpp        TLS + X.509 + bundel CA sendiri
                net.cpp         socket / http_get / http_dengar
                mod_*.cpp       modul sistem tertanam: http, ws, crypto, audio, gambar
                plugin.cpp      pemuat plugin .so + registri modul tertanam
                sysmod.cpp      pemanggil plugin sistem opsional (js, qr)
                gobridge*.cpp   `nusa go add` (+ sumber jembatan Go yang ditanam)
plugins/        plugin .so: sqlite, js (QuickJS), qr (quirc+stb), hello (contoh); vendor C di sini
tools/          gobridge/ (bridge.go = runtime jembatan Go, gen/main.go = generator registri),
                embed_gobridge.py, gen_ca_bundle.py
examples/       contoh program
tests/          run.sh (golden test), cases/, golden/, tls/ (vektor kripto)
bench/          microbenchmark
```

## Tes

```bash
make opt && tests/run.sh          # golden test: banding output tiap tests/cases/*.ns dengan tests/golden/
tests/run.sh --filter=peta        # sebagian
tests/run.sh --update             # tulis ulang golden (hanya kalau perubahan output disengaja)
make tls-test                     # vektor kripto TLS (dari implementasi independen)
```

`tests/run.sh` juga mengecek `src/gobridge_embed.cpp` tidak usang. Setelah mengubah `tools/gobridge/*`: `python3 tools/embed_gobridge.py > src/gobridge_embed.cpp`.

## Batasan

- `angka` selalu `double` (bilangan bulat tepat sampai 2^53).
- Satu GIL: goroutine tidak paralel untuk CPU murni.
- Parameter default: argumen yang tidak diisi bernilai `None`, jadi `None` eksplisit ikut memicu nilai default.
- Belum ada: `yield`/generator, format spec di f-string (`{x:.2f}`), `with`, dekorator.
- WASM/browser: plugin native tidak tersedia.

## Lisensi

Lihat [NOTICE](NOTICE).
