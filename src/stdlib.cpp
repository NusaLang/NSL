// Python-style standard modules, embedded as NSL source: `import math`, `import http`, ...
// Each binds native builtins (pystd.cpp / pylib.cpp / interpreter builtins) to friendly names.
#include <string>
#include <unordered_map>

#include "pystd.hpp"

namespace pystd {
namespace {

const std::unordered_map<std::string, const char*>& modules() {
    static const std::unordered_map<std::string, const char*> m = {
{"math", R"NSL(
pi = 3.141592653589793
e = 2.718281828459045
tau = 6.283185307179586
inf = 1e308 * 10
nan = inf - inf
sqrt = _math_sqrt
sin = _math_sin
cos = _math_cos
tan = _math_tan
asin = _math_asin
acos = _math_acos
atan = _math_atan
atan2 = _math_atan2
sinh = _math_sinh
cosh = _math_cosh
tanh = _math_tanh
exp = _math_exp
log = _math_log
log2 = _math_log2
log10 = _math_log10
floor = _math_floor
ceil = _math_ceil
trunc = _math_trunc
fabs = _math_fabs
fmod = _math_fmod
hypot = _math_hypot
copysign = _math_copysign
degrees = _math_degrees
radians = _math_radians
gcd = _math_gcd
factorial = _math_factorial
isnan = _math_isnan
isinf = _math_isinf
isfinite = _math_isfinite
isclose = _math_isclose
erf = _math_erf
gamma = _math_gamma
cbrt = _math_cbrt
pow = _pow
def lcm(a, b):
    return abs(a * b) // gcd(a, b) if a != 0 and b != 0 else 0
def prod(xs, start=1):
    r = start
    for x in xs:
        r = r * x
    return r
def comb(n, k):
    return factorial(n) // (factorial(k) * factorial(n - k))
)NSL"},
{"json", R"NSL(
dumps = _json_dumps
loads = json_decode
def dump(obj, f, indent=None):
    f.write(_json_dumps(obj, indent=indent))
def load(f):
    return json_decode(f.read())
)NSL"},
{"os", R"NSL(
getcwd = _os_getcwd
chdir = _os_chdir
listdir = _os_listdir
mkdir = _os_mkdir
makedirs = _os_makedirs
remove = _os_remove
unlink = _os_remove
rmdir = _os_rmdir
rename = _os_rename
system = _os_system
getpid = _os_getpid
cpu_count = _os_cpu_count
environ = _os_environ()
sep = "/"
name = "posix"
linesep = "\n"
def getenv(key, default=None):
    return _os_getenv(key, default)
path = {"join": _os_join, "exists": _os_exists, "isfile": _os_isfile, "isdir": _os_isdir,
        "getsize": _os_getsize, "abspath": _os_abspath, "basename": _os_basename,
        "dirname": _os_dirname, "splitext": _os_splitext, "expanduser": _os_expanduser, "sep": "/"}
)NSL"},
{"sys", R"NSL(
argv = _sys_argv()
platform = _sys_platform()
version = "nusa 0.0.1"
maxsize = 9007199254740991
def exit(code=None):
    _sys_exit(code)
class _Out:
    def __init__(self, err):
        self.err = err
    def write(self, s):
        return _sys_write(s, self.err)
    def flush(self):
        pass
class _In:
    def readline(self):
        line = _readline()
        return "" if line is None else line + "\n"
    def read(self):
        return _stdin_read()
stdout = _Out(False)
stderr = _Out(True)
stdin = _In()
)NSL"},
{"time", R"NSL(
time = waktu
monotonic = _time_monotonic
perf_counter = _time_monotonic
def sleep(seconds):
    tidur(seconds * 1000)
def strftime(fmt, t=None):
    return _time_strftime(fmt, t)
def localtime(t=None):
    return _time_parts(t)
def gmtime(t=None):
    return _time_parts(t, utc=True)
def mktime(t):
    return _time_mktime(t[0], t[1], t[2], t[3], t[4], t[5])
)NSL"},
{"random", R"NSL(
random = _random_random
uniform = _random_uniform
randint = _random_randint
choice = _random_choice
shuffle = _random_shuffle
sample = _random_sample
seed = _random_seed
gauss = _random_gauss
def randrange(a, b=None, step=1):
    if b is None:
        a, b = 0, a
    return a + step * _random_randint(0, (b - a - 1) // step)
)NSL"},
{"re", R"NSL(
IGNORECASE = 2
I = 2
MULTILINE = 8
M = 8
DOTALL = 16
S = 16
class Match:
    def __init__(self, t, string):
        self._g = t[0]
        self._s = t[1]
        self._e = t[2]
        self._n = t[3]
        self.string = string
    def group(self, n=0):
        if isinstance(n, str):
            n = self._n[n]
        return self._g[n]
    def groups(self):
        return self._g[1:]
    def groupdict(self):
        return {k: self._g[v] for k in keys(self._n) for v in [self._n[k]]}
    def start(self, n=0):
        return self._s[n]
    def end(self, n=0):
        return self._e[n]
    def span(self, n=0):
        return (self._s[n], self._e[n])
def _one(pattern, string, flags, mode, pos=0):
    t = _re_exec(pattern, string, flags, mode, pos)
    return None if t is None else Match(t, string)
def search(pattern, string, flags=0):
    return _one(pattern, string, flags, "search")
def match(pattern, string, flags=0):
    return _one(pattern, string, flags, "match")
def fullmatch(pattern, string, flags=0):
    return _one(pattern, string, flags, "fullmatch")
def finditer(pattern, string, flags=0):
    return [Match(t, string) for t in _re_findall(pattern, string, flags)]
def findall(pattern, string, flags=0):
    out = []
    for t in _re_findall(pattern, string, flags):
        g = t[0]
        if len(g) == 1:
            out.append(g[0])
        elif len(g) == 2:
            out.append(g[1])
        else:
            out.append(g[1:])
    return out
def sub(pattern, repl, string, count=0, flags=0):
    return _re_sub(pattern, repl, string, count, flags)
def split(pattern, string, maxsplit=0, flags=0):
    return _re_split(pattern, string, maxsplit, flags)
def escape(s):
    out = ""
    for c in s:
        out = out + ("\\" + c if c in ".^$*+?{}[]\\|()" else c)
    return out
class Pattern:
    def __init__(self, pattern, flags=0):
        self.pattern = pattern
        self.flags = flags
    def search(self, string):
        return search(self.pattern, string, self.flags)
    def match(self, string):
        return match(self.pattern, string, self.flags)
    def fullmatch(self, string):
        return fullmatch(self.pattern, string, self.flags)
    def findall(self, string):
        return findall(self.pattern, string, self.flags)
    def finditer(self, string):
        return finditer(self.pattern, string, self.flags)
    def sub(self, repl, string, count=0):
        return sub(self.pattern, repl, string, count, self.flags)
    def split(self, string, maxsplit=0):
        return split(self.pattern, string, maxsplit, self.flags)
def compile(pattern, flags=0):
    return Pattern(pattern, flags)
)NSL"},
{"string", R"NSL(
ascii_lowercase = "abcdefghijklmnopqrstuvwxyz"
ascii_uppercase = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
ascii_letters = ascii_lowercase + ascii_uppercase
digits = "0123456789"
hexdigits = "0123456789abcdefABCDEF"
octdigits = "01234567"
punctuation = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
whitespace = " \t\n\r"
printable = digits + ascii_letters + punctuation + whitespace
)NSL"},
{"base64", R"NSL(
b64encode = base64_encode
b64decode = base64_decode
)NSL"},
{"hashlib", R"NSL(
class _Hash:
    def __init__(self, data):
        self.data = data
    def update(self, more):
        self.data = self.data + more
    def hexdigest(self):
        return sha256_hex(self.data)
def sha256(data=""):
    return _Hash(data)
)NSL"},
{"copy", R"NSL(
def copy(x):
    if isinstance(x, list):
        return list(x)
    if isinstance(x, dict):
        return dict(x)
    return x
def deepcopy(x):
    if isinstance(x, list):
        return [deepcopy(v) for v in x]
    if isinstance(x, dict):
        return {k: deepcopy(x[k]) for k in x}
    return x
)NSL"},
{"functools", R"NSL(
def reduce(f, xs, init=None):
    it = list(xs)
    if init is None:
        acc = it[0]
        it = it[1:]
    else:
        acc = init
    for x in it:
        acc = f(acc, x)
    return acc
)NSL"},
{"collections", R"NSL(
def Counter(items=None):
    c = {}
    for x in (items or []):
        c[x] = c.get(x, 0) + 1
    return c
OrderedDict = dict
def defaultdict(factory=None):
    return {}
)NSL"},
{"subprocess", R"NSL(
class CompletedProcess:
    def __init__(self, args, returncode, stdout, stderr):
        self.args = args
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr
def run(args, shell=False, capture_output=True, text=True, check=False):
    if isinstance(args, str):
        r = jalankan_perintah("sh", ["-c", args])
    elif shell:
        r = jalankan_perintah("sh", ["-c", " ".join(args)])
    else:
        r = jalankan_perintah(args[0], args[1:])
    p = CompletedProcess(args, r["status"], r["keluaran"], r["error"])
    if check and p.returncode != 0:
        raise "perintah gagal (kode " + str(p.returncode) + "): " + p.stderr
    return p
def check_output(args, shell=False):
    return run(args, shell=shell, check=True).stdout
)NSL"},
{"datetime", R"NSL(
class DateTime:
    def __init__(self, ts, utc=False):
        self.ts = ts
        p = _time_parts(ts, utc=utc)
        self.year = p[0]
        self.month = p[1]
        self.day = p[2]
        self.hour = p[3]
        self.minute = p[4]
        self.second = p[5]
        self.microsecond = p[8]
        self._utc = utc
    def weekday(self):
        return _time_parts(self.ts, utc=self._utc)[6]
    def strftime(self, fmt):
        return _time_strftime(fmt, self.ts, utc=self._utc)
    def isoformat(self):
        return _time_strftime("%Y-%m-%dT%H:%M:%S", self.ts, utc=self._utc)
    def timestamp(self):
        return self.ts
    def date(self):
        return _time_strftime("%Y-%m-%d", self.ts, utc=self._utc)
def _now():
    return DateTime(waktu())
def _utcnow():
    return DateTime(waktu(), True)
def _fromtimestamp(ts):
    return DateTime(ts)
def _new(year, month=1, day=1, hour=0, minute=0, second=0):
    return DateTime(_time_mktime(year, month, day, hour, minute, second))
datetime = {"now": _now, "utcnow": _utcnow, "today": _now, "fromtimestamp": _fromtimestamp, "new": _new}
date = datetime
)NSL"},
{"http", R"NSL(
_plugin = muat_plugin("http")

def urlencode(params):
    parts = []
    for k in params:
        parts.append(quote(str(k)) + "=" + quote(str(params[k])))
    return "&".join(parts)

def quote(s):
    safe = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~"
    out = ""
    for c in str(s):
        if c in safe:
            out = out + c
        else:
            out = out + "%" + ("%02X" % ord(c))
    return out

class Response:
    def __init__(self, status, headers, text):
        self.status = status
        self.status_code = status
        self.headers = headers
        self.text = text
        self.body = text
        self.ok = status >= 200 and status < 400
    def json(self):
        return json_decode(self.text)
    def raise_for_status(self):
        if not self.ok:
            raise "HTTP " + str(self.status)

def request(method, url, headers=None, data=None, json=None, params=None, timeout=None):
    h = headers or {}
    body = ""
    if params:
        url = url + ("&" if "?" in url else "?") + urlencode(params)
    if json is not None:
        body = json_encode(json)
        if "Content-Type" not in h and "content-type" not in h:
            h["Content-Type"] = "application/json"
    elif data is not None:
        if isinstance(data, dict):
            body = urlencode(data)
            if "Content-Type" not in h and "content-type" not in h:
                h["Content-Type"] = "application/x-www-form-urlencoded"
        else:
            body = data
    ms = 0 if timeout is None else timeout * 1000
    r = json_decode(_plugin.minta(method.upper(), "", url, json_encode(h), body, ms))
    if not r["ok"]:
        raise r["error"]
    return Response(r["status"], r["header"], r["tubuh"])

def get(url, headers=None, params=None, timeout=None):
    return request("GET", url, headers=headers, params=params, timeout=timeout)
def head(url, headers=None, params=None, timeout=None):
    return request("HEAD", url, headers=headers, params=params, timeout=timeout)
def post(url, data=None, json=None, headers=None, params=None, timeout=None):
    return request("POST", url, headers=headers, data=data, json=json, params=params, timeout=timeout)
def put(url, data=None, json=None, headers=None, params=None, timeout=None):
    return request("PUT", url, headers=headers, data=data, json=json, params=params, timeout=timeout)
def patch(url, data=None, json=None, headers=None, params=None, timeout=None):
    return request("PATCH", url, headers=headers, data=data, json=json, params=params, timeout=timeout)
def delete(url, headers=None, params=None, timeout=None):
    return request("DELETE", url, headers=headers, params=params, timeout=timeout)
)NSL"},
{"__gen", R"NSL(
# Generator runtime: the body of a `def` with `yield` runs on its own goroutine and hands each
# value over a channel, so it only advances when the consumer asks for the next item.
class Iter:
    def __len__(self):
        r = self._nx()
        if r[0]:
            self._cur = r[1]
            return 9007199254740991
        return 0
    def __getitem__(self, i):
        return self._cur
    def __next__(self):
        r = self._nx()
        if r[0]:
            return r[1]
        raise "StopIteration"
    def __iter__(self):
        return self

class Generator(Iter):
    def __init__(self, body):
        self._body = body
        self._req = None
        self._res = None
        self._st = 0
        self.gi_running = False
    def _start(self):
        self._req = kanal_baru()
        self._res = kanal_baru()
        req = self._req
        res = self._res
        body = self._body
        def y(v):
            kanal_kirim(res, [1, v])
            m = kanal_terima(req)
            if m[0] == 2:
                raise m[1]
            return m[1]
        def run():
            latar()
            m = kanal_terima(req)
            if m[0] == 2:
                kanal_kirim(res, [0, None])
                return
            try:
                r = body(y)
                kanal_kirim(res, [0, r])
            except e:
                kanal_kirim(res, [2, e])
        jalan(run)
    def _resume(self, kind, v):
        if self._st == 3:
            return [False]
        if self._st == 0:
            if kind == 2:
                self._st = 3
                raise v
            self._start()
        self._st = 1
        kanal_kirim(self._req, [kind, v])
        m = kanal_terima(self._res)
        if m[0] == 1:
            return [True, m[1]]
        self._st = 3
        if m[0] == 2:
            raise m[1]
        return [False]
    def _nx(self):
        return self._resume(1, None)
    def send(self, v):
        if self._st == 0 and v is not None:
            raise "TypeError: can't send non-None value to a just-started generator"
        r = self._resume(1, v)
        if r[0]:
            return r[1]
        raise "StopIteration"
    def throw_(self, err):
        r = self._resume(2, err)
        if r[0]:
            return r[1]
        raise "StopIteration"
    def close(self):
        if self._st == 1:
            self._resume(2, "GeneratorExit")
        self._st = 3
    def __next__(self):
        r = self._resume(1, None)
        if r[0]:
            return r[1]
        raise "StopIteration"
    def __iter__(self):
        return self

class VmGenerator(Iter):
    def __init__(self, body):
        self._h = _gennew(body)
    def _nx(self):
        return _genresume(self._h, 1, None)
    def send(self, v):
        r = _genresume(self._h, 1, v)
        if r[0]:
            return r[1]
        raise "StopIteration"
    def throw_(self, err):
        r = _genresume(self._h, 2, err)
        if r[0]:
            return r[1]
        raise "StopIteration"
    def close(self):
        _genclose(self._h)

class ListIter(Iter):
    def __init__(self, items):
        self._items = items
        self._i = 0
    def _nx(self):
        if self._i >= len(self._items):
            return [False]
        v = self._items[self._i]
        self._i = self._i + 1
        return [True, v]
    def __next__(self):
        r = self._nx()
        if r[0]:
            return r[1]
        raise "StopIteration"
    def __iter__(self):
        return self

class CallIter(Iter):
    def __init__(self, fn, sentinel):
        self._fn = fn
        self._sentinel = sentinel
        self._done = False
    def _nx(self):
        if self._done:
            return [False]
        v = self._fn()
        if v == self._sentinel:
            self._done = True
            return [False]
        return [True, v]
    def __next__(self):
        r = self._nx()
        if r[0]:
            return r[1]
        raise "StopIteration"
    def __iter__(self):
        return self

class NextIter(Iter):
    def __init__(self, obj):
        self._obj = obj
    def _nx(self):
        try:
            return [True, self._obj.__next__()]
        except e:
            if "StopIteration" in str(e):
                return [False]
            raise e

def adapt(obj):
    return NextIter(obj)
def mk(body):
    if _isvmgen(body):
        return VmGenerator(body)
    return Generator(body)
def listiter(items):
    return ListIter(items)
def calliter(fn, sentinel):
    return CallIter(fn, sentinel)
)NSL"},
{"__io", R"NSL(
class File:
    def __init__(self, path, mode="r"):
        self.path = path
        self.mode = mode
        self.closed = False
        self._pos = 0
        self._buf = ""
        self._dirty = False
        if "r" in mode or "a" in mode:
            if file_ada(path):
                self._buf = baca_file(path)
            elif "r" in mode:
                raise "FileNotFoundError: " + path
        if "w" in mode:
            self._dirty = True
    def read(self, n=None):
        rest = self._buf[self._pos:]
        if n is not None:
            rest = rest[:n]
        self._pos = self._pos + len(rest)
        return rest
    def readline(self):
        rest = self._buf[self._pos:]
        i = rest.find("\n")
        line = rest if i < 0 else rest[:i + 1]
        self._pos = self._pos + len(line)
        return line
    def readlines(self):
        lines = []
        while self._pos < len(self._buf):
            lines.append(self.readline())
        return lines
    def write(self, s):
        self._buf = self._buf + str(s)
        self._dirty = True
        return len(str(s))
    def writelines(self, lines):
        for l in lines:
            self.write(l)
    def flush(self):
        if self._dirty:
            tulis_file(self.path, self._buf)
            self._dirty = False
    def close(self):
        if not self.closed:
            self.flush()
        self.closed = True
)NSL"},
    };
    return m;
}

}  // namespace

const char* embeddedModule(const std::string& name) {
    auto it = modules().find(name);
    return it == modules().end() ? nullptr : it->second;
}

}  // namespace pystd
