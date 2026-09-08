const obj = { z: 1, a: 2, m: 3 };
console.log("Object.keys:", Object.keys(obj));
console.log("Object.values:", Object.values(obj));
console.log("Object.entries:", JSON.stringify(Object.entries(obj)));

const arr = [5, 3, 8, 1, 9];
console.log("map/filter/sort:", arr.map(n => n * 2).filter(n => n > 5).sort((a, b) => a - b));
console.log("Math:", Math.max(1, 9, 3), Math.min(1, 9, 3), Math.round(3.6), Math.floor(3.9));

console.log("String methods:", "  Hello World  ".trim().toUpperCase(), "abc".repeat(3), "a,b,c".split(","));

const m = new Map();
m.set("k1", 1).set("k2", 2);
console.log("Map:", m.size, m.get("k1"), [...m.keys()]);

const s = new Set([1, 2, 2, 3, 3, 3]);
console.log("Set:", s.size, [...s]);

const parsed = JSON.parse('{"nested":{"list":[1,2,3]},"ok":true}');
console.log("JSON parse/stringify roundtrip:", JSON.stringify(parsed));
