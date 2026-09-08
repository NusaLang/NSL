// arrow functions, let/const, template literals, destructuring, spread/rest
const add = (a, b) => a + b;
console.log("add:", add(2, 3));

const name = "Nusantara";
const version = 1.4;
console.log(`hello ${name} v${version}, 2+2=${2 + 2}`);

const [x, y, ...rest] = [1, 2, 3, 4, 5];
console.log("destructure array:", x, y, rest);

const { a, b, ...others } = { a: 1, b: 2, c: 3, d: 4 };
console.log("destructure object:", a, b, others);

function sum(...nums) {
  return nums.reduce((acc, n) => acc + n, 0);
}
console.log("rest params sum:", sum(1, 2, 3, 4, 5));

const arr1 = [1, 2, 3];
const arr2 = [...arr1, 4, 5];
console.log("spread array:", arr2);

const obj1 = { p: 1, q: 2 };
const obj2 = { ...obj1, r: 3 };
console.log("spread object:", JSON.stringify(obj2));
