// Nusantara <-> Go bridge (built as a c-shared plugin by `nusa go add`).
//
// One generic, reflection-based runtime serves every wrapped Go package: the generated
// registry_gen.go only lists what exists (functions, types, constants); this file knows how
// to call them. Values cross the plugin ABI as JSON text:
//
//   - numbers, strings, booleans, null, arrays and objects map to their Go counterparts;
//   - pointers, interfaces, funcs and channels stay on the Go side and are handed out as
//     handles: {"$h": "h12", "tipe": "*pkg.Type", "m": {"Method": <arity>, ...}};
//   - a Go func parameter is filled from {"$cb": "<queue>"}: Go calls are queued as events
//     that the Nusantara side drains (see the generated index.ns).
package main

/*
#include <stdlib.h>
#include <string.h>

typedef enum { NS_NULL = 0, NS_BOOL = 1, NS_NUMBER = 2, NS_STRING = 3 } NsType;
typedef struct { NsType type; int boolean; double number; char* str; int str_len; } NsValue;
typedef NsValue (*NsFn)(int argc, const NsValue* argv);
typedef void (*NsRegisterFn)(void* registry, const char* name, NsFn fn);

extern NsValue nsGoInvoke(int op, int argc, NsValue* argv);

static NsValue t_panggil(int argc, const NsValue* argv) { return nsGoInvoke(0, argc, (NsValue*)argv); }
static NsValue t_buat(int argc, const NsValue* argv) { return nsGoInvoke(1, argc, (NsValue*)argv); }
static NsValue t_field(int argc, const NsValue* argv) { return nsGoInvoke(2, argc, (NsValue*)argv); }
static NsValue t_setfield(int argc, const NsValue* argv) { return nsGoInvoke(3, argc, (NsValue*)argv); }
static NsValue t_konst(int argc, const NsValue* argv) { return nsGoInvoke(4, argc, (NsValue*)argv); }
static NsValue t_event(int argc, const NsValue* argv) { return nsGoInvoke(5, argc, (NsValue*)argv); }
static NsValue t_daftar(int argc, const NsValue* argv) { return nsGoInvoke(6, argc, (NsValue*)argv); }
static NsValue t_bebas(int argc, const NsValue* argv) { return nsGoInvoke(7, argc, (NsValue*)argv); }

static void register_all(void* r, NsRegisterFn reg) {
    reg(r, "panggil", t_panggil);
    reg(r, "buat", t_buat);
    reg(r, "field", t_field);
    reg(r, "set_field", t_setfield);
    reg(r, "konst", t_konst);
    reg(r, "event", t_event);
    reg(r, "daftar", t_daftar);
    reg(r, "bebas", t_bebas);
}

static NsValue make_string(const char* s, int n) {
    NsValue v;
    memset(&v, 0, sizeof v);
    v.type = NS_STRING;
    v.str = (char*)malloc((size_t)n + 1);
    memcpy(v.str, s, (size_t)n);
    v.str[n] = 0;
    v.str_len = n;
    return v;
}
static NsValue make_null(void) { NsValue v; memset(&v, 0, sizeof v); return v; }
*/
import "C"

import (
	"context"
	"encoding"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"reflect"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
	"unsafe"
)

//export ns_plugin_init
func ns_plugin_init(registry unsafe.Pointer, reg C.NsRegisterFn) { C.register_all(registry, reg) }

//export nusa_abi_version
func nusa_abi_version() C.int { return 2 }

func main() {}

// ---------------------------------------------------------------- handles ----

var (
	hmu     sync.Mutex
	handles = map[string]reflect.Value{}
	hnext   int
)

func newHandle(v reflect.Value) string {
	hmu.Lock()
	defer hmu.Unlock()
	hnext++
	id := "h" + strconv.Itoa(hnext)
	handles[id] = v
	return id
}

func lookupHandle(id string) (reflect.Value, bool) {
	hmu.Lock()
	defer hmu.Unlock()
	v, ok := handles[id]
	return v, ok
}

var methodCache sync.Map // reflect.Type -> map[string]int

func methodTable(t reflect.Type) map[string]int {
	if m, ok := methodCache.Load(t); ok {
		return m.(map[string]int)
	}
	out := map[string]int{}
	for i := 0; i < t.NumMethod(); i++ {
		m := t.Method(i)
		if t.Kind() == reflect.Interface {
			out[m.Name] = m.Type.NumIn()
		} else {
			out[m.Name] = m.Type.NumIn() - 1 // drop the receiver
		}
	}
	// A struct value's pointer-receiver methods are reachable through a pointer copy.
	if t.Kind() != reflect.Ptr && t.Kind() != reflect.Interface {
		pt := reflect.PtrTo(t)
		for i := 0; i < pt.NumMethod(); i++ {
			m := pt.Method(i)
			if _, dup := out[m.Name]; !dup {
				out[m.Name] = m.Type.NumIn() - 1
			}
		}
	}
	methodCache.Store(t, out)
	return out
}

func hasMethods(t reflect.Type) bool {
	if t.NumMethod() > 0 {
		return true
	}
	return t.Kind() != reflect.Ptr && t.Kind() != reflect.Interface && reflect.PtrTo(t).NumMethod() > 0
}

// fieldSnapshot: the exported scalar fields of a struct (or pointer to one), so Nusantara code can
// read `obj.Name` directly. It is a snapshot; obj.field("Name") always reads the live value.
func fieldSnapshot(v reflect.Value) map[string]interface{} {
	e := indirectValue(v)
	if e.Kind() != reflect.Struct {
		return nil
	}
	m := map[string]interface{}{}
	t := e.Type()
	for i := 0; i < t.NumField(); i++ {
		sf := t.Field(i)
		if !sf.IsExported() {
			continue
		}
		fv := e.Field(i)
		switch fv.Kind() {
		case reflect.Bool, reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64,
			reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Float32,
			reflect.Float64, reflect.String:
			m[sf.Name] = encodeOpt(fv, false)
		}
	}
	return m
}

// handleRef stores v and describes it: "m" = its methods (name -> arity), "f" = scalar field snapshot,
// "d" = its data view when it isn't a pointer ("nilai" on the Nusantara side).
func handleRef(v reflect.Value) map[string]interface{} {
	ref := map[string]interface{}{"$h": newHandle(v), "tipe": v.Type().String(), "m": methodTable(v.Type())}
	if f := fieldSnapshot(v); len(f) > 0 {
		ref["f"] = f
	}
	if k := v.Kind(); k != reflect.Ptr && k != reflect.Func && k != reflect.Chan && k != reflect.Interface {
		ref["d"] = encodeOpt(v, false)
	}
	return ref
}

// ----------------------------------------------------------------- encode ----

const maxSafeInt = 1 << 53

// encode turns a Go value into something encoding/json can marshal for the Nusantara side.
func encode(v reflect.Value) interface{} { return encodeOpt(v, true) }

// encodeOpt: with methodsAsHandle, a value whose type has methods (time.Time, url.Values, ...)
// becomes a handle so those methods stay callable; without it, just its data.
func encodeOpt(v reflect.Value, methodsAsHandle bool) interface{} {
	if !v.IsValid() {
		return nil
	}
	if !v.CanInterface() {
		return nil
	}
	if methodsAsHandle {
		switch v.Kind() {
		case reflect.Interface, reflect.Ptr, reflect.Func, reflect.Chan:
		default:
			if hasMethods(v.Type()) {
				return handleRef(v)
			}
		}
	}
	switch v.Kind() {
	case reflect.Bool:
		return v.Bool()
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		n := v.Int()
		if n > maxSafeInt || n < -maxSafeInt {
			return strconv.FormatInt(n, 10)
		}
		return n
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		n := v.Uint()
		if n > maxSafeInt {
			return strconv.FormatUint(n, 10)
		}
		return n
	case reflect.Float32, reflect.Float64:
		f := v.Float()
		if math.IsNaN(f) || math.IsInf(f, 0) {
			return nil
		}
		return f
	case reflect.String:
		return v.String()
	case reflect.Slice, reflect.Array:
		if v.Kind() == reflect.Slice && v.IsNil() {
			return []interface{}{}
		}
		if v.Type().Elem().Kind() == reflect.Uint8 {
			b := make([]byte, v.Len())
			reflect.Copy(reflect.ValueOf(b), v)
			return string(b)
		}
		out := make([]interface{}, v.Len())
		for i := range out {
			out[i] = encode(v.Index(i))
		}
		return out
	case reflect.Map:
		if v.IsNil() {
			return map[string]interface{}{}
		}
		out := make(map[string]interface{}, v.Len())
		iter := v.MapRange()
		for iter.Next() {
			out[fmt.Sprint(iter.Key().Interface())] = encode(iter.Value())
		}
		return out
	case reflect.Struct:
		if b, err := json.Marshal(v.Interface()); err == nil {
			return json.RawMessage(b)
		}
		p := reflect.New(v.Type())
		p.Elem().Set(v)
		return handleRef(p)
	case reflect.Ptr:
		if v.IsNil() {
			return nil
		}
		switch v.Elem().Kind() {
		case reflect.Bool, reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64,
			reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Float32,
			reflect.Float64, reflect.String:
			return encode(v.Elem())
		}
		return handleRef(v)
	case reflect.Interface:
		if v.IsNil() {
			return nil
		}
		return encode(v.Elem())
	default: // func, chan, unsafe pointer, complex
		if v.Kind() == reflect.Func || v.Kind() == reflect.Chan {
			if v.IsNil() {
				return nil
			}
		}
		return handleRef(v)
	}
}

// ---------------------------------------------------------------- decode ----

var (
	ctxType         = reflect.TypeOf((*context.Context)(nil)).Elem()
	errType         = reflect.TypeOf((*error)(nil)).Elem()
	textUnmarshaler = reflect.TypeOf((*encoding.TextUnmarshaler)(nil)).Elem()
)

func decodeError(t reflect.Type, why string) error {
	return fmt.Errorf("argumen untuk tipe %s: %s", t, why)
}

// convertArg builds a Go value of type t from the JSON the Nusantara side sent.
func convertArg(raw json.RawMessage, t reflect.Type) (reflect.Value, error) {
	trimmed := strings.TrimSpace(string(raw))
	if trimmed == "" || trimmed == "null" {
		if t == ctxType {
			return reflect.ValueOf(context.Background()), nil
		}
		return reflect.Zero(t), nil
	}
	// handle reference / callback marker
	if strings.HasPrefix(trimmed, "{") {
		var marker struct {
			H  string `json:"$h"`
			Cb string `json:"$cb"`
		}
		if err := json.Unmarshal(raw, &marker); err == nil {
			if marker.H != "" {
				hv, ok := lookupHandle(marker.H)
				if !ok {
					return reflect.Value{}, fmt.Errorf("handle %s sudah dibebaskan atau tidak ada", marker.H)
				}
				return adaptHandle(hv, t)
			}
			if marker.Cb != "" {
				if t.Kind() != reflect.Func {
					return reflect.Value{}, decodeError(t, "callback hanya bisa dikirim ke parameter bertipe fungsi")
				}
				return makeCallback(t, marker.Cb), nil
			}
		}
	}
	if t.Kind() == reflect.Slice && t.Elem().Kind() == reflect.Uint8 && strings.HasPrefix(trimmed, "\"") {
		var s string
		if err := json.Unmarshal(raw, &s); err != nil {
			return reflect.Value{}, err
		}
		return reflect.ValueOf([]byte(s)).Convert(t), nil
	}
	if strings.HasPrefix(trimmed, "\"") && reflect.PtrTo(t).Implements(textUnmarshaler) && t.Kind() != reflect.String {
		var s string
		if err := json.Unmarshal(raw, &s); err != nil {
			return reflect.Value{}, err
		}
		p := reflect.New(t)
		if err := p.Interface().(encoding.TextUnmarshaler).UnmarshalText([]byte(s)); err != nil {
			return reflect.Value{}, err
		}
		return p.Elem(), nil
	}
	if t.Kind() == reflect.Interface {
		if t.NumMethod() == 0 {
			var generic interface{}
			if err := json.Unmarshal(raw, &generic); err != nil {
				return reflect.Value{}, err
			}
			return reflect.ValueOf(generic), nil
		}
		return reflect.Value{}, decodeError(t, "butuh handle objek Go, bukan data JSON")
	}
	p := reflect.New(t)
	if err := json.Unmarshal(raw, p.Interface()); err != nil {
		return reflect.Value{}, decodeError(t, err.Error())
	}
	return p.Elem(), nil
}

// adaptHandle fits a stored value to a parameter type (dereferencing or taking an address as needed).
func adaptHandle(hv reflect.Value, t reflect.Type) (reflect.Value, error) {
	if hv.Type().AssignableTo(t) {
		return hv, nil
	}
	if hv.Kind() == reflect.Ptr && !hv.IsNil() && hv.Elem().Type().AssignableTo(t) {
		return hv.Elem(), nil
	}
	if t.Kind() == reflect.Ptr && hv.Type().AssignableTo(t.Elem()) {
		p := reflect.New(t.Elem())
		p.Elem().Set(hv)
		return p, nil
	}
	if hv.Type().ConvertibleTo(t) && hv.Kind() != reflect.Ptr && t.Kind() != reflect.Interface {
		return hv.Convert(t), nil
	}
	return reflect.Value{}, fmt.Errorf("handle bertipe %s tidak cocok untuk parameter bertipe %s", hv.Type(), t)
}

// ------------------------------------------------------------- callbacks ----

type eventQueue struct{ ch chan string }

var (
	qmu    sync.Mutex
	queues = map[string]*eventQueue{}
)

func queueFor(name string) *eventQueue {
	qmu.Lock()
	defer qmu.Unlock()
	q, ok := queues[name]
	if !ok {
		q = &eventQueue{ch: make(chan string, 8192)}
		queues[name] = q
	}
	return q
}

func (q *eventQueue) push(s string) {
	for {
		select {
		case q.ch <- s:
			return
		default:
			select { // full: drop the oldest event rather than block Go's own goroutines
			case <-q.ch:
			default:
			}
		}
	}
}

// makeCallback builds a Go func of type t that turns each call into a queued event.
func makeCallback(t reflect.Type, queue string) reflect.Value {
	q := queueFor(queue)
	return reflect.MakeFunc(t, func(args []reflect.Value) []reflect.Value {
		enc := make([]interface{}, len(args))
		for i, a := range args {
			enc[i] = encode(a)
		}
		if b, err := json.Marshal(enc); err == nil {
			q.push(string(b))
		}
		outs := make([]reflect.Value, t.NumOut())
		for i := range outs {
			outs[i] = reflect.Zero(t.Out(i))
		}
		return outs
	})
}

// ----------------------------------------------------------------- calls ----

type callResult struct {
	Values []interface{}
	Err    error
}

func invoke(fv reflect.Value, rawArgs []json.RawMessage) (res callResult) {
	defer func() {
		if r := recover(); r != nil {
			res = callResult{Err: fmt.Errorf("panic di Go: %v", r)}
		}
	}()
	t := fv.Type()
	nIn := t.NumIn()
	variadic := t.IsVariadic()
	fixed := nIn
	if variadic {
		fixed = nIn - 1
	}
	if len(rawArgs) < fixed || (!variadic && len(rawArgs) > fixed) {
		return callResult{Err: fmt.Errorf("butuh %d argumen, dapat %d", fixed, len(rawArgs))}
	}
	in := make([]reflect.Value, 0, len(rawArgs))
	for i, raw := range rawArgs {
		var pt reflect.Type
		if i < fixed {
			pt = t.In(i)
		} else {
			pt = t.In(nIn - 1).Elem()
		}
		v, err := convertArg(raw, pt)
		if err != nil {
			return callResult{Err: fmt.Errorf("argumen ke-%d: %w", i+1, err)}
		}
		in = append(in, v)
	}
	var out []reflect.Value
	if variadic {
		out = fv.Call(in)
	} else {
		out = fv.Call(in)
	}
	n := len(out)
	if n > 0 && t.Out(n-1) == errType {
		if e := out[n-1]; !e.IsNil() {
			return callResult{Err: e.Interface().(error)}
		}
		n--
	}
	vals := make([]interface{}, n)
	for i := 0; i < n; i++ {
		vals[i] = encode(out[i])
	}
	return callResult{Values: vals}
}

func envelope(v interface{}, err error) string {
	var m map[string]interface{}
	if err != nil {
		m = map[string]interface{}{"err": err.Error()}
	} else {
		m = map[string]interface{}{"ok": v}
	}
	b, e := json.Marshal(m)
	if e != nil {
		b, _ = json.Marshal(map[string]interface{}{"err": "gagal meng-encode hasil: " + e.Error()})
	}
	return string(b)
}

func resultValue(r callResult) interface{} {
	switch len(r.Values) {
	case 0:
		return nil
	case 1:
		return r.Values[0]
	default:
		return r.Values
	}
}

func parseArgs(argsJSON string) ([]json.RawMessage, error) {
	if strings.TrimSpace(argsJSON) == "" {
		return nil, nil
	}
	var args []json.RawMessage
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return nil, fmt.Errorf("argumen harus larik JSON: %w", err)
	}
	return args, nil
}

func callTarget(target, name, argsJSON string) string {
	args, err := parseArgs(argsJSON)
	if err != nil {
		return envelope(nil, err)
	}
	if target == "" {
		fv, ok := funcs[name]
		if !ok {
			return envelope(nil, fmt.Errorf("fungsi Go '%s' tidak ada", name))
		}
		r := invoke(fv, args)
		return envelope(resultValue(r), r.Err)
	}
	hv, ok := lookupHandle(target)
	if !ok {
		return envelope(nil, fmt.Errorf("handle %s sudah dibebaskan atau tidak ada", target))
	}
	switch name {
	case "$json":
		b, err := json.Marshal(indirectValue(hv).Interface())
		if err != nil {
			return envelope(nil, err)
		}
		return `{"ok":` + string(b) + `}`
	case "$str":
		return envelope(fmt.Sprint(hv.Interface()), nil)
	case "$tipe":
		return envelope(hv.Type().String(), nil)
	}
	mv := hv.MethodByName(name)
	if !mv.IsValid() && hv.Kind() != reflect.Ptr && hv.Kind() != reflect.Interface {
		p := reflect.New(hv.Type())
		p.Elem().Set(hv)
		mv = p.MethodByName(name)
	}
	if !mv.IsValid() {
		return envelope(nil, fmt.Errorf("tipe %s tidak punya metode '%s'", hv.Type(), name))
	}
	r := invoke(mv, args)
	return envelope(resultValue(r), r.Err)
}

func indirectValue(v reflect.Value) reflect.Value {
	for v.Kind() == reflect.Ptr || v.Kind() == reflect.Interface {
		if v.IsNil() {
			break
		}
		v = v.Elem()
	}
	return v
}

func fieldOf(handle, name string) (reflect.Value, error) {
	hv, ok := lookupHandle(handle)
	if !ok {
		return reflect.Value{}, fmt.Errorf("handle %s sudah dibebaskan atau tidak ada", handle)
	}
	e := indirectValue(hv)
	if e.Kind() != reflect.Struct {
		return reflect.Value{}, fmt.Errorf("%s bukan struct, tidak punya field", hv.Type())
	}
	f := e.FieldByName(name)
	if !f.IsValid() || !f.CanInterface() {
		return reflect.Value{}, fmt.Errorf("tipe %s tidak punya field publik '%s'", e.Type(), name)
	}
	return f, nil
}

func createValue(typeName, jsonInit string) string {
	t, ok := typs[typeName]
	if !ok {
		return envelope(nil, fmt.Errorf("tipe Go '%s' tidak ada", typeName))
	}
	p := reflect.New(t)
	if s := strings.TrimSpace(jsonInit); s != "" && s != "null" {
		if err := json.Unmarshal([]byte(jsonInit), p.Interface()); err != nil {
			return envelope(nil, decodeError(t, err.Error()))
		}
	}
	return envelope(handleRef(p), nil)
}

func manifest() string {
	type fn struct {
		In       int  `json:"masuk"`
		Variadic bool `json:"variadic"`
	}
	out := map[string]interface{}{}
	fs := map[string]fn{}
	for name, v := range funcs {
		fs[name] = fn{In: v.Type().NumIn(), Variadic: v.Type().IsVariadic()}
	}
	out["fungsi"] = fs
	tn := make([]string, 0, len(typs))
	for n := range typs {
		tn = append(tn, n)
	}
	sort.Strings(tn)
	out["tipe"] = tn
	cn := make([]string, 0, len(consts))
	for n := range consts {
		cn = append(cn, n)
	}
	sort.Strings(cn)
	out["konstanta"] = cn
	b, _ := json.Marshal(out)
	return string(b)
}

// ------------------------------------------------------------ ABI entry ----

func argString(argv []C.NsValue, i int) string {
	if i >= len(argv) || nsType(&argv[i]) != C.NS_STRING {
		return ""
	}
	n := int(argv[i].str_len)
	if n < 0 {
		return C.GoString(argv[i].str)
	}
	return C.GoStringN(argv[i].str, C.int(n))
}

func nsType(v *C.NsValue) C.NsType { return v._type }

func retString(s string) C.NsValue {
	cs := C.CString(s)
	defer C.free(unsafe.Pointer(cs))
	return C.make_string(cs, C.int(len(s)))
}

//export nsGoInvoke
func nsGoInvoke(op C.int, argc C.int, argv *C.NsValue) (ret C.NsValue) {
	defer func() {
		if r := recover(); r != nil {
			ret = retString(envelope(nil, fmt.Errorf("panic di jembatan Go: %v", r)))
		}
	}()
	var args []C.NsValue
	if argc > 0 {
		args = unsafe.Slice(argv, int(argc))
	}
	switch op {
	case 0:
		return retString(callTarget(argString(args, 0), argString(args, 1), argString(args, 2)))
	case 1:
		return retString(createValue(argString(args, 0), argString(args, 1)))
	case 2:
		f, err := fieldOf(argString(args, 0), argString(args, 1))
		if err != nil {
			return retString(envelope(nil, err))
		}
		return retString(envelope(encode(f), nil))
	case 3:
		f, err := fieldOf(argString(args, 0), argString(args, 1))
		if err != nil {
			return retString(envelope(nil, err))
		}
		if !f.CanSet() {
			return retString(envelope(nil, errors.New("field tidak bisa diubah")))
		}
		v, err := convertArg(json.RawMessage(argString(args, 2)), f.Type())
		if err != nil {
			return retString(envelope(nil, err))
		}
		f.Set(v)
		return retString(envelope(nil, nil))
	case 4:
		name := argString(args, 0)
		if v, ok := consts[name]; ok {
			return retString(envelope(encode(reflect.ValueOf(v)), nil))
		}
		if get, ok := vars[name]; ok {
			return retString(envelope(encode(get()), nil))
		}
		return retString(envelope(nil, fmt.Errorf("konstanta/variabel Go '%s' tidak ada", name)))
	case 5:
		q := queueFor(argString(args, 0))
		ms := 0
		if len(args) > 1 && nsType(&args[1]) == C.NS_NUMBER {
			ms = int(args[1].number)
		}
		select {
		case s := <-q.ch:
			return retString(s)
		case <-time.After(time.Duration(ms) * time.Millisecond):
			return retString("")
		}
	case 6:
		return retString(manifest())
	case 7:
		hmu.Lock()
		delete(handles, argString(args, 0))
		hmu.Unlock()
		return C.make_null()
	}
	return C.make_null()
}
