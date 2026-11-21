// vectors: faithful port of bench/vectors.pas (the four shapes of
// p3's vector[N] of T, llvm-23.md LB-6). p3's whole-vector lanes carry
// no overflow checks (LLVM's vectors have no checked add/sub/mul), so
// the integer chain opts out per op with wrapping_*.
const M: usize = 5_000_000;   // the whole-vector chains' iterations per pass
const K: usize = 500_000;     // the dynamic arrays' length (vectors, or doubles)
const ROUNDS: usize = 8;
const MROUNDS: usize = 10;

fn main() {
    // fvec: the whole-vector chain, on [f64; 4] (LLVM's <4 x double>)
    let a: [f64; 4] = [1.0, -2.25, 0.75, 3.125];
    let b: [f64; 4] = [0.5, -0.125, 2.0, -1.75];
    let mut c = [0.0f64; 4];
    for _ in 0..ROUNDS {
        for _ in 0..M {
            // Rust's stable arrays have no vector arithmetic: the
            // whole-vector statements are written lane by lane (LLVM
            // sees the same <4 x f64> ops as p3's vector types).
            c = [
                c[0] * 0.999 + a[0] * b[0],
                c[1] * 0.999 + a[1] * b[1],
                c[2] * 0.999 + a[2] * b[2],
                c[3] * 0.999 + a[3] * b[3],
            ];
        }
    }
    println!("fvec {:12.8}", c[0] * 10000.0 + c[1] * 100.0 + c[2] * 10.0 + c[3]);

    // ivec: the integer chain, wrapping (see the comment on line 1).
    // p3's chains live in global slots, so its LLVM keeps the 40M
    // iterations' code (the asm shows an add.4s loop per pass); Rust's
    // register-only chain would be evaluated at compile time, so each
    // pass hands the accumulator to black_box to keep it honest.
    let q: [i32; 4] = [-1, 3, -7, 11];
    let mut p: [i32; 4] = [12345, -67890, 246809, -135791];
    for _ in 0..ROUNDS {
        for _ in 0..M {
            p = [
                p[0].wrapping_add(q[0].wrapping_mul(q[0])),
                p[1].wrapping_add(q[1].wrapping_mul(q[1])),
                p[2].wrapping_add(q[2].wrapping_mul(q[2])),
                p[3].wrapping_add(q[3].wrapping_mul(q[3])),
            ];
        }
        p = std::hint::black_box(p); // a borrow would still const-fold
    }
    println!("ivec {}", p[0] + p[3].checked_rem(1_000_000).unwrap());

    // pool: the element loop over an array of vectors
    let mut pool1: Vec<[f64; 4]> = vec![[0.0; 4]; K];
    let mut pool2: Vec<[f64; 4]> = vec![[0.0; 4]; K];
    for i in 0..K {
        for j0 in 0..4 {
            let j = j0 + 1; // p3's elements are 1..N here
            pool1[i][j0] = (((i * 3 + j) % 7) as f64) + ((i % 13) as f64) * 0.25;
            pool2[i][j0] = (((i * 5 + j * 2) % 11) as f64) - 5.0;
        }
    }
    for _ in 0..MROUNDS {
        for i in 0..K {
            pool1[i] = [
                pool1[i][0] * 0.99 + pool2[i][0],
                pool1[i][1] * 0.99 + pool2[i][1],
                pool1[i][2] * 0.99 + pool2[i][2],
                pool1[i][3] * 0.99 + pool2[i][3],
            ];
        }
    }
    println!(
        "pool {:14.8}",
        pool1[0][0] * 1000.0 + pool1[K - 1][3] * 100.0
    );

    // plain: the element loop over plain doubles (the auto-vectorized
    // reference; <4 x f64> for both, without the vector types)
    let mut lanes: Vec<f64> = vec![0.0; K * 4];
    let mut plains: Vec<f64> = vec![0.0; K * 4];
    for i in 0..K * 4 {
        lanes[i] = ((i * 7) % 9) as f64 + ((i % 17) as f64) * 0.125;
        plains[i] = ((i * 3) % 13) as f64 - 6.0;
    }
    for _ in 0..MROUNDS {
        for i in 0..K * 4 {
            lanes[i] = lanes[i] * 0.99 + plains[i] * 0.5;
        }
    }
    println!("plain {:14.8}", lanes[0] * 100.0 + lanes[K * 4 - 1] * 10.0);
    println!("rounds {} {}", ROUNDS, MROUNDS);
}
