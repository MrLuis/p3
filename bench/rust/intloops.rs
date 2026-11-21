// intloops: faithful port of bench/intloops.pas
const N: usize = 2_000_000;
const M: usize = 120;

fn count_primes(sieve: &mut Vec<u8>) -> i64 {
    for v in sieve.iter_mut() {
        *v = 1;
    }
    let mut i = 2usize;
    while i * i <= N {
        if sieve[i - 2] != 0 {
            let mut j = i * i;
            while j <= N {
                sieve[j - 2] = 0;
                j += i;
            }
        }
        i += 1;
    }
    sieve.iter().map(|&v| v as i64).sum()
}

fn product(rounds: i64, a: &mut [[i32; M]], b: &mut [[i32; M]], c: &mut [[i32; M]]) -> i64 {
    for i in 0..M {
        for j in 0..M {
            a[i][j] = ((i + 1 + j + 1) % 7) as i32;
            b[i][j] = (((i + 1) * (j + 1)) % 5) as i32;
        }
    }
    let mut total: i64 = 0;
    for _ in 1..=rounds {
        for i in 0..M {
            for j in 0..M {
                let mut sum: i32 = 0;
                for k in 0..M {
                    sum += a[i][k] * b[k][j];
                }
                c[i][j] = sum % 1000;
            }
        }
        a[0][0] = c[M - 1][M - 1] % 7;
    }
    for i in 0..M {
        total = (total + c[i][i] as i64) % 1_000_000;
    }
    total
}

fn main() {
    let mut sieve = vec![0u8; N - 1];
    let mut primes: i64 = 0;
    for _ in 1..=30 {
        primes += count_primes(&mut sieve);
    }
    println!("{}", primes);
    let mut a = vec![[0i32; M]; M];
    let mut b = vec![[0i32; M]; M];
    let mut c = vec![[0i32; M]; M];
    println!("{}", product(100, &mut a, &mut b, &mut c));
}
