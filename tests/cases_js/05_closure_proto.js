function makeCounter() {
  let count = 0;
  return {
    inc: () => ++count,
    dec: () => --count,
    value: () => count,
  };
}

const c1 = makeCounter();
c1.inc(); c1.inc(); c1.inc(); c1.dec();
console.log("closure counter:", c1.value());

function Point(x, y) {
  this.x = x;
  this.y = y;
}
Point.prototype.toString = function () {
  return `(${this.x}, ${this.y})`;
};
Point.prototype.dist = function (other) {
  return Math.sqrt((this.x - other.x) ** 2 + (this.y - other.y) ** 2);
};

const p1 = new Point(0, 0);
const p2 = new Point(3, 4);
console.log("prototype toString:", p1.toString(), p2.toString());
console.log("prototype method:", p1.dist(p2));

let error;
try {
  null.foo;
} catch (e) {
  error = e;
}
console.log("error handling:", error instanceof TypeError, error.message.length > 0);
