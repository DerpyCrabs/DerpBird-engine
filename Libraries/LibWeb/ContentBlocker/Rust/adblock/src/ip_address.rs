//! Address-dependent blocking is a separate stage: these rules must never turn
//! into unconditional URL blocks before DNS produces an address.
use std::{collections::BTreeMap, net::{IpAddr, Ipv4Addr}};
use crate::{Engine, FilterSet, lists::ParseOptions, request::Request};
use crate::regex_manager::{CompatibleRegex, compile_removeparam_regex};

enum AddressPattern { Exact(String), Prefix(String), Regex(CompatibleRegex), Lan, Loopback }
impl AddressPattern {
    fn parse(value: &str) -> Option<Self> {
        if value.is_empty() || value.len() > 16384 { return None; }
        Some(match value {
            "lan" => Self::Lan,
            "loopback" => Self::Loopback,
            _ if value.starts_with('/') => Self::Regex(compile_removeparam_regex(value)?),
            _ if value.ends_with('*') => Self::Prefix(value[..value.len()-1].to_owned()),
            _ => Self::Exact(value.to_owned()),
        })
    }
    fn matches(&self, text: &str, address: IpAddr) -> bool {
        match self {
            Self::Exact(value) => text == value,
            Self::Prefix(value) => text.starts_with(value),
            Self::Regex(value) => value.matches(text) == Some(true),
            Self::Loopback => address == IpAddr::V4(Ipv4Addr::LOCALHOST) || text == "::1",
            Self::Lan => match address {
                IpAddr::V4(ip) => private_v4(ip),
                IpAddr::V6(ip) => ip.is_unspecified() || ip.is_loopback()
                    || ip.segments()[0] & 0xfe00 == 0xfc00
                    || ip.to_ipv4_mapped().is_some_and(private_v4),
            },
        }
    }
}
pub(crate) fn matches_operand(pattern: &str, address: &str, regex_manager: &mut crate::regex_manager::RegexManager) -> bool {
    // Request admission canonicalizes the address once. Literal and prefix
    // predicates need neither another parse nor an allocation per matched rule.
    match pattern {
        "lan" | "loopback" => {
            let Ok(ip) = address.parse::<IpAddr>() else { return false; };
            let predicate = if pattern == "lan" { AddressPattern::Lan } else { AddressPattern::Loopback };
            predicate.matches(address, ip)
        }
        _ if pattern.starts_with('/') => regex_manager.matches_removeparam(pattern, address) == Some(true),
        _ => pattern.strip_suffix('*').map_or(address == pattern, |prefix| address.starts_with(prefix)),
    }
}

fn private_v4(ip: Ipv4Addr) -> bool {
    let [a,b,_,_] = ip.octets();
    a == 0 || a == 10 || a == 127 || (a == 169 && b == 254)
        || (a == 172 && (16..=31).contains(&b)) || (a == 192 && b == 168)
}

pub struct IpAddressEngine { groups: Vec<(AddressPattern, Engine, Vec<String>)> }
impl IpAddressEngine {
    pub fn new_for_sources<'a>(sources: impl Iterator<Item = (&'a str, &'a str)>) -> Self {
        let mut groups = BTreeMap::<String, Vec<(String, String)>>::new();
        for (source, name) in sources {
            for (index, line) in source.lines().map(str::trim).enumerate() {
                let Some((value, transformed)) = extract(line) else { continue; };
                let original = if name.is_empty() { line.to_owned() }
                    else { format!("{line}\n! Source: {} · line {}", name.replace(['\r', '\n'], " "), index + 1) };
                groups.entry(value.to_owned()).or_default().push((transformed, crate::sourcemap::bounded_rule_text(&original)));
            }
        }
        Self { groups: groups.into_iter().filter_map(|(value, rules)| {
            crate::regex_manager::reset_host_regex_budget();
            let pattern = AddressPattern::parse(&value)?;
            let mut filters = FilterSet::new(true);
            filters.add_filter_list(rules.iter().map(|(rule, _)| rule.as_str()).collect::<Vec<_>>().join("\n"), ParseOptions::default());
            Some((pattern, Engine::new_with_filter_set_no_optimize(filters), rules.into_iter().map(|(_, original)| original).collect()))
        }).collect() }
    }
    pub fn is_empty(&self) -> bool { self.groups.is_empty() }

    /// `address` is supplied by the transport, never inferred from the source
    /// page or retained in the HTTP cache. Empty/invalid addresses cannot match.
    pub fn check(&self, request: &Request, address: &str, ordinary_exception: bool) -> (bool, String) {
        let Ok(ip) = address.parse::<IpAddr>() else { return (false, String::new()); };
        let canonical = ip.to_string();
        let mut blocked = false;
        let mut exempt = ordinary_exception;
        let mut matched_rule = String::new();
        for (pattern, engine, originals) in &self.groups {
            if !pattern.matches(&canonical, ip) { continue; }
            let result = engine.check_network_request_subset(request, false, true);
            if let Some(filter) = &result.filter {
                blocked = true;
                matched_rule = filter.source_location.as_ref()
                    .and_then(|location| originals.get(location.line_number as usize)).cloned().unwrap_or_default();
                if result.important { return (true, matched_rule); }
            }
            exempt |= result.exception.is_some();
        }
        if blocked && !exempt { (true, matched_rule) } else { (false, String::new()) }
    }
}

fn extract(line: &str) -> Option<(&str, String)> {
    if line.is_empty() || line.starts_with('!') || line.contains("##") || line.contains("#@#") { return None; }
    let dollar = crate::filters::network_filter_option_start(line)?;
    let options = crate::filters::split_filter_options(&line[dollar + 1..]).ok()?;
    let mut address = None;
    let mut retained = Vec::new();
    for option in options {
        if let Some(value) = option.strip_prefix("ipaddress=") {
            if address.replace(value).is_some() || value.is_empty() { return None; }
        } else {
            // Keep stages separate; unhandled mixed modifiers are rejected by
            // the ordinary parser, not stripped into broader blocking rules.
            retained.push(option);
        }
    }
    let address = address?;
    let transformed = if retained.is_empty() { line[..dollar].to_owned() }
        else { format!("{}${}", &line[..dollar], retained.join(",")) };
    Some((address, transformed))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn shared_predicate_survives_header_modifier_and_badfilter_paths() {
        let mut filters = FilterSet::new(true);
        filters.add_filter_list("*$script,ipaddress=10.*\n*$script,ipaddress=192.168.*\n*$script,ipaddress=10.*,badfilter".to_owned(), ParseOptions::default());
        let engine = Engine::new_with_filter_set_no_optimize(filters);
        let request = Request::new("https://ads.example/a", "https://page.example/", "script", "GET").unwrap();
        assert!(!engine.check_network_request(&request).should_block());
        assert!(!engine.check_network_request(&request.clone().with_ip_addresses("10.1.2.3")).should_block());
        assert!(engine.check_network_request(&request.clone().with_ip_addresses("192.168.1.2")).should_block());
        let headers = crate::response_headers::ResponseHeaderEngine::new(
            r"*$script,ipaddress=/^192\.168\.[0-9]{1,3}\./,header=x-ad:yes");
        let response = vec![("x-ad".to_owned(), "yes".to_owned())];
        assert!(!headers.should_block(&request, &response, false));
        assert!(headers.should_block(&request.clone().with_ip_addresses("192.168.1.2"), &response, false));
        assert!(!headers.should_block(&request.with_ip_addresses("203.0.113.1"), &response, false));
    }

    #[test]
    fn address_rules_keep_url_scope_exceptions_priority_and_original_attribution() {
        let rules = "*$script,ipaddress=192.168.*\n@@||allowed.example^$script,ipaddress=192.168.*\n||important.example^$script,important,ipaddress=lan\n@@||important.example^$script,ipaddress=lan\n*$image,ipaddress=/^203\\.0\\.113\\.[0-9]{1,3}$/";
        let engine = IpAddressEngine::new_for_sources(std::iter::once((rules, "Address fixture")));
        let request = |url, kind| Request::new(url, "https://page.example/", kind, "GET").unwrap();
        let blocked = engine.check(&request("https://ads.example/a", "script"), "192.168.1.2", false);
        assert!(blocked.0);
        assert!(blocked.1.contains("ipaddress=192.168.*"));
        assert!(blocked.1.contains("Address fixture · line 1"));
        assert!(!engine.check(&request("https://allowed.example/a", "script"), "192.168.1.2", false).0);
        assert!(engine.check(&request("https://important.example/a", "script"), "192.168.1.2", true).0);
        assert!(!engine.check(&request("https://ads.example/a", "script"), "192.168.1.2", true).0);
        assert!(!engine.check(&request("https://ads.example/a", "script"), "", false).0);
        assert!(!engine.check(&request("https://ads.example/a", "script"), "not-an-address", false).0);
        assert!(engine.check(&request("https://ads.example/a", "image"), "203.0.113.12", false).0);
        assert!(!engine.check(&request("https://ads.example/a", "image"), "203.0.112.12", false).0);
    }
    #[test]
    fn address_classes_and_badfilter_preserve_native_boundaries() {
        let rules = "*$script,ipaddress=lan\n*$image,ipaddress=loopback\n*$xhr,ipaddress=203.0.113.1\n*$xhr,ipaddress=203.0.113.1,badfilter";
        let engine = IpAddressEngine::new_for_sources(std::iter::once((rules, "")));
        let request = |kind| Request::new("https://ads.example/", "https://page.example/", kind, "GET").unwrap();
        for address in ["10.0.0.1", "172.16.0.1", "192.168.0.1", "169.254.0.1", "::1", "::", "fc00::1", "fd00::2", "::ffff:192.168.1.1"] {
            assert!(engine.check(&request("script"), address, false).0, "{address}");
        }
        for address in ["8.8.8.8", "172.32.0.1", "2001:4860:4860::8888"] {
            assert!(!engine.check(&request("script"), address, false).0, "{address}");
        }
        assert!(engine.check(&request("image"), "127.0.0.1", false).0);
        assert!(!engine.check(&request("image"), "127.0.0.2", false).0);
        assert!(!engine.check(&request("xmlhttprequest"), "203.0.113.1", false).0);
    }
}
