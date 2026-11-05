//! Wire helpers for lobby protocol v1 (docs/lobby-protocol.md §1-§2): every
//! frame is a flat JSON object whose values are all strings.

use std::collections::HashMap;

use rand::Rng;

/// Alphabet of room codes: no I, O, 0 or 1.
pub const CODE_ALPHABET: &[u8; 32] = b"ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
/// Length of a room code.
pub const CODE_LEN: usize = 5;
/// Largest frame either side accepts (`kfm::net::signaling::kMaxMessageBytes`).
pub const MAX_FRAME_BYTES: usize = 64 * 1024;

/// WebSocket close code after a `refused` or `closed` message.
pub const CLOSE_NORMAL: u16 = 1000;
/// WebSocket close code of a protocol error (no message precedes it).
pub const CLOSE_PROTOCOL_ERROR: u16 = 1002;

/// A decoded frame: the keys and string values of a flat JSON object.
pub type Fields = HashMap<String, String>;

/// Decodes a frame. `None` unless the text is one JSON object whose values
/// are all strings (no nesting, no numbers, no null).
pub fn parse(text: &str) -> Option<Fields> {
    serde_json::from_str::<Fields>(text).ok()
}

/// Encodes a flat object of strings. Keys come out sorted, which no reader
/// depends on.
pub fn encode(pairs: &[(&str, &str)]) -> String {
    let map: serde_json::Map<String, serde_json::Value> = pairs
        .iter()
        .map(|(k, v)| ((*k).to_owned(), serde_json::Value::String((*v).to_owned())))
        .collect();
    serde_json::Value::Object(map).to_string()
}

/// `{"type":"refused","reason":reason}`.
pub fn refused(reason: &str) -> String {
    encode(&[("type", "refused"), ("reason", reason)])
}

/// `{"type":"closed","reason":reason}`.
pub fn closed(reason: &str) -> String {
    encode(&[("type", "closed"), ("reason", reason)])
}

/// A fresh room code (uniqueness among live rooms is the caller's job).
pub fn random_code() -> String {
    let mut rng = rand::rng();
    (0..CODE_LEN)
        .map(|_| CODE_ALPHABET[rng.random_range(0..CODE_ALPHABET.len())] as char)
        .collect()
}

/// A fresh connection token: 128 random bits as 32 lower-case hex digits.
pub fn random_token() -> String {
    let bytes: [u8; 16] = rand::rng().random();
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// A region label: short lower-case ASCII (`eu`, `us`, `asia`). Digits, `-`
/// and `_` are allowed; a comma never is, since the region list is
/// comma-separated.
pub fn valid_region(region: &str) -> bool {
    !region.is_empty()
        && region.len() <= 32
        && region
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-' || b == b'_')
}

/// Compares two secrets in time independent of where they differ.
pub fn secrets_equal(a: &str, b: &str) -> bool {
    let (a, b) = (a.as_bytes(), b.as_bytes());
    if a.len() != b.len() {
        return false;
    }
    a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_accepts_only_flat_objects_of_strings() {
        assert!(parse(r#"{"type":"offer","sdp":"v=0\r\n"}"#).is_some());
        assert!(parse(r#"{}"#).is_some());
        assert!(parse(r#"{"type":1}"#).is_none());
        assert!(parse(r#"{"type":"x","n":null}"#).is_none());
        assert!(parse(r#"{"type":"x","o":{"a":"b"}}"#).is_none());
        assert!(parse(r#"["type"]"#).is_none());
        assert!(parse(r#""type""#).is_none());
        assert!(parse("not json").is_none());
    }

    #[test]
    fn codes_and_tokens_have_the_documented_shape() {
        for _ in 0..100 {
            let code = random_code();
            assert_eq!(code.len(), CODE_LEN);
            assert!(code.bytes().all(|b| CODE_ALPHABET.contains(&b)));
            let token = random_token();
            assert_eq!(token.len(), 32);
            assert!(token
                .bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)));
        }
    }

    #[test]
    fn encode_round_trips() {
        let text = encode(&[("type", "signal"), ("msg", r#"{"type":"offer"}"#)]);
        let fields = parse(&text).unwrap();
        assert_eq!(fields["msg"], r#"{"type":"offer"}"#);
    }

    #[test]
    fn secrets_compare() {
        assert!(secrets_equal("dev", "dev"));
        assert!(!secrets_equal("dev", "deV"));
        assert!(!secrets_equal("dev", "devx"));
    }
}
