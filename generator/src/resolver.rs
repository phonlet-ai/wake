//! Frozen lexicon, wordwise g2p_en fallback and explicit overrides (wake_g2p/resolver.py).
use crate::g2p::G2p;
use crate::text::{self, repr};
use crate::world::PHONES;
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::sync::Mutex;

pub type Phones = Vec<u8>;

/// A phrase to enroll: plain text, or text with explicit pronunciations.
#[derive(Clone, Debug)]
pub enum Item {
    Text(String),
    Explicit { text: String, pronunciations: Vec<String> },
}

impl Item {
    /// serve.phrase / the CLI's --phrase syntax: 'snowboy=S N OW B OY|S N OW B OW'.
    pub fn parse(line: &str) -> Item {
        let line = text::strip(line);
        match line.split_once('=') {
            Some((t, phones)) => Item::Explicit {
                text: text::strip(t).to_string(),
                pronunciations: phones.split('|').map(|p| text::strip(p).to_string()).collect(),
            },
            None => Item::Text(line.to_string()),
        }
    }

    pub fn text(&self) -> &str {
        match self {
            Item::Text(t) | Item::Explicit { text: t, .. } => t,
        }
    }
}

#[derive(Serialize, Clone)]
pub struct Word {
    pub word: String,
    pub source: String,
    pub pronunciations: Vec<String>,
}

#[derive(Serialize, Clone)]
pub struct Resolved {
    pub text: String,
    pub source: String,
    pub words: Vec<Word>,
    pub generated_words: Vec<String>,
    pub variants: Vec<Phones>,
    pub pronunciations: Vec<String>,
    pub variant_combinations: u128,
    pub variants_truncated: bool,
    pub lexicon_sha256: String,
    pub g2p_asset_sha256: String,
}

pub fn parse_phones(value: &str) -> Result<Phones, String> {
    let names: Vec<String> = text::split(value).iter().map(|p| {
        let up = p.to_uppercase();
        match up.strip_suffix(['0', '1', '2']) { Some(s) => s.to_string(), None => up }
    }).collect();
    let ids: Option<Phones> = names.iter()
        .map(|n| PHONES.iter().position(|p| p == n).filter(|&i| i > 0).map(|i| i as u8)).collect();
    match ids {
        Some(ids) if !ids.is_empty() => Ok(ids),
        _ => Err(format!("empty, blank or unknown ARPAbet pronunciation: {}", repr(value))),
    }
}

pub fn spell(v: &[u8]) -> String {
    v.iter().map(|&i| PHONES[i as usize]).collect::<Vec<_>>().join(" ")
}

pub struct Resolver {
    table: HashMap<String, Vec<Phones>>,
    g2p: G2p,
    words: Mutex<HashMap<String, (Vec<Phones>, String)>>,
    pub lexicon_sha256: String,
}

impl Resolver {
    pub fn load() -> Resolver {
        let mut blob = crate::assets::pronunciation();
        let raw = blob.take("lexicon").u8();
        let lexicon_sha256 = Sha256::digest(&raw).iter().map(|b| format!("{b:02x}")).collect();
        let mut table: HashMap<String, Vec<Phones>> = HashMap::new();
        for line in std::str::from_utf8(&raw).expect("lexicon is UTF-8").lines() {
            let tokens = text::split(line);
            if tokens.len() < 2 {
                continue;
            }
            let word = strip_variant(&tokens[0].to_uppercase()).to_string();
            let phones = parse_phones(&tokens[1..].join(" ")).expect("lexicon pronunciation");
            let bucket = table.entry(word).or_default();
            if !bucket.contains(&phones) {
                bucket.push(phones);
            }
        }
        Resolver { table, g2p: G2p::load(&mut blob), words: Mutex::new(HashMap::new()), lexicon_sha256 }
    }

    pub fn lexicon_words(&self) -> usize {
        self.table.len()
    }

    pub fn contains(&self, word: &str) -> bool {
        self.table.contains_key(word)
    }

    pub fn g2p(&self) -> &G2p {
        &self.g2p
    }

    /// One word's pronunciations and where they came from.
    pub fn word(&self, word: &str) -> Result<(Vec<Phones>, String), String> {
        if let Some(p) = self.table.get(word) {
            return Ok((p.clone(), "lexicon".into()));
        }
        if let Some(hit) = self.words.lock().unwrap().get(word) {
            return Ok(hit.clone());
        }
        let branch = self.g2p.branch(word);
        // The original preparation called G2p once per word, not on a sentence.
        let phones = parse_phones(&self.g2p.call(word)?.join(" "))?;
        let hit = (vec![phones], format!("g2p_en:{branch}"));
        self.words.lock().unwrap().insert(word.to_string(), hit.clone());
        Ok(hit)
    }

    pub fn resolve(&self, item: &Item, max_variants: usize) -> Result<Resolved, String> {
        let (text, explicit) = match item {
            Item::Text(t) => match t.split_once('=') {
                Some((t, v)) => (t, Some(v.split('|').map(str::to_string).collect::<Vec<_>>())),
                None => (t.as_str(), None),
            },
            Item::Explicit { text, pronunciations } => {
                if pronunciations.is_empty() {
                    return Err("pronunciations must be a nonempty list".into());
                }
                (text.as_str(), Some(pronunciations.clone()))
            }
        };
        if text::strip(text).is_empty() {
            return Err("text must be nonempty".into());
        }
        let text = text::strip(text).to_string();
        let (words, variants, total, source) = if let Some(explicit) = explicit {
            let mut variants: Vec<Phones> = Vec::new();
            for p in &explicit {
                let v = parse_phones(p)?;
                if !variants.contains(&v) {
                    variants.push(v);
                }
            }
            if variants.len() > max_variants {
                return Err(format!("explicit pronunciations exceed {max_variants}"));
            }
            let total = variants.len() as u128;
            (Vec::new(), variants, total, "explicit".to_string())
        } else {
            let mut words = Vec::new();
            let mut variants: Vec<Phones> = vec![Vec::new()];
            let mut total: u128 = 1;
            for word in text::tokenize(&text)? {
                let (prons, origin) = self.word(&word)?;
                words.push(Word {
                    word: word.clone(),
                    source: origin,
                    pronunciations: prons.iter().map(|p| spell(p)).collect(),
                });
                total = total.checked_mul(prons.len() as u128).ok_or("too many pronunciation combinations")?;
                let mut next = Vec::new();
                'outer: for a in &variants {
                    for b in &prons {
                        if next.len() == max_variants {
                            break 'outer;
                        }
                        next.push([a.as_slice(), b.as_slice()].concat());
                    }
                }
                variants = next;
            }
            let mut unique: Vec<Phones> = Vec::new();
            for v in variants {
                if !unique.contains(&v) {
                    unique.push(v);
                }
            }
            let source = if words.iter().any(|w| w.source != "lexicon") { "g2p" } else { "lexicon" };
            (words, unique, total, source.to_string())
        };
        Ok(Resolved {
            generated_words: words.iter().filter(|w| w.source != "lexicon").map(|w| w.word.clone()).collect(),
            pronunciations: variants.iter().map(|v| spell(v)).collect(),
            text,
            source,
            words,
            variants,
            variant_combinations: total,
            variants_truncated: total > max_variants as u128,
            lexicon_sha256: self.lexicon_sha256.clone(),
            g2p_asset_sha256: crate::assets::G2P_ASSET_SHA256.to_string(),
        })
    }
}

/// re.sub(r"\(\d+\)$", "", word).
fn strip_variant(w: &str) -> &str {
    w.strip_suffix(')').and_then(|s| s.rsplit_once('('))
        .filter(|(_, n)| !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit()))
        .map_or(w, |(w, _)| w)
}
