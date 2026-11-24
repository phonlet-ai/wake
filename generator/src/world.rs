//! The calibration text realised from the evidence stores (thresholds.py world / mixture_world),
//! and the phrase composer over it (native/compose.cc).
//!
//! Slot records are kept as the drawn quantile bytes (45 per text phone) rather than the
//! reference's int16 rows; `value` applies the same scaling on read, halving memory.
use crate::assets::Blob;
use crate::rng::{self, Pcg64};
use std::sync::atomic::{AtomicUsize, Ordering};

pub const RECORD: usize = 45;
pub const COST_COLUMNS: usize = 42;
const QUANTILES: usize = 8;
const MIN_COUNT: i64 = 20;
const TRAIL: usize = 40;
const BSPIKE: usize = 41;
const FRAMES: usize = 42;
const BLANKS: usize = 43;
const PHONE_FRAMES: usize = 44;
const MAX_GAP: usize = 4;
const BIG: i64 = 1 << 30;

pub const PHONES: [&str; 40] = [
    "blank", "AA", "AE", "AH", "AO", "AW", "AY", "B", "CH", "D", "DH", "EH", "ER", "EY", "F", "G", "HH",
    "IH", "IY", "JH", "K", "L", "M", "N", "NG", "OW", "OY", "P", "R", "S", "SH", "T", "TH", "UH", "UW",
    "V", "W", "Y", "Z", "ZH",
];

fn class(phone: i64) -> i64 {
    match PHONES[phone as usize] {
        "blank" => 0,
        "AA" | "AE" | "AH" | "AO" | "AW" | "AY" | "EH" | "ER" | "EY" | "IH" | "IY" | "OW" | "OY" | "UH" | "UW" => 1,
        "B" | "D" | "G" | "K" | "P" | "T" => 2,
        "DH" | "F" | "HH" | "S" | "SH" | "TH" | "V" | "Z" | "ZH" => 3,
        "CH" | "JH" => 4,
        "M" | "N" | "NG" => 5,
        "L" | "R" => 6,
        _ => 7,
    }
}

/// tstore.keys for one transcript: (tri, ctx, phone) keys per spoken phone.
pub fn keys(t: &[u8]) -> Vec<[i64; 3]> {
    (0..t.len()).map(|i| {
        let p = t[i] as i64;
        let l = if i > 0 { t[i - 1] as i64 } else { 0 };
        let r = if i + 1 < t.len() { t[i + 1] as i64 } else { 0 };
        [(l * 64 + p) * 64 + r, (class(l) * 64 + p) * 64 + class(r), p]
    }).collect()
}

pub struct Store {
    keys: [Vec<i64>; 3],
    counts: [Vec<i64>; 3],
    /// tri, ctx and phone quantile tables stacked: [rows, QUANTILES, RECORD].
    table: Vec<u8>,
}

impl Store {
    fn load(blob: &mut Blob, i: usize) -> Store {
        let mut keys: [Vec<i64>; 3] = Default::default();
        let mut counts: [Vec<i64>; 3] = Default::default();
        let mut table = Vec::new();
        for (l, level) in ["tri", "ctx", "phone"].iter().enumerate() {
            keys[l] = blob.take(&format!("store{i}/{level}_keys")).i64();
            counts[l] = blob.take(&format!("store{i}/{level}_counts")).i64();
            let q = blob.take(&format!("store{i}/{level}_q"));
            assert_eq!(q.shape, [keys[l].len(), QUANTILES, RECORD]);
            table.extend(q.u8());
        }
        Store { keys, counts, table }
    }

    /// slot_rows: per text phone, its stacked table row -- the triphone with MIN_COUNT records,
    /// else the class context, else the phone.
    fn rows(&self, text_keys: &[[i64; 3]]) -> Result<Vec<u32>, String> {
        let mut rows = vec![-1i64; text_keys.len()];
        let mut base = 0i64;
        for level in 0..3 {
            let (k, c) = (&self.keys[level], &self.counts[level]);
            for (row, key) in rows.iter_mut().zip(text_keys) {
                let pos = crate::numeric::searchsorted_left(k, &key[level]).min(k.len() - 1);
                if *row < 0 && k[pos] == key[level] && (c[pos] >= MIN_COUNT || level == 2) {
                    *row = base + pos as i64;
                }
            }
            base += k.len() as i64;
        }
        let missing = rows.iter().filter(|&&r| r < 0).count();
        if missing > 0 {
            return Err(format!("{missing} text phones without any evidence"));
        }
        Ok(rows.into_iter().map(|r| r as u32).collect())
    }
}

/// The calibration pass's transcripts: phone ids, utterance offsets.
pub struct Text {
    pub ids: Vec<u8>,
    pub offsets: Vec<usize>,
}

impl Text {
    pub fn len(&self) -> usize {
        self.offsets.len() - 1
    }
    pub fn utterance(&self, u: usize) -> &[u8] {
        &self.ids[self.offsets[u]..self.offsets[u + 1]]
    }

    /// said(): which utterances contain some variant as a contiguous phone sequence.
    pub fn said(&self, variants: &[Vec<u8>]) -> Vec<bool> {
        (0..self.len()).map(|u| {
            let t = self.utterance(u);
            variants.iter().any(|v| v.len() <= t.len() && t.windows(v.len()).any(|w| w == v.as_slice()))
        }).collect()
    }
}

pub struct World {
    pub text: Text,
    pub draws: usize,
    /// [draws * phones, RECORD] drawn quantile bytes.
    slots: Vec<u8>,
    pub hours: f64,
    /// Triphone record counts summed over all stores (thresholds.Evidence).
    pub tri_keys: Vec<i64>,
    pub tri_counts: Vec<i64>,
}

struct SendPtr(*mut u8);
unsafe impl Send for SendPtr {}
unsafe impl Sync for SendPtr {}

impl World {
    /// mixture_world(stores, weights, text, seed, draws), or world() for a single store.
    pub fn build(weights: &[f64], seed: u64, draws: usize) -> Result<World, String> {
        let mut blob = crate::assets::evidence();
        let stores: Vec<Store> = (0..weights.len()).map(|i| Store::load(&mut blob, i)).collect();
        let ids = blob.take("text/ids").u8();
        let offsets: Vec<usize> = blob.take("text/offsets").i64().into_iter().map(|o| o as usize).collect();
        let text = Text { ids, offsets };
        let mut text_keys = Vec::with_capacity(text.ids.len());
        for u in 0..text.len() {
            text_keys.extend(keys(text.utterance(u)));
        }
        let rows: Vec<Vec<u32>> = stores.iter().map(|s| s.rows(&text_keys)).collect::<Result<_, _>>()?;
        let n = text.ids.len();
        let mut slots = vec![0u8; draws * n * RECORD];
        // Which store each slot draws from, then each (draw, store) stream fills its own slots.
        let picks: Vec<Vec<u8>> = (0..draws).map(|d| {
            if stores.len() == 1 {
                return vec![0; n];
            }
            let cdf = rng::cdf(weights);
            let mut g = Pcg64::new(&[seed, d as u64]);
            (0..n).map(|_| g.choice(&cdf) as u8).collect()
        }).collect();
        let out = SendPtr(slots.as_mut_ptr());
        let mut tasks: Vec<crate::par::Task> = Vec::new();
        for d in 0..draws {
            for k in 0..stores.len() {
                let (out, picks, rows, table) = (&out, &picks[d], &rows[k], &stores[k].table);
                let entropy = if stores.len() == 1 { seed + d as u64 } else { (seed + d as u64) * 1000 + k as u64 };
                tasks.push(Box::new(move || {
                    let mut g = Pcg64::new(&[entropy]);
                    for i in (0..n).filter(|&i| picks[i] as usize == k) {
                        // SAFETY: each (draw, slot) is written by exactly one (d, k) task.
                        let v = unsafe { std::slice::from_raw_parts_mut(out.0.add((d * n + i) * RECORD), RECORD) };
                        let row = rows[i] as usize;
                        for (c, x) in v.iter_mut().enumerate() {
                            *x = table[(row * QUANTILES + g.below(QUANTILES as u32) as usize) * RECORD + c];
                        }
                        v[FRAMES] = v[FRAMES].max(v[PHONE_FRAMES]);
                        v[PHONE_FRAMES] = v[PHONE_FRAMES].max(1);
                        v[FRAMES] = v[FRAMES].max(1);
                    }
                }));
            }
        }
        crate::par::run(tasks);
        let frames: i64 = slots.chunks_exact(RECORD).map(|v| v[FRAMES] as i64).sum();
        let (tri_keys, tri_counts) = merge_counts(&stores);
        Ok(World { text, draws, slots, hours: frames as f64 / 112500.0, tri_keys, tri_counts })
    }

    pub fn slots(&self) -> usize {
        self.slots.len() / RECORD
    }

    /// The reference's int16 record for slot k (draw_slots' scaling).
    pub fn record(&self, k: usize) -> [i16; RECORD] {
        let mut out = [0i16; RECORD];
        let v = &self.slots[k * RECORD..(k + 1) * RECORD];
        for c in 0..RECORD {
            out[c] = value(c, v[c]);
        }
        out
    }

    /// Best composed score of one keyword per world utterance (text repeated `draws` times),
    /// max over its variants; -32768 where no placement survives.
    pub fn compose(&self, variants: &[Vec<u8>], threads: usize) -> Vec<i16> {
        let u_text = self.text.len();
        let total = u_text * self.draws;
        let n = self.text.ids.len();
        let mut out = vec![-32768i16; total];
        let next = AtomicUsize::new(0);
        let ptr = SendPtr(out.as_mut_ptr() as *mut u8);
        let mut tasks: Vec<crate::par::Task> = Vec::new();
        for _ in 0..threads.max(1).min(total.div_ceil(64).max(1)) {
            let (next, ptr) = (&next, &ptr);
            tasks.push(Box::new(move || {
                let mut dp = Dp::default();
                loop {
                    let start = next.fetch_add(64, Ordering::Relaxed);
                    if start >= total {
                        break;
                    }
                    for w in start..(start + 64).min(total) {
                        let (d, u) = (w / u_text, w % u_text);
                        let (a, b) = (self.text.offsets[u], self.text.offsets[u + 1]);
                        let slots = &self.slots[(d * n + a) * RECORD..(d * n + b) * RECORD];
                        let mut best = -32768i16;
                        if b > a {
                            for v in variants {
                                best = best.max(dp.best_score(v, slots));
                            }
                        }
                        // SAFETY: each utterance index is claimed by exactly one task.
                        unsafe { *(ptr.0 as *mut i16).add(w) = best };
                    }
                }
            }));
        }
        crate::par::run(tasks);
        out
    }
}

/// draw_slots' int16 value of column c from its quantile byte: costs scaled by 16, a full-scale
/// placement cost meaning absent (32767), frame counts unscaled.
#[inline]
fn value(c: usize, v: u8) -> i16 {
    if c < TRAIL && v == 255 {
        32767
    } else if c < COST_COLUMNS {
        v as i16 * 16
    } else {
        v as i16
    }
}

fn merge_counts(stores: &[Store]) -> (Vec<i64>, Vec<i64>) {
    let mut all: Vec<(i64, i64)> = stores.iter()
        .flat_map(|s| s.keys[0].iter().copied().zip(s.counts[0].iter().copied()))
        .collect();
    all.sort_by_key(|&(k, _)| k);
    let (mut keys, mut counts) = (Vec::new(), Vec::<i64>::new());
    for (k, c) in all {
        if keys.last() == Some(&k) {
            *counts.last_mut().unwrap() += c;
        } else {
            keys.push(k);
            counts.push(c);
        }
    }
    (keys, counts)
}

#[derive(Default)]
struct Dp {
    best: Vec<i64>,
    back: Vec<i32>,
    groups: Vec<i64>,
}

impl Dp {
    /// compose.cc best_score over one utterance's slots.
    fn best_score(&mut self, x: &[u8], slots: &[u8]) -> i16 {
        let l = x.len();
        let n = slots.len() / RECORD;
        let w = l.min(7);
        let rec = |k: usize, c: usize| value(c, slots[k * RECORD + c]) as i64;
        let gap_cost = |kp: usize, k: usize| {
            if kp == k {
                return 0;
            }
            let mut g = rec(kp, TRAIL);
            for m in kp + 1..k {
                g += rec(m, BSPIKE) + rec(m, TRAIL);
            }
            g
        };
        self.best.clear();
        self.best.resize(l * n, BIG);
        self.back.clear();
        self.back.resize(l * n, -1);
        self.groups.resize(l, 0);
        for k in 0..n {
            self.best[k] = rec(k, x[0] as usize);
        }
        for i in 1..l {
            for k in 0..n {
                let here = rec(k, x[i] as usize);
                let (mut b, mut arg) = (BIG, -1i32);
                for kp in k.saturating_sub(MAX_GAP + 1)..=k {
                    let prev = self.best[(i - 1) * n + kp];
                    if prev >= BIG || (kp == k && rec(k, FRAMES) < 2) {
                        continue;
                    }
                    let tot = prev + gap_cost(kp, k) + here;
                    if tot < b {
                        b = tot;
                        arg = kp as i32;
                    }
                }
                self.best[i * n + k] = b;
                self.back[i * n + k] = arg;
            }
        }
        let mut out: i32 = -32768;
        for k in 0..n {
            if self.best[(l - 1) * n + k] >= BIG {
                continue;
            }
            let fin = if rec(k, BLANKS) > 0 {
                rec(k, TRAIL) / rec(k, BLANKS)
            } else if k + 1 < n {
                rec(k + 1, BSPIKE)
            } else {
                0
            };
            let (mut kk, mut nk, mut k0) = (k, k, k);
            for i in (0..l).rev() {
                let g = rec(kk, x[i] as usize) + if i == l - 1 { fin } else { gap_cost(kk, nk) };
                self.groups[i] = g.min(32767);
                nk = kk;
                if i == 0 {
                    k0 = kk;
                } else {
                    kk = self.back[i * n + kk] as usize;
                }
            }
            let mut span = rec(k, PHONE_FRAMES) + 1;
            for m in k0..k {
                span += rec(m, FRAMES);
            }
            if span > 5 * l as i64 {
                continue;
            }
            let mut worst = 0i64;
            for s in 0..=l - w {
                let t: i64 = self.groups[s..s + w].iter().sum();
                worst = worst.max(t / w as i64);
            }
            out = out.max(-(worst.min(32767) as i32));
        }
        out as i16
    }
}
