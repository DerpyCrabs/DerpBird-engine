//! Response-stage network rules. Header predicates never enter the request-stage
//! blocker: removing `$header` there would turn conditional rules into unconditional blocks.
use std::collections::BTreeMap;
use crate::{Engine, request::Request, regex_manager::{regex_literal_end, compile_removeparam_regex}};

enum Value { Present, Literal(String), Regex(crate::regex_manager::CompatibleRegex) }
struct Predicate { name: String, value: Value, invert: bool }
impl Predicate {
    fn parse(raw: &str) -> Option<Self> {
        let (name, value) = raw.split_once(':').map_or((raw, None), |(n,v)| (n,Some(v)));
        if name.is_empty() || !name.bytes().all(|c| c.is_ascii_alphanumeric() || b"!#$%&'*+-.^_`|~".contains(&c)) { return None; }
        let mut invert = false;
        let value = match value {
            None => Value::Present,
            Some(raw) => {
                let raw = if let Some(v) = raw.strip_prefix('~') { invert = true; v } else { raw };
                if raw.starts_with('/') { Value::Regex(compile_removeparam_regex(raw)?) }
                else { Value::Literal(raw.replace(r"\,", ",").replace(r"\$", "$")) }
            }
        };
        Some(Self { name: name.to_ascii_lowercase(), value, invert })
    }
    fn matches(&self, headers: &[(String, String)]) -> bool {
        // Absent headers do not satisfy a negated value comparison either.
        headers.iter().any(|(name, value)| name.eq_ignore_ascii_case(&self.name) &&
            (match &self.value { Value::Present => Some(true), Value::Literal(want) => Some(value == want),
                Value::Regex(regex) => regex.matches(value) }).is_some_and(|matched| matched != self.invert))
    }
}

/// Header conditions share an indexed URL engine. Regexes compile once at list load,
/// with the same bounded source, compiled-program and DFA budgets as removeparam.
pub struct ResponseHeaderEngine { groups: Vec<(Option<Predicate>, Engine, Vec<String>)> }
impl ResponseHeaderEngine {
    pub fn new(rules: &str) -> Self { Self::new_for_kind(rules, false) }
    pub fn new_for_kind(rules: &str, request: bool) -> Self {
        Self::new_for_sources(std::iter::once((rules, "")), request)
    }
    pub fn new_for_sources<'a>(sources: impl Iterator<Item = (&'a str, &'a str)>, request: bool) -> Self {
        let mut groups = BTreeMap::<String, Vec<(String, String)>>::new();
        for (rules, name) in sources {
            for (line_number, line) in rules.lines().map(str::trim).enumerate() {
                if let Some((condition, rule)) = extract(line, request) {
                    let original = if name.is_empty() { line.to_owned() } else {
                        format!("{}\n! Source: {} · line {}", line, name.replace(['\r', '\n'], " "), line_number + 1)
                    };
                    groups.entry(condition.to_owned()).or_default().push((rule, crate::sourcemap::bounded_rule_text(&original)));
                }
            }
        }
        Self { groups: groups.into_iter().filter_map(|(condition, rules)| {
            let predicate = if condition.is_empty() { None } else { Some(Predicate::parse(&condition)?) };
            let mut filters = crate::FilterSet::new(true);
            filters.add_filter_list(rules.iter().map(|(rule, _)| rule.as_str()).collect::<Vec<_>>().join("\n"), crate::lists::ParseOptions::default());
            Some((predicate, Engine::new_with_filter_set_no_optimize(filters), rules.into_iter().map(|(_, original)| original).collect()))
        }).collect() }
    }
    pub fn is_empty(&self) -> bool { self.groups.is_empty() }
    pub fn should_block(&self, request: &Request, headers: &[(String, String)], ordinary_exception: bool) -> bool {
        self.check(request, headers, ordinary_exception).0
    }
    pub fn check(&self, request: &Request, headers: &[(String, String)], ordinary_exception: bool) -> (bool, String) {
        let mut blocked = false;
        let mut matched_rule = String::new();
        let mut exempt = ordinary_exception;
        for (predicate, engine, originals) in &self.groups {
            if predicate.as_ref().is_some_and(|p| !p.matches(headers)) { continue; }
            let result = engine.check_network_request_subset(request, false, true);
            if let Some(filter) = &result.filter {
                if let Some(location) = &filter.source_location {
                    if let Some(original) = originals.get(location.line_number as usize) { matched_rule = original.clone(); }
                }
            }
            if result.important { return (true, matched_rule); }
            blocked |= result.filter.is_some();
            exempt |= result.exception.is_some();
        }
        if blocked && !exempt { (true, matched_rule) } else { (false, String::new()) }
    }
}

fn extract(line: &str, request: bool) -> Option<(&str, String)> {
    if line.starts_with('!') || line.contains("##") || line.contains("#@#") { return None; }
    let dollar = crate::filters::network_filter_option_start(line)?;
    let mut rest = &line[dollar + 1..];
    let selected = |name: &str| if request { name == "requestheader" } else { name == "header" || name == "responseheader" };
    let mut condition = None;
    let mut options = Vec::new();
    while !rest.is_empty() {
        // A header regex may contain commas and dollar anchors. Escaped commas
        // in literal values are also part of the value, not option separators.
        let regex_start = rest.split_once('=').filter(|(name, _)| selected(name)).and_then(|(_, v)| v.split_once(':'))
            .map(|(_,v)| v.strip_prefix('~').unwrap_or(v)).filter(|v| v.starts_with('/'));
        let scan_from = match regex_start {
            Some(v) => rest.len() - v.len() + regex_literal_end(v)? + 1,
            None => 0,
        };
        let mut escaped = false;
        let mut end = rest.bytes().enumerate().skip(scan_from).find_map(|(i,b)| {
            if escaped { escaped = false; return None; }
            if b == b'\\' { escaped = true; return None; }
            (b == b',').then_some(i)
        }).unwrap_or(rest.len());
        // Share regex-aware tokenization for retained address/domain predicates.
        // Their quantifier commas belong to the operand, just like header regexes.
        if ["ipaddress=", "domain=", "from=", "to=", "top=", "denyallow="]
            .iter().any(|prefix| rest.starts_with(prefix)) {
            end = crate::filters::split_filter_options(rest).ok()?.first()?.len();
        }
        let option = &rest[..end];
        let (name, value) = option.split_once('=').unwrap_or((option, ""));
        if selected(name) {
            if condition.is_some() { return None; }
            if value.is_empty() && !line.starts_with("@@") { return None; }
            condition = Some(value);
        } else {
            // These modifiers describe different filtering stages; never reinterpret
            // a mixed action as an ordinary response block.
            let key = option.split('=').next()?;
            if matches!(key, "redirect" | "redirect-rule" | "removeparam" | "csp" | "replace" | "urlskip" | "permissions" | "requestheader" | "responseheader" | "header") { return None; }
            options.push(option);
        }
        rest = if end == rest.len() { "" } else { &rest[end + 1..] };
    }
    let condition = condition?;
    let mut transformed = line[..dollar].to_owned();
    if !options.is_empty() { transformed.push('$'); transformed.push_str(&options.join(",")); }
    Some((condition, transformed))
}
