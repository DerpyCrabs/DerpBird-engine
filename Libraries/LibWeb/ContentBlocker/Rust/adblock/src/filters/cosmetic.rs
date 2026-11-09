//! Filters that take effect at a page-content level, including CSS selector-based filtering and
//! content script injection.

use memchr::{memchr as find_char, memrchr as find_char_reverse};
use serde::{Deserialize, Serialize};
use thiserror::Error;

use crate::resources::PermissionMask;
use crate::utils::Hash;

use css_validation::{is_valid_css_style, validate_css_selector};

#[derive(Debug, Error, PartialEq)]
pub enum CosmeticFilterError {
    #[error("punycode error")]
    PunycodeError,
    #[error("invalid action specifier")]
    InvalidActionSpecifier,
    #[error("unsupported syntax")]
    UnsupportedSyntax,
    #[error("missing sharp")]
    MissingSharp,
    #[error("invalid css style")]
    InvalidCssStyle,
    #[error("invalid css selector")]
    InvalidCssSelector,
    #[error("generic unhide")]
    GenericUnhide,
    #[error("generic script inject")]
    GenericScriptInject,
    #[error("procedural and action filters cannot be generic")]
    GenericAction,
    #[error("double negation")]
    DoubleNegation,
    #[error("empty rule")]
    EmptyRule,
    #[error("html filtering is unsupported")]
    HtmlFilteringUnsupported,
    #[error("scriptlet args could not be parsed")]
    InvalidScriptletArgs,
    #[error("location modifiers are unsupported")]
    LocationModifiersUnsupported,
    #[error("procedural filters can only accept a single CSS selector")]
    ProceduralFilterWithMultipleSelectors,
}

/// Refer to <https://github.com/uBlockOrigin/uBlock-issues/wiki/Static-filter-syntax#action-operators>
#[derive(PartialEq, Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", content = "arg")]
#[serde(rename_all = "kebab-case")]
pub enum CosmeticFilterAction {
    /// Rules with a remove action, e.g. `example.com##.ad:remove()`.
    ///
    /// Matching elements are to be removed from the DOM.
    Remove,
    /// Rules with a custom style for an element, e.g. `example.com##.ad:style(margin: 0)`.
    ///
    /// Argument is one or more CSS property declarations, separated by the standard ;. Some
    /// characters, strings, and values are forbidden.
    ///
    /// The specified CSS styling should be applied to matching elements in the DOM.
    Style(String),
    /// Rules with a remove attribute action, e.g. `example.com##.ad:remove-attr(onclick)`.
    ///
    /// Argument is the name of an HTML attribute.
    ///
    /// The attribute should be removed from matching elements in the DOM.
    RemoveAttr(String),
    /// Rules with a remove class action, e.g. `example.com##.ad:remove-class(advert)`.
    ///
    /// The parameter is the name of a CSS class.
    ///
    /// The class should be removed from matching elements in the DOM.
    RemoveClass(String),
}

impl CosmeticFilterAction {
    fn new_style(style: &str) -> Result<Self, CosmeticFilterError> {
        if !is_valid_css_style(style) {
            return Err(CosmeticFilterError::InvalidCssStyle);
        }
        Ok(Self::Style(style.to_string()))
    }

    fn new_remove_attr(attr: &str) -> Result<Self, CosmeticFilterError> {
        Ok(CosmeticFilterAction::RemoveAttr(Self::name_pattern(attr)?))
    }

    fn new_remove_class(class: &str) -> Result<Self, CosmeticFilterError> {
        Ok(CosmeticFilterAction::RemoveClass(Self::name_pattern(class)?))
    }

    /// Preserve ECMAScript patterns for the renderer instead of validating them
    /// with Rust's different regex language. The adapter compiles each pattern
    /// before admitting its rule, so malformed patterns cannot abort a commit.
    fn name_pattern(arg: &str) -> Result<String, CosmeticFilterError> {
        let arg = arg.trim();
        if arg.is_empty() || arg.len() > 16 * 1024 || arg.contains('\0') {
            return Err(CosmeticFilterError::InvalidActionSpecifier);
        }
        Ok(arg.to_string())
    }
}

bitflags::bitflags! {
    /// Boolean flags for cosmetic filter rules.
    #[derive(Serialize, Deserialize, Clone, Debug)]
    #[serde(transparent)]
    pub struct CosmeticFilterMask: u8 {
        const UNHIDE = 1 << 0;
        const SCRIPT_INJECT = 1 << 1;

        // Careful with checking for NONE - will always match
        const NONE = 0;
    }
}

/// Struct representing a parsed cosmetic filter rule.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CosmeticFilter {
    pub entities: Option<Vec<Hash>>,
    pub hostnames: Option<Vec<Hash>>,
    pub mask: CosmeticFilterMask,
    pub not_entities: Option<Vec<Hash>>,
    pub not_hostnames: Option<Vec<Hash>>,
    pub raw_line: Option<Box<String>>,
    pub selector: Vec<CosmeticFilterOperator>,
    pub action: Option<CosmeticFilterAction>,
    pub permission: PermissionMask,
    #[serde(default)]
    pub regex_domains: Vec<String>,
    #[serde(default)]
    pub not_regex_domains: Vec<String>,
    #[serde(default)]
    pub wildcard_scope: bool,
}

/// Individual parts of a cosmetic filter's selector. Most rules have a CSS selector; some may also
/// have one or more procedural operators.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "type", content = "arg")]
#[serde(rename_all = "kebab-case")]
pub enum CosmeticFilterOperator {
    CssSelector(String),
    Has(Vec<CosmeticFilterOperator>),
    Any(Vec<Vec<CosmeticFilterOperator>>),
    Not(Vec<CosmeticFilterOperator>),
    Matches(Vec<CosmeticFilterOperator>),
    MatchesSelector(String),
    If(Vec<CosmeticFilterOperator>),
    IfNot(Vec<CosmeticFilterOperator>),
    HasText(String),
    MatchesAttr(String),
    MatchesProp(String),
    MatchesCss(String),
    MatchesCssBefore(String),
    MatchesCssAfter(String),
    MatchesMedia(String),
    WatchAttr(String),
    Others(String),
    MatchesPath(String),
    MinTextLength(String),
    Upward(String),
    Xpath(String),
}

pub(crate) enum CosmeticFilterLocationType {
    Entity,
    NotEntity,
    Hostname,
    NotHostname,
    Unsupported,
}

/// Contains hashes of all of the comma separated location items that were populated before the
/// hash separator in a cosmetic filter rule.
#[derive(Default)]
struct CosmeticFilterLocations {
    wildcard_scope: bool,
    regex_domains: Vec<String>,
    not_regex_domains: Vec<String>,
    /// Locations of the form `entity.*`
    entities: Option<Vec<Hash>>,
    /// Locations of the form `~entity.*`
    not_entities: Option<Vec<Hash>>,
    /// Locations of the form `hostname`
    hostnames: Option<Vec<Hash>>,
    /// Locations of the form `~hostname`
    not_hostnames: Option<Vec<Hash>>,
}

// Action-looking text in an attribute, quoted string or nested procedural
// predicate is selector data. Only a top-level action terminates the selector.
fn cosmetic_action_start(value: &str) -> Option<usize> {
    let bytes = value.as_bytes();
    let mut index = 0;
    let mut parentheses = 0usize;
    let mut brackets = 0usize;
    let mut quote = None;
    while index < bytes.len() {
        let byte = bytes[index];
        if byte == b'\\' { index += 2; continue; }
        if let Some(delimiter) = quote {
            if byte == delimiter { quote = None; }
            index += 1; continue;
        }
        if byte == b'"' || byte == b'\'' { quote = Some(byte); index += 1; continue; }
        if bytes[index..].starts_with(b"/*") {
            if let Some(end) = value[index + 2..].find("*/") { index += end + 4; continue; }
            return None;
        }
        if byte == b'/' && parentheses > 0 {
            if let Some(end) = crate::regex_manager::regex_literal_end(&value[index..]) {
                index += end + 1; continue;
            }
        }
        if byte == b':' && parentheses == 0 && brackets == 0 &&
            [":style(", ":remove(", ":remove-attr(", ":remove-class("].iter().any(|token| value[index..].starts_with(token)) {
            return Some(index);
        }
        match byte {
            b'[' => brackets += 1,
            b']' => brackets = brackets.saturating_sub(1),
            b'(' if brackets == 0 => parentheses += 1,
            b')' if brackets == 0 => parentheses = parentheses.saturating_sub(1),
            _ => {}
        }
        index += 1;
    }
    None
}

// Commas and hashes inside a regex are not domain/separator delimiters.
pub(crate) fn cosmetic_separator(line: &str) -> Option<usize> {
    let mut cursor = 0;
    while cursor < line.len() {
        let rest = &line[cursor..];
        let expression = rest.strip_prefix('~').unwrap_or(rest);
        if expression.starts_with('/') {
            let end = crate::regex_manager::regex_literal_end(expression)?;
            cursor += rest.len() - expression.len() + end + 1;
        }
        let tail = &line[cursor..];
        match tail.bytes().position(|b| b == b',' || b == b'#') {
            Some(index) if tail.as_bytes()[index] == b'#' => return Some(cursor + index),
            Some(index) => cursor += index + 1,
            None => return None,
        }
    }
    None
}

/// Split standard cosmetic syntax without mistaking regex contents for markers.
/// Response-stage consumers use this before translating to ordinary cosmetics.
pub fn split_cosmetic_rule(line: &str) -> Option<(&str, &str, bool)> {
    if line.starts_with('!') { return None; }
    let index = cosmetic_separator(line)?;
    let suffix = &line[index..];
    if let Some(selector) = suffix.strip_prefix("#@#") {
        Some((&line[..index], selector, true))
    } else {
        suffix.strip_prefix("##").map(|selector| (&line[..index], selector, false))
    }
}

fn domain_parts(mut value: &str) -> Vec<&str> {
    let mut result = Vec::new();
    while !value.is_empty() {
        let expression = value.strip_prefix('~').unwrap_or(value);
        let scan_from = if expression.starts_with('/') {
            crate::regex_manager::regex_literal_end(expression)
                .map(|end| value.len() - expression.len() + end + 1).unwrap_or(value.len())
        } else { 0 };
        let end = value[scan_from..].find(',').map(|i| scan_from + i).unwrap_or(value.len());
        result.push(&value[..end]);
        value = value.get(end + 1..).unwrap_or("");
    }
    result
}

impl CosmeticFilter {
    #[inline]
    pub(crate) fn locations_before_sharp(
        line: &str,
        sharp_index: usize,
    ) -> impl Iterator<Item = (CosmeticFilterLocationType, &str)> {
        domain_parts(&line[..sharp_index]).into_iter().filter_map(|part| {
            if part.is_empty() {
                return None;
            }
            let hostname = part;
            let negation = hostname.starts_with('~');
            let entity = hostname.ends_with(".*");
            let start = if negation { 1 } else { 0 };
            let end = if entity {
                hostname.len() - 2
            } else {
                hostname.len()
            };
            let location = &hostname[start..end];
            // AdGuard regex syntax
            if location.starts_with('/') {
                return Some((CosmeticFilterLocationType::Unsupported, part));
            }
            Some(match (negation, entity) {
                (true, true) => (CosmeticFilterLocationType::NotEntity, location),
                (true, false) => (CosmeticFilterLocationType::NotHostname, location),
                (false, true) => (CosmeticFilterLocationType::Entity, location),
                (false, false) => (CosmeticFilterLocationType::Hostname, location),
            })
        })
    }

    /// Parses the contents of a cosmetic filter rule up to the `##` or `#@#` separator.
    ///
    /// On success, returns hashes of all the comma separated location items that were populated in
    /// the rule.
    ///
    /// This should only be called if `sharp_index` is greater than 0, in which case all four are
    /// guaranteed to be `None`.
    #[inline]
    fn parse_before_sharp(
        line: &str,
        sharp_index: usize,
    ) -> Result<CosmeticFilterLocations, CosmeticFilterError> {
        let mut entities_vec = vec![];
        let mut not_entities_vec = vec![];
        let mut hostnames_vec = vec![];
        let mut not_hostnames_vec = vec![];

        if line.starts_with('[') {
            return Err(CosmeticFilterError::LocationModifiersUnsupported);
        }

        let mut wildcard_scope = false;
        let mut regex_domains = Vec::new();
        let mut not_regex_domains = Vec::new();
        for (location_type, location) in Self::locations_before_sharp(line, sharp_index) {
            if matches!(location_type, CosmeticFilterLocationType::Hostname) && location == "*" {
                wildcard_scope = true;
                continue;
            }
            if matches!(location_type, CosmeticFilterLocationType::Unsupported) {
                let (negated, expression) = location.strip_prefix('~').map_or((false, location), |s| (true, s));
                crate::regex_manager::reset_host_regex_budget();
                if crate::regex_manager::compile_removeparam_regex(expression).is_none() {
                    return Err(CosmeticFilterError::UnsupportedSyntax);
                }
                if negated { not_regex_domains.push(expression.to_owned()); }
                else { regex_domains.push(expression.to_owned()); }
                continue;
            }
            let hash = if location.is_ascii() {
                crate::utils::fast_hash(location)
            } else {
                match idna::domain_to_ascii(location) {
                    Ok(x) if !x.is_empty() => crate::utils::fast_hash(&x),
                    _ => return Err(CosmeticFilterError::PunycodeError),
                }
            };
            match location_type {
                CosmeticFilterLocationType::NotEntity => not_entities_vec.push(hash),
                CosmeticFilterLocationType::NotHostname => not_hostnames_vec.push(hash),
                CosmeticFilterLocationType::Entity => entities_vec.push(hash),
                CosmeticFilterLocationType::Hostname => hostnames_vec.push(hash),
                CosmeticFilterLocationType::Unsupported => {
                    unreachable!();
                }
            }
        }
        /// Sorts `vec` and wraps it in `Some` if it's not empty, or returns `None` if it is.
        #[inline]
        fn sorted_or_none<T: std::cmp::Ord>(mut vec: Vec<T>) -> Option<Vec<T>> {
            if !vec.is_empty() {
                vec.sort();
                Some(vec)
            } else {
                None
            }
        }

        let entities = sorted_or_none(entities_vec);
        let hostnames = sorted_or_none(hostnames_vec);
        let not_entities = sorted_or_none(not_entities_vec);
        let not_hostnames = sorted_or_none(not_hostnames_vec);

        Ok(CosmeticFilterLocations {
            wildcard_scope,
            regex_domains,
            not_regex_domains,
            entities,
            not_entities,
            hostnames,
            not_hostnames,
        })
    }

    /// Turns a string of the format `.selector { background: red }` into
    /// `Some((".selector", "background: red"))`.
    fn split_abp_brace_suffix(s: &str) -> Option<(&str, &str)> {
        if !s.ends_with('}') {
            return None;
        }
        let open = s.rfind('{')?;
        if open == 0 {
            return None;
        }
        let selector = s[..open].trim_end();
        let body = s[open + 1..s.len() - 1].trim();
        if selector.is_empty() {
            return None;
        }
        Some((selector, body))
    }

    /// Returns `Some(true)` for ` remove: true; ` with optional whitespace and semicolon,
    /// `Some(false)` for the corresponding false construction, or `None` otherwise.
    fn parse_abp_remove_body(body: &str) -> Option<bool> {
        let (key, mut val) = body.split_once(":")?;
        if key.trim() != "remove" {
            return None;
        }
        val = val.trim();
        if let Some(stripped_val) = val.strip_suffix(';') {
            val = stripped_val;
        }
        match val {
            "true" => Some(true),
            "false" => Some(false),
            _ => None,
        }
    }

    /// Parses ABP curly-bracketed style injections and `remove:` directives into corresponding
    /// `CosmeticFilterAction`s.
    fn parse_abp_style_injection(
        after_sharp: &str,
    ) -> Option<Result<(&str, Option<CosmeticFilterAction>), CosmeticFilterError>> {
        let (selector, body) = match Self::split_abp_brace_suffix(after_sharp) {
            Some(parts) => parts,
            None if after_sharp.ends_with('}') => {
                return Some(Err(CosmeticFilterError::InvalidActionSpecifier));
            }
            None => return None,
        };

        Some(match Self::parse_abp_remove_body(body) {
            Some(true) => Ok((selector, Some(CosmeticFilterAction::Remove))),
            Some(false) => Err(CosmeticFilterError::InvalidActionSpecifier),
            None => CosmeticFilterAction::new_style(body).map(|action| (selector, Some(action))),
        })
    }

    /// Parses the contents of a cosmetic filter rule following the `##` or `#@#` separator.
    ///
    /// On success, returns `selector` and `style` according to the rule.
    ///
    /// This should only be called if the rule part after the separator has been confirmed not to
    /// be a script injection rule using `+js()`.
    #[inline]
    fn parse_after_sharp_nonscript(
        after_sharp: &str,
    ) -> Result<(&str, Option<CosmeticFilterAction>), CosmeticFilterError> {
        if after_sharp.starts_with('^') {
            return Err(CosmeticFilterError::HtmlFilteringUnsupported);
        }

        if let Some(result) = Self::parse_abp_style_injection(after_sharp) {
            return result;
        }

        let Some(index) = cosmetic_action_start(after_sharp) else {
            return Ok((after_sharp, None));
        };
        let suffix = &after_sharp[index..];
        let action = if suffix == ":remove()" {
            CosmeticFilterAction::Remove
        } else {
            type Constructor = fn(&str) -> Result<CosmeticFilterAction, CosmeticFilterError>;
            let constructors: [(&str, Constructor); 3] = [
                (":style(", CosmeticFilterAction::new_style),
                (":remove-attr(", CosmeticFilterAction::new_remove_attr),
                (":remove-class(", CosmeticFilterAction::new_remove_class),
            ];
            let (token, constructor) = constructors.into_iter().find(|(token, _)| suffix.starts_with(token))
                .ok_or(CosmeticFilterError::InvalidActionSpecifier)?;
            let argument = suffix.strip_suffix(')').ok_or(CosmeticFilterError::InvalidActionSpecifier)?;
            constructor(&argument[token.len()..])?
        };
        Ok((&after_sharp[..index], Some(action)))
    }

    /// Returns the CSS selector, for rules which only consist of a CSS selector.
    /// If a rule contains procedural operators, this method will return `None`.
    pub fn plain_css_selector(&self) -> Option<&str> {
        assert!(!self.selector.is_empty());
        if self.selector.len() > 1 {
            return None;
        }
        match &self.selector[0] {
            CosmeticFilterOperator::CssSelector(s) => Some(s),
            _ => None,
        }
    }

    /// Parse the rule in `line` into a `CosmeticFilter`. If `debug` is true, the original rule
    /// will be reported in the resulting `CosmeticFilter` struct as well. Use `permission` to
    /// manage the filter's access to scriptlet resources for `+js(...)` injections.
    pub fn parse(
        line: &str,
        debug: bool,
        permission: PermissionMask,
    ) -> Result<CosmeticFilter, CosmeticFilterError> {
        let mut mask = CosmeticFilterMask::NONE;
        if let Some(sharp_index) = cosmetic_separator(line) {
            let after_sharp_index = sharp_index + 1;

            let second_sharp_index = match find_char(b'#', &line.as_bytes()[after_sharp_index..]) {
                Some(i) => i + after_sharp_index,
                None => return Err(CosmeticFilterError::UnsupportedSyntax),
            };

            let mut translate_abp_syntax = false;

            // Consume filter options embedded in the `##` marker:
            let mut between_sharps = &line[after_sharp_index..second_sharp_index];
            if between_sharps.starts_with('@') {
                // Exception marker will always come first.
                if sharp_index == 0 {
                    return Err(CosmeticFilterError::GenericUnhide);
                }
                mask |= CosmeticFilterMask::UNHIDE;
                between_sharps = &between_sharps[1..];
            }
            if between_sharps.starts_with('%') {
                // AdGuard script injection syntax - not supported
                // `#%#` / `#@%#`
                return Err(CosmeticFilterError::UnsupportedSyntax);
            }
            if between_sharps.starts_with('$') {
                // AdGuard `:style` syntax - not supported for now
                // `#$?#` for CSS rules, `#@$?#` — for exceptions
                return Err(CosmeticFilterError::UnsupportedSyntax);
            }
            if between_sharps.starts_with('?') {
                // ABP/ADG extended CSS syntax:
                // - #?# — for element hiding, #@?# — for exceptions
                translate_abp_syntax = true;
                between_sharps = &between_sharps[1..];
            }
            if !between_sharps.is_empty() {
                return Err(CosmeticFilterError::UnsupportedSyntax);
            }

            let suffix_start_index = second_sharp_index + 1;

            let CosmeticFilterLocations {
                wildcard_scope,
                regex_domains,
                not_regex_domains,
                entities,
                not_entities,
                hostnames,
                not_hostnames,
            } = if sharp_index > 0 {
                CosmeticFilter::parse_before_sharp(line, sharp_index)?
            } else {
                CosmeticFilterLocations::default()
            };

            let after_sharp = &line[suffix_start_index..].trim();

            if after_sharp.is_empty() {
                return Err(CosmeticFilterError::EmptyRule);
            }

            let (selector, action) = if line.len() - suffix_start_index > 4
                && line[suffix_start_index..].starts_with("+js(")
                && line.ends_with(')')
            {
                if sharp_index == 0 {
                    return Err(CosmeticFilterError::GenericScriptInject);
                }
                let args = &line[suffix_start_index + 4..line.len() - 1];
                if crate::resources::parse_scriptlet_args(args).is_none() {
                    return Err(CosmeticFilterError::InvalidScriptletArgs);
                }
                mask |= CosmeticFilterMask::SCRIPT_INJECT;
                (
                    // TODO: overloading `CssSelector` here is not ideal.
                    vec![CosmeticFilterOperator::CssSelector(String::from(
                        &line[suffix_start_index + 4..line.len() - 1],
                    ))],
                    None,
                )
            } else {
                let (selector, action) = CosmeticFilter::parse_after_sharp_nonscript(after_sharp)?;
                let validated_selector = validate_css_selector(selector, translate_abp_syntax)?;
                if sharp_index == 0 && action.is_some() {
                    return Err(CosmeticFilterError::GenericAction);
                }
                (validated_selector, action)
            };

            if (not_entities.is_some() || not_hostnames.is_some() || !not_regex_domains.is_empty())
                && mask.contains(CosmeticFilterMask::UNHIDE)
            {
                return Err(CosmeticFilterError::DoubleNegation);
            }

            let this = Self {
                wildcard_scope,
                regex_domains,
                not_regex_domains,
                entities,
                hostnames,
                mask,
                not_entities,
                not_hostnames,
                raw_line: if debug {
                    Some(Box::new(String::from(line)))
                } else {
                    None
                },
                selector,
                action,
                permission,
            };

            if !this.has_hostname_constraint() && this.plain_css_selector().is_none() {
                return Err(CosmeticFilterError::GenericAction);
            }

            Ok(this)
        } else {
            Err(CosmeticFilterError::MissingSharp)
        }
    }

    /// Any cosmetic filter rule that specifies (possibly negated) hostnames or entities has a
    /// hostname constraint.
    pub fn has_hostname_constraint(&self) -> bool {
        self.wildcard_scope || !self.regex_domains.is_empty() || !self.not_regex_domains.is_empty()
            || self.hostnames.is_some()
            || self.entities.is_some()
            || self.not_entities.is_some()
            || self.not_hostnames.is_some()
    }

    /// In general, adding a hostname or entity to a rule *increases* the number of situations in
    /// which it applies. However, if a specific rule only has negated hostnames or entities, it
    /// technically should apply to any hostname which does not match a negation.
    ///
    /// See: <https://github.com/chrisaljoudi/uBlock/issues/145>
    ///
    /// To account for this inconsistency, this method will generate and return the corresponding
    /// 'hidden' generic rule if one applies.
    ///
    /// Note that this behavior is not applied to script injections or rules with actions.
    pub fn hidden_generic_rule(&self) -> Option<CosmeticFilter> {
        if self.wildcard_scope || self.hostnames.is_some() || self.entities.is_some() || !self.regex_domains.is_empty() || !self.not_regex_domains.is_empty() {
            None
        } else if (self.not_hostnames.is_some() || self.not_entities.is_some())
            && (self.action.is_none() && !self.mask.contains(CosmeticFilterMask::SCRIPT_INJECT))
        {
            let mut generic_rule = self.clone();
            generic_rule.not_hostnames = None;
            generic_rule.not_entities = None;
            Some(generic_rule)
        } else {
            None
        }
    }
}

/// Returns a slice of `hostname` up to and including the segment that overlaps with the first
/// segment of `domain`, which has the effect of stripping ".com", ".co.uk", etc., as well as the
/// public suffix itself.
fn get_hostname_without_public_suffix<'a>(
    hostname: &'a str,
    domain: &str,
) -> Option<(&'a str, &'a str)> {
    let index_of_dot = find_char(b'.', domain.as_bytes());

    if let Some(index_of_dot) = index_of_dot {
        let public_suffix = &domain[index_of_dot + 1..];
        Some((
            &hostname[0..hostname.len() - public_suffix.len() - 1],
            &hostname[hostname.len() - domain.len() + index_of_dot + 1..],
        ))
    } else {
        None
    }
}

/// Given a hostname and the indices of an end position and the start of the domain, returns a
/// `Vec` of hashes of all subdomains the hostname falls under, ordered from least to most
/// specific.
///
/// Check the `label_hashing` tests for examples.
fn get_hashes_from_labels(hostname: &str, end: usize, start_of_domain: usize) -> Vec<Hash> {
    let mut hashes = vec![];
    if end == 0 {
        return hashes;
    }
    let mut dot_ptr = start_of_domain;

    while let Some(dot_index) = find_char_reverse(b'.', &hostname.as_bytes()[..dot_ptr]) {
        dot_ptr = dot_index;
        hashes.push(crate::utils::fast_hash(&hostname[dot_ptr + 1..end]));
    }

    hashes.push(crate::utils::fast_hash(&hostname[..end]));

    hashes
}

/// Returns a `Vec` of the hashes of all segments of `hostname` that may match an
/// entity-constrained rule.
pub(crate) fn get_entity_hashes_from_labels(hostname: &str, domain: &str) -> Vec<Hash> {
    if let Some((hostname_without_public_suffix, public_suffix)) =
        get_hostname_without_public_suffix(hostname, domain)
    {
        let mut hashes = get_hashes_from_labels(
            hostname_without_public_suffix,
            hostname_without_public_suffix.len(),
            hostname_without_public_suffix.len(),
        );
        hashes.push(crate::utils::fast_hash(public_suffix));
        hashes
    } else {
        vec![]
    }
}

/// Returns a `Vec` of the hashes of all segments of `hostname` that may match a
/// hostname-constrained rule.
pub(crate) fn get_hostname_hashes_from_labels(hostname: &str, _domain: &str) -> Vec<Hash> {
    // Hostname scopes include every suffix; only entity scopes depend on PSL.
    get_hashes_from_labels(hostname, hostname.len(), hostname.len())
}

#[cfg(not(feature = "css-validation"))]
mod css_validation {
    use super::{CosmeticFilterError, CosmeticFilterOperator};

    pub fn validate_css_selector(
        selector: &str,
        _accept_abp_selectors: bool,
    ) -> Result<Vec<CosmeticFilterOperator>, CosmeticFilterError> {
        Ok(vec![CosmeticFilterOperator::CssSelector(
            selector.to_string(),
        )])
    }

    pub fn is_valid_css_style(_style: &str) -> bool {
        true
    }
}

#[cfg(feature = "css-validation")]
mod css_validation {
    //! Methods for validating CSS selectors and style rules extracted from cosmetic filter rules.
    use super::{CosmeticFilterError, CosmeticFilterOperator};
    use core::fmt::{Result as FmtResult, Write};
    use cssparser::{CowRcStr, ParseError, Parser, ParserInput, SourceLocation, ToCss, Token};
    use precomputed_hash::PrecomputedHash;
    use selectors::parser::SelectorParseErrorKind;

    /// Returns a validated canonical CSS selector for the given input, or nothing if one can't be
    /// determined.
    ///
    /// For the majority of filters, this works by trivial regex matching. More complex filters are
    /// assembled into a mock stylesheet which is then parsed using `cssparser` and validated.
    ///
    /// In addition to normalizing formatting, this function will remove unsupported procedural
    /// selectors and convert others to canonical representations (i.e. `:-abp-has` -> `:has`).
    pub fn validate_css_selector(
        selector: &str,
        accept_abp_selectors: bool,
    ) -> Result<Vec<CosmeticFilterOperator>, CosmeticFilterError> {
        validate_css_selector_nested(selector, accept_abp_selectors, 0)
    }

    fn validate_css_selector_nested(selector: &str, accept_abp_selectors: bool, depth: usize)
        -> Result<Vec<CosmeticFilterOperator>, CosmeticFilterError>
    {
        if depth > 32 { return Err(CosmeticFilterError::InvalidCssSelector); }
        use regex::Regex;
        use std::sync::LazyLock;
        static RE_SIMPLE_SELECTOR: LazyLock<Regex> =
            LazyLock::new(|| Regex::new(r"^[#.]?[A-Za-z_][\w-]*$").unwrap());

        if RE_SIMPLE_SELECTOR.is_match(selector) {
            return Ok(vec![CosmeticFilterOperator::CssSelector(
                selector.to_string(),
            )]);
        }

        // Use `mock-stylesheet-marker` where uBO uses `color: red` since we have control over the
        // parsing logic within the block.
        let mock_stylesheet = format!("{selector}{{mock-stylesheet-marker}}");
        let mut pi = ParserInput::new(&mock_stylesheet);
        let mut parser = Parser::new(&mut pi);
        let mut parser_impl = QualifiedRuleParserImpl {
            accept_abp_selectors, depth,
        };
        let mut rule_list_parser = cssparser::StyleSheetParser::new(&mut parser, &mut parser_impl);

        let prelude = rule_list_parser.next().and_then(|r| r.ok());

        // There should only be one rule
        if rule_list_parser.next().is_some() {
            return Err(CosmeticFilterError::InvalidCssSelector);
        }

        fn has_procedural_operator(selector: &selectors::parser::Selector<SelectorImpl>) -> bool {
            let mut iter = selector.iter();
            loop {
                for component in iter.by_ref() {
                    if is_procedural_operator(component) {
                        return true;
                    }
                }
                if iter.next_sequence().is_none() {
                    break false;
                }
            }
        }

        fn is_procedural_operator(c: &selectors::parser::Component<SelectorImpl>) -> bool {
            use selectors::parser::Component;
            if let Component::Negation(selectors) | Component::Is(selectors) | Component::Where(selectors) = c {
                return selectors.slice().iter().any(has_procedural_operator);
            }
            // Avoid using `to_procedural_operator.is_some()`, which will re-allocate the argument string.
            matches!(
                c,
                Component::NonTSPseudoClass(NonTSPseudoClass::HasText(_))
                    | Component::NonTSPseudoClass(NonTSPseudoClass::Nested(_, _, _))
                    | Component::NonTSPseudoClass(NonTSPseudoClass::MatchesAttr(_))
                    | Component::NonTSPseudoClass(NonTSPseudoClass::MatchesProp(_))
                    | Component::NonTSPseudoClass(NonTSPseudoClass::MatchesCss(_))
                    | Component::NonTSPseudoClass(NonTSPseudoClass::MatchesCssBefore(_))
                    | Component::NonTSPseudoClass(NonTSPseudoClass::MatchesCssAfter(_))
                    | Component::NonTSPseudoClass(NonTSPseudoClass::MatchesMedia(_))
                    | Component::NonTSPseudoClass(NonTSPseudoClass::WatchAttr(_))
                    | Component::NonTSPseudoClass(NonTSPseudoClass::Others(_))
                    | Component::NonTSPseudoClass(NonTSPseudoClass::MatchesPath(_))
                    | Component::NonTSPseudoClass(NonTSPseudoClass::MinTextLength(_))
                    | Component::NonTSPseudoClass(NonTSPseudoClass::Upward(_))
                    | Component::NonTSPseudoClass(NonTSPseudoClass::Xpath(_))
            )
        }

        if let Some(prelude) = prelude {
            if !prelude.slice().iter().any(has_procedural_operator) {
                // There are no procedural filters, so all selectors use standard CSS.
                // It's ok to return that as a "single" selector.
                return Ok(vec![CosmeticFilterOperator::CssSelector(
                    prelude.to_css_string(),
                )]);
            }

            if prelude.slice().len() != 1 {
                let branches = prelude.slice().iter().map(|selector|
                    validate_css_selector_nested(&selector.to_css_string(), accept_abp_selectors, depth + 1)
                ).collect::<Result<Vec<_>, _>>()?;
                return Ok(vec![CosmeticFilterOperator::Any(branches)]);
            }

            // Safe; early return if length is not 1
            let selector = prelude.slice().iter().next().unwrap();

            /// Shim for items returned by `selectors::parser::SelectorIter`. Sequences and
            /// components iterate in opposite directions, but we want to process them in a
            /// consistent order, so they need to be manually collected and reversed.
            enum SelectorsPart<'a> {
                /// `SelectorIter` iterates over `Component`s from left-to-right.
                Component(&'a selectors::parser::Component<SelectorImpl>),
                /// `SelectorIter` iterates over sequences of `Component`s separated by
                /// `Combinator`s from right-to-left.
                Combinator(selectors::parser::Combinator),
            }

            // Collect the selector parts into `parts` in right-to-left order, reversing the
            // components of each sequence as necessary.
            let mut parts = vec![];
            let mut iter = selector.iter();
            loop {
                // Manual iteration/collection necessary due to `SelectorIter`'s multi-iterator
                // construction.
                // - `.rev()` cannot work directly because it's not a fixed-size iterator.
                // - `.collect()` cannot work because it takes ownership of the iterator, which is
                //                still required later for `next_sequence`.
                let mut components = vec![];
                for component in iter.by_ref() {
                    components.push(SelectorsPart::Component(component));
                }
                parts.extend(components.into_iter().rev());
                if let Some(combinator) = iter.next_sequence() {
                    parts.push(SelectorsPart::Combinator(combinator));
                } else {
                    break;
                }
            }

            // We can now prepare `output` by iterating over all the parts of the selector in
            // left-to-right order.
            let mut pending_css_selector = String::new();
            let mut following_procedural = false;
            let css_part = |text, same_subject| if same_subject { CosmeticFilterOperator::MatchesSelector(text) } else { CosmeticFilterOperator::CssSelector(text) };
            let mut output = vec![];
            for part in parts.into_iter().rev() {
                use selectors::parser::Component;
                match part {
                    SelectorsPart::Component(Component::NonTSPseudoClass(c)) => {
                        if let Some(procedural_operator) = c.to_procedural_operator() {
                            if !pending_css_selector.is_empty() {
                                output.push(css_part(pending_css_selector, following_procedural));
                                pending_css_selector = String::new();
                            }
                            output.push(procedural_operator);
                            following_procedural = true;
                        } else {
                            c.to_css(&mut pending_css_selector)
                                .map_err(|_| CosmeticFilterError::InvalidCssSelector)?;
                        }
                    }
                    SelectorsPart::Component(Component::Negation(selectors)) if selectors.slice().iter().any(has_procedural_operator) => {
                        if !pending_css_selector.is_empty() {
                            output.push(css_part(core::mem::take(&mut pending_css_selector), following_procedural));
                        }
                        let branches = selectors.slice().iter().map(|selector|
                            validate_css_selector_nested(&selector.to_css_string(), accept_abp_selectors, depth + 1)
                        ).collect::<Result<Vec<_>, _>>()?;
                        output.push(CosmeticFilterOperator::Not(vec![CosmeticFilterOperator::Any(branches)]));
                        following_procedural = true;
                    }
                    SelectorsPart::Component(Component::Is(selectors) | Component::Where(selectors)) if selectors.slice().iter().any(has_procedural_operator) => {
                        if !pending_css_selector.is_empty() {
                            output.push(css_part(core::mem::take(&mut pending_css_selector), following_procedural));
                        }
                        let branches = selectors.slice().iter().map(|selector|
                            validate_css_selector_nested(&selector.to_css_string(), accept_abp_selectors, depth + 1)
                        ).collect::<Result<Vec<_>, _>>()?;
                        output.push(CosmeticFilterOperator::Matches(vec![CosmeticFilterOperator::Any(branches)]));
                        following_procedural = true;
                    }
                    SelectorsPart::Component(other) => {
                        other
                            .to_css(&mut pending_css_selector)
                            .map_err(|_| CosmeticFilterError::InvalidCssSelector)?;
                    }
                    SelectorsPart::Combinator(combinator) => {
                        if following_procedural && !pending_css_selector.is_empty() {
                            output.push(css_part(core::mem::take(&mut pending_css_selector), true));
                        }
                        following_procedural = false;
                        combinator
                            .to_css(&mut pending_css_selector)
                            .map_err(|_| CosmeticFilterError::InvalidCssSelector)?;
                    }
                }
            }
            if !pending_css_selector.is_empty() {
                output.push(css_part(pending_css_selector, following_procedural));
            }

            Ok(output)
        } else {
            Err(CosmeticFilterError::InvalidCssSelector)
        }
    }

    struct QualifiedRuleParserImpl {
        accept_abp_selectors: bool,
        depth: usize,
    }

    impl<'i> cssparser::QualifiedRuleParser<'i> for QualifiedRuleParserImpl {
        type Prelude = selectors::SelectorList<SelectorImpl>;
        type QualifiedRule = selectors::SelectorList<SelectorImpl>;
        type Error = ();

        fn parse_prelude<'t>(
            &mut self,
            input: &mut Parser<'i, 't>,
        ) -> Result<Self::Prelude, ParseError<'i, Self::Error>> {
            selectors::SelectorList::parse(
                &SelectorParseImpl {
                    accept_abp_selectors: self.accept_abp_selectors,
                    depth: self.depth,
                },
                input,
                selectors::parser::ParseRelative::No,
            )
            .map_err(|_| ParseError {
                kind: cssparser::ParseErrorKind::Custom(()),
                location: SourceLocation { line: 0, column: 0 },
            })
        }

        /// Check that the block is exactly equal to "mock-stylesheet-marker", and return just the
        /// selector list as the prelude
        fn parse_block<'t>(
            &mut self,
            prelude: Self::Prelude,
            _start: &cssparser::ParserState,
            input: &mut Parser<'i, 't>,
        ) -> Result<Self::QualifiedRule, ParseError<'i, Self::Error>> {
            let err = Err(ParseError {
                kind: cssparser::ParseErrorKind::Custom(()),
                location: SourceLocation { line: 0, column: 0 },
            });
            match input.next() {
                Ok(Token::Ident(i)) if i.as_ref() == "mock-stylesheet-marker" => (),
                _ => return err,
            }
            if input.next().is_ok() {
                return err;
            }
            Ok(prelude)
        }
    }

    /// Default implementations for `AtRuleParser` parsing methods return false. This is
    /// acceptable; at-rules should not be valid in cosmetic rules.
    impl cssparser::AtRuleParser<'_> for QualifiedRuleParserImpl {
        type Prelude = ();
        /// unused; just to satisfy type checking
        type AtRule = selectors::SelectorList<SelectorImpl>;
        type Error = ();
    }

    pub fn is_valid_css_style(style: &str) -> bool {
        use regex::{Regex, RegexBuilder};
        use std::sync::LazyLock;
        static RE_INVALID_DIRECTIVE: LazyLock<Regex> = LazyLock::new(|| {
            RegexBuilder::new(r"image-set\(|url\(|\\|\/\*")
                .case_insensitive(true)
                .build()
                .unwrap()
        });

        if RE_INVALID_DIRECTIVE.is_match(style) {
            return false;
        }
        true
    }

    struct SelectorParseImpl {
        accept_abp_selectors: bool,
        depth: usize,
    }

    fn nested_matching_close(arg: &Token) -> Option<Token<'static>> {
        match arg {
            Token::Function(..) => Some(Token::CloseParenthesis),
            Token::ParenthesisBlock => Some(Token::CloseParenthesis),
            Token::CurlyBracketBlock => Some(Token::CloseCurlyBracket),
            Token::SquareBracketBlock => Some(Token::CloseSquareBracket),
            _ => None,
        }
    }

    /// Just convert the rest of the selector to a string
    fn to_css_nested<'i>(
        arguments: &mut Parser<'i, '_>,
    ) -> Result<String, ParseError<'i, SelectorParseErrorKind<'i>>> {
        let mut inner = String::new();
        while let Ok(arg) = arguments.next_including_whitespace() {
            if arg.to_css(&mut inner).is_err() {
                return Err(arguments.new_custom_error(SelectorParseErrorKind::InvalidState));
            };
            if let Some(closing_token) = nested_matching_close(arg) {
                let nested = arguments.parse_nested_block(to_css_nested)?;
                inner.push_str(&nested);
                closing_token.to_css(&mut inner).map_err(|_| {
                    arguments.new_custom_error(SelectorParseErrorKind::InvalidState)
                })?;
            }
        }
        Ok(inner)
    }

    impl<'i> selectors::parser::Parser<'i> for SelectorParseImpl {
        type Impl = SelectorImpl;
        type Error = SelectorParseErrorKind<'i>;

        fn parse_slotted(&self) -> bool {
            true
        }
        fn parse_part(&self) -> bool {
            true
        }
        fn parse_is_and_where(&self) -> bool {
            true
        }
        fn parse_host(&self) -> bool {
            true
        }
        fn parse_non_ts_pseudo_class(
            &self,
            _location: SourceLocation,
            name: CowRcStr<'i>,
        ) -> Result<
            <Self::Impl as selectors::parser::SelectorImpl>::NonTSPseudoClass,
            ParseError<'i, Self::Error>,
        > {
            Ok(NonTSPseudoClass::AnythingElse(name.to_string(), None))
        }
        fn parse_non_ts_functional_pseudo_class<'t>(
            &self,
            name: CowRcStr<'i>,
            arguments: &mut Parser<'i, 't>,
            _after_part: bool,
        ) -> Result<
            <Self::Impl as selectors::parser::SelectorImpl>::NonTSPseudoClass,
            ParseError<'i, Self::Error>,
        > {
            let canonical_name = match (self.accept_abp_selectors, name.as_ref()) {
                (true, "-abp-has") => Some("has"),
                (true, "-abp-contains") => Some("has-text"),
                (true, "contains") => Some("has-text"),
                _ => None,
            }
            .unwrap_or(name.as_ref());
            match canonical_name {
                "has" | "if" | "if-not" => {
                    let text = to_css_nested(arguments)?;
                    let trimmed = text.trim();
                    if trimmed.is_empty() { return Err(arguments.new_custom_error(SelectorParseErrorKind::InvalidState)); }
                    let alternatives = {
                        let mut input = ParserInput::new(trimmed);
                        let mut parser = Parser::new(&mut input);
                        parser.parse_comma_separated(to_css_nested)
                            .map_err(|_| arguments.new_custom_error(SelectorParseErrorKind::InvalidState))?
                    };
                    let mut branches = Vec::new();
                    for alternative in alternatives {
                        let alternative = alternative.trim();
                        let relative = alternative.starts_with(['>', '+', '~']);
                        let rooted = if relative { format!(":scope {alternative}") } else { alternative.to_string() };
                        let mut branch = validate_css_selector_nested(&rooted, self.accept_abp_selectors, self.depth + 1)
                            .map_err(|_| arguments.new_custom_error(SelectorParseErrorKind::InvalidState))?;
                        if relative {
                            if let Some(CosmeticFilterOperator::CssSelector(css)) = branch.first_mut() {
                                *css = css.strip_prefix(":scope ").unwrap_or(css).to_string();
                            }
                        }
                        branches.push(branch);
                    }
                    // Ordinary :has stays native CSS, preserving its fast path.
                    if canonical_name == "has" && branches.iter().all(|branch| matches!(branch.as_slice(), [CosmeticFilterOperator::CssSelector(_)])) {
                        return Ok(NonTSPseudoClass::AnythingElse("has".to_string(), Some(text)));
                    }
                    let nested = if branches.len() == 1 { branches.pop().unwrap() }
                        else { vec![CosmeticFilterOperator::Any(branches)] };
                    return Ok(NonTSPseudoClass::Nested(canonical_name.to_string(), text, nested));
                }
                "has-text" => {
                    let text = to_css_nested(arguments)?;
                    return Ok(NonTSPseudoClass::HasText(text));
                }
                "matches-attr" => {
                    let text = to_css_nested(arguments)?;
                    return Ok(NonTSPseudoClass::MatchesAttr(text));
                }
                "matches-prop" => {
                    let text = to_css_nested(arguments)?;
                    return Ok(NonTSPseudoClass::MatchesProp(text));
                }
                "matches-css" => {
                    let text = to_css_nested(arguments)?;
                    return Ok(NonTSPseudoClass::MatchesCss(text));
                }
                "matches-css-before" => {
                    let text = to_css_nested(arguments)?;
                    return Ok(NonTSPseudoClass::MatchesCssBefore(text));
                }
                "matches-css-after" => {
                    let text = to_css_nested(arguments)?;
                    return Ok(NonTSPseudoClass::MatchesCssAfter(text));
                }
                "matches-media" => {
                    let text = to_css_nested(arguments)?;
                    return Ok(NonTSPseudoClass::MatchesMedia(text));
                }
                "watch-attr" => {
                    let text = to_css_nested(arguments)?;
                    return Ok(NonTSPseudoClass::WatchAttr(text));
                }
                "others" => {
                    let text = to_css_nested(arguments)?;
                    return Ok(NonTSPseudoClass::Others(text));
                }
                "matches-path" => {
                    let text = to_css_nested(arguments)?;
                    return Ok(NonTSPseudoClass::MatchesPath(text));
                }
                "min-text-length" => {
                    let text = to_css_nested(arguments)?;
                    return Ok(NonTSPseudoClass::MinTextLength(text));
                }
                "upward" => {
                    let text = to_css_nested(arguments)?;
                    return Ok(NonTSPseudoClass::Upward(text));
                }
                "xpath" => {
                    let text = to_css_nested(arguments)?;
                    return Ok(NonTSPseudoClass::Xpath(text));
                }
                "-abp-contains" | "-abp-has" | "-abp-properties" | "contains"
                | "matches-property" | "nth-ancestor" | "properties" | "subject" | "remove"
                | "remove-attr" | "remove-class" => {
                    return Err(arguments.new_custom_error(
                        SelectorParseErrorKind::UnsupportedPseudoClassOrElement(name),
                    ));
                }
                _ => (),
            }

            let inner_selector = to_css_nested(arguments)?;

            Ok(NonTSPseudoClass::AnythingElse(
                canonical_name.to_string(),
                Some(inner_selector),
            ))
        }
        fn parse_pseudo_element(
            &self,
            _location: SourceLocation,
            name: CowRcStr<'i>,
        ) -> Result<
            <Self::Impl as selectors::parser::SelectorImpl>::PseudoElement,
            ParseError<'i, Self::Error>,
        > {
            Ok(PseudoElement(name.to_string(), None))
        }
        fn parse_functional_pseudo_element<'t>(
            &self,
            name: CowRcStr<'i>,
            arguments: &mut Parser<'i, 't>,
        ) -> Result<
            <Self::Impl as selectors::parser::SelectorImpl>::PseudoElement,
            ParseError<'i, Self::Error>,
        > {
            let inner_selector = to_css_nested(arguments)?;
            Ok(PseudoElement(name.to_string(), Some(inner_selector)))
        }
    }

    /// The `selectors` library requires an object that implements `SelectorImpl` to store data
    /// about a parsed selector. For performance, the actual content of parsed selectors is
    /// discarded as much as possible - it only matters whether the returned `Result` is `Ok` or
    /// `Err`.
    #[derive(Debug, Clone)]
    struct SelectorImpl;

    impl selectors::parser::SelectorImpl for SelectorImpl {
        type ExtraMatchingData<'a> = ();
        type AttrValue = CssString;
        type Identifier = CssIdent;
        type LocalName = CssIdent;
        type NamespaceUrl = DummyValue;
        type NamespacePrefix = DummyValue;
        type BorrowedNamespaceUrl = DummyValue;
        type BorrowedLocalName = CssIdent;
        type NonTSPseudoClass = NonTSPseudoClass;
        type PseudoElement = PseudoElement;
    }

    /// Simple implementation of [PrecomputedHash] that just uses the default hasher for its
    /// argument.
    fn precomputed_hash_impl<H: std::hash::Hash>(this: &H) -> u32 {
        use std::hash::{DefaultHasher, Hasher};
        let mut hasher = DefaultHasher::new();
        this.hash(&mut hasher);
        hasher.finish() as u32
    }

    /// Serialized using `CssStringWriter`.
    #[derive(Debug, Clone, PartialEq, Eq, Default)]
    struct CssString(String);

    impl ToCss for CssString {
        fn to_css<W: Write>(&self, dest: &mut W) -> core::fmt::Result {
            dest.write_str("\"")?;
            {
                let mut dest = cssparser::CssStringWriter::new(dest);
                dest.write_str(&self.0)?;
            }
            dest.write_str("\"")
        }
    }

    impl<'a> From<&'a str> for CssString {
        fn from(s: &'a str) -> Self {
            CssString(s.to_string())
        }
    }

    impl PrecomputedHash for CssString {
        fn precomputed_hash(&self) -> u32 {
            precomputed_hash_impl(&self.0)
        }
    }

    /// Serialized using `serialize_identifier`.
    #[derive(Debug, Clone, PartialEq, Eq, Default)]
    struct CssIdent(String);

    impl ToCss for CssIdent {
        fn to_css<W: Write>(&self, dest: &mut W) -> core::fmt::Result {
            cssparser::serialize_identifier(&self.0, dest)
        }
    }

    impl<'a> From<&'a str> for CssIdent {
        fn from(s: &'a str) -> Self {
            CssIdent(s.to_string())
        }
    }

    impl PrecomputedHash for CssIdent {
        fn precomputed_hash(&self) -> u32 {
            precomputed_hash_impl(&self.0)
        }
    }

    /// For performance, individual fields of parsed selectors are discarded. Instead, they are
    /// parsed into a `DummyValue` with no fields.
    #[derive(Debug, Clone, PartialEq, Eq, Default)]
    struct DummyValue(String);

    impl ToCss for DummyValue {
        fn to_css<W: Write>(&self, dest: &mut W) -> core::fmt::Result {
            write!(dest, "{}", self.0)
        }
    }

    impl<'a> From<&'a str> for DummyValue {
        fn from(s: &'a str) -> Self {
            DummyValue(s.to_string())
        }
    }

    impl PrecomputedHash for DummyValue {
        fn precomputed_hash(&self) -> u32 {
            precomputed_hash_impl(&self.0)
        }
    }

    /// Dummy struct for non-tree-structural pseudo-classes.
    #[derive(Clone, PartialEq, Eq)]
    enum NonTSPseudoClass {
        Nested(String, String, Vec<CosmeticFilterOperator>),
        /// The `:has-text` or `:-abp-contains` procedural operator.
        HasText(String),
        /// The `:matches-attr` procedural operator.
        MatchesAttr(String),
    MatchesProp(String),
        /// The `:matches-css` procedural operator.
        MatchesCss(String),
        /// The `:matches-css-before` procedural operator.
        MatchesCssBefore(String),
        /// The `:matches-css-after` procedural operator.
        MatchesCssAfter(String),
        /// The media-query procedural operator.
        MatchesMedia(String),
        /// Request attribute mutation observation.
        WatchAttr(String),
        /// Select nodes outside the matched branches.
        Others(String),
        /// The `:matches-path` procedural operator.
        MatchesPath(String),
        /// The `:min-text-length` procedural operator.
        MinTextLength(String),
        /// The `:upward` procedural operator.
        Upward(String),
        /// The `:xpath` procedural operator.
        Xpath(String),
        /// Any native CSS pseudoclass that isn't a procedural operator. Second argument contains inner arguments, if present.
        AnythingElse(String, Option<String>),
    }

    impl selectors::parser::NonTSPseudoClass for NonTSPseudoClass {
        type Impl = SelectorImpl;
        fn is_active_or_hover(&self) -> bool {
            false
        }
        fn is_user_action_state(&self) -> bool {
            false
        }
    }

    impl ToCss for NonTSPseudoClass {
        fn to_css<W: Write>(&self, dest: &mut W) -> FmtResult {
            write!(dest, ":")?;
            match self {
                Self::Nested(name, text, _) => write!(dest, "{name}({text})")?,
                Self::HasText(text) => write!(dest, "has-text({text})")?,
                Self::MatchesAttr(text) => write!(dest, "matches-attr({text})")?,
                Self::MatchesProp(text) => write!(dest, "matches-prop({text})")?,
                Self::MatchesCss(text) => write!(dest, "matches-css({text})")?,
                Self::MatchesCssBefore(text) => write!(dest, "matches-css-before({text})")?,
                Self::MatchesCssAfter(text) => write!(dest, "matches-css-after({text})")?,
                Self::MatchesMedia(text) => write!(dest, "matches-media({text})")?,
                Self::WatchAttr(text) => write!(dest, "watch-attr({text})")?,
                Self::Others(text) => write!(dest, "others({text})")?,
                Self::MatchesPath(text) => write!(dest, "matches-path({text})")?,
                Self::MinTextLength(text) => write!(dest, "min-text-length({text})")?,
                Self::Upward(text) => write!(dest, "upward({text})")?,
                Self::Xpath(text) => write!(dest, "xpath({text})")?,
                Self::AnythingElse(name, None) => write!(dest, "{name}")?,
                Self::AnythingElse(name, Some(args)) => write!(dest, "{name}({args})")?,
            }
            Ok(())
        }
    }

    impl NonTSPseudoClass {
        fn to_procedural_operator(&self) -> Option<CosmeticFilterOperator> {
            match self {
                NonTSPseudoClass::Nested(name, _, operators) => Some(match name.as_str() {
                    "has" => CosmeticFilterOperator::Has(operators.clone()),
                    "if" => CosmeticFilterOperator::If(operators.clone()),
                    _ => CosmeticFilterOperator::IfNot(operators.clone()),
                }),
                NonTSPseudoClass::HasText(a) => Some(CosmeticFilterOperator::HasText(a.to_owned())),
                NonTSPseudoClass::MatchesAttr(a) => {
                    Some(CosmeticFilterOperator::MatchesAttr(a.to_owned()))
                }
                NonTSPseudoClass::MatchesProp(a) => Some(CosmeticFilterOperator::MatchesProp(a.to_owned())),
                NonTSPseudoClass::MatchesCss(a) => {
                    Some(CosmeticFilterOperator::MatchesCss(a.to_owned()))
                }
                NonTSPseudoClass::MatchesCssBefore(a) => {
                    Some(CosmeticFilterOperator::MatchesCssBefore(a.to_owned()))
                }
                NonTSPseudoClass::MatchesCssAfter(a) => {
                    Some(CosmeticFilterOperator::MatchesCssAfter(a.to_owned()))
                }
                NonTSPseudoClass::MatchesMedia(a) => Some(CosmeticFilterOperator::MatchesMedia(a.to_owned())),
                NonTSPseudoClass::WatchAttr(a) => Some(CosmeticFilterOperator::WatchAttr(a.to_owned())),
                NonTSPseudoClass::Others(a) => Some(CosmeticFilterOperator::Others(a.to_owned())),
                NonTSPseudoClass::MatchesPath(a) => {
                    Some(CosmeticFilterOperator::MatchesPath(a.to_owned()))
                }
                NonTSPseudoClass::MinTextLength(a) => {
                    Some(CosmeticFilterOperator::MinTextLength(a.to_owned()))
                }
                NonTSPseudoClass::Upward(a) => Some(CosmeticFilterOperator::Upward(a.to_owned())),
                NonTSPseudoClass::Xpath(a) => Some(CosmeticFilterOperator::Xpath(a.to_owned())),
                _ => None,
            }
        }
    }

    /// Dummy struct for pseudo-elements.
    #[derive(Clone, PartialEq, Eq)]
    struct PseudoElement(String, Option<String>);

    impl selectors::parser::PseudoElement for PseudoElement {
        type Impl = SelectorImpl;

        fn valid_after_slotted(&self) -> bool {
            true
        }
    }

    impl ToCss for PseudoElement {
        fn to_css<W: Write>(&self, dest: &mut W) -> FmtResult {
            write!(dest, "::")?;
            match self {
                Self(name, None) => write!(dest, "{name}")?,
                Self(name, Some(args)) => write!(dest, "{name}({args})")?,
            }
            Ok(())
        }
    }
}

#[cfg(test)]
#[path = "../../tests/unit/filters/cosmetic.rs"]
mod unit_tests;
