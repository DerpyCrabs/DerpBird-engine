//! Trusted navigation transformations. Matching is delegated to the normal URL
//! engine; transformations never execute script from a filter list.
use std::collections::BTreeMap;
use base64::{Engine as _, engine::{GeneralPurpose, GeneralPurposeConfig, DecodePaddingMode}, alphabet};
use crate::{Engine, FilterSet, lists::ParseOptions, request::Request, regex_manager::{regex_literal_end, capture_first}};

pub struct UrlSkipEngine { groups: Vec<(String, Engine)> }
impl UrlSkipEngine {
    pub fn new<'a>(sources: impl Iterator<Item = (&'a str, bool)>) -> Self {
        let mut groups = BTreeMap::<String, Vec<String>>::new();
        for (text, trusted) in sources {
            for line in text.lines().map(str::trim) {
                if !trusted && !line.starts_with("@@") { continue; }
                if let Some((steps, rule)) = extract(line) {
                    groups.entry(steps.to_owned()).or_default().push(rule);
                }
            }
        }
        Self { groups: groups.into_iter().map(|(steps, rules)| {
            let mut filters = FilterSet::new(true);
            filters.add_filter_list(rules.join("\n"), ParseOptions::default());
            (steps, Engine::new_with_filter_set_no_optimize(filters))
        }).collect() }
    }
    pub fn rewrite(&self, request: &Request, url: &str, blocked: bool) -> Option<(String, String)> {
        // An empty exception cancels every transformation for the request.
        for (steps, engine) in &self.groups {
            if steps.is_empty() && engine.check_network_request_subset(request, false, true).exception.is_some() { return None; }
        }
        for (steps, engine) in &self.groups {
            if steps.is_empty() { continue; }
            let result = engine.check_network_request(request);
            if result.exception.is_some() || result.filter.is_none() { continue; }
            if let Some(value) = transform(url, blocked, steps) {
                if value != url {
                    let base = result.filter.as_ref().and_then(|f| f.raw_line.as_deref()).unwrap_or("*");
                    let rule = format!("{}{}urlskip={}", base, if base.contains('$') { "," } else { "$" }, steps);
                    return Some((value, rule));
                }
            }
        }
        None
    }
}

fn decode_component(value: &str) -> Option<String> {
    let bytes = value.as_bytes();
    for (i, byte) in bytes.iter().enumerate() {
        if *byte == b'%' && (i + 2 >= bytes.len() || !bytes[i+1].is_ascii_hexdigit() || !bytes[i+2].is_ascii_hexdigit()) { return None; }
    }
    percent_encoding::percent_decode_str(value).decode_utf8().ok().map(|v| v.into_owned())
}

fn transform(input: &str, blocked: bool, steps: &str) -> Option<String> {
    if input.len() > 16384 || steps.len() > 4096 { return None; }
    let mut value = input.to_owned();
    let mut redirect_blocked = false;
    for (index, step) in steps.split_ascii_whitespace().enumerate() {
        if index >= 32 || value.len() > 16384 { return None; }
        value = match step {
            "#" => value.split_once('#').map_or("", |(_, fragment)| fragment).to_owned(),
            "+https" => format!("https://{}", value.strip_prefix("https://").or_else(|| value.strip_prefix("http://")).unwrap_or(&value)),
            "-blocked" => { redirect_blocked = true; continue; },
            "-uricomponent" => decode_component(&value)?,
            "-base64" | "-safebase64" => {
                let normalized = if step == "-safebase64" { value.replace('-', "+").replace('_', "/") } else { value };
                let compact: String = normalized.chars().filter(|c| !matches!(c, '\t' | '\n' | '\x0c' | '\r' | ' ')).collect();
                let decoder = GeneralPurpose::new(&alphabet::STANDARD, GeneralPurposeConfig::new()
                    .with_decode_padding_mode(DecodePaddingMode::Indifferent).with_decode_allow_trailing_bits(true));
                decoder.decode(compact).ok()?.into_iter().map(char::from).collect()
            },
            _ if step.starts_with('?') => {
                url::Url::parse(&value).ok()?.query_pairs().find(|(key, _)| key == &step[1..])?.1.replace(' ', "%20")
            },
            _ if step.starts_with('&') => {
                let position = step[1..].parse::<usize>().ok()?.checked_sub(1)?;
                let parsed = url::Url::parse(&value).ok()?;
                let key = parsed.query_pairs().nth(position)?.0.into_owned();
                decode_component(&key)?
            },
            _ if step.starts_with('/') && regex_literal_end(step) == Some(step.len() - 1) => capture_first(&step[1..step.len()-1], &value)?,
            _ => return None,
        };
    }
    if blocked && !redirect_blocked || value.len() > 16384 { return None; }
    let parsed = url::Url::parse(&value).ok()?;
    if !matches!(parsed.scheme(), "http" | "https") { return None; }
    Some(parsed.to_string())
}

fn extract(line: &str) -> Option<(&str, String)> {
    if line.starts_with('!') || line.contains("##") || line.contains("#@#") { return None; }
    let dollar = crate::filters::network_filter_option_start(line)?;
    let mut rest = &line[dollar+1..];
    let mut steps = None;
    let mut options = Vec::new();
    while !rest.is_empty() {
        let mut end;
        if rest.starts_with("urlskip=") {
            end = "urlskip=".len();
            let mut at_step = true;
            while end < rest.len() {
                let byte = rest.as_bytes()[end];
                if byte == b',' { break; }
                if at_step && byte == b'/' {
                    end += regex_literal_end(&rest[end..])? + 1;
                    at_step = false;
                    continue;
                }
                at_step = byte.is_ascii_whitespace();
                end += 1;
            }
        } else {
            end = rest.find(',').unwrap_or(rest.len());
        }
        if ["ipaddress=", "domain=", "from=", "to=", "top=", "denyallow="]
            .iter().any(|prefix| rest.starts_with(prefix)) {
            end = crate::filters::split_filter_options(rest).ok()?.first()?.len();
        }
        let option = &rest[..end];
        if option == "urlskip" || option.starts_with("urlskip=") {
            if steps.is_some() { return None; }
            let value = option.strip_prefix("urlskip=").unwrap_or("");
            if value.len() > 4096 || value.is_empty() && !line.starts_with("@@") { return None; }
            steps = Some(value);
        } else {
            let key = option.split('=').next()?;
            if matches!(key, "redirect" | "redirect-rule" | "removeparam" | "csp" | "header" | "replace" | "permissions") { return None; }
            options.push(option);
        }
        rest = if end == rest.len() { "" } else { &rest[end+1..] };
    }
    let steps = steps?;
    let mut rule = line[..dollar].to_owned();
    if !options.is_empty() { rule.push('$'); rule.push_str(&options.join(",")); }
    Some((steps, rule))
}
