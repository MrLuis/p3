// dynarrays: faithful port of bench/dynarrays.pas
fn build(n: usize, seed: i64) -> Vec<i64> {
    let mut result: Vec<i64> = Vec::new();
    for i in 0..n {
        // Vec::push grows with headroom; the Pascal build's SetLength+1
        // reallocates at each step. Both produce the same list.
        result.push((seed * 31 + (i as i64) * 17) % 1000);
    }
    result
}

fn sum(a: &[i64]) -> i64 {
    a.iter().sum()
}

fn sorted(a: &[i64]) -> Vec<i64> {
    let mut result = a.to_vec();
    for i in 1..result.len() {
        let x = result[i];
        let mut j = i as i64 - 1;
        while j >= 0 && result[j as usize] > x {
            result[(j + 1) as usize] = result[j as usize];
            j -= 1;
        }
        result[(j + 1) as usize] = x;
    }
    result
}

fn grid_rounds(n: i64) -> i64 {
    let mut total: i64 = 0;
    for r in 1..=n {
        let mut g: Vec<Vec<i64>> = vec![Vec::new(); 16];
        for i in 0..16usize {
            let len = (8 + ((r + i as i64) % 8)) as usize;
            g[i].resize(len, 0);
            for j in 0..len {
                g[i][j] = (r * i as i64 + j as i64) % 97;
            }
        }
        for row in g.iter() {
            total += sum(row) + row.len() as i64;
        }
        drop(g);
    }
    total
}

fn list_rounds(n: i64) -> i64 {
    let mut total: i64 = 0;
    for r in 1..=n {
        let a = build(200, r);
        let s = sorted(&a);
        total += sum(&s) + s[0] + s[s.len() - 1] + a[0];
    }
    total
}

fn main() {
    println!("{}", list_rounds(6000));
    println!("{}", grid_rounds(120000));
}
