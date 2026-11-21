// reals: faithful port of bench/reals.pas (strict IEEE, no fast-math)
const N: usize = 64;

fn simulate(steps: i64, px: &mut [f64], py: &mut [f64], vx: &mut [f64],
            vy: &mut [f64], m: &mut [f64]) -> f64 {
    for i in 0..N {
        px[i] = (i + 1) as f64 * 1.5;
        py[i] = ((i + 1) % 7) as f64 * 2.0;
        vx[i] = 0.0;
        vy[i] = 0.0;
        m[i] = 1.0 + ((i + 1) % 3) as f64;
    }
    for _ in 0..steps {
        for i in 0..N {
            let mut ax = 0.0f64;
            let mut ay = 0.0f64;
            for j in 0..N {
                if i != j {
                    let dx = px[j] - px[i];
                    let dy = py[j] - py[i];
                    let d2 = dx * dx + dy * dy + 0.01;
                    let inv = m[j] / (d2 * d2.sqrt());
                    ax += dx * inv;
                    ay += dy * inv;
                }
            }
            vx[i] += ax * 0.001;
            vy[i] += ay * 0.001;
        }
        for i in 0..N {
            px[i] += vx[i] * 0.001;
            py[i] += vy[i] * 0.001;
        }
    }
    let mut e = 0.0f64;
    for i in 0..N {
        e += m[i] * (vx[i] * vx[i] + vy[i] * vy[i]);
    }
    e
}

fn mandelbrot(size: i64) -> i64 {
    let mut inside: i64 = 0;
    for y in 0..size {
        for x in 0..size {
            let cr = 3.0 * x as f64 / size as f64 - 2.0;
            let ci = 2.0 * y as f64 / size as f64 - 1.0;
            let mut zr = 0.0f64;
            let mut zi = 0.0f64;
            let mut it = 0;
            while it < 100 && zr * zr + zi * zi <= 4.0 {
                let t = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = t;
                it += 1;
            }
            if it == 100 {
                inside += 1;
            }
        }
    }
    inside
}

fn main() {
    let mut px = vec![0.0f64; N];
    let mut py = vec![0.0f64; N];
    let mut vx = vec![0.0f64; N];
    let mut vy = vec![0.0f64; N];
    let mut m = vec![0.0f64; N];
    println!("{:.6}", simulate(6000, &mut px, &mut py, &mut vx, &mut vy, &mut m));
    println!("{}", mandelbrot(1500) / 1000);
}
