//! The few NumPy numerics whose evaluation order or rounding reaches the output.

/// np.add.reduce on a contiguous float64 array: numpy's pairwise summation.
pub fn sum(a: &[f64]) -> f64 {
    let n = a.len();
    if n < 8 {
        let mut res = 0.0;
        for &x in a {
            res += x;
        }
        res
    } else if n <= 128 {
        let mut r = [0.0f64; 8];
        r.copy_from_slice(&a[..8]);
        let mut i = 8;
        while i < n - n % 8 {
            for j in 0..8 {
                r[j] += a[i + j];
            }
            i += 8;
        }
        let mut res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        while i < n {
            res += a[i];
            i += 1;
        }
        res
    } else {
        let half = (n / 2) - (n / 2) % 8;
        sum(&a[..half]) + sum(&a[half..])
    }
}

pub fn mean(a: &[f64]) -> f64 {
    sum(a) / a.len() as f64
}

/// np.searchsorted(a, v, side='left') on an ascending array.
pub fn searchsorted_left<T: PartialOrd>(a: &[T], v: &T) -> usize {
    a.partition_point(|x| x < v)
}

/// np.interp(x, xp, fp) for one x, with numpy's evaluation order.
pub fn interp(x: f64, xp: &[f64], fp: &[f64]) -> f64 {
    let n = xp.len();
    if x <= xp[0] {
        return fp[0];
    }
    if x >= xp[n - 1] {
        return fp[n - 1];
    }
    let j = xp.partition_point(|&v| v <= x) - 1;
    if x == xp[j] {
        return fp[j];
    }
    let slope = (fp[j + 1] - fp[j]) / (xp[j + 1] - xp[j]);
    slope * (x - xp[j]) + fp[j]
}

/// Python's round(x, ndigits) for floats: correctly rounded, ties to even on the exact value.
pub fn py_round(x: f64, ndigits: usize) -> f64 {
    format!("{x:.ndigits$}").parse().unwrap()
}
