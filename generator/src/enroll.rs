//! Phrase text -> a keyword file: each phrase with its pronunciations and calibrated thresholds,
//! bound to the wake model, in the layout every Phonlet Wake SDK reads.
use crate::assets;
use crate::calibrate::Generator;
use crate::numeric::{interp, py_round};
use crate::resolver::{Item, Resolved, Resolver, Phones};
use serde::Serialize;
use serde_json::{Value, json};

const FLAG_UNITS: f64 = 80.0;
const MAX_VARIANTS: usize = 64;

/// The wake model these thresholds are for. The evidence bundle names its source model
/// (`SOURCE_MODEL_SHA256`); `en-1` is that model's weights in the engine's container.
pub const MODEL_ID: &str = "en-1";
pub const MODEL_SHA256: &str = "8be3687d060f953d0069480cf53d1fd0aa66e04551c2a4b600823a992a864af8";
pub const SOURCE_MODEL_SHA256: &str = "77164377a658a749d3f41057216c0b23d8ad822e434d8ac79c5fb40aa9ac9f7e";

#[derive(Serialize)]
pub struct Errors {
    pub threshold_loose: f64,
    pub threshold_strict: f64,
    pub default: f64,
}

/// Per-phrase diagnostics; the engine never reads them.
#[derive(Serialize)]
pub struct Diagnostics {
    /// Where the pronunciations came from: "lexicon", "g2p" (predicted) or "explicit".
    pub source: String,
    /// Expected threshold error in engine units at each end and at the default sensitivity.
    pub expected_error: Errors,
    pub fa_factor_at_default: f64,
    pub flag: bool,
    pub warnings: Vec<Warning>,
}

/// One phrase of the keyword file.
#[derive(Serialize)]
pub struct Keyword {
    pub name: String,
    pub pronunciations: Vec<Phones>,
    pub threshold_loose: f64,
    pub threshold_strict: f64,
    pub endpoint: bool,
    pub metadata: Diagnostics,
}

#[derive(Serialize)]
pub struct Unknown {
    pub text: String,
    pub error: String,
}

#[derive(Serialize)]
pub struct Warning {
    pub code: &'static str,
    pub message: String,
}

#[derive(Serialize)]
pub struct ModelRef {
    pub id: &'static str,
    pub sha256: &'static str,
}

#[derive(Serialize)]
pub struct FileMetadata {
    pub generator: String,
    /// False alarms per hour of speech the loose and strict thresholds were set for.
    pub rate_loose: f64,
    pub rate_strict: f64,
    /// Phrases that were not enrolled, and why.
    pub unknown: Vec<Unknown>,
    /// How each enrolled phrase was pronounced, word by word.
    pub resolved: Vec<Resolved>,
}

/// The keyword file: `{"model": ..., "keywords": [...], "metadata": {...}}`.
#[derive(Serialize)]
pub struct Response {
    pub model: ModelRef,
    pub keywords: Vec<Keyword>,
    pub metadata: FileMetadata,
}

pub struct Options {
    pub endpoint: bool,
    pub rate_a: Option<f64>,
    pub rate_b: Option<f64>,
    pub quality: Quality,
}

/// Phrase-quality limits. A warned phrase is still enrolled unless `strict` is set.
pub struct Quality {
    pub min_phonemes: usize,
    pub max_phonemes: usize,
    /// A loose (sensitivity 1) threshold above this (thr units) means the phrase is so common in
    /// ordinary speech that even full sensitivity needs a near-perfect match.
    pub max_loose: f64,
    pub strict: bool,
}

impl Default for Quality {
    fn default() -> Self {
        Quality { min_phonemes: 4, max_phonemes: 12, max_loose: -0.75, strict: false }
    }
}

impl Default for Options {
    fn default() -> Self {
        Options { endpoint: false, rate_a: None, rate_b: None, quality: Quality::default() }
    }
}

/// Why a phrase makes a poor wake word, judged from its pronunciations and thresholds.
pub fn warnings(kw: &Keyword, q: &Quality) -> Vec<Warning> {
    let mut out = Vec::new();
    let shortest = kw.pronunciations.iter().map(Vec::len).min().unwrap_or(0);
    let longest = kw.pronunciations.iter().map(Vec::len).max().unwrap_or(0);
    if shortest < q.min_phonemes {
        out.push(Warning { code: "too_short", message: format!(
            "{shortest} phonemes (minimum {}): short phrases occur inside ordinary words and speech", q.min_phonemes) });
    }
    if longest > q.max_phonemes {
        out.push(Warning { code: "too_long", message: format!(
            "{longest} phonemes (maximum {}): beyond the calibration phrases, thresholds are extrapolated", q.max_phonemes) });
    }
    if kw.threshold_loose > q.max_loose {
        out.push(Warning { code: "too_common", message: format!(
            "loose threshold {} is above {}: the phrase is so frequent in speech that it fires whenever it is said",
            kw.threshold_loose, q.max_loose) });
    }
    if kw.threshold_strict >= 0.0 {
        out.push(Warning { code: "strict_at_ceiling", message:
            "strict threshold is 0: at sensitivity 0 only a perfect match fires".into() });
    }
    if kw.metadata.flag {
        out.push(Warning { code: "uncertain", message: format!(
            "expected threshold error {} units at the default sensitivity (over {FLAG_UNITS})", kw.metadata.expected_error.default) });
    }
    out
}

/// Python's repr() of a float, for messages.
pub fn float_repr(x: f64) -> String {
    if x.is_finite() && x == x.trunc() && x.abs() < 1e16 { format!("{x:.1}") } else { format!("{x}") }
}

pub fn model_sha256() -> String {
    let m: Value = serde_json::from_str(assets::BUNDLE_MANIFEST).unwrap();
    m["model_sha256"].as_str().unwrap().to_string()
}

pub fn policy() -> Value {
    serde_json::from_str(assets::POLICY).unwrap()
}

/// The bundle's identities agree: evidence, G2P assets and lexicon all belong to one model.
pub fn check_identity(resolver: &Resolver) -> Result<(), String> {
    let model = model_sha256();
    if model != SOURCE_MODEL_SHA256 {
        return Err(format!("the embedded bundle is for model {model}, not {MODEL_ID}"));
    }
    for meta in assets::META {
        let m: Value = serde_json::from_str(meta).unwrap();
        if m["value"]["model"].as_str() != Some(&model) {
            return Err("evidence belongs to a different model".into());
        }
    }
    let g: Value = serde_json::from_str(assets::G2P_MANIFEST).unwrap();
    if g["model_sha256"].as_str() != Some(&model) || g["files"]["lexicon.txt"].as_str() != Some(&resolver.lexicon_sha256) {
        return Err("G2P assets do not match this model and lexicon".into());
    }
    Ok(())
}

pub fn enroll(generator: &Generator, resolver: &Resolver, items: &[Item], opts: &Options) -> Result<Response, String> {
    let pol = policy();
    let rate_loose = opts.rate_a.unwrap_or_else(|| pol["rate_a"].as_f64().unwrap());
    let rate_strict = opts.rate_b.unwrap_or_else(|| pol["rate_b"].as_f64().unwrap());
    if !(0.0 < rate_strict && rate_strict <= rate_loose) {
        return Err(format!("need 0 < rate_strict <= rate_loose (false alarms per hour), got rate_loose {}, rate_strict {}",
                           float_repr(rate_loose), float_repr(rate_strict)));
    }
    let mut out = Response {
        model: ModelRef { id: MODEL_ID, sha256: MODEL_SHA256 },
        keywords: Vec::new(),
        metadata: FileMetadata { generator: format!("wake-threshold {}", env!("CARGO_PKG_VERSION")),
                                 rate_loose, rate_strict, unknown: Vec::new(), resolved: Vec::new() },
    };
    // One at a time: an unknown word fails only itself.
    for item in items {
        match resolver.resolve(item, MAX_VARIANTS) {
            Ok(resolved) => {
                let mut kw = keyword(generator, &resolved, rate_loose, rate_strict, opts.endpoint);
                let warned = warnings(&kw, &opts.quality);
                if opts.quality.strict && !warned.is_empty() {
                    let why: Vec<&str> = warned.iter().map(|w| w.message.as_str()).collect();
                    out.metadata.unknown.push(Unknown { text: item.text().to_string(),
                                                        error: format!("rejected by --strict: {}", why.join("; ")) });
                    continue;
                }
                kw.metadata.warnings = warned;
                out.keywords.push(kw);
                out.metadata.resolved.push(resolved);
            }
            Err(err) => {
                let hint = match item {
                    Item::Text(t) => format!(" (give its phones: \"{t}=PH PH ...\")"),
                    Item::Explicit { .. } => String::new(),
                };
                out.metadata.unknown.push(Unknown { text: item.text().to_string(), error: format!("{err}{hint}") });
            }
        }
    }
    Ok(out)
}

fn keyword(generator: &Generator, resolved: &Resolved, rate_loose: f64, rate_strict: f64, endpoint: bool) -> Keyword {
    let variants = &resolved.variants;
    let lad = generator.ladder(variants);
    let (t_a, t_b) = (generator.at(&lad, rate_loose), generator.at(&lad, rate_strict));
    let unc = generator.uncertainty(variants);
    let log_rates = generator.calibration_rates_log();
    let u = |r: f64| interp(r.ln(), log_rates, &unc);
    let (u_a, u_b) = (u(rate_loose), u(rate_strict));
    let mid = (rate_loose * rate_strict).sqrt();
    let u_mid = u(mid);
    // Units per factor e of false alarms around the default: the ladder's own slope.
    let (t_lo, t_hi) = (generator.at(&lad, mid / std::f64::consts::E), generator.at(&lad, mid * std::f64::consts::E));
    let per_e = ((t_lo - t_hi) as f64 / 2.0).max(1.0);
    Keyword {
        name: resolved.text.clone(),
        pronunciations: variants.clone(),
        threshold_loose: t_a as f64 / 1024.0,
        threshold_strict: t_b as f64 / 1024.0,
        endpoint,
        metadata: Diagnostics {
            source: resolved.source.clone(),
            expected_error: Errors { threshold_loose: py_round(u_a, 1), threshold_strict: py_round(u_b, 1), default: py_round(u_mid, 1) },
            fa_factor_at_default: py_round((u_mid / per_e).exp(), 2),
            flag: u_mid > FLAG_UNITS,
            warnings: Vec::new(),
        },
    }
}

/// What the enrollment API's /health reports, for the embedded bundle.
pub fn info(generator: Option<&Generator>, resolver: &Resolver) -> Value {
    let manifest: Value = serde_json::from_str(assets::BUNDLE_MANIFEST).unwrap();
    let cal: Value = serde_json::from_str::<Value>(assets::GENERATOR).unwrap()["calibration"].clone();
    let one_hour = cal["rates"].as_array().unwrap().iter().position(|r| r.as_f64() == Some(1.0)).unwrap();
    let mut out = json!({
        "model": {"sha256": model_sha256()},
        "wake_model": {"id": MODEL_ID, "sha256": MODEL_SHA256},
        "generator": "embedded en-1 calibration bundle",
        "lexicon": "embedded lexicon.txt",
        "policy": policy(),
        "g2p": {"asset_sha256": assets::G2P_ASSET_SHA256, "fallback": "g2p_en==2.1.0",
                "lexicon_sha256": resolver.lexicon_sha256},
        "calibration_error_1h": cal["crossfit_mae"][one_hour],
        "bundle": {"schema": manifest["schema"], "language": manifest["language"], "files": manifest["files"]},
        "embedded": {"evidence_blob_sha256": assets::EVIDENCE_BLOB_SHA256,
                     "pronunciation_blob_sha256": assets::PRONUNCIATION_BLOB_SHA256,
                     "lexicon_words": resolver.lexicon_words()},
        "version": env!("CARGO_PKG_VERSION"),
    });
    if let Some(g) = generator {
        out["world"] = json!({"utterances": g.world.text.len() * g.world.draws, "slots": g.world.slots(),
                              "text_hours": g.world.hours});
    }
    out
}
