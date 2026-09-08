function boom() {
  throw new Error("deliberate failure");
}
boom();
