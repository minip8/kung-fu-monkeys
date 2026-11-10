//! Abuse limits (docs/lobby-protocol.md §7): token buckets, the client
//! address behind the proxy, and what the lobby tracks per address.

use std::net::{IpAddr, SocketAddr};

use axum::http::HeaderMap;
use tokio::time::Instant;

/// A token bucket: `rate` tokens per second, `burst` at most, full at first.
#[derive(Debug, Clone)]
pub(crate) struct Bucket {
    tokens: f64,
    rate: f64,
    burst: f64,
    last: Instant,
}

impl Bucket {
    pub(crate) fn new(rate: f64, burst: f64) -> Self {
        Bucket {
            tokens: burst,
            rate,
            burst,
            last: Instant::now(),
        }
    }

    fn refill(&mut self) {
        let now = Instant::now();
        let elapsed = now.duration_since(self.last).as_secs_f64();
        self.last = now;
        self.tokens = (self.tokens + elapsed * self.rate).min(self.burst);
    }

    /// Takes one token; `false` (and nothing taken) when there is none.
    pub(crate) fn take(&mut self) -> bool {
        self.refill();
        if self.tokens >= 1.0 {
            self.tokens -= 1.0;
            true
        } else {
            false
        }
    }

    /// Whether a token is there, without taking it.
    pub(crate) fn has(&mut self) -> bool {
        self.refill();
        self.tokens >= 1.0
    }

    /// Back to full: nothing to remember.
    fn full(&mut self) -> bool {
        self.refill();
        self.tokens >= self.burst
    }
}

/// What the lobby keeps per client address.
#[derive(Debug)]
pub(crate) struct Address {
    /// Open `/client` sockets.
    pub connections: usize,
    /// Live rooms this address created.
    pub rooms: usize,
    /// `create` and `join` requests.
    pub requests: Bucket,
    /// Joins refused `unknown_code` or `bad_token` (guesses).
    pub failures: Bucket,
}

impl Address {
    /// Nothing open and both buckets full: forgetting it changes nothing.
    pub(crate) fn idle(&mut self) -> bool {
        self.connections == 0 && self.rooms == 0 && self.requests.full() && self.failures.full()
    }
}

/// The address a `/client` socket counts against: the TCP peer, or, when the
/// peer is one of the configured proxies, the last address of its
/// `X-Forwarded-For` (the one that proxy appended; anything before it came
/// from the client and is not trusted). A proxy that sends no usable header
/// counts as itself.
pub(crate) fn client_address(peer: SocketAddr, headers: &HeaderMap, trusted: &[IpAddr]) -> IpAddr {
    let ip = canonical(peer.ip());
    if !trusted.iter().any(|t| canonical(*t) == ip) {
        return ip;
    }
    headers
        .get_all("x-forwarded-for")
        .iter()
        .next_back()
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.rsplit(',').next())
        .and_then(|last| last.trim().parse::<IpAddr>().ok())
        .map_or(ip, canonical)
}

/// An IPv4-mapped IPv6 address as the IPv4 address it is.
fn canonical(ip: IpAddr) -> IpAddr {
    match ip {
        IpAddr::V6(v6) => v6.to_ipv4_mapped().map_or(ip, IpAddr::V4),
        IpAddr::V4(_) => ip,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn headers(values: &[&str]) -> HeaderMap {
        let mut h = HeaderMap::new();
        for v in values {
            h.append("x-forwarded-for", v.parse().unwrap());
        }
        h
    }

    #[test]
    fn forwarded_for_is_read_only_from_a_trusted_proxy() {
        let proxy: SocketAddr = "127.0.0.1:5000".parse().unwrap();
        let other: SocketAddr = "203.0.113.9:5000".parse().unwrap();
        let trusted = ["127.0.0.1".parse().unwrap()];
        let h = headers(&["198.51.100.1, 192.0.2.7"]);
        // The proxy's own entry is the last one.
        assert_eq!(
            client_address(proxy, &h, &trusted),
            "192.0.2.7".parse::<IpAddr>().unwrap()
        );
        // Anyone else's header is ignored.
        assert_eq!(client_address(other, &h, &trusted), other.ip());
        // No proxy configured: the header never counts.
        assert_eq!(client_address(proxy, &h, &[]), proxy.ip());
        // A missing or unparsable header: the proxy itself.
        assert_eq!(
            client_address(proxy, &HeaderMap::new(), &trusted),
            proxy.ip()
        );
        assert_eq!(
            client_address(proxy, &headers(&["junk"]), &trusted),
            proxy.ip()
        );
        // Several headers: the last one, its last entry.
        let h = headers(&["198.51.100.1", "192.0.2.8"]);
        assert_eq!(
            client_address(proxy, &h, &trusted),
            "192.0.2.8".parse::<IpAddr>().unwrap()
        );
        // IPv4-mapped IPv6 is the IPv4 address (proxy and client alike).
        let mapped: SocketAddr = "[::ffff:127.0.0.1]:5000".parse().unwrap();
        let h = headers(&["::ffff:192.0.2.9"]);
        assert_eq!(
            client_address(mapped, &h, &trusted),
            "192.0.2.9".parse::<IpAddr>().unwrap()
        );
    }

    #[test]
    fn buckets_take_and_refill() {
        let mut b = Bucket::new(1000.0, 2.0);
        assert!(b.take());
        assert!(b.has());
        assert!(b.take());
        assert!(!b.take());
        std::thread::sleep(std::time::Duration::from_millis(5));
        assert!(b.take());
    }
}
