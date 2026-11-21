// ansistr: faithful port of bench/ansistr.pas
fn digits(n: i64) -> String {
    let mut s = String::new();
    let mut n = n;
    loop {
        s.insert(0, (b'0' + (n % 10) as u8) as char);
        n /= 10;
        if n == 0 { break; }
    }
    s
}

fn join(a: &str, sep: char, b: &str) -> String {
    let mut s = String::with_capacity(a.len() + 2 + b.len());
    s.push_str(a);
    s.push(sep);
    s.push_str(b);
    s
}

fn hash(s: &str) -> i64 {
    let mut h: i64 = 0;
    for &c in s.as_bytes() {
        h = (h * 31 + (c as i64)) & 0xFFFFFF;
    }
    h
}

fn longest(a: &String, b: &String) -> String {
    if a.len() >= b.len() { a.clone() } else { b.clone() }
}

fn main() {
    let mut sum: i64 = 0;
    let mut best = String::new();
    for i in 1..=300000 {
        let line = join(&digits(i as i64), ',', &digits(i as i64 * 7));
        best = longest(&best, &line);
        if line == best {
            sum += 1;
        }
        sum = (sum + hash(&line)) & 0xFFFFFF;
    }
    // building a long string piece by piece
    let mut all = String::new();
    for i in 1..=40000 as i64 {
        all.push_str(&digits(i));
        all.push(' ');
    }
    println!("{} {} {}", sum, all.len(), hash(&all));
}
