//! Response-only modifiers kept out of the request blocking engine.
use std::collections::BTreeMap;
use crate::{Engine, FilterSet, lists::ParseOptions, request::Request, regex_manager::regex_literal_end};
#[derive(serde::Serialize, Clone)]
pub struct Replacement {
    pub pattern: String, pub replacement: String, pub flags: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub json_kind: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub query: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub matched_rule: Option<String>,
}
pub struct ResponseModifiers { transforms: Vec<(String, Option<Replacement>, ModifierEngine)>, permissions: Vec<(String, ModifierEngine)>, replacements: Vec<(String, Option<Replacement>, ModifierEngine)> }
fn replacement(value: &str) -> Option<Replacement> {
    if value.is_empty() { return None; }
    let end = regex_literal_end(value)?;
    let mut cursor = end + 1;
    let start = cursor;
    let bytes = value.as_bytes();
    while cursor < bytes.len() {
        if bytes[cursor] == b'\\' { cursor += 2; continue; }
        if bytes[cursor] == b'/' { break; }
        cursor += 1;
    }
    if cursor >= bytes.len() { return None; }
    let flags = &value[cursor+1..];
    let mut used = std::collections::HashSet::new();
    if !flags.bytes().all(|flag| b"gimsu".contains(&flag) && used.insert(flag)) { return None; }
    Some(Replacement { pattern: value[1..end].replace(r"\/", "/"), replacement: value[start..cursor].replace(r"\/", "/"), flags: flags.to_owned(), json_kind: None, query: None, matched_rule: None })
}
fn body_replacement(value: &str) -> Option<Replacement> {
    if value.starts_with('/') { return replacement(value).filter(|op| !op.pattern.is_empty()); }
    let (kind, query) = value.split_once(':')?;
    if !matches!(kind, "json" | "jsonl") || query.is_empty() { return None; }
    Some(Replacement { pattern: String::new(), replacement: String::new(), flags: String::new(), json_kind: Some(kind.to_owned()), query: Some(query.to_owned()), matched_rule: None })
}
// JSONPath commas inside selectors, expressions and JSON assignments belong to the value.
fn json_option_end(value: &str) -> usize {
    let mut depth = 0usize;
    let mut quote = 0u8;
    let mut escaped = false;
    let mut regex = false;
    for (index, byte) in value.bytes().enumerate() {
        if escaped { escaped = false; continue; }
        if byte == b'\\' { escaped = true; continue; }
        if quote != 0 { if byte == quote { quote = 0; } continue; }
        if regex { if byte == b'/' { regex = false; } continue; }
        match byte {
            b'"' | b'\'' => quote = byte,
            b'/' => regex = true,
            b'[' | b'(' | b'{' => depth += 1,
            b']' | b')' | b'}' => depth = depth.saturating_sub(1),
            b',' if depth == 0 => return index,
            _ => {},
        }
    }
    value.len()
}
fn extract<'a>(line: &'a str, key: &str) -> Option<(String, String)> {
    if line.starts_with('!') || line.contains("##") || line.contains("#@#") { return None; }
    let dollar = crate::filters::network_filter_option_start(line)?;
    let mut rest = &line[dollar+1..];
    let mut value = None;
    let mut options = Vec::new();
    while !rest.is_empty() {
        let mut end = rest.find(',').unwrap_or(rest.len());
        if let Some(raw) = rest.strip_prefix("replace=").or_else(|| rest.strip_prefix("uritransform=")) {
            if raw.starts_with('/') {
                let regex_end = regex_literal_end(raw)?;
                let mut pos = regex_end + 1;
                while pos < raw.len() { if raw.as_bytes()[pos] == b'\\' { pos += 2; continue; } if raw.as_bytes()[pos] == b'/' { break; } pos += 1; }
                if pos >= raw.len() { return None; }
                end = rest.len() - raw.len() + pos + 1;
                while end < rest.len() && rest.as_bytes()[end] != b',' { end += 1; }
            }
        }
        if rest.starts_with("replace=json:") || rest.starts_with("replace=jsonl:") {
            end = json_option_end(rest);
        }
        if ["ipaddress=", "domain=", "from=", "to=", "top=", "denyallow="]
            .iter().any(|prefix| rest.starts_with(prefix)) {
            end = crate::filters::split_filter_options(rest).ok()?.first()?.len();
        }
        let part = &rest[..end];
        let (name, content) = part.split_once('=').unwrap_or((part, ""));
        if name == key {
            if value.is_some() || content.len() > 8192 || content.is_empty() && !line.starts_with("@@") { return None; }
            value = Some(content.to_owned());
        } else {
            if matches!(name, "replace" | "permissions" | "redirect" | "redirect-rule" | "csp" | "removeparam" | "urlskip" | "header" | "uritransform") { return None; }
            options.push(part);
        }
        rest = if end == rest.len() { "" } else { &rest[end+1..] };
    }
    let mut filter = line[..dollar].to_owned();
    if key == "permissions" && !options.iter().any(|part| matches!(*part, "document" | "doc" | "subdocument" | "frame")) { options.push("document"); options.push("subdocument"); }
    if !options.is_empty() { filter.push('$'); filter.push_str(&options.join(",")); }
    Some((value?, filter))
}
struct ModifierEngine { engine: Engine, originals: Vec<String> }
impl ModifierEngine {
    fn check_network_request(&self, request: &Request) -> crate::blocker::BlockerResult {
        let mut result = self.engine.check_network_request(request);
        if let Some(filter) = result.filter.as_mut() {
            if let Some(original) = filter.source_location.as_ref().and_then(|location| self.originals.get(location.line_number as usize)) {
                filter.raw_line = Some(original.clone());
            }
        }
        result
    }
    fn check_network_request_subset(&self, request: &Request, matched: bool, force: bool) -> crate::blocker::BlockerResult {
        self.engine.check_network_request_subset(request, matched, force)
    }
}
fn engine(rules: Vec<(String, String)>) -> ModifierEngine {
    let mut filters = FilterSet::new(true);
    filters.add_filter_list(rules.iter().map(|(rule, _)| rule.as_str()).collect::<Vec<_>>().join("\n"), ParseOptions::default());
    ModifierEngine { engine: Engine::new_with_filter_set_no_optimize(filters), originals: rules.into_iter().map(|(_, original)| original).collect() }
}
fn ordered_operations(mut operations: Vec<(bool, Replacement)>) -> Vec<Replacement> {
    // Never drop an important action in favour of a normal action at the cap.
    operations.sort_by_key(|(important, _)| !important);
    operations.truncate(64);
    // uBO applies surviving normal actions before surviving important actions.
    operations.sort_by_key(|(important, _)| *important);
    operations.into_iter().map(|(_, operation)| operation).collect()
}
impl ResponseModifiers {
    pub fn new<'a>(sources: impl Iterator<Item = (&'a str, bool, &'a str)>) -> Self {
        let mut permissions: BTreeMap<String, Vec<(String, String)>> = BTreeMap::new();
        let mut transforms: BTreeMap<String, Vec<(String, String)>> = BTreeMap::new();
        let mut replacements: BTreeMap<String, Vec<(String, String)>> = BTreeMap::new();
        for (text, trusted, name) in sources {
            for (line_number, line) in text.lines().map(str::trim).enumerate() {
                let original = crate::sourcemap::bounded_rule_text(&format!("{}\n! Source: {} · line {}", line, name.replace(['\r', '\n'], " "), line_number + 1));
                if let Some((value, filter)) = extract(line, "permissions") { permissions.entry(value).or_default().push((filter, original.clone())); }
                if trusted || line.starts_with("@@") {
                    if let Some((value, filter)) = extract(line, "uritransform") { transforms.entry(value).or_default().push((filter, original.clone())); }
                    if let Some((value, filter)) = extract(line, "replace") { replacements.entry(value).or_default().push((filter, original.clone())); }
                }
            }
        }
        Self { transforms: transforms.into_iter().filter_map(|(value, rules)| {
                let operation = if value.is_empty() { None } else { Some(replacement(&value)?) };
                Some((value, operation, engine(rules)))
            }).collect(), permissions: permissions.into_iter().map(|(value, rules)| (value, engine(rules))).collect(),
            replacements: replacements.into_iter().filter_map(|(value, rules)| {
                let operation = if value.is_empty() { None } else { Some(body_replacement(&value)?) };
                Some((value, operation, engine(rules)))
            }).collect() }
    }
    pub fn transforms(&self, request: &Request) -> Vec<Replacement> {
        let except_all = self.transforms.iter().any(|(value, _, engine)| value.is_empty() && engine.check_network_request_subset(request, false, true).exception.is_some());
        ordered_operations(self.transforms.iter().filter_map(|(_, operation, engine)| {
            let result = engine.check_network_request(request);
            if !result.should_block() || except_all && !result.important { return None; }
            let mut operation = operation.clone()?;
            operation.matched_rule = result.filter.as_ref().and_then(|filter| filter.raw_line.clone());
            if operation.pattern.is_empty() {
                let raw = result.filter.as_ref()?.raw_line.as_deref()?;
                let pattern = if let Some(end) = regex_literal_end(raw) { return Some((result.important, Replacement { pattern: raw[1..end].to_owned(), ..operation })); } else { raw.split('$').next()? };
                let hostname = pattern.starts_with("||");
                let start = hostname || pattern.starts_with('|');
                let end = pattern.ends_with('|');
                let mut expression = String::new();
                if hostname { expression.push_str(r"^[a-z][a-z0-9+.-]*://(?:[^/?#]*\.)?"); }
                else if start { expression.push('^'); }
                for ch in pattern.trim_start_matches('|').trim_end_matches('|').chars() {
                    match ch { '*' => expression.push_str(".*"), '^' => expression.push_str(r"(?:[^%0-9A-Za-z_.-]|$)"), _ => expression.push_str(&regex::escape(&ch.to_string())) }
                }
                if end { expression.push('$'); }
                operation.pattern = expression;
            }
            Some((result.important, operation))
        }).collect())
    }
    pub fn permissions(&self, request: &Request) -> String { self.permission_details(request).0 }
    pub fn permission_details(&self, request: &Request) -> (String, Vec<String>) {
        let except_all = self.permissions.iter().any(|(value, engine)| value.is_empty() && engine.check_network_request_subset(request, false, true).exception.is_some());
        let mut selected = self.permissions.iter().filter_map(|(value, engine)| {
            let result = engine.check_network_request(request);
            if !value.is_empty() && result.should_block() && (!except_all || result.important) && !value.bytes().any(|c| c < 32 || c == 127) { Some((result.important, value.replace('|', ", "), result.filter.and_then(|f| f.raw_line))) } else { None }
        }).collect::<Vec<_>>();
        selected.sort_by_key(|(important, _, _)| *important);
        let rules = selected.iter().filter_map(|(_, _, rule)| rule.clone()).collect();
        (selected.into_iter().map(|(_, value, _)| value).collect::<Vec<_>>().join(", "), rules)
    }
    pub fn replacements(&self, request: &Request) -> Vec<Replacement> {
        let except_all = self.replacements.iter().any(|(value, _, engine)| value.is_empty() && engine.check_network_request_subset(request, false, true).exception.is_some());
        ordered_operations(self.replacements.iter().filter_map(|(_, operation, engine)| {
            let result = engine.check_network_request(request);
            if !result.should_block() || except_all && !result.important { return None; }
            let mut operation = operation.clone()?;
            operation.matched_rule = result.filter.as_ref().and_then(|filter| filter.raw_line.clone());
            Some((result.important, operation))
        }).collect())
    }
}
