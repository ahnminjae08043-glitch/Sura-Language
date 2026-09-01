use std::collections::HashMap;
use std::time::Instant;
const REPS: usize = 5;
use std::sync::atomic::{AtomicU64, Ordering};
static SINK: AtomicU64 = AtomicU64::new(0);
fn fib(n: i64) -> i64 { if n <= 1 { n } else { fib(n-1) + fib(n-2) } }
fn numeric_loop(n: i64) -> f64 {
    let mut acc = 0.0f64; let mut i = 1i64;
    while i <= n { acc += (i*3-1) as f64 / 2.0; i += 1; }
    acc
}
fn array_work(n: i64) -> f64 {
    let mut a: Vec<f64> = Vec::with_capacity(n as usize);
    let mut i = 0i64; while i < n { a.push(i as f64 * 0.5); i += 1; }
    let mut total = 0.0f64; let mut j = 0i64; while j < n { total += a[j as usize]; j += 1; }
    total
}
fn string_work(n: i64) -> usize {
    let mut parts: Vec<&str> = Vec::with_capacity(n as usize);
    let mut i = 0i64; while i < n { parts.push("item"); i += 1; }
    parts.join(",").len()
}
fn dict_work(n: i64) -> usize {
    let mut counts: HashMap<String, i64> = HashMap::new();
    let mut i = 0i64;
    while i < n { let k = format!("k{}", i % 50000); *counts.entry(k).or_insert(0) += 1; i += 1; }
    counts.len()
}
fn sort_work(n: i64) -> i64 {
    let mut a: Vec<i64> = Vec::with_capacity(n as usize);
    let mut seed: i64 = 12345; let mut i = 0i64;
    while i < n { seed = (seed.wrapping_mul(1103515245) + 12345) % 2147483648; a.push(seed % 1000000); i += 1; }
    a.sort();
    a[0] + a[(n-1) as usize]
}
struct Point { x: f64, y: f64 }
fn object_work(n: i64) -> f64 {
    let mut total = 0.0f64; let mut i = 0i64;
    while i < n {
        let mut p = Box::new(Point { x: i as f64, y: (i+1) as f64 });
        p.x = p.x + 1.0; total += p.x + p.y; i += 1;
    }
    total
}
fn matmul_work(n: usize) -> f64 {
    let mut a = vec![vec![0.0f64; n]; n];
    let mut b = vec![vec![0.0f64; n]; n];
    let mut c = vec![vec![0.0f64; n]; n];
    for i in 0..n { for j in 0..n { let idx = i*n+j; a[i][j] = (idx % 7) as f64 * 0.5; b[i][j] = (idx % 5) as f64 * 0.25; } }
    for r in 0..n {
        for k in 0..n {
            let av = a[r][k];
            for q in 0..n { c[r][q] += av * b[k][q]; }
        }
    }
    c[0][0]
}
fn measure<F: Fn(i64) -> f64>(name: &str, f: F) {
    let mut best = f64::MAX;
    for rep in 0..REPS {
        let t0 = Instant::now();
        SINK.fetch_add(std::hint::black_box(f(rep as i64)) as u64, Ordering::Relaxed);
        let dt = t0.elapsed().as_secs_f64() * 1000.0;
        if dt < best { best = dt; }
    }
    println!("{}={}", name, best);
}
fn main() {
    for _ in 0..60 { fib(12); numeric_loop(200); array_work(200); string_work(200); dict_work(200); sort_work(200); object_work(200); matmul_work(12); }
    measure("fib", |r| fib(std::hint::black_box(30 + r)) as f64);
    measure("numeric", |r| numeric_loop(std::hint::black_box(3000000 + r)));
    measure("array", |r| array_work(std::hint::black_box(1000000 + r)));
    measure("string", |r| string_work(std::hint::black_box(200000 + r)) as f64);
    measure("dict", |r| dict_work(std::hint::black_box(200000 + r)) as f64);
    measure("sort", |r| sort_work(std::hint::black_box(300000 + r)) as f64);
    measure("object", |r| object_work(std::hint::black_box(500000 + r)));
    measure("matmul", |r| matmul_work(std::hint::black_box(256 + r as usize)));
    println!("checksum={}", SINK.load(Ordering::Relaxed));
}
