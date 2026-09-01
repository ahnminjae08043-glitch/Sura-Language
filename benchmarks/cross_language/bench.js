const REPS = 5;
let sink = 0.0;
function fib(n) { return n <= 1 ? n : fib(n-1) + fib(n-2); }
function numericLoop(n) { let acc = 0.0, i = 1; while (i <= n) { acc += (i*3-1)/2; i += 1; } return acc; }
function arrayWork(n) {
  const a = []; let i = 0;
  while (i < n) { a.push(i * 0.5); i += 1; }
  let total = 0.0, j = 0;
  while (j < n) { total += a[j]; j += 1; }
  return total;
}
function stringWork(n) {
  const parts = []; let i = 0;
  while (i < n) { parts.push("item"); i += 1; }
  return parts.join(",").length;
}
function dictWork(n) {
  const counts = new Map(); let i = 0;
  while (i < n) { const k = "k" + (i % 50000); counts.set(k, (counts.get(k) || 0) + 1); i += 1; }
  return counts.size;
}
function sortWork(n) {
  const a = []; let seed = 12345, i = 0;
  while (i < n) { seed = (seed * 1103515245 + 12345) % 2147483648; a.push(seed % 1000000); i += 1; }
  a.sort((x, y) => x - y);
  return a[0] + a[n-1];
}
class Point { constructor(x, y) { this.x = x; this.y = y; } }
function objectWork(n) {
  let total = 0, i = 0;
  while (i < n) { const p = new Point(i, i+1); p.x = p.x + 1; total += p.x + p.y; i += 1; }
  return total;
}
function matmulWork(n) {
  const a = [], b = [];
  for (let i = 0; i < n; i++) {
    const ra = [], rb = [];
    for (let j = 0; j < n; j++) { const idx = i*n+j; ra.push((idx%7)*0.5); rb.push((idx%5)*0.25); }
    a.push(ra); b.push(rb);
  }
  const c = []; for (let i = 0; i < n; i++) c.push(new Array(n).fill(0.0));
  for (let r = 0; r < n; r++)
    for (let k = 0; k < n; k++) {
      const av = a[r][k], brow = b[k], crow = c[r];
      for (let q = 0; q < n; q++) crow[q] += av * brow[q];
    }
  return c[0][0];
}
function measure(name, fn, n) {
  let best = Infinity;
  for (let i = 0; i < REPS; i++) {
    const t0 = process.hrtime.bigint(); sink += fn(n + i);
    const dt = Number(process.hrtime.bigint() - t0) / 1e6;
    if (dt < best) best = dt;
  }
  console.log(`${name}=${best}`);
}
for (let w = 0; w < 60; w++) { fib(12); numericLoop(200); arrayWork(200); stringWork(200); dictWork(200); sortWork(200); objectWork(200); matmulWork(12); }
measure("fib", fib, 30);
measure("numeric", numericLoop, 3000000);
measure("array", arrayWork, 1000000);
measure("string", stringWork, 200000);
measure("dict", dictWork, 200000);
measure("sort", sortWork, 300000);
measure("object", objectWork, 500000);
measure("matmul", matmulWork, 256);
console.log(`checksum=${sink.toFixed(3)}`);
