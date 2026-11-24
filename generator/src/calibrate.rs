//! The saved ridge calibration applied to a phrase (thresholds.py Generator): composed
//! false-alarm curve -> features -> threshold ladder over the calibration rates.
use crate::numeric::{self, mean, searchsorted_left};
use crate::world::{World, keys};
use serde::Deserialize;
use std::collections::HashMap;
use std::sync::Mutex;

pub const CAL_RATES: [f64; 7] = [0.03, 0.1, 0.3, 1.0, 3.0, 10.0, 30.0];
const MIN_COUNT: i64 = 20;

/// GRID = np.arange(-2400, 1, 16).
pub fn grid() -> Vec<i64> {
    (-2400..=0).step_by(16).collect()
}

#[derive(Deserialize)]
pub struct Ridge {
    mean: Vec<f64>,
    sd: Vec<f64>,
    w: Vec<f64>,
    b: f64,
}

impl Ridge {
    /// (X - mean) / sd @ w + b for one feature row.
    fn apply(&self, x: &[f64]) -> f64 {
        let mut dot = 0.0;
        for i in 0..x.len() {
            dot += (x[i] - self.mean[i]) / self.sd[i] * self.w[i];
        }
        dot + self.b
    }
}

#[derive(Deserialize)]
pub struct Calibration {
    pub model: String,
    pub rates: Vec<f64>,
    pub draws: usize,
    pub seed: u64,
    pub models: Vec<Ridge>,
    pub error_models: Vec<Ridge>,
    pub crossfit_mae: Vec<f64>,
}

#[derive(Deserialize)]
struct StoreSpec {
    weight: f64,
}

#[derive(Deserialize)]
struct Spec {
    stores: Vec<StoreSpec>,
    calibration: Calibration,
}

pub struct Generator {
    pub world: World,
    pub calibration: Calibration,
    log_rates: Vec<f64>,
    threads: usize,
    /// Per single-variant features (curve composition dominates the cost; ladder and
    /// uncertainty both need them).
    cache: Mutex<HashMap<Vec<u8>, Vec<f64>>>,
}

impl Generator {
    pub fn load(threads: usize) -> Result<Generator, String> {
        let spec: Spec = serde_json::from_str(crate::assets::GENERATOR).map_err(|e| e.to_string())?;
        let cal = spec.calibration;
        if cal.model != "ridge" || cal.rates != CAL_RATES {
            return Err("only the ridge calibration over the standard rates is supported".into());
        }
        let weights: Vec<f64> = spec.stores.iter().map(|s| s.weight).collect();
        let world = World::build(&weights, cal.seed, cal.draws)?;
        let log_rates = cal.rates.iter().map(|r| r.ln()).collect();
        Ok(Generator { world, calibration: cal, log_rates, threads, cache: Mutex::new(HashMap::new()) })
    }

    /// curve(): composed false alarms per text hour at each GRID threshold.
    pub fn curve(&self, variants: &[Vec<u8>]) -> Vec<f64> {
        let best = self.world.compose(variants, self.threads);
        let said = self.world.text.said(variants);
        let u = said.len();
        let mut b: Vec<i64> = best.iter().enumerate().filter(|(w, _)| !said[w % u]).map(|(_, &s)| s as i64).collect();
        b.sort_unstable();
        grid().iter().map(|t| (b.len() - searchsorted_left(&b, t)) as f64 / self.world.hours).collect()
    }

    /// Evidence.features for one variant: share of its triphones with MIN_COUNT records, and their
    /// mean log record count.
    fn coverage(&self, v: &[u8]) -> (f64, f64) {
        let (k, c) = (&self.world.tri_keys, &self.world.tri_counts);
        let counts: Vec<i64> = keys(v).iter().map(|key| {
            let pos = searchsorted_left(k, &key[0]).min(k.len() - 1);
            if k[pos] == key[0] { c[pos] } else { 0 }
        }).collect();
        let share = mean(&counts.iter().map(|&n| (n >= MIN_COUNT) as u8 as f64).collect::<Vec<_>>());
        let logn = mean(&counts.iter().map(|&n| (n as f64).ln_1p()).collect::<Vec<_>>());
        (share, logn)
    }

    /// phrase_features for a single pronunciation.
    pub fn features(&self, v: &[u8]) -> Vec<f64> {
        if let Some(f) = self.cache.lock().unwrap().get(v) {
            return f.clone();
        }
        let variants = [v.to_vec()];
        let c = self.curve(&variants);
        let mut f: Vec<f64> = CAL_RATES.iter().map(|&r| threshold_at(&c, r) as f64).collect();
        f.push(v.len() as f64);
        f.push((self.world.text.said(&variants).iter().filter(|&&s| s).count() as f64).ln_1p());
        let (share, logn) = self.coverage(v);
        f.extend([share, logn]);
        self.cache.lock().unwrap().insert(v.to_vec(), f.clone());
        f
    }

    /// Calibrated T at each calibration rate. A keyword fires when any variant fires, so each
    /// variant is predicted alone and their false-alarm rates are added.
    pub fn ladder(&self, variants: &[Vec<u8>]) -> Vec<f64> {
        if variants.len() > 1 {
            let lads: Vec<Vec<f64>> = variants.iter().map(|v| self.ladder(std::slice::from_ref(v))).collect();
            let ts: Vec<f64> = (0..=600).map(|i| (-2400 + 4 * i) as f64).collect();
            let mut fa = vec![0.0; ts.len()];
            for l in &lads {
                for (acc, &t) in fa.iter_mut().zip(&ts) {
                    *acc += self.rate(l, t);
                }
            }
            let t: Vec<f64> = self.calibration.rates.iter().map(|&r| {
                fa.iter().position(|&x| x <= r).map_or(0.0, |i| ts[i])
            }).collect();
            return minimum_accumulate(t);
        }
        let x = self.features(&variants[0]);
        minimum_accumulate(self.calibration.models.iter().map(|m| m.apply(&x)).collect())
    }

    /// Expected |threshold error| per calibration rate: the worst variant's error model, floored at
    /// a quarter of the cross-fitted mean error.
    pub fn uncertainty(&self, variants: &[Vec<u8>]) -> Vec<f64> {
        let mae = &self.calibration.crossfit_mae;
        let mut worst = vec![f64::NEG_INFINITY; mae.len()];
        for v in variants {
            let x = self.features(v);
            for (w, m) in worst.iter_mut().zip(&self.calibration.error_models) {
                *w = w.max(m.apply(&x));
            }
        }
        worst.iter().zip(mae).map(|(&w, &m)| w.max(0.25 * m)).collect()
    }

    /// T at any rate: linear in log rate between calibration rates, extended by the end slopes.
    pub fn at(&self, ladder: &[f64], rate: f64) -> i64 {
        let x = &self.log_rates;
        let lr = rate.ln();
        let j = (searchsorted_left(x, &lr) as i64 - 1).clamp(0, x.len() as i64 - 2) as usize;
        let t = ladder[j] + (ladder[j + 1] - ladder[j]) * (lr - x[j]) / (x[j + 1] - x[j]);
        (t.round_ties_even() as i64).clamp(-32768, 0)
    }

    /// Predicted false alarms per hour at threshold t (the inverse of `at`).
    pub fn rate(&self, ladder: &[f64], t: f64) -> f64 {
        let x: Vec<f64> = self.log_rates.iter().rev().copied().collect();
        let y: Vec<f64> = ladder.iter().rev().copied().collect();
        let j = (searchsorted_left(&y, &t) as i64 - 1).clamp(0, y.len() as i64 - 2) as usize;
        if y[j + 1] == y[j] {
            return x[j].exp();
        }
        (x[j] + (x[j + 1] - x[j]) * (t - y[j]) / (y[j + 1] - y[j])).exp()
    }

    pub fn calibration_rates_log(&self) -> &[f64] {
        &self.log_rates
    }
}

fn minimum_accumulate(mut t: Vec<f64>) -> Vec<f64> {
    for i in 1..t.len() {
        t[i] = t[i].min(t[i - 1]);
    }
    t
}

/// The loosest grid threshold whose rate, and every stricter threshold's, is within target.
pub fn threshold_at(rate: &[f64], target: f64) -> i64 {
    let g = grid();
    let mut first = None;
    for i in (0..rate.len()).rev() {
        if rate[i] <= target + 1e-12 {
            first = Some(i);
        } else {
            break;
        }
    }
    first.map_or(*g.last().unwrap(), |i| g[i])
}

pub use numeric::interp;
