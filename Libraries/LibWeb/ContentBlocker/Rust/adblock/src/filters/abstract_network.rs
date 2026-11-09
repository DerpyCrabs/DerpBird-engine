use crate::regex_manager::{compile_removeparam_regex, regex_literal_end};

use super::network::NetworkFilterError;

use regex::Regex;
use std::sync::LazyLock;

/// For now, only support `$removeparam` with simple alphanumeric/dash/underscore patterns.
static VALID_PARAM: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^[a-zA-Z0-9_\-]+$").unwrap());

#[derive(Clone, Copy)]
pub(crate) enum NetworkFilterLeftAnchor {
    /// A `||` token, which represents a match to the start of a domain or subdomain segment.
    DoublePipe,
    /// A `|` token, which represents a match to the exact start of the URL.
    SinglePipe,
}

#[derive(Clone, Copy)]
pub(crate) enum NetworkFilterRightAnchor {
    /// A `|` token, which represents a match to the exact end of the URL.
    SinglePipe,
}

/// Pattern for a network filter, describing what URLs to match against.
#[derive(Clone)]
pub(crate) struct NetworkFilterPattern {
    pub(crate) left_anchor: Option<NetworkFilterLeftAnchor>,
    pub(crate) start: usize,
    pub(crate) end: usize,
    pub(crate) right_anchor: Option<NetworkFilterRightAnchor>,
}

#[derive(Clone, Copy)]
pub(crate) enum HttpMethod {
    Get,
    Head,
    Post,
    Connect,
    Delete,
    Options,
    Patch,
    Put,
    Trace,
}

/// Parses a pipe-delimited string of domains into a vector of domain entries with negation flags.
/// Splits the input string by '|', strips '~' prefixes to mark negated domains,
/// and preserves regex patterns as complete entries, including embedded pipes/commas.
fn parse_pipe_delimited_domains<'a>(
    value: &'a str,
) -> Result<Vec<(bool, &'a str)>, NetworkFilterError> {
    let mut domains = Vec::new();
    let mut rest = value;
    while !rest.is_empty() {
        let (enabled, raw) = rest.strip_prefix('~').map(|v| (false, v)).unwrap_or((true, rest));
        let end = if raw.starts_with('/') {
            let end = regex_literal_end(raw).ok_or(NetworkFilterError::NoSupportedDomains)? + 1;
            if crate::regex_manager::compile_removeparam_regex(&raw[..end]).is_none() { return Err(NetworkFilterError::NoSupportedDomains); }
            end
        } else { raw.find('|').unwrap_or(raw.len()) };
        if end == 0 { return Err(NetworkFilterError::NoSupportedDomains); }
        domains.push((enabled, &raw[..end]));
        rest = &raw[end..];
        if !rest.is_empty() { rest = rest.strip_prefix('|').ok_or(NetworkFilterError::NoSupportedDomains)?; if rest.is_empty() { return Err(NetworkFilterError::NoSupportedDomains); } }
    }
    if domains.is_empty() { return Err(NetworkFilterError::NoSupportedDomains); }
    Ok(domains)
}

/// Any option that appears on the right side of a network filter as initiated by a `$` character.
/// All `bool` arguments below are `true` if the option stands alone, or `false` if the option is
/// negated using a prepended `~`.
#[derive(Clone)]
pub(crate) enum NetworkFilterOption<'a> {
    Domain(Vec<(bool, &'a str)>),
    To(Vec<(bool, &'a str)>),
    Top(Vec<(bool, &'a str)>),
    IpAddress(&'a str),
    Badfilter,
    Important,
    MatchCase,
    ThirdParty(bool),
    StrictThirdParty(bool),
    FirstParty(bool),
    Tag(&'a str),
    Redirect(&'a str),
    RedirectRule(&'a str),
    Csp(Option<&'a str>),
    Removeparam(&'a str),
    Generichide,
    Document,
    Popup(bool),
    Popunder,
    InlineScript(bool),
    InlineFont(bool),
    Image(bool),
    Media(bool),
    Object(bool),
    Other(bool),
    Ping(bool),
    Script(bool),
    Stylesheet(bool),
    Subdocument(bool),
    XmlHttpRequest(bool),
    Websocket(bool),
    Font(bool),
    All,
    Method(Vec<(bool, HttpMethod)>),
}

impl NetworkFilterOption<'_> {
    pub fn is_content_type(&self) -> bool {
        matches!(
            self,
            Self::Document
                | Self::Image(..)
                | Self::Media(..)
                | Self::Object(..)
                | Self::Other(..)
                | Self::Ping(..)
                | Self::Script(..)
                | Self::Stylesheet(..)
                | Self::Subdocument(..)
                | Self::XmlHttpRequest(..)
                | Self::Websocket(..)
                | Self::Font(..)
                | Self::All
        )
    }

    pub fn is_redirection(&self) -> bool {
        matches!(self, Self::Redirect(..) | Self::RedirectRule(..))
    }
}

/// Abstract syntax representation of a network filter. This representation can fully specify the
/// string representation of a filter as written, with the exception of aliased options like `1p`
/// or `ghide`. This allows separation of concerns between parsing and interpretation.
pub(crate) struct AbstractNetworkFilter<'a> {
    pub(crate) exception: bool,
    pub(crate) pattern: NetworkFilterPattern,
    pub(crate) options: Option<Vec<NetworkFilterOption<'a>>>,
}

impl AbstractNetworkFilter<'_> {
    pub(crate) fn parse<'a>(
        line: &'a str,
    ) -> Result<AbstractNetworkFilter<'a>, NetworkFilterError> {
        let mut filter_index_start: usize = 0;
        let mut filter_index_end: usize = line.len();

        let mut exception = false;
        if line.starts_with("@@") {
            filter_index_start += 2;
            exception = true;
        }

        let maybe_options_index = network_filter_option_start(line);

        let mut options = None;
        if let Some(options_index) = maybe_options_index {
            filter_index_end = options_index;

            // slicing here is safe; the first byte after '$' will be a character boundary
            let raw_options = &line[filter_index_end + 1..];

            options = Some(parse_filter_options(raw_options, exception)?);
        }

        let left_anchor = if line[filter_index_start..].starts_with("||") {
            filter_index_start += 2;
            Some(NetworkFilterLeftAnchor::DoublePipe)
        } else if line[filter_index_start..].starts_with('|') {
            filter_index_start += 1;
            Some(NetworkFilterLeftAnchor::SinglePipe)
        } else {
            None
        };

        let right_anchor = if filter_index_end > 0
            && filter_index_end > filter_index_start
            && line[..filter_index_end].ends_with('|')
        {
            filter_index_end -= 1;
            Some(NetworkFilterRightAnchor::SinglePipe)
        } else {
            None
        };

        Ok(AbstractNetworkFilter {
            exception,
            pattern: NetworkFilterPattern {
                left_anchor,
                start: filter_index_start,
                end: filter_index_end,
                right_anchor,
            },
            options,
        })
    }
}

/// A slash-leading URL path is not necessarily a regex. A closing slash
/// starts an option boundary only when followed immediately by `$`.
pub(crate) fn network_filter_option_start(line: &str) -> Option<usize> {
    let pattern = line.strip_prefix("@@").unwrap_or(line);
    let prefix = line.len() - pattern.len();
    if let Some(end) = regex_literal_end(pattern) {
        if end + 1 == pattern.len() { return None; }
        if pattern.as_bytes().get(end + 1) == Some(&b'$') { return Some(prefix + end + 1); }
    }
    pattern.find('$').map(|index| prefix + index)
}

pub(crate) fn split_filter_options(raw_options: &str) -> Result<Vec<&str>, NetworkFilterError> {
    let mut result = Vec::new();
    if raw_options.is_empty() || raw_options.ends_with(',') {
        return Err(NetworkFilterError::FilterParseError);
    }

    let mut remaining = raw_options;
    while !remaining.is_empty() {
        let regex_value = remaining.strip_prefix("removeparam=").or_else(|| remaining.strip_prefix("queryprune=")).or_else(|| remaining.strip_prefix("ipaddress="))
            .map(|value| value.strip_prefix('~').unwrap_or(value));
        let domain_value = ["domain=", "from=", "to=", "top=", "denyallow="]
            .iter().find_map(|prefix| remaining.strip_prefix(prefix));
        let option_end = if let Some(mut value) = domain_value {
            let mut cursor = remaining.len() - value.len();
            loop {
                if let Some(v) = value.strip_prefix('~') { value = v; cursor += 1; }
                let count = if value.starts_with('/') {
                    regex_literal_end(value).ok_or(NetworkFilterError::NoSupportedDomains)? + 1
                } else { value.find(['|', ',']).unwrap_or(value.len()) };
                cursor += count;
                value = &value[count..];
                if value.starts_with('|') { value = &value[1..]; cursor += 1; continue; }
                break if value.starts_with(',') { Some(cursor) } else { None };
            }
        } else if let Some(value) = regex_value.filter(|value| value.starts_with('/')) {
            let end = regex_literal_end(value).ok_or(NetworkFilterError::RemoveparamRegexUnsupported)?;
            let suffix_start = remaining.len() - value.len() + end + 1;
            remaining[suffix_start..].find(',').map(|index| suffix_start + index)
        } else {
            remaining.find(',')
        };
        let raw_option = &remaining[..option_end.unwrap_or(remaining.len())];
        remaining = option_end.map(|index| &remaining[index + 1..]).unwrap_or("");

        if raw_option.is_empty() { return Err(NetworkFilterError::FilterParseError); }
        result.push(raw_option);
    }
    Ok(result)
}

fn parse_filter_options<'a>(
    raw_options: &'a str,
    exception: bool,
) -> Result<Vec<NetworkFilterOption<'a>>, NetworkFilterError> {
    let mut result = vec![];
    for raw_option in split_filter_options(raw_options)? {

        // uBO's underscore-only option is a no-op delimiter/disambiguator.
        if !raw_option.is_empty() && raw_option.bytes().all(|byte| byte == b'_') {
            continue;
        }

        // Check for negation: ~option
        let negation = raw_option.starts_with('~');
        let maybe_negated_option = raw_option.trim_start_matches('~');

        // Check for options: option=value1|value2
        let mut option_and_values = maybe_negated_option.splitn(2, '=');
        let (option, value) = (
            option_and_values.next().unwrap(),
            option_and_values.next().unwrap_or_default(),
        );

        result.push(match (option, negation) {
            ("domain", _) | ("from", _) => {
                NetworkFilterOption::Domain(parse_pipe_delimited_domains(value)?)
            }
            ("denyallow", false) => {
                if value.is_empty() || value.contains('~') { return Err(NetworkFilterError::UnrecognisedOption); }
                NetworkFilterOption::To(parse_pipe_delimited_domains(value)?.into_iter().map(|(_, domain)| (false, domain)).collect())
            }
            ("strict3p", negated) | ("strict-third-party", negated) => NetworkFilterOption::StrictThirdParty(!negated),
            ("strict1p", negated) | ("strict-first-party", negated) => NetworkFilterOption::StrictThirdParty(negated),
            ("ipaddress", false) if !value.is_empty() && value.len() <= 16384 => {
                if value.starts_with('/') && compile_removeparam_regex(value).is_none() {
                    return Err(NetworkFilterError::UnrecognisedOption);
                }
                NetworkFilterOption::IpAddress(value)
            }
            ("top", false) => NetworkFilterOption::Top(parse_pipe_delimited_domains(value)?),
            ("to", _) => NetworkFilterOption::To(parse_pipe_delimited_domains(value)?),
            ("badfilter", true) => return Err(NetworkFilterError::NegatedBadFilter),
            ("badfilter", false) => NetworkFilterOption::Badfilter,
            ("important", true) => return Err(NetworkFilterError::NegatedImportant),
            ("important", false) => NetworkFilterOption::Important,
            ("match-case", true) => return Err(NetworkFilterError::NegatedOptionMatchCase),
            ("match-case", false) => NetworkFilterOption::MatchCase,
            ("third-party", negated) | ("3p", negated) => NetworkFilterOption::ThirdParty(!negated),
            ("first-party", negated) | ("1p", negated) => NetworkFilterOption::FirstParty(!negated),
            ("tag", true) => return Err(NetworkFilterError::NegatedTag),
            ("tag", false) => NetworkFilterOption::Tag(value),
            // `rewrite` is the ABP-syntax alias for `redirect`. The `abp-resource:`-prefixed
            // values it uses are shipped by uBO as aliases of the corresponding redirect
            // resources, so no further translation is needed here.
            ("redirect", true) | ("rewrite", true) => {
                return Err(NetworkFilterError::NegatedRedirection);
            }
            ("redirect", false) | ("rewrite", false) => {
                // Ignore this filter if no redirection resource is specified
                if value.is_empty() {
                    return Err(NetworkFilterError::EmptyRedirection);
                }

                NetworkFilterOption::Redirect(value)
            }
            ("empty", false) => NetworkFilterOption::Redirect("empty"),
            ("mp4", false) => NetworkFilterOption::Redirect("noopmp4-1s"),
            ("redirect-rule", true) => return Err(NetworkFilterError::NegatedRedirection),
            ("redirect-rule", false) => {
                if value.is_empty() && !exception {
                    return Err(NetworkFilterError::EmptyRedirection);
                }

                NetworkFilterOption::RedirectRule(value)
            }
            ("csp", _) => {
                NetworkFilterOption::Csp(if !value.is_empty() { Some(value) } else { None })
            }
            ("removeparam", true) | ("queryprune", true) => return Err(NetworkFilterError::NegatedRemoveparam),
            ("removeparam", false) | ("queryprune", false) => {
                // An empty selector removes all parameters, or exempts all
                // parameter removal when used by an exception rule.
                let selector = value.strip_prefix('~').unwrap_or(value);
                let valid = if selector.starts_with('/') {
                    compile_removeparam_regex(selector).is_some()
                } else {
                    selector.is_empty() || VALID_PARAM.is_match(selector)
                };
                if (value.starts_with('~') && selector.is_empty()) || !valid {
                    return Err(NetworkFilterError::RemoveparamRegexUnsupported);
                }
                NetworkFilterOption::Removeparam(value)
            }
            ("generichide", true) | ("ghide", true) => {
                return Err(NetworkFilterError::NegatedGenericHide);
            }
            ("generichide", false) | ("ghide", false) => NetworkFilterOption::Generichide,
            ("document", true) | ("doc", true) => return Err(NetworkFilterError::NegatedDocument),
            ("document", false) | ("doc", false) => NetworkFilterOption::Document,
            ("popunder", false) => NetworkFilterOption::Popunder,
            ("popup", negated) => NetworkFilterOption::Popup(!negated),
            ("inline-script", negated) => NetworkFilterOption::InlineScript(!negated),
            ("inline-font", negated) => NetworkFilterOption::InlineFont(!negated),
            ("image", negated) => NetworkFilterOption::Image(!negated),
            ("media", negated) => NetworkFilterOption::Media(!negated),
            ("object", negated) | ("object-subrequest", negated) => {
                NetworkFilterOption::Object(!negated)
            }
            ("other", negated) => NetworkFilterOption::Other(!negated),
            ("ping", negated) | ("beacon", negated) => NetworkFilterOption::Ping(!negated),
            ("script", negated) => NetworkFilterOption::Script(!negated),
            ("stylesheet", negated) | ("css", negated) => NetworkFilterOption::Stylesheet(!negated),
            ("subdocument", negated) | ("frame", negated) => {
                NetworkFilterOption::Subdocument(!negated)
            }
            ("xmlhttprequest", negated) | ("xhr", negated) => {
                NetworkFilterOption::XmlHttpRequest(!negated)
            }
            ("websocket", negated) => NetworkFilterOption::Websocket(!negated),
            ("font", negated) => NetworkFilterOption::Font(!negated),
            ("all", true) => return Err(NetworkFilterError::NegatedAll),
            ("all", false) => NetworkFilterOption::All,
            ("method", true) => return Err(NetworkFilterError::UnrecognisedOption),
            ("method", false) => {
                if value.is_empty() {
                    return Err(NetworkFilterError::UnrecognisedOption);
                }
                let methods: Vec<(bool, HttpMethod)> = value
                    .split('|')
                    .filter_map(|method| {
                        let (enabled, name) = if let Some(negated) = method.strip_prefix('~') {
                            (false, negated)
                        } else {
                            (true, method)
                        };
                        if name.eq_ignore_ascii_case("get") {
                            Some((enabled, HttpMethod::Get))
                        } else if name.eq_ignore_ascii_case("head") {
                            Some((enabled, HttpMethod::Head))
                        } else if name.eq_ignore_ascii_case("post") {
                            Some((enabled, HttpMethod::Post))
                        } else if name.eq_ignore_ascii_case("connect") {
                            Some((enabled, HttpMethod::Connect))
                        } else if name.eq_ignore_ascii_case("delete") {
                            Some((enabled, HttpMethod::Delete))
                        } else if name.eq_ignore_ascii_case("options") {
                            Some((enabled, HttpMethod::Options))
                        } else if name.eq_ignore_ascii_case("patch") {
                            Some((enabled, HttpMethod::Patch))
                        } else if name.eq_ignore_ascii_case("put") {
                            Some((enabled, HttpMethod::Put))
                        } else if name.eq_ignore_ascii_case("trace") {
                            Some((enabled, HttpMethod::Trace))
                        } else {
                            None
                        }
                    })
                    .collect();
                if methods.is_empty() {
                    return Err(NetworkFilterError::UnrecognisedOption);
                }
                NetworkFilterOption::Method(methods)
            }
            (_, _) => return Err(NetworkFilterError::UnrecognisedOption),
        });
    }
    Ok(result)
}

#[cfg(test)]
#[path = "../../tests/unit/filters/abstract_network.rs"]
mod unit_tests;
