class Animal {
  constructor(name) {
    this.name = name;
  }
  speak() {
    return `${this.name} makes a noise.`;
  }
  get label() {
    return `[${this.name}]`;
  }
}

class Dog extends Animal {
  constructor(name, breed) {
    super(name);
    this.breed = breed;
  }
  speak() {
    return `${this.name} barks. (${super.speak()})`;
  }
  static fromJson(json) {
    const data = JSON.parse(json);
    return new Dog(data.name, data.breed);
  }
}

const d = new Dog("Rex", "Husky");
console.log(d.speak());
console.log(d.label);
console.log(d instanceof Animal, d instanceof Dog);

const d2 = Dog.fromJson('{"name":"Fido","breed":"Beagle"}');
console.log(d2.speak());
