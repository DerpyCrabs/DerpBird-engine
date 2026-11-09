/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[cfg(feature = "allocator")]
extern crate ladybird_allocator;

#[path = "../../../../RustDemangle.rs"]
mod rust_demangle;

#[path = "../../../../RustPanic.rs"]
mod rust_panic;

use crate::rust_panic::abort_on_panic;
use std::collections::HashMap;
use std::collections::HashSet;

include!("derpbird_resources.rs");

#[repr(C)]
pub struct ContentBlockerString {
    data: *mut u8,
    length: usize,
}

#[repr(C)]
pub struct ContentBlockerDecision {
    blocked: bool,
    trusted_url_skip: bool,
    matched_rule: ContentBlockerString,
    rewritten_url: ContentBlockerString,
    redirect_data_url: ContentBlockerString,
}

impl ContentBlockerDecision {
    fn allow() -> Self {
        Self { blocked: false, trusted_url_skip: false, matched_rule: string_to_ffi(String::new()), rewritten_url: string_to_ffi(String::new()), redirect_data_url: string_to_ffi(String::new()) }
    }
}

#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct FilterSource { name: String, trusted: bool, text: String, #[serde(default)] user_owned: bool }
#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct FilterBundle {
    #[serde(rename = "derpbirdFilterBundle")]
    version: u32,
    lists: Vec<FilterSource>,
}

pub struct ContentBlockerEngine {
    has_ip_addresses: bool,
    request_headers: adblock::response_headers::ResponseHeaderEngine,
    cosmetic_policy: adblock::cosmetic_policy::CosmeticPolicy,
    modifiers: adblock::response_modifiers::ResponseModifiers,
    dynamic: adblock::dynamic_filtering::DynamicFiltering,
    canonical_exceptions: adblock::engine::Engine,
    url_skip: adblock::url_skip::UrlSkipEngine,
    sources: Vec<String>,
    engine: adblock::engine::Engine,
    generic_selector_list_rules: GenericSelectorListRules,
    response_headers: Option<adblock::engine::Engine>,
    header_attribution: HeaderAttribution,
    conditional_headers: adblock::response_headers::ResponseHeaderEngine,
    html_filters: adblock::html_filtering::HtmlFilterEngine,
}

#[derive(Default)]
struct GenericSelectorListRules {
    always_needed: HashSet<String>,
    by_class: HashMap<String, Vec<String>>,
    by_id: HashMap<String, Vec<String>>,
}

enum SelectorKey {
    Class(String),
    Id(String),
}

/// The bytes as a string, or none where they are not UTF-8.
fn as_str(bytes: ak::Span<'_, u8>) -> Option<&str> {
    std::str::from_utf8(bytes.as_slice()).ok()
}

fn rule_to_ffi(mut rule: String) -> ContentBlockerString {
    if rule.len() > 4096 {
        let mut end = 4093;
        while !rule.is_char_boundary(end) { end -= 1; }
        rule.truncate(end);
        rule.push_str("...");
    }
    string_to_ffi(rule)
}

fn string_to_ffi(string: String) -> ContentBlockerString {
    if string.is_empty() {
        return ContentBlockerString {
            data: std::ptr::null_mut(),
            length: 0,
        };
    }

    let bytes = Box::leak(string.into_bytes().into_boxed_slice());
    let data = bytes.as_mut_ptr();
    let length = bytes.len();

    ContentBlockerString { data, length }
}

fn for_each_selector_list_item(selector: &str, mut callback: impl FnMut(&str)) {
    let mut start = 0;
    let mut parenthesis_depth: u32 = 0;
    let mut bracket_depth: u32 = 0;
    let mut quote = None;
    let mut escaped = false;

    for (index, character) in selector.char_indices() {
        if escaped {
            escaped = false;
            continue;
        }

        if character == '\\' {
            escaped = true;
            continue;
        }

        if let Some(quote_character) = quote {
            if character == quote_character {
                quote = None;
            }
            continue;
        }

        match character {
            '"' | '\'' => quote = Some(character),
            '(' => parenthesis_depth += 1,
            ')' => parenthesis_depth = parenthesis_depth.saturating_sub(1),
            '[' => bracket_depth += 1,
            ']' => bracket_depth = bracket_depth.saturating_sub(1),
            ',' if parenthesis_depth == 0 && bracket_depth == 0 => {
                callback(&selector[start..index]);
                start = index + character.len_utf8();
            }
            _ => {}
        }
    }

    callback(&selector[start..]);
}

fn selector_key_from_start(selector: &str) -> Option<SelectorKey> {
    let selector = selector.trim_start();
    let marker = selector.chars().next()?;
    if marker != '.' && marker != '#' {
        return None;
    }

    let mut end = marker.len_utf8();
    for (offset, character) in selector[end..].char_indices() {
        if character == '\\' {
            return None;
        }
        if !character.is_alphanumeric() && character != '_' && character != '-' {
            break;
        }
        end = marker.len_utf8() + offset + character.len_utf8();
    }

    if end == marker.len_utf8() {
        return None;
    }

    let key = selector[marker.len_utf8()..end].to_string();
    match marker {
        '.' => Some(SelectorKey::Class(key)),
        '#' => Some(SelectorKey::Id(key)),
        _ => None,
    }
}

impl GenericSelectorListRules {
    fn from_rules(rules: &str) -> Self {
        let mut selector_list_rules = Self::default();

        for line in rules.lines() {
            let trimmed_line = line.trim();
            if trimmed_line.is_empty() || trimmed_line.starts_with('!') || trimmed_line.starts_with('[') {
                continue;
            }

            let Ok(filter) = adblock::filters::cosmetic::CosmeticFilter::parse(
                trimmed_line,
                false,
                adblock::resources::PermissionMask::default(),
            ) else {
                continue;
            };

            let generic_filter = if filter.has_hostname_constraint() {
                filter.hidden_generic_rule()
            } else {
                Some(filter)
            };

            let Some(generic_filter) = generic_filter else {
                continue;
            };
            let Some(selector) = generic_filter.plain_css_selector() else {
                continue;
            };
            selector_list_rules.add_selector(selector);
        }

        selector_list_rules
    }

    fn add_selector(&mut self, selector: &str) {
        let mut selector_list_item_count = 0;
        let mut has_unkeyable_item = false;
        let mut class_keys = HashSet::new();
        let mut id_keys = HashSet::new();
        for_each_selector_list_item(selector, |selector_list_item| {
            selector_list_item_count += 1;

            match selector_key_from_start(selector_list_item) {
                Some(SelectorKey::Class(class)) => {
                    class_keys.insert(class);
                }
                Some(SelectorKey::Id(id)) => {
                    id_keys.insert(id);
                }
                None => has_unkeyable_item = true,
            }
        });

        if selector_list_item_count < 2 {
            return;
        }

        let selector = selector.to_string();
        if has_unkeyable_item {
            self.always_needed.insert(selector);
            return;
        }

        for class in class_keys {
            self.by_class.entry(class).or_default().push(selector.clone());
        }
        for id in id_keys {
            self.by_id.entry(id).or_default().push(selector.clone());
        }
    }

    fn hidden_selectors(
        &self,
        classes: impl IntoIterator<Item = impl AsRef<str>>,
        ids: impl IntoIterator<Item = impl AsRef<str>>,
        exceptions: &HashSet<String>,
    ) -> Vec<String> {
        let mut selectors: Vec<_> = self
            .always_needed
            .iter()
            .filter(|selector| !exceptions.contains(*selector))
            .cloned()
            .collect();

        for class in classes {
            if let Some(class_selectors) = self.by_class.get(class.as_ref()) {
                selectors.extend(
                    class_selectors
                        .iter()
                        .filter(|selector| !exceptions.contains(*selector))
                        .cloned(),
                );
            }
        }

        for id in ids {
            if let Some(id_selectors) = self.by_id.get(id.as_ref()) {
                selectors.extend(
                    id_selectors
                        .iter()
                        .filter(|selector| !exceptions.contains(*selector))
                        .cloned(),
                );
            }
        }

        selectors
    }

    fn has_hidden_selectors_for_class_or_id(
        &self,
        classes: impl IntoIterator<Item = impl AsRef<str>>,
        ids: impl IntoIterator<Item = impl AsRef<str>>,
        exceptions: &HashSet<String>,
    ) -> bool {
        for class in classes {
            if let Some(class_selectors) = self.by_class.get(class.as_ref())
                && class_selectors.iter().any(|selector| !exceptions.contains(selector))
            {
                return true;
            }
        }

        for id in ids {
            if let Some(id_selectors) = self.by_id.get(id.as_ref())
                && id_selectors.iter().any(|selector| !exceptions.contains(selector))
            {
                return true;
            }
        }

        false
    }
}

fn cosmetic_css_for_url(engine: &ContentBlockerEngine, url: &str, classes: &[&str], ids: &[&str]) -> String {
    let (specific, all) = engine.cosmetic_policy.flags(url);
    let resources = engine.engine.url_cosmetic_resources_with_policy(url, 0, specific, all);
    let mut selectors = resources.hide_selectors;

    if !resources.generichide {
        selectors.extend(engine.engine.hidden_class_id_selectors(
            classes.iter().copied(),
            ids.iter().copied(),
            &resources.exceptions,
        ));
        selectors.extend(engine.generic_selector_list_rules.hidden_selectors(
            classes.iter().copied(),
            ids.iter().copied(),
            &resources.exceptions,
        ));
    }

    let mut selector_styles: Vec<_> = selectors
        .into_iter()
        .map(|selector| (selector, "display: none !important".to_string()))
        .collect();

    for action in resources.procedural_actions {
        let Ok(filter) = serde_json::from_str::<adblock::cosmetic_filter_cache::ProceduralOrActionFilter>(&action)
        else {
            continue;
        };
        if let Some((selector, style)) = filter.as_css() {
            selector_styles.push((selector, style));
        }
    }

    selector_styles.sort_unstable();
    selector_styles.dedup();

    let mut css = String::new();
    for (selector, style) in selector_styles {
        css.push_str(&selector);
        css.push_str(" { ");
        css.push_str(&style);
        if !style.trim_end().ends_with(';') {
            css.push(';');
        }
        css.push_str(" }\n");
    }
    css
}

fn has_generic_cosmetic_selectors_for_url(
    engine: &ContentBlockerEngine,
    url: &str,
    classes: &[&str],
    ids: &[&str],
) -> bool {
    let (specific, all) = engine.cosmetic_policy.flags(url);
    let resources = engine.engine.url_cosmetic_resources_with_policy(url, 0, specific, all);
    if resources.generichide {
        return false;
    }

    !engine
        .engine
        .hidden_class_id_selectors(classes.iter().copied(), ids.iter().copied(), &resources.exceptions)
        .is_empty()
        || engine.generic_selector_list_rules.has_hidden_selectors_for_class_or_id(
            classes.iter().copied(),
            ids.iter().copied(),
            &resources.exceptions,
        )
}

/// An engine for `rules`, which C++ frees with `rust_content_blocker_free`, or null where the rules are not UTF-8.
#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_create(rules: ak::Span<'_, u8>) -> *mut ContentBlockerEngine {
    abort_on_panic(|| {
        adblock::regex_manager::reset_host_regex_budget();
        let Some(rules) = as_str(rules) else {
            return std::ptr::null_mut();
        };

        let sources = if rules.trim_start().starts_with('{') {
            let Ok(bundle) = serde_json::from_str::<FilterBundle>(rules) else { return std::ptr::null_mut(); };
            if bundle.version != 1 || bundle.lists.len() > 1024 || bundle.lists.iter().any(|s| s.name.len() > 4096) {
                return std::ptr::null_mut();
            }
            bundle.lists
        } else {
            // Raw rules remain supported for tests and native embedders, with no
            // authority to use trusted scriptlets.
            vec![FilterSource { name: String::new(), trusted: false, text: rules.to_owned(), user_owned: false }]
        };
        let combined = sources.iter().map(|s| s.text.as_str()).collect::<Vec<_>>().join("\n");
        let rules = combined.as_str();
        let mut filter_set = adblock::lists::FilterSet::new(true);
        for source in &sources {
            filter_set.add_filter_list(source.text.clone(), adblock::lists::ParseOptions {
                permissions: adblock::resources::PermissionMask::from_bits(u8::from(source.trusted)),
                ..Default::default()
            });
        }
        let mut engine = adblock::engine::Engine::new_with_filter_set(filter_set);
        let Ok(resources) = serde_json::from_str::<Vec<adblock::resources::Resource>>(DERPBIRD_REDIRECT_RESOURCES) else {
            return std::ptr::null_mut();
        };
        engine.use_resources(resources);
        let generic_selector_list_rules = GenericSelectorListRules::from_rules(rules);
        let dynamic_text = sources.iter().filter(|s| s.trusted && s.user_owned)
            .flat_map(|s| s.text.lines().filter_map(|line| line.strip_prefix("!#derpbird-dynamic "))).collect::<Vec<_>>().join("\n");
        let engine = ContentBlockerEngine {
            has_ip_addresses: sources.iter().any(|source| source.text.lines().any(|line| !line.starts_with('!') && (line.contains("$ipaddress=") || line.contains(",ipaddress=")))),
            request_headers: adblock::response_headers::ResponseHeaderEngine::new_for_sources(sources.iter().map(|s| (s.text.as_str(), s.name.as_str())), true),
            cosmetic_policy: adblock::cosmetic_policy::CosmeticPolicy::new(rules),
            modifiers: adblock::response_modifiers::ResponseModifiers::new(sources.iter().map(|s| (s.text.as_str(), s.trusted, s.name.as_str()))),
            dynamic: adblock::dynamic_filtering::DynamicFiltering::new(&dynamic_text),
            canonical_exceptions: canonical_exception_engine(rules),
            url_skip: adblock::url_skip::UrlSkipEngine::new(sources.iter().map(|s| (s.text.as_str(), s.trusted))),
            sources: sources.iter().map(|s| s.name.clone()).collect(),
            engine,
            generic_selector_list_rules,
            response_headers: response_header_engine(rules),
            header_attribution: HeaderAttribution::new(&sources),
            conditional_headers: adblock::response_headers::ResponseHeaderEngine::new_for_sources(sources.iter().map(|s| (s.text.as_str(), s.name.as_str())), false),
            html_filters: adblock::html_filtering::HtmlFilterEngine::new(rules),
        };
        Box::into_raw(Box::new(engine))
    })
}

/// # Safety
/// - `engine` must be null or a valid pointer returned by `rust_content_blocker_create`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_content_blocker_free(engine: *mut ContentBlockerEngine) {
    abort_on_panic(|| {
        adblock::regex_manager::reset_host_regex_budget();
        if !engine.is_null() {
            drop(unsafe { Box::from_raw(engine) });
        }
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_check(
    engine: Option<&ContentBlockerEngine>,
    url: ak::Span<'_, u8>,
    source_url: ak::Span<'_, u8>,
    request_type: ak::Span<'_, u8>,
    request_method: ak::Span<'_, u8>, top_context: ak::Span<'_, u8>,
    ip_addresses: ak::Span<'_, u8>,
) -> ContentBlockerDecision {
    abort_on_panic(|| {
        adblock::regex_manager::reset_host_regex_budget();
        let Some(engine) = engine else {
            return ContentBlockerDecision::allow();
        };
        let Some(url) = as_str(url) else {
            return ContentBlockerDecision::allow();
        };
        let Some(source_url) = as_str(source_url) else {
            return ContentBlockerDecision::allow();
        };
        let Some(request_type) = as_str(request_type) else {
            return ContentBlockerDecision::allow();
        };

        let Some(request_method) = as_str(request_method) else {
            return ContentBlockerDecision::allow();
        };
        let top_source = as_str(top_context).filter(|value| !value.is_empty()).unwrap_or(source_url);
        let Ok(request) = adblock::request::Request::new(url, source_url, request_type, request_method).map(|request| request.with_top_url(top_source).with_ip_addresses(as_str(ip_addresses).unwrap_or_default())) else {
            return ContentBlockerDecision::allow();
        };

        if let Some((action, rule)) = engine.dynamic.check(&request, top_source) {
            match action {
                adblock::dynamic_filtering::Action::Block => return ContentBlockerDecision { blocked: true, matched_rule: rule_to_ffi(format!("dynamic: {}", rule)), ..ContentBlockerDecision::allow() },
                adblock::dynamic_filtering::Action::Allow => return ContentBlockerDecision::allow(),
                adblock::dynamic_filtering::Action::Noop => {},
            }
        }
        let mut result = engine.engine.check_network_request(&request);
        if request_type == "popunder" && !result.should_block() {
            // Popunders may be described by narrow popup rules. Never apply broad
            // popup/path-only rules to ordinary opener navigation.
            let authority = url.find("://").map(|i| i + 3).unwrap_or(0);
            let origin_end = url[authority..].find(['/', '?', '#']).map_or(url.len(), |i| authority + i);
            let origin = format!("{}/", &url[..origin_end]);
            for candidate in [url, origin.as_str()] {
                let Ok(mut popup) = adblock::request::Request::new(candidate, source_url, "popup", request_method) else { continue; };
                popup = popup.with_top_url(top_source).with_ip_addresses(as_str(ip_addresses).unwrap_or_default());
                popup.hostname_only_filter = true;
                let matched = engine.engine.check_network_request_subset(&popup, false, true);
                if matched.filter.is_some() || matched.exception.is_some() { result = matched; break; }
            }
        }
        let blocked = result.should_block();
        if let Some((rewritten, rule)) = engine.url_skip.rewrite(&request, url, blocked) {
            return ContentBlockerDecision { trusted_url_skip: true, rewritten_url: string_to_ffi(rewritten), matched_rule: rule_to_ffi(rule), ..ContentBlockerDecision::allow() };
        }
        ContentBlockerDecision {
            trusted_url_skip: false,
            blocked,
            matched_rule: rule_to_ffi({
                if !blocked && result.rewritten_url.is_some() {
                    result.rewritten_rules.iter().map(|info| source_rule(engine, info)).collect::<Vec<_>>().join("\n")
                } else {
                    (if blocked { result.filter.as_ref() } else { result.exception.as_ref() }).map(|info| source_rule(engine, info)).unwrap_or_default()
                }
            }),
            rewritten_url: string_to_ffi(if blocked { String::new() } else { result.rewritten_url.unwrap_or_default() }),
            // redirect-rule alone must never replace an allowed request.
            redirect_data_url: string_to_ffi(if blocked { result.redirect.unwrap_or_default() } else { String::new() }),
        }
    })
}

/// Selected at response time; the caller appends this policy without replacing
/// any policy supplied by the server. Only document/subdocument requests qualify.
#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_csp(
    engine: Option<&ContentBlockerEngine>,
    url: ak::Span<'_, u8>,
    source_url: ak::Span<'_, u8>,
    request_type: ak::Span<'_, u8>,
    request_method: ak::Span<'_, u8>, top_context: ak::Span<'_, u8>,
    ip_addresses: ak::Span<'_, u8>,
) -> ContentBlockerString {
    abort_on_panic(|| {
        adblock::regex_manager::reset_host_regex_budget();
        let (Some(engine), Some(url), Some(source_url), Some(request_type), Some(method)) =
            (engine, as_str(url), as_str(source_url), as_str(request_type), as_str(request_method))
        else { return string_to_ffi(String::new()); };
        let top_source = as_str(top_context).filter(|value| !value.is_empty()).unwrap_or(source_url);
        let Ok(request) = adblock::request::Request::new(url, source_url, request_type, method).map(|request| request.with_top_url(top_source).with_ip_addresses(as_str(ip_addresses).unwrap_or_default()))
        else { return string_to_ffi(String::new()); };
        if matches!(engine.dynamic.check(&request, top_source), Some((adblock::dynamic_filtering::Action::Allow, _))) { return string_to_ffi(String::new()); }
        let mut policy = engine.engine.get_csp_directives(&request).unwrap_or_default();
        for (kind, directive) in [("inline-script", "script-src * blob: data: 'unsafe-eval'"), ("inline-font", "font-src *")] {
            if let Ok(inline) = adblock::request::Request::new(url, source_url, kind, method).map(|request| request.with_top_url(top_source).with_ip_addresses(as_str(ip_addresses).unwrap_or_default())) {
                let blocked = match engine.dynamic.check(&inline, top_source) {
                    Some((adblock::dynamic_filtering::Action::Block, _)) => true,
                    Some((adblock::dynamic_filtering::Action::Allow, _)) => false,
                    _ => engine.engine.check_network_request(&inline).should_block(),
                };
                if blocked {
                    if !policy.is_empty() { policy.push_str(", "); }
                    policy.push_str(directive);
                }
            }
        }
        string_to_ffi(policy)
    })
}

/// Scriptlets run once in a private lexical scope in the document's page realm.
#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_scriptlets(
    engine: Option<&ContentBlockerEngine>,
    url: ak::Span<'_, u8>,
    isolated: bool,
) -> ContentBlockerString {
    abort_on_panic(|| {
        adblock::regex_manager::reset_host_regex_budget();
        let Some(engine) = engine else { return string_to_ffi(String::new()); };
        let Some(url) = as_str(url) else { return string_to_ffi(String::new()); };
        let source = if isolated {
            let (specific, all) = engine.cosmetic_policy.flags(url);
            let resources = engine.engine.url_cosmetic_resources_with_policy(url, 2, specific, all);
            let mut source = resources.isolated_script;
            // Rules expressible as ordinary CSS already use the native stylesheet
            // path; do not create a JS world/observer merely to apply them twice.
            let mut rules: Vec<_> = resources.procedural_actions.into_iter().filter(|rule| {
                serde_json::from_str::<adblock::cosmetic_filter_cache::ProceduralOrActionFilter>(rule)
                    .is_ok_and(|filter| filter.as_css().is_none())
            }).collect();
            if !rules.is_empty() {
                rules.sort_unstable();
                source.push_str(DERPBIRD_PROCEDURAL_RUNTIME);
                source.push_str("\n__derpbirdInstallProcedural(");
                source.push_str(&serde_json::to_string(&rules).unwrap());
                source.push_str(");");
            }
            source
        } else {
            engine.engine.url_scriptlet_source(url, false)
        };
        if source.is_empty() { return string_to_ffi(String::new()); }
        string_to_ffi(format!("(() => {{ const scriptletGlobals = Object.create(null);\n{source}\n}})();"))
    })
}

/// C++ frees the returned string with `rust_content_blocker_free_string`.
#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_cosmetic_css(
    engine: Option<&ContentBlockerEngine>,
    url: ak::Span<'_, u8>,
    classes: ak::Span<'_, u8>,
    ids: ak::Span<'_, u8>,
) -> ContentBlockerString {
    abort_on_panic(|| {
        adblock::regex_manager::reset_host_regex_budget();
        let Some(engine) = engine else {
            return string_to_ffi(String::new());
        };
        let Some(url) = as_str(url) else {
            return string_to_ffi(String::new());
        };
        let Some(classes) = as_str(classes) else {
            return string_to_ffi(String::new());
        };
        let Some(ids) = as_str(ids) else {
            return string_to_ffi(String::new());
        };

        let classes: Vec<_> = classes.lines().collect();
        let ids: Vec<_> = ids.lines().collect();
        string_to_ffi(cosmetic_css_for_url(engine, url, &classes, &ids))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_has_generic_cosmetic_selectors(
    engine: Option<&ContentBlockerEngine>,
    url: ak::Span<'_, u8>,
    classes: ak::Span<'_, u8>,
    ids: ak::Span<'_, u8>,
) -> bool {
    abort_on_panic(|| {
        adblock::regex_manager::reset_host_regex_budget();
        let Some(engine) = engine else {
            return false;
        };
        let Some(url) = as_str(url) else {
            return false;
        };
        let Some(classes) = as_str(classes) else {
            return false;
        };
        let Some(ids) = as_str(ids) else {
            return false;
        };

        let classes: Vec<_> = classes.lines().collect();
        let ids: Vec<_> = ids.lines().collect();
        has_generic_cosmetic_selectors_for_url(engine, url, &classes, &ids)
    })
}

/// # Safety
/// - `data` and `length` must match a string returned by `rust_content_blocker_cosmetic_css`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_content_blocker_free_string(data: *mut u8, length: usize) {
    abort_on_panic(|| {
        adblock::regex_manager::reset_host_regex_budget();
        if !data.is_null() {
            drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(data, length)) });
        }
    });
}

// Keep response-header scope matching in the same indexed cosmetic matcher,
// separate from page CSS so these synthetic selectors can never reach the DOM.
const RESPONSE_HEADERS: [&str; 4] = ["location", "refresh", "report-to", "set-cookie"];
const RESPONSE_CLASSES: [&str; 4] = ["derpbird-rh-0", "derpbird-rh-1", "derpbird-rh-2", "derpbird-rh-3"];
fn response_header_engine(rules: &str) -> Option<adblock::engine::Engine> {
    let mut selected = String::new();
    for line in rules.lines().map(str::trim) {
        let Some((scope, selector, exception)) = adblock::filters::cosmetic::split_cosmetic_rule(line) else { continue; };
        let Some(selector) = selector.strip_prefix("^responseheader(") else { continue; };
        let separator = if exception { "#@#" } else { "##" };
        let Some(header) = selector.strip_suffix(')') else { continue; };
        let Some(index) = RESPONSE_HEADERS.iter().position(|candidate| *candidate == header) else { continue; };
        selected.push_str(&format!("{scope}{separator}.{}\n", RESPONSE_CLASSES[index]));
    }
    if selected.is_empty() { return None; }
    let mut filters = adblock::lists::FilterSet::new(false);
    filters.add_filter_list(selected, adblock::lists::ParseOptions::default());
    Some(adblock::engine::Engine::new_with_filter_set(filters))
}
#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_response_headers(engine: Option<&ContentBlockerEngine>, url: ak::Span<'_, u8>) -> u8 {
    abort_on_panic(|| {
        adblock::regex_manager::reset_host_regex_budget();
        let (Some(engine), Some(url)) = (engine.and_then(|e| e.response_headers.as_ref()), as_str(url)) else { return 0; };
        let resources = engine.url_cosmetic_resources_without_scriptlets(url);
        let mut selectors = resources.hide_selectors;
        selectors.extend(engine.hidden_class_id_selectors(RESPONSE_CLASSES, std::iter::empty::<&str>(), &resources.exceptions));
        RESPONSE_CLASSES.iter().enumerate().fold(0, |mask, (index, class)| {
            if selectors.contains(&format!(".{class}")) { mask | (1 << index) } else { mask }
        })
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_has_header_filters(engine: Option<&ContentBlockerEngine>) -> bool {
    abort_on_panic(|| engine.is_some_and(|e| !e.conditional_headers.is_empty()))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_block_response(
    engine: Option<&ContentBlockerEngine>, url: ak::Span<'_, u8>, source: ak::Span<'_, u8>,
    request_type: ak::Span<'_, u8>, method: ak::Span<'_, u8>, headers: ak::Span<'_, u8>, top_context: ak::Span<'_, u8>,
    ip_addresses: ak::Span<'_, u8>,
) -> ContentBlockerDecision {
    abort_on_panic(|| {
        adblock::regex_manager::reset_host_regex_budget();
        let (Some(engine), Some(url), Some(source), Some(request_type), Some(method), Some(headers)) =
            (engine, as_str(url), as_str(source), as_str(request_type), as_str(method), as_str(headers))
        else { return ContentBlockerDecision::allow(); };
        if engine.conditional_headers.is_empty() { return ContentBlockerDecision::allow(); }
        let top_source = as_str(top_context).filter(|value| !value.is_empty()).unwrap_or(source);
        let Ok(request) = adblock::request::Request::new(url, source, request_type, method).map(|request| request.with_top_url(top_source).with_ip_addresses(as_str(ip_addresses).unwrap_or_default())) else { return ContentBlockerDecision::allow(); };
        let Ok(headers) = serde_json::from_str::<Vec<(String, String)>>(headers) else { return ContentBlockerDecision::allow(); };
        if matches!(engine.dynamic.check(&request, top_source), Some((adblock::dynamic_filtering::Action::Allow, _))) { return ContentBlockerDecision::allow(); }
        let exception = engine.engine.check_network_request_subset(&request, false, true).exception.is_some();
        let (blocked, rule) = engine.conditional_headers.check(&request, &headers, exception);
        ContentBlockerDecision { blocked, matched_rule: rule_to_ffi(rule), ..ContentBlockerDecision::allow() }
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_html_rules(engine: Option<&ContentBlockerEngine>, url: ak::Span<'_, u8>) -> ContentBlockerString {
    abort_on_panic(|| {
        adblock::regex_manager::reset_host_regex_budget();
        let (Some(engine), Some(url)) = (engine, as_str(url)) else { return string_to_ffi(String::new()); };
        let rules = engine.html_filters.rules_for_url(url);
        if rules.is_empty() { return string_to_ffi(String::new()); }
        string_to_ffi(serde_json::to_string(&rules).unwrap_or_default())
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_html_runtime() -> ContentBlockerString {
    abort_on_panic(|| string_to_ffi(DERPBIRD_HTML_FILTER_RUNTIME.to_owned()))
}

// $cname exceptions affect the unmasked check only. They must never become
// ordinary allow rules for the visible URL.
fn canonical_exception_engine(rules: &str) -> adblock::engine::Engine {
    let mut exceptions = String::new();
    for line in rules.lines().map(str::trim) {
        if !line.starts_with("@@") { continue; }
        let Some((pattern, options)) = line.rsplit_once('$') else { continue; };
        if !options.split(',').any(|option| option == "cname") { continue; }
        let remaining = options.split(',').filter(|option| *option != "cname").collect::<Vec<_>>();
        exceptions.push_str(pattern);
        if !remaining.is_empty() { exceptions.push('$'); exceptions.push_str(&remaining.join(",")); }
        exceptions.push('\n');
    }
    let mut filters = adblock::lists::FilterSet::new(false);
    filters.add_filter_list(exceptions, Default::default());
    adblock::engine::Engine::new_with_filter_set(filters)
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_canonical_url_exempt(
    engine: Option<&ContentBlockerEngine>, url: ak::Span<'_, u8>, source: ak::Span<'_, u8>,
    request_type: ak::Span<'_, u8>, method: ak::Span<'_, u8>, top_context: ak::Span<'_, u8>,
    ip_addresses: ak::Span<'_, u8>,
) -> bool {
    abort_on_panic(|| {
        adblock::regex_manager::reset_host_regex_budget();
        let (Some(engine), Some(url), Some(source), Some(request_type), Some(method)) =
            (engine, as_str(url), as_str(source), as_str(request_type), as_str(method)) else { return true; };
        let top_source = as_str(top_context).filter(|value| !value.is_empty()).unwrap_or(source);
        let Ok(request) = adblock::request::Request::new(url, source, request_type, method).map(|request| request.with_top_url(top_source).with_ip_addresses(as_str(ip_addresses).unwrap_or_default())) else { return true; };
        matches!(engine.dynamic.check(&request, top_source), Some((adblock::dynamic_filtering::Action::Allow, _)))
            || engine.engine.check_network_request_subset(&request, false, true).exception.is_some()
            || engine.canonical_exceptions.check_network_request_subset(&request, false, true).exception.is_some()
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_install_regex(evaluator: unsafe extern "C" fn(*const u8, usize, bool, *const u8, usize, u8, *mut u64, *mut u8, usize, *mut usize) -> i8) {
    adblock::regex_manager::install_host_regex(evaluator);
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_response_modifiers(
    engine: Option<&ContentBlockerEngine>, url: ak::Span<'_, u8>, source_url: ak::Span<'_, u8>, request_type: ak::Span<'_, u8>, method: ak::Span<'_, u8>, permissions: bool, top_context: ak::Span<'_, u8>,
    ip_addresses: ak::Span<'_, u8>,
) -> ContentBlockerString {
    abort_on_panic(|| {
        adblock::regex_manager::reset_host_regex_budget();
        let (Some(engine), Some(url), Some(source), Some(kind), Some(method)) = (engine, as_str(url), as_str(source_url), as_str(request_type), as_str(method)) else { return string_to_ffi(String::new()); };
        let top_source = as_str(top_context).filter(|value| !value.is_empty()).unwrap_or(source);
        let Ok(request) = adblock::request::Request::new(url, source, kind, method).map(|request| request.with_top_url(top_source).with_ip_addresses(as_str(ip_addresses).unwrap_or_default())) else { return string_to_ffi(String::new()); };
        if matches!(engine.dynamic.check(&request, top_source), Some((adblock::dynamic_filtering::Action::Allow, _))) { return string_to_ffi(String::new()); }
        string_to_ffi(if permissions { engine.modifiers.permissions(&request) } else { serde_json::to_string(&engine.modifiers.replacements(&request)).unwrap_or_default() })
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_block_request_headers(
    engine: Option<&ContentBlockerEngine>, url: ak::Span<'_, u8>, source: ak::Span<'_, u8>,
    request_type: ak::Span<'_, u8>, method: ak::Span<'_, u8>, headers: ak::Span<'_, u8>, top_context: ak::Span<'_, u8>,
    ip_addresses: ak::Span<'_, u8>,
) -> ContentBlockerDecision {
    abort_on_panic(|| {
        adblock::regex_manager::reset_host_regex_budget();
        let (Some(engine), Some(url), Some(source), Some(request_type), Some(method), Some(headers)) =
            (engine, as_str(url), as_str(source), as_str(request_type), as_str(method), as_str(headers))
        else { return ContentBlockerDecision::allow(); };
        if engine.request_headers.is_empty() { return ContentBlockerDecision::allow(); }
        let top_source = as_str(top_context).filter(|value| !value.is_empty()).unwrap_or(source);
        let Ok(request) = adblock::request::Request::new(url, source, request_type, method).map(|request| request.with_top_url(top_source).with_ip_addresses(as_str(ip_addresses).unwrap_or_default())) else { return ContentBlockerDecision::allow(); };
        let Ok(headers) = serde_json::from_str::<Vec<(String, String)>>(headers) else { return ContentBlockerDecision::allow(); };
        if matches!(engine.dynamic.check(&request, top_source), Some((adblock::dynamic_filtering::Action::Allow, _))) { return ContentBlockerDecision::allow(); }
        let exception = engine.engine.check_network_request_subset(&request, false, true).exception.is_some();
        let (blocked, rule) = engine.request_headers.check(&request, &headers, exception);
        ContentBlockerDecision { blocked, matched_rule: rule_to_ffi(rule), ..ContentBlockerDecision::allow() }
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_has_request_header_filters(engine: Option<&ContentBlockerEngine>) -> bool { abort_on_panic(|| engine.is_some_and(|e| !e.request_headers.is_empty())) }

#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_uri_transforms(engine: Option<&ContentBlockerEngine>, url: ak::Span<'_, u8>, source: ak::Span<'_, u8>, kind: ak::Span<'_, u8>, method: ak::Span<'_, u8>, top_context: ak::Span<'_, u8>,
    ip_addresses: ak::Span<'_, u8>,
) -> ContentBlockerString {
    abort_on_panic(|| {
        adblock::regex_manager::reset_host_regex_budget();
        let (Some(engine), Some(url), Some(source), Some(kind), Some(method)) = (engine, as_str(url), as_str(source), as_str(kind), as_str(method)) else { return string_to_ffi(String::new()); };
        let top_source = as_str(top_context).filter(|value| !value.is_empty()).unwrap_or(source);
        let Ok(request) = adblock::request::Request::new(url, source, kind, method).map(|request| request.with_top_url(top_source).with_ip_addresses(as_str(ip_addresses).unwrap_or_default())) else { return string_to_ffi(String::new()); };
        if matches!(engine.dynamic.check(&request, top_source), Some((adblock::dynamic_filtering::Action::Allow, _))) { return string_to_ffi(String::new()); }
        string_to_ffi(serde_json::to_string(&engine.modifiers.transforms(&request)).unwrap_or_default())
    })
}

fn source_rule(engine: &ContentBlockerEngine, info: &adblock::sourcemap::FilterRuleDebugInfo) -> String {
    let mut rule = info.raw_line.clone().unwrap_or_default();
    if let Some(location) = &info.source_location {
        if let Some(name) = engine.sources.get(location.source_index as usize).filter(|s| !s.is_empty()) {
            rule.push_str(&format!("\n! Source: {} · line {}", name.replace(['\r', '\n'], " "), location.line_number + 1));
        }
    }
    adblock::sourcemap::bounded_rule_text(&rule)
}

// Diagnostic selectors have unique ids, so equally scoped rules retain their
// source identity. The authoritative removal mask gates exceptions separately.
struct HeaderAttribution { engine: adblock::engine::Engine, rules: Vec<(usize, String)> }
impl HeaderAttribution {
    fn new(sources: &[FilterSource]) -> Self {
        let mut filters = adblock::lists::FilterSet::new(false);
        let mut rules = Vec::new();
        let mut transformed = String::new();
        for source in sources {
            for (line_number, line) in source.text.lines().map(str::trim).enumerate() {
                let Some((scope, selector, false)) = adblock::filters::cosmetic::split_cosmetic_rule(line) else { continue; };
                let Some(selector) = selector.strip_prefix("^responseheader(") else { continue; };
                let Some(header) = selector.strip_suffix(')') else { continue; };
                let Some(index) = RESPONSE_HEADERS.iter().position(|name| *name == header) else { continue; };
                transformed.push_str(&format!("{scope}##.derpbird-header-source-{}\n", rules.len()));
                rules.push((index, adblock::sourcemap::bounded_rule_text(&format!("{line}\n! Source: {} · line {}", source.name.replace(['\r', '\n'], " "), line_number + 1))));
            }
        }
        filters.add_filter_list(transformed, adblock::lists::ParseOptions::default());
        Self { engine: adblock::engine::Engine::new_with_filter_set(filters), rules }
    }
    fn rules_for_url(&self, url: &str, index: usize) -> Vec<String> {
        let resources = self.engine.url_cosmetic_resources_without_scriptlets(url);
        let classes: Vec<_> = self.rules.iter().enumerate().filter(|(_, (kind, _))| *kind == index).map(|(i, _)| format!("derpbird-header-source-{i}")).collect();
        let mut selectors = resources.hide_selectors;
        selectors.extend(self.engine.hidden_class_id_selectors(classes.iter().map(String::as_str), std::iter::empty::<&str>(), &resources.exceptions));
        self.rules.iter().enumerate().filter(|(i, (kind, _))| *kind == index && selectors.contains(&format!(".derpbird-header-source-{i}"))).map(|(_, (_, rule))| rule.clone()).collect()
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_action_rules(
    engine: Option<&ContentBlockerEngine>, url_span: ak::Span<'_, u8>, source: ak::Span<'_, u8>, kind: ak::Span<'_, u8>, method: ak::Span<'_, u8>, top_context: ak::Span<'_, u8>, action: u8,
    ip_addresses: ak::Span<'_, u8>,
) -> ContentBlockerString {
    abort_on_panic(|| {
        adblock::regex_manager::reset_host_regex_budget();
        let (Some(engine), Some(url), Some(source), Some(kind), Some(method)) = (engine, as_str(url_span), as_str(source), as_str(kind), as_str(method)) else { return string_to_ffi(String::new()); };
        let top = as_str(top_context).filter(|v| !v.is_empty()).unwrap_or(source);
        let Ok(request) = adblock::request::Request::new(url, source, kind, method).map(|r| r.with_top_url(top).with_ip_addresses(as_str(ip_addresses).unwrap_or_default())) else { return string_to_ffi(String::new()); };
        let mut rules = Vec::new();
        if action >= 2 && action < 6 {
            let index = usize::from(action - 2);
            if rust_content_blocker_response_headers(Some(engine), url_span) & (1 << index) != 0 {
                rules = engine.header_attribution.rules_for_url(url, index);
            }
        } else if !matches!(engine.dynamic.check(&request, top), Some((adblock::dynamic_filtering::Action::Allow, _))) {
            if action == 1 { rules = engine.modifiers.permission_details(&request).1; }
            if action == 0 {
                if let Some((_, selected)) = engine.engine.get_csp_details(&request) {
                    rules.extend(selected.iter().map(|info| source_rule(engine, info)));
                }
                for inline_kind in ["inline-script", "inline-font"] {
                    if let Ok(inline) = adblock::request::Request::new(url, source, inline_kind, method).map(|r| r.with_top_url(top).with_ip_addresses(as_str(ip_addresses).unwrap_or_default())) {
                        match engine.dynamic.check(&inline, top) {
                            Some((adblock::dynamic_filtering::Action::Block, rule)) => rules.push(format!("dynamic: {rule}")),
                            Some((adblock::dynamic_filtering::Action::Allow, _)) => {},
                            _ => { let result = engine.engine.check_network_request(&inline); if result.should_block() { if let Some(info) = result.filter { rules.push(source_rule(engine, &info)); } } },
                        }
                    }
                }
            }
        }
        rules.sort(); rules.dedup(); rules.truncate(128);
        string_to_ffi(serde_json::to_string(&rules).unwrap_or_default())
    })
}


#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_has_ip_address_filters(engine: Option<&ContentBlockerEngine>) -> bool {
    abort_on_panic(|| engine.is_some_and(|engine| engine.has_ip_addresses))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_content_blocker_check_ip_address(
    engine: Option<&ContentBlockerEngine>, url: ak::Span<'_, u8>, source: ak::Span<'_, u8>,
    kind: ak::Span<'_, u8>, method: ak::Span<'_, u8>, address: ak::Span<'_, u8>, top: ak::Span<'_, u8>,
) -> ContentBlockerDecision {
    rust_content_blocker_check(engine, url, source, kind, method, top, address)
}
