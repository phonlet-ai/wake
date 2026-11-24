//! Parity with the Python enrollment reference, from values recorded by running it (commit and
//! environment in the fixture). The fixture holds expected values only; nothing here runs Python.
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::sync::OnceLock;
use wake_threshold::calibrate::{Generator, threshold_at};
use wake_threshold::enroll::{self, Options};
use wake_threshold::resolver::{Item, Resolver};
use wake_threshold::rng::{Pcg64, cdf, entropy_words, seed_state};

fn goldens() -> &'static Value {
    static G: OnceLock<Value> = OnceLock::new();
    G.get_or_init(|| serde_json::from_str(include_str!("fixtures/goldens.json")).unwrap())
}

fn generator() -> &'static Generator {
    static G: OnceLock<Generator> = OnceLock::new();
    G.get_or_init(|| Generator::load(std::thread::available_parallelism().map_or(4, |n| n.get())).unwrap())
}

fn resolver() -> &'static Resolver {
    static R: OnceLock<Resolver> = OnceLock::new();
    R.get_or_init(Resolver::load)
}

/// Python float.hex().
fn from_hex(s: &str) -> f64 {
    let (neg, s) = s.strip_prefix('-').map_or((false, s), |r| (true, r));
    let v = match s {
        "inf" => f64::INFINITY,
        _ => {
            let s = s.strip_prefix("0x").unwrap();
            let (mant, exp) = s.split_once('p').unwrap();
            let (int, frac) = mant.split_once('.').unwrap_or((mant, ""));
            let mut m = u64::from_str_radix(int, 16).unwrap() as f64;
            for (i, c) in frac.chars().enumerate() {
                m += c.to_digit(16).unwrap() as f64 / 16f64.powi(i as i32 + 1);
            }
            m * 2f64.powi(exp.parse().unwrap())
        }
    };
    if neg { -v } else { v }
}

fn hexes(v: &Value) -> Vec<f64> {
    v.as_array().unwrap().iter().map(|x| from_hex(x.as_str().unwrap())).collect()
}

fn entropy(v: &Value) -> Vec<u64> {
    match v {
        Value::Array(a) => a.iter().map(|x| x.as_u64().unwrap()).collect(),
        x => vec![x.as_u64().unwrap()],
    }
}

fn variants(v: &Value) -> Vec<Vec<u8>> {
    serde_json::from_value(v.clone()).unwrap()
}

fn sha(bytes: &[u8]) -> String {
    Sha256::digest(bytes).iter().map(|b| format!("{b:02x}")).collect()
}

#[test]
fn numpy_random_streams() {
    for case in goldens()["rng"].as_array().unwrap() {
        let e = entropy(&case["entropy"]);
        let state: Vec<String> = seed_state(&entropy_words(&e), 4).iter().map(|x| format!("{x:016x}")).collect();
        assert_eq!(serde_json::to_value(&state).unwrap(), case["state"], "{e:?}");
        let mut g = Pcg64::new(&e);
        let raw: Vec<String> = (0..8).map(|_| format!("{:016x}", g.next_u64())).collect();
        assert_eq!(serde_json::to_value(&raw).unwrap(), case["raw"], "{e:?}");
        let mut g = Pcg64::new(&e);
        let ints: Vec<u32> = (0..135).map(|_| g.below(8)).collect();
        assert_eq!(serde_json::to_value(&ints).unwrap(), case["integers8"], "{e:?}");
        let mut g = Pcg64::new(&e);
        let doubles: Vec<f64> = (0..16).map(|_| g.next_f64()).collect();
        assert_eq!(doubles, hexes(&case["random"]), "{e:?}");
        let mut g = Pcg64::new(&e);
        let c = cdf(&[0.5, 0.5]);
        let picks: Vec<usize> = (0..64).map(|_| g.choice(&c)).collect();
        assert_eq!(serde_json::to_value(&picks).unwrap(), case["choice2"], "{e:?}");
    }
}

#[test]
fn evidence_world_is_identical() {
    let w = &goldens()["world"];
    let g = generator();
    assert_eq!(g.world.text.len() * g.world.draws, w["utterances"].as_u64().unwrap() as usize);
    assert_eq!(g.world.slots(), w["slots"].as_u64().unwrap() as usize);
    assert_eq!(g.world.hours.to_bits(), from_hex(w["hours"].as_str().unwrap()).to_bits());
    let mut h = Sha256::new();
    for k in 0..g.world.slots() {
        for v in g.world.record(k) {
            h.update(v.to_le_bytes());
        }
    }
    let got: String = h.finalize().iter().map(|b| format!("{b:02x}")).collect();
    assert_eq!(got, w["slots_sha256"].as_str().unwrap());
    let text = &g.world.text;
    let mut offsets = vec![0i64];
    for _ in 0..g.world.draws {
        for u in 0..text.len() {
            offsets.push(offsets.last().unwrap() + text.utterance(u).len() as i64);
        }
    }
    let bytes: Vec<u8> = offsets.iter().flat_map(|o| o.to_le_bytes()).collect();
    assert_eq!(sha(&bytes), w["offsets_sha256"].as_str().unwrap());
}

#[test]
fn composition_curve_features_ladder() {
    let g = generator();
    for case in goldens()["phrases"].as_array().unwrap() {
        let v = variants(&case["variants"]);
        let best = g.world.compose(&v, 8);
        let bytes: Vec<u8> = best.iter().flat_map(|s| s.to_le_bytes()).collect();
        assert_eq!(sha(&bytes), case["compose_sha256"].as_str().unwrap(), "{v:?}");
        let said = g.world.text.said(&v).iter().filter(|&&s| s).count();
        assert_eq!(said as u64, case["said"].as_u64().unwrap());
        let curve = g.curve(&v);
        assert_eq!(curve, hexes(&case["curve"]), "{v:?}");
        let expected = hexes(&case["features"]);
        assert_eq!(g.features(&v[0]), expected, "{v:?}");
        assert_eq!(threshold_at(&curve, 1.0) as f64, expected[3]);
        close(&g.ladder(&v), &hexes(&case["ladder"]), &format!("ladder {v:?}"));
        close(&g.uncertainty(&v), &hexes(&case["uncertainty"]), &format!("uncertainty {v:?}"));
    }
}

/// Ridge dot products run through BLAS in NumPy, so the last bits may differ by summation order.
fn close(got: &[f64], expected: &[f64], what: &str) {
    assert_eq!(got.len(), expected.len(), "{what}");
    for (a, b) in got.iter().zip(expected) {
        assert!((a - b).abs() <= 1e-9 * b.abs().max(1.0), "{what}: {got:?} != {expected:?}");
    }
}

#[test]
fn g2p_fallback_words() {
    let r = resolver();
    let mut mismatches = Vec::new();
    let cases = goldens()["g2p"].as_object().unwrap();
    for (word, case) in cases {
        assert!(!r.contains(word), "{word} is in the lexicon");
        assert_eq!(format!("g2p_en:{}", r.g2p().branch(word)), case["source"].as_str().unwrap(), "{word}");
        let phones = r.g2p().call(word).unwrap();
        if serde_json::to_value(&phones).unwrap() != case["phones"] {
            mismatches.push(format!("{word}: {phones:?} != {}", case["phones"]));
        }
    }
    assert!(mismatches.is_empty(), "{} of {} words differ:\n{}", mismatches.len(), cases.len(), mismatches.join("\n"));
}

#[test]
fn homographs_outside_the_lexicon_are_recorded() {
    let r = resolver();
    for word in r.g2p().homograph_words() {
        let up = word.to_uppercase();
        if !r.contains(&up) && up.bytes().all(|b| b.is_ascii_uppercase()) {
            r.g2p().call(&up).unwrap_or_else(|e| panic!("{e}"));
        }
    }
}

#[test]
fn resolver_results_and_errors() {
    let r = resolver();
    for case in goldens()["resolve"].as_array().unwrap() {
        let item = case["item"].as_str().unwrap();
        match r.resolve(&Item::Text(item.to_string()), 64) {
            Ok(res) => assert_eq!(serde_json::to_value(res).unwrap(), case["result"], "{item:?}"),
            Err(e) => assert_eq!(Some(e.as_str()), case["error"].as_str(), "{item:?}"),
        }
    }
    let capped = &goldens()["resolve_capped"];
    let res = r.resolve(&Item::Text(capped["item"].as_str().unwrap().into()), 2).unwrap();
    assert_eq!(serde_json::to_value(res).unwrap(), capped["result"]);
}

/// The keyword file in the reference API's layout (with this product's names for the two
/// thresholds), so every value is compared with the recorded Python response; no value is touched. The phrase-quality warnings,
/// pronunciation sources and generator name are this product's additions and are dropped.
fn reference_layout(file: &Value) -> Value {
    assert_eq!(file["model"], serde_json::json!({"id": enroll::MODEL_ID, "sha256": enroll::MODEL_SHA256}));
    let m = &file["metadata"];
    let keywords: Vec<Value> = file["keywords"].as_array().unwrap().iter().map(|k| {
        let d = &k["metadata"];
        serde_json::json!({
            "name": k["name"], "variants": k["pronunciations"],
            "threshold_loose": k["threshold_loose"], "threshold_strict": k["threshold_strict"], "endpoint": k["endpoint"],
            "expected_error": {"threshold_loose": d["expected_error"]["threshold_loose"],
                               "threshold_strict": d["expected_error"]["threshold_strict"],
                               "default": d["expected_error"]["default"]},
            "fa_factor_at_default": d["fa_factor_at_default"], "flag": d["flag"],
        })
    }).collect();
    serde_json::json!({
        "model": {"sha256": enroll::SOURCE_MODEL_SHA256},
        "rate_a": m["rate_loose"], "rate_b": m["rate_strict"],
        "keywords": keywords, "unknown": m["unknown"], "pronunciations": m["resolved"],
    })
}

#[test]
fn enrollment_responses() {
    let (g, r) = (generator(), resolver());
    enroll::check_identity(r).unwrap();
    for case in goldens()["enroll"].as_array().unwrap() {
        let items: Vec<Item> = case["phrases"].as_array().unwrap().iter()
            .map(|p| Item::parse(p.as_str().unwrap())).collect();
        let o = &case["options"];
        let opts = Options { endpoint: o["endpoint"].as_bool().unwrap(), rate_a: o["rate_a"].as_f64(),
                             rate_b: o["rate_b"].as_f64(), ..Options::default() };
        let file = serde_json::to_value(enroll::enroll(g, r, &items, &opts).unwrap()).unwrap();
        assert_eq!(reference_layout(&file), case["result"], "{}", case["phrases"]);
        for k in file["keywords"].as_array().unwrap() {
            let source = k["metadata"]["source"].as_str().unwrap();
            assert!(["lexicon", "g2p", "explicit"].contains(&source), "{source}");
        }
    }
}

#[test]
fn keyword_file_layout() {
    // Exactly the fields the Phonlet Wake SDKs accept: unknown keys there are errors.
    let res = enroll::enroll(generator(), resolver(), &[Item::parse("hey kettle")], &Options::default()).unwrap();
    let file = serde_json::to_value(res).unwrap();
    let keys = |v: &Value| -> Vec<String> { v.as_object().unwrap().keys().cloned().collect() };
    assert_eq!(keys(&file), ["model", "keywords", "metadata"]);
    assert_eq!(keys(&file["model"]), ["id", "sha256"]);
    assert_eq!(keys(&file["keywords"][0]),
               ["name", "pronunciations", "threshold_loose", "threshold_strict", "endpoint", "metadata"]);
    assert_eq!(file["keywords"][0]["pronunciations"], serde_json::json!([[16, 13, 20, 11, 31, 3, 21]]));
    assert_eq!(file["keywords"][0]["threshold_loose"], -1.21875);
    assert_eq!(file["keywords"][0]["threshold_strict"], -0.8740234375);
}

#[test]
fn quality_warnings_and_strict() {
    let (g, r) = (generator(), resolver());
    let codes = |phrase: &str, strict: bool| {
        let mut opts = Options::default();
        opts.quality.strict = strict;
        let res = enroll::enroll(g, r, &[Item::parse(phrase)], &opts).unwrap();
        let codes: Vec<&str> = res.keywords.iter().flat_map(|k| k.metadata.warnings.iter().map(|x| x.code)).collect();
        (codes, res.keywords.len(), res.metadata.unknown.len())
    };
    assert_eq!(codes("hey computer", false), (vec![], 1, 0));
    assert_eq!(codes("alexa", false), (vec![], 1, 0));
    assert_eq!(codes("a", false), (vec!["too_short", "too_common", "uncertain"], 1, 0));
    assert_eq!(codes("the", false), (vec!["too_short", "too_common", "uncertain"], 1, 0));
    assert_eq!(codes("read", false), (vec!["too_short", "too_common", "uncertain"], 1, 0));
    assert_eq!(codes("hey there my good computer friend", false), (vec!["too_long", "strict_at_ceiling"], 1, 0));
    assert_eq!(codes("zorblax quintessence", false), (vec!["too_long"], 1, 0));
    assert_eq!(codes("the", true), (vec![], 0, 1));
    assert_eq!(codes("hey computer", true), (vec![], 1, 0));
}

#[test]
fn info_matches_reference_health() {
    let info = enroll::info(None, resolver());
    let reference = &goldens()["info"];
    for key in ["model", "policy", "g2p", "calibration_error_1h"] {
        assert_eq!(info[key], reference[key], "{key}");
    }
}
