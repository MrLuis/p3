//! The operations of ../jsonbench.pas with serde_json and sonic-rs, on the
//! same files: parse into a document, write it back, find one late value
//! (sonic-rs only), read canada.json into structs.
//!
//!     cargo run --release -- ../data [SECONDS]
use serde::{Deserialize, Serialize};
use sonic_rs::JsonValueTrait;
use std::time::{Duration, Instant};

#[derive(Serialize, Deserialize)]
struct Properties {
    name: String,
}

#[derive(Serialize, Deserialize)]
struct Geometry {
    #[serde(rename = "type")]
    kind: String,
    coordinates: Vec<Vec<Vec<f64>>>,
}

#[derive(Serialize, Deserialize)]
struct Feature {
    #[serde(rename = "type")]
    kind: String,
    properties: Properties,
    geometry: Geometry,
}

#[derive(Serialize, Deserialize)]
struct FeatureCollection {
    #[serde(rename = "type")]
    kind: String,
    features: Vec<Feature>,
}

fn measure(name: &str, len: usize, budget: f64, mut op: impl FnMut() -> usize) {
    let mut best = Duration::MAX;
    let mut runs = 0;
    let start = Instant::now();
    let mut sink = 0usize;
    while start.elapsed().as_secs_f64() < budget || runs < 3 {
        let t = Instant::now();
        sink = sink.wrapping_add(op());
        let d = t.elapsed();
        if d < best {
            best = d;
        }
        runs += 1;
    }
    let s = best.as_secs_f64();
    println!(
        "  {:<22} {:8.1} MB/s  ({:6.2} ms){}",
        name,
        len as f64 / s / 1e6,
        s * 1000.0,
        if sink == 42 { " " } else { "" }
    );
}

fn bench(dir: &str, name: &str, path: &sonic_rs::JsonPointer, budget: f64) {
    let text = std::fs::read_to_string(format!("{}/{}", dir, name)).unwrap();
    let n = text.len();
    println!("{} ({} bytes)", name, n);
    measure("serde_json parse dom", n, budget, || {
        let v: serde_json::Value = serde_json::from_str(&text).unwrap();
        v.is_object() as usize
    });
    measure("sonic-rs parse dom", n, budget, || {
        let v: sonic_rs::Value = sonic_rs::from_str(&text).unwrap();
        v.is_object() as usize
    });
    let sv: serde_json::Value = serde_json::from_str(&text).unwrap();
    measure("serde_json write dom", n, budget, || {
        serde_json::to_string(&sv).unwrap().len()
    });
    let kv: sonic_rs::Value = sonic_rs::from_str(&text).unwrap();
    measure("sonic-rs write dom", n, budget, || {
        sonic_rs::to_string(&kv).unwrap().len()
    });
    measure("sonic-rs lazy get", n, budget, || {
        sonic_rs::get(&text, path).unwrap().as_raw_str().len()
    });
    if name == "canada.json" {
        measure("serde_json typed read", n, budget, || {
            let c: FeatureCollection = serde_json::from_str(&text).unwrap();
            c.features.len()
        });
        measure("sonic-rs typed read", n, budget, || {
            let c: FeatureCollection = sonic_rs::from_str(&text).unwrap();
            c.features.len()
        });
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let dir = args.get(1).map(String::as_str).unwrap_or("../data");
    let budget: f64 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(1.0);
    bench(dir, "twitter.json", &sonic_rs::pointer!["statuses", 99, "user", "screen_name"], budget);
    bench(dir, "citm_catalog.json", &sonic_rs::pointer!["performances", 242, "id"], budget);
    bench(dir, "canada.json", &sonic_rs::pointer!["features", 0, "geometry", "coordinates", 479, 0, 0], budget);
}
