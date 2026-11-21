use std::sync::mpsc;
use std::sync::atomic::{AtomicI64, Ordering};

fn main() {
    // 1M increments, same single-task shape
    let a = AtomicI64::new(0);
    let t0 = std::time::Instant::now();
    let mut v = 0i64;
    for _ in 0..1_000_000 {
        v = a.fetch_add(1, Ordering::Relaxed);
    }
    println!("01 rust AtomicI64 1M     {} ms  {}", t0.elapsed().as_millis(), v);

    // 20k send/receive over an unbounded channel (the same task shape)
    for _ in 0..2 {
        let (tx, rx) = mpsc::channel();
        let t0 = std::time::Instant::now();
        std::thread::scope(|s| {
            s.spawn(move || {
                for i in 1..=20000 {
                    tx.send(i).unwrap();
                }
            });
            let mut s2 = 0i64;
            while let Ok(x) = rx.recv() {
                s2 += x;
            }
            println!("03 rust mpsc 20k         {}", t0.elapsed().as_millis());
        });
        // no drop check needed; channel closed
    }
}
