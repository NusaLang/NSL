function delay(ms, value) {
  return new Promise(resolve => {
    resolve(value);
  });
}

async function run() {
  console.log("start");
  const a = await delay(0, "A");
  const b = await delay(0, "B");
  console.log("got", a, b);

  try {
    await Promise.reject(new Error("boom"));
  } catch (e) {
    console.log("caught:", e.message);
  }

  const all = await Promise.all([delay(0, 1), delay(0, 2), delay(0, 3)]);
  console.log("Promise.all:", all);

  console.log("end");
}

run().then(() => console.log("run() finished"));
