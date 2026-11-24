//! Training-compatible word normalization (wake_g2p/normalization.py) and the Python string
//! semantics the resolver's results and error messages depend on.
use unicode_general_category::{GeneralCategory as G, get_general_category};
use unicode_normalization::UnicodeNormalization;

/// str.isspace().
pub fn is_space(c: char) -> bool {
    c.is_whitespace() || ('\x1c'..='\x1f').contains(&c)
}

/// str.strip() with no arguments.
pub fn strip(s: &str) -> &str {
    s.trim_matches(is_space)
}

/// str.split() with no arguments.
pub fn split(s: &str) -> Vec<&str> {
    s.split(is_space).filter(|p| !p.is_empty()).collect()
}

/// str.isalpha() for one character: general category L*.
fn is_alpha(c: char) -> bool {
    matches!(get_general_category(c),
        G::UppercaseLetter | G::LowercaseLetter | G::TitlecaseLetter | G::ModifierLetter | G::OtherLetter)
}

/// str.isdigit() for one character: decimal digits plus the Unicode "digit" numerals
/// (superscripts, subscripts, circled and parenthesised digits).
fn is_digit(c: char) -> bool {
    get_general_category(c) == G::DecimalNumber
        || matches!(c, '\u{b2}' | '\u{b3}' | '\u{b9}' | '\u{2070}' | '\u{2074}'..='\u{2079}'
            | '\u{2080}'..='\u{2089}' | '\u{2460}'..='\u{2468}' | '\u{2474}'..='\u{247c}'
            | '\u{2488}'..='\u{2490}' | '\u{24ea}' | '\u{24f5}'..='\u{24fd}' | '\u{24ff}'
            | '\u{2776}'..='\u{277e}' | '\u{2780}'..='\u{2788}' | '\u{278a}'..='\u{2792}'
            | '\u{1369}'..='\u{1371}' | '\u{19da}' | '\u{1f100}'..='\u{1f10a}')
}

/// Python repr() of a str.
pub fn repr(s: &str) -> String {
    let quote = if s.contains('\'') && !s.contains('"') { '"' } else { '\'' };
    let mut out = String::from(quote);
    for c in s.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c == quote => { out.push('\\'); out.push(c) }
            c if (c as u32) < 0x20 || c as u32 == 0x7f => out.push_str(&format!("\\x{:02x}", c as u32)),
            c if !c.is_ascii() && !is_printable(c) => {
                let n = c as u32;
                out.push_str(&if n <= 0xff { format!("\\x{n:02x}") } else if n <= 0xffff { format!("\\u{n:04x}") } else { format!("\\U{n:08x}") })
            }
            c => out.push(c),
        }
    }
    out.push(quote);
    out
}

/// str.isprintable() for one non-ASCII character: not Cc, Cf, Cs, Co, Cn, Zl, Zp or Zs.
fn is_printable(c: char) -> bool {
    !matches!(get_general_category(c), G::Control | G::Format | G::Surrogate | G::PrivateUse
        | G::Unassigned | G::LineSeparator | G::ParagraphSeparator | G::SpaceSeparator)
}

fn latin(c: char) -> Option<&'static str> {
    Some(match c {
        'œ' => "oe", 'Œ' => "Oe", 'æ' => "ae", 'Æ' => "Ae", 'ł' => "l", 'Ł' => "L", 'ø' => "o", 'Ø' => "O",
        'ß' => "ss", 'ı' => "i", 'þ' => "th", 'Þ' => "Th", 'ð' => "th", 'Ð' => "Th", 'đ' => "d", 'Đ' => "D",
        'ﬀ' => "ff", 'ﬁ' => "fi", 'ﬂ' => "fl", 'ﬃ' => "ffi", 'ﬄ' => "ffl",
        _ => return None,
    })
}

fn quotes(c: char) -> char {
    match c as u32 {
        0x2018 | 0x2019 | 0x201B | 0x02BC => '\'',
        0x201C | 0x201D => '"',
        0x2013 | 0x2014 => '-',
        _ => c,
    }
}

/// tokenize(): the upper-case words of a phrase, or why the text cannot be enrolled as words.
pub fn tokenize(text: &str) -> Result<Vec<String>, String> {
    let stripped: String = text.nfd().filter(|&c| get_general_category(c) != G::NonspacingMark).collect();
    let mut t = String::new();
    for c in stripped.nfc() {
        match latin(c) {
            Some(s) => t.extend(s.chars().map(quotes)),
            None => t.push(quotes(c)),
        }
    }
    if t.chars().any(|c| is_digit(c) || (is_alpha(c) && !c.is_ascii())) {
        return Err("spell numbers in words; unsupported non-Latin letters need explicit phones".into());
    }
    if t.chars().any(|c| !(c.is_ascii() && (c.is_ascii_alphabetic() || is_space(c) || "'.,!?;:-\"()".contains(c)))) {
        return Err("unsupported text symbol; provide words or explicit phones".into());
    }
    // WORD = [A-Z]+(?:'[A-Z]+)?, scanned left to right like re.findall.
    let up: Vec<u8> = t.to_ascii_uppercase().into_bytes();
    let mut words = Vec::new();
    let mut i = 0;
    while i < up.len() {
        if !up[i].is_ascii_uppercase() {
            i += 1;
            continue;
        }
        let start = i;
        while i < up.len() && up[i].is_ascii_uppercase() {
            i += 1;
        }
        if i + 1 < up.len() && up[i] == b'\'' && up[i + 1].is_ascii_uppercase() {
            i += 1;
            while i < up.len() && up[i].is_ascii_uppercase() {
                i += 1;
            }
        }
        words.push(String::from_utf8(up[start..i].to_vec()).unwrap());
    }
    if words.is_empty() {
        return Err("text contains no words".into());
    }
    Ok(words)
}
