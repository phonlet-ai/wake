//! NumPy's default_rng, bit for bit: SeedSequence -> PCG64 (XSL-RR 128/64) and the exact
//! Generator.integers / random / choice algorithms the reference draws its evidence with.

const INIT_A: u32 = 0x43b0_d7e5;
const MULT_A: u32 = 0x931e_8875;
const INIT_B: u32 = 0x8b51_f9dd;
const MULT_B: u32 = 0x58f3_8ded;
const MIX_MULT_L: u32 = 0xca01_f9dd;
const MIX_MULT_R: u32 = 0x4973_f715;
const POOL: usize = 4;

/// numpy's _coerce_to_uint32_array for non-negative integers: 32-bit words, least significant first.
pub fn entropy_words(values: &[u64]) -> Vec<u32> {
    let mut out = Vec::new();
    for &v in values {
        if v == 0 {
            out.push(0);
        }
        let mut v = v;
        while v > 0 {
            out.push(v as u32);
            v >>= 32;
        }
    }
    out
}

fn hashmix(value: u32, hash_const: &mut u32) -> u32 {
    let mut v = value ^ *hash_const;
    *hash_const = hash_const.wrapping_mul(MULT_A);
    v = v.wrapping_mul(*hash_const);
    v ^ (v >> 16)
}

fn mix(x: u32, y: u32) -> u32 {
    let r = MIX_MULT_L.wrapping_mul(x).wrapping_sub(MIX_MULT_R.wrapping_mul(y));
    r ^ (r >> 16)
}

/// SeedSequence(entropy).generate_state(n, np.uint64), for entropy without a spawn key.
pub fn seed_state(entropy: &[u32], n: usize) -> Vec<u64> {
    let mut pool = [0u32; POOL];
    let mut hc = INIT_A;
    for (i, slot) in pool.iter_mut().enumerate() {
        *slot = hashmix(entropy.get(i).copied().unwrap_or(0), &mut hc);
    }
    for src in 0..POOL {
        for dst in 0..POOL {
            if src != dst {
                let h = hashmix(pool[src], &mut hc);
                pool[dst] = mix(pool[dst], h);
            }
        }
    }
    for &e in entropy.iter().skip(POOL) {
        for dst in 0..POOL {
            let h = hashmix(e, &mut hc);
            pool[dst] = mix(pool[dst], h);
        }
    }
    let mut hc = INIT_B;
    let words: Vec<u32> = (0..2 * n).map(|i| {
        let mut v = pool[i % POOL] ^ hc;
        hc = hc.wrapping_mul(MULT_B);
        v = v.wrapping_mul(hc);
        v ^ (v >> 16)
    }).collect();
    words.chunks_exact(2).map(|w| w[0] as u64 | (w[1] as u64) << 32).collect()
}

const MULT: u128 = (2549297995355413924u128 << 64) | 4865540595714422341u128;

pub struct Pcg64 {
    state: u128,
    inc: u128,
    buffered: Option<u32>,
}

impl Pcg64 {
    /// np.random.default_rng(entropy) for integer or integer-list entropy.
    pub fn new(entropy: &[u64]) -> Pcg64 {
        let s = seed_state(&entropy_words(entropy), 4);
        let initstate = (s[0] as u128) << 64 | s[1] as u128;
        let initseq = (s[2] as u128) << 64 | s[3] as u128;
        let mut rng = Pcg64 { state: 0, inc: initseq << 1 | 1, buffered: None };
        rng.step();
        rng.state = rng.state.wrapping_add(initstate);
        rng.step();
        rng
    }

    fn step(&mut self) {
        self.state = self.state.wrapping_mul(MULT).wrapping_add(self.inc);
    }

    pub fn next_u64(&mut self) -> u64 {
        self.step();
        ((self.state >> 64) as u64 ^ self.state as u64).rotate_right((self.state >> 122) as u32)
    }

    pub fn next_u32(&mut self) -> u32 {
        if let Some(v) = self.buffered.take() {
            return v;
        }
        let next = self.next_u64();
        self.buffered = Some((next >> 32) as u32);
        next as u32
    }

    /// Generator.random(): 53-bit double in [0, 1).
    pub fn next_f64(&mut self) -> f64 {
        (self.next_u64() >> 11) as f64 * (1.0 / 9007199254740992.0)
    }

    /// One value of Generator.integers(0, high) (int64, high <= 2**32): Lemire's method on the
    /// buffered 32-bit stream, as random_bounded_uint64_fill does for 32-bit ranges.
    pub fn below(&mut self, high: u32) -> u32 {
        let rng = high - 1;
        if rng == 0 {
            return 0;
        }
        let excl = rng as u64 + 1;
        let mut m = self.next_u32() as u64 * excl;
        let mut leftover = m as u32;
        if (leftover as u64) < excl {
            let threshold = ((u32::MAX - rng) as u64 % excl) as u32;
            while leftover < threshold {
                m = self.next_u32() as u64 * excl;
                leftover = m as u32;
            }
        }
        (m >> 32) as u32
    }

    /// One value of Generator.choice(len(p), p=p) with replacement: searchsorted(cdf, u, 'right').
    pub fn choice(&mut self, cdf: &[f64]) -> usize {
        let u = self.next_f64();
        cdf.partition_point(|&c| c <= u)
    }
}

/// p.cumsum() / cdf[-1] for normalised weights p, as Generator.choice computes it.
pub fn cdf(weights: &[f64]) -> Vec<f64> {
    let total: f64 = crate::numeric::sum(weights);
    let p: Vec<f64> = weights.iter().map(|w| w / total).collect();
    let mut acc = 0.0;
    let mut cdf: Vec<f64> = p.iter().map(|x| { acc += x; acc }).collect();
    let last = *cdf.last().unwrap();
    cdf.iter_mut().for_each(|c| *c /= last);
    cdf
}
