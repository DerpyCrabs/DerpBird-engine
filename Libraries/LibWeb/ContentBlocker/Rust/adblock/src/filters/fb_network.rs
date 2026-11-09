//! Flatbuffer-compatible versions of [NetworkFilter] and related functionality.

use crate::filters::filter_data_context::FilterDataContext;
use crate::filters::network::{NetworkFilterMask, NetworkFilterMaskHelper, NetworkMatchable};
use crate::flatbuffers::unsafe_tools::fb_vector_to_slice;

use crate::regex_manager::RegexManager;
use crate::request::Request;

use crate::filters::flatbuffer_generated::fb;
use crate::sourcemap::{FilterRuleDebugInfo, SourceLocation};

pub(crate) const NO_SOURCE_LINE_INFO: u32 = u32::MAX;

/// A list of string parts that can be matched against a URL.
pub(crate) enum FlatPatterns<'a> {
    /// No patterns to match
    Empty,
    /// Memory-usage optimization - ~95% of filters have <= 1 pattern. Special-casing avoids the
    /// need to hold an extra pointer and vector length.
    Single(&'a str),
    /// More than 1 pattern to match
    Multi(flatbuffers::Vector<'a, flatbuffers::ForwardsUOffset<&'a str>>),
}

impl<'a> FlatPatterns<'a> {
    #[inline(always)]
    pub fn new(
        single_pattern: Option<&'a str>,
        multi_patterns: Option<flatbuffers::Vector<'a, flatbuffers::ForwardsUOffset<&'a str>>>,
    ) -> Self {
        if let Some(single_pattern) = single_pattern {
            FlatPatterns::Single(single_pattern)
        } else if let Some(patterns) = multi_patterns {
            FlatPatterns::Multi(patterns)
        } else {
            FlatPatterns::Empty
        }
    }

    #[inline(always)]
    pub fn iter(&self) -> FlatPatternsIterator<'_> {
        FlatPatternsIterator {
            patterns: self,
            index: 0,
        }
    }
}

/// Iterator over [FlatPatterns].
pub(crate) struct FlatPatternsIterator<'a> {
    patterns: &'a FlatPatterns<'a>,
    index: usize,
}

impl<'a> Iterator for FlatPatternsIterator<'a> {
    type Item = &'a str;

    #[inline(always)]
    fn next(&mut self) -> Option<Self::Item> {
        match &self.patterns {
            FlatPatterns::Empty => None,
            FlatPatterns::Single(s) => {
                if self.index == 0 {
                    self.index += 1;
                    Some(*s)
                } else {
                    None
                }
            }
            FlatPatterns::Multi(v) => {
                if self.index < v.len() {
                    let result = v.get(self.index);
                    self.index += 1;
                    Some(result)
                } else {
                    None
                }
            }
        }
    }
}

impl ExactSizeIterator for FlatPatternsIterator<'_> {
    #[inline(always)]
    fn len(&self) -> usize {
        match &self.patterns {
            FlatPatterns::Empty => 0,
            FlatPatterns::Single(_) => 1_usize.saturating_sub(self.index),
            FlatPatterns::Multi(v) => v.len().saturating_sub(self.index),
        }
    }
}

/// Internal implementation of [NetworkFilter] that is compatible with flatbuffers.
pub(crate) struct FlatNetworkFilter<'a> {
    key: u64,
    filter_data_context: &'a FilterDataContext,
    fb_filter: &'a fb::NetworkFilter<'a>,

    pub(crate) mask: NetworkFilterMask,
}

impl<'a> FlatNetworkFilter<'a> {
    #[inline(always)]
    pub fn new(
        filter: &'a fb::NetworkFilter<'a>,
        filter_data_context: &'a FilterDataContext,
    ) -> Self {
        // Use the flatbuffer relative location as key, it's unique for
        // each filter regardless of the filter list it belongs to.
        let key = filter._tab.loc() as u64;

        Self {
            key,
            fb_filter: filter,
            mask: NetworkFilterMask::from_bits_retain(filter.mask()),
            filter_data_context,
        }
    }

    #[inline(always)]
    pub fn tag(&self) -> Option<&'a str> {
        self.fb_filter.tag()
    }

    #[inline(always)]
    pub fn modifier_option(&self) -> Option<String> {
        self.fb_filter.modifier_option().map(|o| o.to_string())
    }

    #[inline(always)]
    pub fn hostname(&self) -> Option<&'a str> {
        if self.mask.is_hostname_anchor() {
            self.fb_filter.hostname()
        } else {
            None
        }
    }

    #[inline(always)]
    pub fn patterns(&self) -> FlatPatterns<'_> {
        FlatPatterns::new(
            self.fb_filter.single_pattern(),
            self.fb_filter.multi_patterns(),
        )
    }

    #[inline(always)]
    pub fn raw_line(&self) -> String {
        debug_assert!(
            self.filter_data_context.debug,
            "raw_line is only available in debug mode"
        );
        match self.fb_filter.raw_line() {
            Some(v) => crate::sourcemap::bounded_rule_text(v),
            None => {
                debug_assert!(false, "raw_line is not set");
                Default::default()
            }
        }
    }

    #[inline(always)]
    fn source_index(&self) -> Option<u32> {
        if !self.filter_data_context.debug {
            debug_assert!(false, "source_index is only available in debug mode");
            return None;
        }

        let index = self.fb_filter.source_index();
        if index == NO_SOURCE_LINE_INFO {
            None
        } else {
            Some(index)
        }
    }

    #[inline(always)]
    fn line_number(&self) -> Option<u32> {
        if !self.filter_data_context.debug {
            debug_assert!(false, "line_number is only available in debug mode");
            return None;
        }

        let number = self.fb_filter.line_number();
        if number == NO_SOURCE_LINE_INFO {
            None
        } else {
            Some(number)
        }
    }

    fn source_location(&self) -> Option<SourceLocation> {
        let source_index = self.source_index();
        let line_number = self.line_number();

        match (source_index, line_number) {
            (Some(source_index), Some(line_number)) => Some(SourceLocation {
                source_index,
                line_number,
            }),
            (None, None) => None,
            _ => {
                debug_assert!(
                    false,
                    "source_index and line_number should always be available together"
                );
                None
            }
        }
    }

    /// Gets [FilterRuleDebugInfo] corresponding to the original filter rule if debug information
    /// was enabled.
    pub fn get_rule_debug_info(&self) -> Option<FilterRuleDebugInfo> {
        if !self.filter_data_context.debug {
            return None;
        }

        let raw_line = Some(self.raw_line());
        let source_location = self.source_location();

        Some(FilterRuleDebugInfo {
            raw_line,
            source_location,
        })
    }
}

impl NetworkFilterMaskHelper for FlatNetworkFilter<'_> {
    #[inline]
    fn has_flag(&self, v: NetworkFilterMask) -> bool {
        self.mask.contains(v)
    }
}

impl NetworkMatchable for FlatNetworkFilter<'_> {
    fn matches(&self, request: &Request, regex_manager: &mut RegexManager) -> bool {
        use crate::filters::network_matchers::{
            check_options, check_pattern,
        };
        if !check_options(self.mask, request) {
            return false;
        }
        if let Some(expressions) = self.fb_filter.domain_regexes() {
            for expression in expressions {
                if let Some(pattern) = expression.strip_prefix("i+") {
                    if !request.resolved_ip_addresses.iter().any(|address|
                        crate::ip_address::matches_operand(pattern, address, regex_manager)) {
                        return false;
                    }
                }
            }
        }
        // Positive hostnames and regexes form a union; all exclusions take precedence.
        for (context, include, exclude, hashes, hostname) in [
            (b'd', self.fb_filter.opt_domains(), self.fb_filter.opt_not_domains(), request.source_hostname_hashes.as_deref(), request.source_hostname.as_str()),
            (b't', self.fb_filter.opt_to_domains(), self.fb_filter.opt_not_to_domains(), request.destination_hostname_hashes.as_deref(), request.hostname.as_str()),
            (b'p', self.fb_filter.opt_top_domains(), self.fb_filter.opt_not_top_domains(), request.top_hostname_hashes.as_deref(), request.top_hostname.as_str()),
        ] {
            let matches = |domains: flatbuffers::Vector<'_, u32>| {
                hashes.into_iter().flatten().any(|hash| self.filter_data_context.unique_domains_hashes_map.get(hash)
                    .is_some_and(|index| fb_vector_to_slice(domains).binary_search(index).is_ok()))
            };
            if exclude.is_some_and(matches) { return false; }
            let mut has_include = include.is_some();
            let mut included = include.is_some_and(matches);
            if let Some(expressions) = self.fb_filter.domain_regexes() {
                for expression in expressions {
                    let bytes = expression.as_bytes();
                    if bytes.len() < 4 || bytes[0] != context { continue; }
                    let positive = bytes[1] == b'+';
                    has_include |= positive;
                    let Some(hit) = regex_manager.matches_removeparam(&expression[2..], hostname) else { return false; };
                    if hit && !positive { return false; }
                    included |= hit && positive;
                }
            }
            if has_include && !included { return false; }
        }
        if request.hostname_only_filter && !self.hostname().is_some_and(|host| !host.is_empty()) {
            let url = request.get_url(self.mask.match_case());
            let Some(authority) = url.find("://").map(|i| i + 3) else { return false; };
            let end = url[authority..].find(['/', '?', '#']).map_or(url.len(), |i| authority + i);
            let host_area = url[authority..end].rfind('@').map_or(authority, |i| authority + i + 1);
            let Some(host_start) = url[host_area..end].find(&request.hostname).map(|i| host_area + i) else { return false; };
            let host_end = host_start + request.hostname.len();
            let touches_host = self.patterns().iter().any(|pattern| {
                let compiled = crate::regex_manager::compile_regex(std::iter::once(pattern), self.mask.is_right_anchor(), self.mask.is_left_anchor(), self.mask.is_complete_regex());
                compiled.first_range(url).is_some_and(|(start, end)| start >= host_start && start < host_end && end > host_start)
            });
            if !touches_host { return false; }
        }
        check_pattern(
            self.mask,
            self.patterns().iter(),
            self.hostname(),
            self.key,
            request,
            regex_manager,
        )
    }
}
