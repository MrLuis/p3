// mbench: the math unit's shapes, faithful port (overflow-checks=on, as p3's checked -O2)
fn floor(x: f64) -> i64 { x.floor() as i64 }
fn ceil(x: f64) -> i64 { x.ceil() as i64 }

fn main() {
    let n = 1_000_000usize;
    let mut d = vec![0.0f64; n];
    let mut l = vec![0i64; n];
    for i in 0..n {
        d[i] = i as f64 * 0.5 - 123456.789;
        l[i] = (i as i64 * 37) % 997;
    }
    // 01 Sum 1M x100
    let t0 = std::time::Instant::now();
    let mut s = 0.0f64;
    for _ in 0..100 {
        s += d.iter().sum::<f64>();
    }
    println!("01 Sum 1M x100       {} ms  {}", t0.elapsed().as_millis(), s);
    // 02 SumInt 1M x100 (p3's longint fold checks each add; sum must fit)
    let t0 = std::time::Instant::now();
    let mut t = 0i64;
    for _ in 0..100 {
        t += l.iter().map(|&x| x).sum::<i64>() / 100;
    }
    println!("02 SumInt 1M x100    {} ms  {}", t0.elapsed().as_millis(), t);
    // 03 MaxIntValue over 1M, x5000
    let t0 = std::time::Instant::now();
    let mut m = 0i64;
    for _ in 0..5000 {
        m += *l.iter().max().unwrap();
    }
    println!("03 MaxInt x5000      {} ms  {}", t0.elapsed().as_millis(), m);
    // 04 Mean of 1000 values, x1000 (Rust: a borrowed slice, no Copy)
    let t0 = std::time::Instant::now();
    let mut s = 0.0f64;
    for _ in 0..1000 {
        s += d[..1000].iter().sum::<f64>() / 1000.0;
    }
    println!("04 Mean 1K x1000     {} ms  {}", t0.elapsed().as_millis(), s);
    // 05 Floor/Ceil 1M
    let t0 = std::time::Instant::now();
    let mut t = 0i64;
    for r in 1..=1_000_000i64 {
        t += floor(r as f64 * 0.25) + ceil(r as f64 * 0.25) + floor(r as f64 * 1.5);
    }
    println!("05 Floor/Ceil 1M     {} ms  {}", t0.elapsed().as_millis(), t);
    // 06 Power/Log2/Hypot x200k
    let t0 = std::time::Instant::now();
    let mut s = 0.0f64;
    for r in 1..=200000i64 {
        let rf = r as f64;
        s += rf.powf(2.5) + rf.log2() + rf.hypot(2.0 * rf);
    }
    println!("06 Power/Log2/Hypot  {} ms  {}", t0.elapsed().as_millis(), s);
    // 07 Min/Max/EnsureRange 1M
    let t0 = std::time::Instant::now();
    let mut t = 0i64;
    for r in 1..=1_000_000i64 {
        t += r.clamp(10, 999990) + 3.min(r as i64) + 2.max(r - 9);
    }
    println!("07 Min/Max/Ensure 1M {} ms  {}", t0.elapsed().as_millis(), t);
}
