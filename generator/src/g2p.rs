//! The pinned g2p_en 2.1.0 fallback for one word the lexicon lacks, as the resolver calls it.
//!
//! The resolver only passes single [A-Z]+('[A-Z]+)? words, which g2p_en's tokenizer keeps as one
//! token, so its sentence pipeline reduces to: homograph, else the neural GRU encoder/decoder.
//! (g2p_en also consults its own copy of CMUdict first; every CMUdict word this resolver can
//! pass is already in the training lexicon except 16 misspellings, so that step is left out.) The part-of-speech tagger only chooses between homograph
//! pronunciations; the three homographs absent from the lexicon have their tagger result
//! recorded in assets/g2p/homograph_oov.json.
use crate::assets::Blob;
use std::collections::{HashMap, HashSet};

const GRAPHEMES: &str = "abcdefghijklmnopqrstuvwxyz";
const PHONEMES: [&str; 74] = [
    "<pad>", "<unk>", "<s>", "</s>", "AA0", "AA1", "AA2", "AE0", "AE1", "AE2", "AH0", "AH1", "AH2", "AO0",
    "AO1", "AO2", "AW0", "AW1", "AW2", "AY0", "AY1", "AY2", "B", "CH", "D", "DH", "EH0", "EH1", "EH2", "ER0",
    "ER1", "ER2", "EY0", "EY1", "EY2", "F", "G", "HH", "IH0", "IH1", "IH2", "IY0", "IY1", "IY2", "JH", "K",
    "L", "M", "N", "NG", "OW0", "OW1", "OW2", "OY0", "OY1", "OY2", "P", "R", "S", "SH", "T", "TH", "UH0",
    "UH1", "UH2", "UW", "UW0", "UW1", "UW2", "V", "W", "Y", "Z", "ZH",
];

struct Gru {
    w_ih: Vec<f32>,
    w_hh: Vec<f32>,
    b_ih: Vec<f32>,
    b_hh: Vec<f32>,
    hidden: usize,
    input: usize,
}

/// x @ w.T + b for one row: w is [out, inp].
fn affine(x: &[f32], w: &[f32], b: &[f32]) -> Vec<f32> {
    let inp = x.len();
    b.iter().enumerate().map(|(o, &bias)| {
        let row = &w[o * inp..(o + 1) * inp];
        let mut acc = 0.0f32;
        for i in 0..inp {
            acc += x[i] * row[i];
        }
        acc + bias
    }).collect()
}

fn sigmoid(x: f32) -> f32 {
    1.0 / (1.0 + (-x).exp())
}

impl Gru {
    fn cell(&self, x: &[f32], h: &[f32]) -> Vec<f32> {
        let n = self.hidden;
        let ih = affine(x, &self.w_ih, &self.b_ih);
        let hh = affine(h, &self.w_hh, &self.b_hh);
        (0..n).map(|j| {
            let r = sigmoid(ih[j] + hh[j]);
            let z = sigmoid(ih[n + j] + hh[n + j]);
            let cand = (ih[2 * n + j] + r * hh[2 * n + j]).tanh();
            (1.0 - z) * cand + z * h[j]
        }).collect()
    }
}

pub struct G2p {
    enc_emb: Vec<f32>,
    dec_emb: Vec<f32>,
    enc: Gru,
    dec: Gru,
    fc_w: Vec<f32>,
    fc_b: Vec<f32>,
    homographs: HashSet<String>,
    homograph_oov: HashMap<String, Vec<String>>,
}

impl G2p {
    pub fn load(blob: &mut Blob) -> G2p {
        let mut f = |name: &str| blob.take(&format!("g2p/{name}"));
        let enc_emb = f("enc_emb");
        let emb = enc_emb.shape[1];
        let gru = |f: &mut dyn FnMut(&str) -> crate::assets::Array, p: &str| {
            let w_hh = f(&format!("{p}_w_hh"));
            Gru {
                hidden: w_hh.shape[1],
                input: emb,
                w_hh: w_hh.f32(),
                w_ih: f(&format!("{p}_w_ih")).f32(),
                b_ih: f(&format!("{p}_b_ih")).f32(),
                b_hh: f(&format!("{p}_b_hh")).f32(),
            }
        };
        let enc = gru(&mut f, "enc");
        let dec = gru(&mut f, "dec");
        let (enc_emb, dec_emb) = (enc_emb.f32(), f("dec_emb").f32());
        let (fc_w, fc_b) = (f("fc_w").f32(), f("fc_b").f32());
        let homographs = blob.take("homographs").text().lines()
            .filter(|l| !l.starts_with('#') && !l.trim().is_empty())
            .map(|l| l.trim().split('|').next().unwrap().to_lowercase()).collect();
        let homograph_oov: HashMap<String, Vec<String>> =
            serde_json::from_str(&blob.take("homograph_oov").text()).expect("homograph table");
        let homograph_oov = homograph_oov.into_iter().map(|(k, v)| (k.to_lowercase(), v)).collect();
        G2p { enc_emb, dec_emb, enc, dec, fc_w, fc_b, homographs, homograph_oov }
    }

    /// Which branch the fallback takes for a word: homograph or neural.
    pub fn branch(&self, word: &str) -> &'static str {
        if self.homographs.contains(&word.to_lowercase()) { "homograph" } else { "neural" }
    }

    /// G2p()(word) for one resolver word: stressed ARPAbet symbols.
    pub fn call(&self, word: &str) -> Result<Vec<String>, String> {
        let lower = word.to_lowercase();
        if self.homographs.contains(&lower) {
            return self.homograph_oov.get(&lower).cloned()
                .ok_or_else(|| format!("homograph {word} needs the part-of-speech tagger"));
        }
        Ok(self.predict(&lower))
    }

    /// The neural encoder/decoder, greedy, at most 20 phonemes.
    pub fn predict(&self, word: &str) -> Vec<String> {
        let emb = self.enc.input;
        let embed = |table: &[f32], i: usize| table[i * emb..(i + 1) * emb].to_vec();
        let mut ids: Vec<usize> = word.chars().map(|c| GRAPHEMES.find(c).map_or(1, |i| i + 3)).collect();
        ids.push(2);
        let mut h = vec![0.0f32; self.enc.hidden];
        for &i in &ids {
            h = self.enc.cell(&embed(&self.enc_emb, i), &h);
        }
        let mut dec = embed(&self.dec_emb, 2);
        let mut preds = Vec::new();
        for _ in 0..20 {
            h = self.dec.cell(&dec, &h);
            let logits = affine(&h, &self.fc_w, &self.fc_b);
            let mut pred = 0;
            for (i, &v) in logits.iter().enumerate() {
                if v > logits[pred] {
                    pred = i;
                }
            }
            if pred == 3 {
                break;
            }
            preds.push(PHONEMES.get(pred).copied().unwrap_or("<unk>").to_string());
            dec = embed(&self.dec_emb, pred);
        }
        preds
    }

    pub fn homograph_words(&self) -> impl Iterator<Item = &String> {
        self.homographs.iter()
    }
}
