//! User-owned hostname firewall. Rules are indexed once, not scanned per request.
//! Rule precedence follows the documented uBO destination/party/type hierarchy.
use std::collections::HashMap;
use crate::request::{Request, RequestType};
#[derive(Clone, Copy, PartialEq)]
pub enum Action { Block, Allow, Noop }
#[derive(Default)]
pub struct DynamicFiltering { rules: HashMap<String, HashMap<String, HashMap<String, (Action, String)>>> }
fn ancestors(host: &str) -> Vec<&str> {
    let mut parts = vec![host];
    // IP addresses are indivisible; domain suffix inheritance is label-based.
    if host.parse::<std::net::IpAddr>().is_err() && !host.starts_with('[') {
        let mut tail = host;
        while let Some((_, next)) = tail.split_once('.') { if !next.is_empty() { parts.push(next); } tail = next; }
    }
    if host != "*" { parts.push("*"); }
    parts
}
impl DynamicFiltering {
    pub fn new(text: &str) -> Self {
        let mut engine = Self::default();
        for line in text.lines().take(1024) {
            let columns: Vec<_> = line.split_ascii_whitespace().collect();
            if columns.len() != 4 { continue; }
            let [source, destination, kind, action] = columns[..] else { continue; };
            if !matches!(kind, "*" | "inline-script" | "1p-script" | "3p-script" | "3p-frame" | "image" | "3p") || destination != "*" && kind != "*" { continue; }
            if [source, destination].iter().any(|host| host.len() > 253 || !host.bytes().all(|c| c.is_ascii_alphanumeric() || b"*._-:[]%".contains(&c))) { continue; }
            let action = match action { "block" => Action::Block, "allow" => Action::Allow, "noop" => Action::Noop, _ => continue };
            engine.rules.entry(source.to_ascii_lowercase()).or_default().entry(destination.to_ascii_lowercase()).or_default().insert(kind.to_owned(), (action, line.to_owned()));
        }
        engine
    }
    fn cell<'a>(&'a self, sources: &[&str], destination: &str, kind: &str) -> Option<(Action, &'a str)> {
        for source in sources {
            if let Some((action, rule)) = self.rules.get(*source).and_then(|destinations| destinations.get(destination)).and_then(|kinds| kinds.get(kind)) { return Some((*action, rule)); }
        }
        None
    }
    pub fn evaluate<'a>(&'a self, source: &str, destination: &str, kind: &str, third_party: bool) -> Option<(Action, &'a str)> {
        if self.rules.is_empty() || destination.is_empty() { return None; }
        let sources = ancestors(source);
        for destination in ancestors(destination).into_iter().filter(|host| *host != "*") {
            if let Some(result) = self.cell(&sources, destination, "*") { return Some(result); }
        }
        let mut kinds = Vec::with_capacity(4);
        if third_party {
            if kind == "script" { kinds.push("3p-script"); }
            if kind == "sub_frame" || kind == "object" { kinds.push("3p-frame"); }
            kinds.push("3p");
        } else if kind == "script" { kinds.push("1p-script"); }
        if matches!(kind, "inline-script" | "image") { kinds.push(kind); }
        kinds.push("*");
        kinds.into_iter().find_map(|kind| self.cell(&sources, "*", kind))
    }
    pub fn check<'a>(&'a self, request: &Request, source_url: &str) -> Option<(Action, &'a str)> {
        if self.rules.is_empty() || request.request_type == RequestType::Document { return None; }
        let source = crate::url_parser::parse_url(source_url)?;
        let destination = crate::url_parser::parse_url(&request.url)?;
        let kind = match request.request_type { RequestType::Script => "script", RequestType::Subdocument => "sub_frame", RequestType::Object => "object", RequestType::Image => "image", RequestType::InlineScript => "inline-script", _ => "other" };
        let result = self.evaluate(source.hostname(), &request.hostname, kind, source.domain() != destination.domain());
        // Hostname firewall block rules govern subresources. Only an explicit allow
        // overrides the popup-specific static engine, as in uBO's popup matcher.
        if matches!(request.request_type, RequestType::Popup | RequestType::Popunder) {
            return result.filter(|(action, _)| *action == Action::Allow);
        }
        result
    }
}
