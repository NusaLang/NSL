# 🏝️ Nusantara (`.ns`)

Bahasa pemrograman bergaya **Python** yang ditulis native di C++17 dan berjalan di bytecode VM sendiri. Keyword-nya bisa **Inggris/Python** atau **Indonesia** (atau dicampur), dengan struktur yang sama: blok berindentasi, `:` di akhir header, tanpa `{ }` dan `;`. Satu binary, tanpa runtime lain, tanpa pustaka luar (TLS-nya pun ditulis sendiri), dan bisa memakai **modul Go** apa saja. Dipakai seperti `node`/`python`: `nusa skrip.ns`.

```python
def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

for i in range(10):
    print(fib(i))
```

Program yang sama dengan keyword Indonesia — strukturnya tetap Python (indentasi, `:`, `for ... in`):

```
fungsi fib(n):
    jika n < 2:
        hasil n
    hasil fib(n - 1) + fib(n - 2)

untuk i dalam rentang(10):
    cetak(fib(i))
```

Bentuk pendeknya juga jalan (`fung`, `jk`, `lain`, `slm`, `kem` = `return`, `henti`, ...).

Isi:
[Build & install](#build--install) · [CLI](#cli) · [Bahasa](#bahasa) · [Sintaks gaya Python](#sintaks-gaya-python) · [OOP](#oop) · [Pustaka standar](#pustaka-standar) · [Concurrency](#concurrency) · [Builtin](#builtin) · [Modul & plugin](#modul--plugin) · [Modul Go](#modul-go) · [Tanpa pustaka luar](#tanpa-pustaka-luar) · [Memori & performa](#memori--performa) · [Struktur repo](#struktur-repo) · [Tes](#tes) · [Batasan](#batasan)

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

`nusa` adalah runtime, seperti `node` atau `python` — tidak ada bundler/toolchain web bawaan.

```bash
nusa                          # REPL
nusa main.ns arg1 arg2        # jalanin file (sys.argv = ["main.ns", "arg1", "arg2"])
nusa app.js                   # jalanin JavaScript lewat QuickJS (plugin js)
nusa -e 'print(1 + 1)'        # eval satu potong kode
nusa -c main.ns               # cek lexer/parser/tipe saja
nusa watch main.ns            # auto re-run tiap file disimpan
nusa get github.com/u/repo    # install paket (git clone); `install` baca ./nusa.json
nusa go add <modul-go> ...    # bangun modul Nusantara dari modul Go (lihat "Modul Go")
nusa update                   # perbarui nusa (dan plugin) ke rilis terbaru
nusa set lang en              # bahasa pesan error: ind | en (atau env NUSA_LANG)
nusa -h                       # bantuan
```

## Bahasa

Keyword Indonesia dan Inggris/Python **setara dan bisa dicampur bebas** dalam satu file:

| Indonesia | Singkat | Inggris / Python | | Indonesia | Singkat | Inggris / Python |
|---|---|---|---|---|---|---|
| `buat` | | `let` (tidak perlu di Python) | | `kelas` | | `class` |
| `fungsi` | `fung` | `def`, `func`, `function` | | `turunan` | | `extends` (atau `class A(B):`) |
| `jika` / `lain` | `jk` | `if` / `else`, `elif` | | `bentuk` | | `struct` |
| `selama` | `slm` | `while` | | `jenis` | | `enum` |
| `untuk` ... `dalam` | | `for` ... `in` | | `coba` / `tangkap` / `akhirnya` | `akhir` | `try` / `except` / `finally` |
| `hasil` | `kem` | `return` | | `lempar` | | `raise`, `throw` |
| `berhenti` / `lanjut` | `henti` | `break` / `continue` | | `benar` / `salah` / `kosong` | | `True` / `False` / `None` |
| `ini` / `induk` | | `self` / `super` | | `bukan` / `dan` / `atau` | | `not` / `and` / `or` |
| `dalam` | | `in` | | `adalah` | | `is` |
| `dengan` ... `sbg` | | `with` ... `as` | | `pastikan` / `hapus` | | `assert` / `del` |

Nama builtin punya padanan Python: `print`=`cetak`, `len`=`panjang`, `range`=`rentang`, `str`=`ke_teks`, `float`=`ke_angka`, `keys`=`peta_kunci`, ... Tipe: `angka` (double), `teks`, `boolean`, `kosong`, `larik`, `peta`. Anotasi tipe opsional dicek sebelum program jalan:

```python
def luas(p: angka, l: angka) -> angka:
    return p * l
```

**Kebenaran (truthiness) seperti Python:** `None`, `False`, `0`, `""`, `[]`, dan `{}` bernilai salah; selain itu benar.

## Sintaks gaya Python

- **Blok berindentasi dengan `:`** — `if x:`, `elif`, `for i in range(n):`, `while c:`, `def f():`, `class A:`, `try:`/`except e:`/`finally:`, `with ... as f:`. Gaya kurung kurawal `{ }` tetap jalan (satu file memakai salah satunya).
- **`;` tidak wajib.** Baris baru mengakhiri pernyataan, juga di gaya kurung kurawal.
- **Deklarasi implisit:** `x = 1` cukup, tanpa `buat`. Nama baru di dalam blok tetap terlihat setelahnya (seperti Python); `global x` / `nonlocal x` untuk menulis ke luar fungsi.
- **Penugasan beruntun dan unpacking:** `a = b = 0`, `a, *rest = xs`, `*init, last = xs`, `a, b = 1, 2`, `a, b = b, a`, `x[i], x[j] = x[j], x[i]`, `q, r = divmod(7, 2)`, `for i, x in enumerate(xs):`, `for k, v in d.items():`. Tuple `(a, b)` berupa larik.
- **Parameter default dan argumen bernama:** `def f(a, b=1)`, `f(1, b=2)`, juga di metode, konstruktor `P(x=1)`, lambda, dan builtin (`print(a, b, sep="-", end="")`, `sorted(xs, key=len, reverse=True)`, `xs.sort(reverse=True)`, `max(xs, default=0)`, `"{a}".format(a=1)`). Nilai default dievaluasi tiap panggilan. Argumen yang dilewat bernilai `None`.
- **Komprehensi:** `[x * 2 for x in xs if x > 0]`, `{k: v for k, v in pairs}`, bersarang (`for a in x for b in y`), dan generator sebagai argumen: `sum(x * x for x in xs)`.
- **Literal peta:** `{"nama": "Rex"}`. **Peta menjaga urutan sisip** seperti `dict` Python; kunci non-teks dipakai sebagai teks (`d[1]` = `d["1"]`).
- **Operator:** `**` (pangkat), `//` (bagi bulat, di file bergaya Python; di gaya `{ }` `//` tetap komentar), `%` (sisa; atau format teks: `"%d item" % 3`), `a if c else b`, `x in xs` / `x not in xs` (teks, larik, peta), `x is None` / `x is not None`, perbandingan berantai `0 <= x < n`, `and`/`or`/`not`, `del x[i]`, `assert cond, "pesan"`.
- **Slice:** `a[1:3]`, `a[-2:]`, `a[::-1]`, `a[::2]`, `a[3:0:-1]` untuk teks dan larik.
- **String:** kutip ganda/tunggal, triple-quote multi-baris, f-string `f"halo {nama}, {x + 1}"` (`{{` `}}` untuk kurung literal), template `` `halo ${nama}` ``, angka ilmiah `1e3`, `2.5e-2`.
- **`*args` / `**kwargs` dan spread:** `def f(*a, **kw)`, `f(*xs, **d)`, `[*a, *b]`, `{**d1, **d2}`.
- **Himpunan & bit:** `{1, 2}`, `a | b`, `a & b`, `a - b`, `a ^ b` (himpunan = larik tanpa duplikat; untuk angka: operator bit), `~x`, `<<`, `>>`. Literal `0x1F`, `0b101`, `0o17`, `1_000`, `.5`.
- **f-string dengan format spec:** `f"{x:.2f}"`, `f"{n:>8,}"`, `f"{v!r}"`.
- **Dekorator:** `@dec`, `@dec(arg)` untuk fungsi dan kelas; di dalam kelas `@staticmethod`, `@classmethod`, `@property` + `@x.setter`; atribut kelas (`total = 0`, `name: str = "x"`); `@dataclass` (`order=True`, `field(default_factory=list)`, pewarisan).
- **Generator:** `yield`, `yield from`, `x = yield v`, `gen.send/throw_/close`, ekspresi generator `(x * x for x in xs)`, `next(g, default)`, `iter(x)`; `for`, `list()`, `sum()`, `sorted()`, ... membacanya secara lazy. Di VM generator = frame yang disimpan/dilanjutkan (tanpa thread, ~1,7 µs per `yield`).
- **Eksepsi ala Python:** hierarki `BaseException > Exception > ValueError/TypeError/KeyError/IndexError/ZeroDivisionError/OSError/...`; `raise ValueError("x")`, `raise Kelas`, `raise` (lempar ulang), `except (A, B) as e:` berjenjang, `except:`, `else:`, `finally:`, kelas sendiri `class MyErr(Exception)`. Error bawaan (`1/0`, indeks di luar batas, ...) bisa ditangkap dengan kelasnya. `raise "teks"` gaya lama tetap jalan.
- **Protokol objek:** `__str__`, `__repr__`, `__eq__`, `__lt__/__le__/__gt__/__ge__`, `__add__/__sub__/__mul__/__truediv__/__mod__`, `__len__`, `__bool__`, `__getitem__`, `__contains__`, `__iter__`/`__next__`, `__call__`, `__enter__`/`__exit__` (`with` penuh, termasuk penelan error), `getattr/setattr/hasattr/vars/dir/type(x).__name__/isinstance/issubclass`.
- **Lain-lain:** default mutabel dibagi seperti Python (`def f(x, acc=[])`), alias di badan kelas (`baca = tulis`), `import a.b` mengikat `a`, `Enum` dengan `.name/.value/Color(1)/Color['RED']/for c in Color`, `KeyError`, `async def` / `await` / `asyncio.run/gather/create_task/sleep/wait_for`, `fractions.Fraction`.
- **Anotasi tipe Python:** `x: List[int] = []`, `def f(a: Dict[str, int]) -> Optional[str]:` diterima dan diabaikan (tipe polos seperti `int`/`str` tetap diperiksa).
- **`try/finally`** tanpa `except`, `with` beberapa manajer, `yield` di dalam `try`.
- **Impor:** `import math, json`, `import a.b`, `from modul import nama as alias`.
- **File:** `open(path, "r"|"w"|"a")` dengan `read()`, `readline()`, `readlines()`, `write()`, `close()`, dan `with open(p) as f:` yang menutup file otomatis.
- **Metode bawaan** — teks: `split join strip lstrip rstrip replace startswith endswith find count upper lower title capitalize isdigit isalpha zfill center ljust rjust format ...`; larik: `append extend insert remove pop index count sort reverse copy clear`; peta: `keys values items get setdefault pop update copy clear`.

## OOP

```python
class Hewan:
    def __init__(self, nama, suara="..."):
        self.nama = nama
        self.suara = suara
    def bicara(self):
        print(f"{self.nama} {self.suara}")

class Anjing(Hewan):
    def bicara(self):
        print(self.nama + " menggonggong")

Anjing("Rex").bicara()
Hewan(suara="mbek", nama="Kambing").bicara()
```

Versi Indonesia (struktur sama):

```
kelas Hewan:
    fungsi konstruktor(ini, nama):
        ini.nama = nama
    fungsi bicara(ini):
        cetak(ini.nama + " bersuara")

kelas Anjing(Hewan):
    fungsi bicara(ini):
        cetak(ini.nama + " menggonggong")

Anjing("Rex").bicara()
```

Plus `bentuk` (struct data polos), `jenis` (enum), exception (`try`/`except`/`finally`/`raise` — boleh melempar nilai apa saja), dan fungsi anonim/lambda.

## Pustaka standar

Modul bergaya Python, tertanam di binary (tanpa instalasi): `import math` ...

| Modul | Isi |
|---|---|
| `math` | `sqrt sin cos tan atan2 exp log log2 log10 floor ceil trunc gcd lcm factorial comb prod hypot isnan isclose pi e tau inf` |
| `json` | `dumps(obj, indent=2, sort_keys=True)`, `loads`, `dump(obj, f)`, `load(f)` |
| `os` | `getcwd chdir listdir walk scandir mkdir makedirs remove rename system getenv environ cpu_count stat`, `os.path.join/exists/isfile/isdir/basename/dirname/splitext/abspath/getsize/getmtime/normpath/relpath` |
| `sys` | `argv exit platform version stdout.write stderr.write stdin.read` |
| `time` | `time sleep monotonic perf_counter strftime localtime` |
| `datetime` | `datetime.now()` → `year month day hour minute second strftime isoformat timestamp` |
| `random` | `random randint uniform choice shuffle sample seed gauss randrange` |
| `re` | `search match fullmatch findall finditer sub split compile` (objek `Match`: `group groups groupdict start end span`) |
| `http` | klien HTTP(S) seperti `requests` (di bawah) |
| `subprocess` | `run(args)` → `returncode stdout stderr`, `check_output` |
| `hashlib`, `base64`, `string`, `copy` | `sha256(x).hexdigest()`, `b64encode/b64decode`, konstanta huruf, `deepcopy` |
| `collections` | `Counter` (`most_common`, `update`, `total`), `defaultdict`, `deque(maxlen=)`, `namedtuple`, `OrderedDict` |
| `functools` | `reduce partial lru_cache cache wraps cmp_to_key` |
| `itertools` | `count cycle repeat chain islice takewhile dropwhile accumulate groupby product permutations combinations zip_longest pairwise batched tee starmap compress` (lazy) |
| `heapq`, `bisect`, `operator`, `statistics` | `heappush/heappop/heapify/nlargest`, `bisect_left/right insort`, `itemgetter add ...`, `mean median mode stdev variance` |
| `pathlib`, `glob`, `fnmatch`, `shutil`, `tempfile` | `Path("a") / "b"` (`read_text write_text exists mkdir iterdir glob suffix stem parent ...`), `glob(pat, recursive=True)`, `copy copytree move rmtree which`, `TemporaryDirectory` |
| `csv`, `io`, `textwrap`, `pprint`, `uuid`, `secrets`, `platform` | `reader/writer/DictReader/DictWriter`, `StringIO`, `wrap fill dedent shorten`, `uuid4()`, `token_hex` |
| `argparse`, `logging`, `unittest` | `ArgumentParser` (`add_argument`, `nargs`, `action`, subparser), `getLogger/basicConfig/info/...`, `TestCase` + `unittest.main([Kelas])` |
| `threading`, `queue`, `contextlib`, `dataclasses`, `typing`, `enum`, `abc` | `Thread Lock Event Timer`, `Queue`, `@contextmanager suppress`, `field asdict replace`, tipe petunjuk (no-op), `Enum` (anggota = nilai kelas), `ABC` |
| `urllib.parse` | `quote unquote urlencode urlparse parse_qs urljoin` |

```python
import http
r = http.get("https://example.com", params={"q": "nusa"}, timeout=10)
print(r.status, r.ok, r.headers["content-type"], len(r.text))

r = http.post("https://httpbin.org/post", json={"nama": "Rex"})   # juga data={...}, headers={...}
print(r.json()["json"])
```

`Response` punya `status`/`status_code`, `ok`, `headers` (kunci huruf kecil), `text`, `json()`, `raise_for_status()`. Metode: `get head post put patch delete request`.

```python
import re, os, sys
for nama in os.listdir("."):
    m = re.match(r"(\w+)\.ns$", nama)
    if m:
        print(m.group(1))
print(sys.argv[1:])
```

## Concurrency

Goroutine (`go`/`jalan`) dan channel (`channel`/`kanal_baru`, `chan_send`, `chan_recv`, `select`), `wg_*` (WaitGroup), di atas satu GIL yang dilepas otomatis saat menunggu I/O, `sleep`, atau memanggil plugin — cocok untuk kerjaan network/background, bukan paralel CPU murni.

```python
c = channel()
for i in range(5):
    go(lambda n: chan_send(c, n * n), i)
print(sum(chan_recv(c) for _ in range(5)))   # 30
```

`latar()` di dalam goroutine menandainya sebagai pekerja latar: proses boleh selesai walau goroutine itu masih jalan (dipakai pendengar event modul Go).

## Builtin

- Python: `abs min max sum round pow divmod chr ord hex bin oct bool int float str list tuple set dict sorted reversed enumerate zip map filter any all isinstance callable len range print input open type`
- Jaringan: `http_get`/`http_post` (HTTPS beneran), `tcp_konek`/`tcp_kirim`/`tcp_terima`, `http_dengar` (server), `email_kirim`
- Data: `json_encode`/`json_decode`, `base64_encode`/`decode`, `sha256_hex`, `hmac_sha256_hex`, `jwt_buat`/`jwt_verifikasi`
- Sistem: `baca_file`/`tulis_file`/`file_ada`, `jalankan_perintah` (argv-safe, tanpa shell), `proc_stream_*`, `ambil_env`, `waktu`, `tidur`
- `qr_baca` (plugin `qr`), `impor()`, `muat_plugin()`, `gc_paksa()`

Daftar lengkap + signature ada di `src/interpreter.cpp` (`builtinNames`/`callBuiltin`) dan `src/pylib.cpp`.

## Modul & plugin

- `import nama` mencari `nusantara_modules/nama/index.ns` (atau `nama.ns`, paket dengan `__init__.ns`) relatif ke folder kerja dulu, baru pustaka standar di atas. `nusa get` memasangnya dari git.
- **Modul sistem `muat_plugin("nama")`:**
  - **Tertanam di binary** (tanpa file `.so`): `http` (klien HTTP/1.1 + TLS, redirect, streaming, proxy), `ws` (WebSocket ws/wss), `crypto`, `audio`, `gambar`.
  - **Plugin `.so`** yang ikut terbangun oleh `make`: `sqlite`, `js` (QuickJS), `qr` (quirc + stb_image). Kode C vendor hanya ada di `plugins/<nama>/vendor/`.
  - Path eksplisit `muat_plugin("./x.so")` memuat plugin buatanmu (ABI di `include/plugin_abi.h`, contoh `plugins/hello`).

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
                pylib.cpp       builtin & metode gaya Python, format string
                pystd.cpp       native di balik math/json/os/sys/time/random/re
                stdlib.cpp      modul inti (math json sys time ...) + kelas Exception (`__exc`) + runtime generator (`__gen`)
                stdlib_embed.cpp  GENERATED dari lib/*.ns (itertools, pathlib, csv, argparse, ...)
                update.cpp      `nusa update`
                plugin.cpp      pemuat plugin .so + registri modul tertanam
                sysmod.cpp      pemanggil plugin sistem opsional (js, qr)
                gobridge*.cpp   `nusa go add` (+ sumber jembatan Go yang ditanam)
lib/            pustaka standar bergaya Python ditulis dalam NSL (satu file per modul; `import a.b` -> lib/a_b.ns)
plugins/        plugin .so: sqlite, js (QuickJS), qr (quirc+stb), hello (contoh); vendor C di sini
tools/          gobridge/ (bridge.go = runtime jembatan Go, gen/main.go = generator registri),
                embed_gobridge.py, embed_lib.py, gen_ca_bundle.py
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

`tests/run.sh` juga mengecek `src/gobridge_embed.cpp` tidak usang. Setelah mengubah `tools/gobridge/*`: `python3 tools/embed_gobridge.py > src/gobridge_embed.cpp`; setelah mengubah `lib/*.ns`: `python3 tools/embed_lib.py > src/stdlib_embed.cpp`.

## Batasan

- `angka` selalu `double` (bilangan bulat tepat sampai 2^53); tuple = larik; `set` = larik tanpa duplikat.
- Satu GIL: goroutine tidak paralel untuk CPU murni.
- Beda dari Python: `d[k]` pada kunci yang tidak ada hanya melempar `KeyError` di file bergaya Python (di gaya `{ }` tetap `None`; `d.k` titik selalu `None`); `Enum` tidak mendukung `Flag` bitwise (`|`) dan alias nilai; `yield from` hanya sebagai pernyataan; generator yang dibuang tidak menjalankan `finally`-nya; `asyncio` memakai goroutine (tugas yang di-`wait_for` timeout tetap selesai di latar); `decimal`, `struct`, `async for` pada async generator belum ada.
- Teks disimpan sebagai UTF-8; `len`, indeks, slice, `find`, `upper/lower` (Latin, Yunani, Sirilik) menghitung karakter seperti Python. `title()`/`isalpha()` dan sejenisnya hanya paham ASCII.
- Pembagian dengan nol melempar `ZeroDivisionError` di VM dan interpreter, tetapi tidak di loop numerik hasil JIT.
- Modul Go: lihat batasannya di bagian "Modul Go".

## Lisensi

Lihat [NOTICE](NOTICE).
