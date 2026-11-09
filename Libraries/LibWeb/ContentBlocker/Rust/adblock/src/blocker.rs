//! Holds [`Blocker`], which handles all network-based adblocking queries.

use memchr::memchr as find_char;
use serde::Serialize;
use std::collections::HashSet;
use std::ops::DerefMut;
use std::sync::OnceLock;

use crate::filters::fb_network_builder::NetworkFilterListId;
use crate::filters::filter_data_context::FilterDataContextRef;
use crate::filters::network::NetworkFilterMaskHelper;
use crate::network_filter_list::NetworkFilterList;
use crate::regex_manager::{RegexManager, RegexManagerDiscardPolicy};
use crate::request::Request;
use crate::resources::ResourceStorage;
use crate::sourcemap::FilterRuleDebugInfo;

fn redirect_token_and_priority(value: &str) -> (&str, i32) {
    if let Some((token, priority)) = value.rsplit_once(':') {
        if let Ok(priority) = priority.parse::<i32>() { return (token, priority); }
    }
    (value, 0)
}

/// Describes how a particular network request should be handled.
#[derive(Debug, Serialize, Default)]
pub struct BlockerResult {
    /// Represents any matched blocking rule.
    /// If you just need to check whether or not to block the request, use
    /// [BlockerResult::should_block].
    ///
    /// If `Some`, the request should be blocked, but this may be invalidated
    /// if an exception was also matched.
    ///
    /// If debugging was _not_ enabled (see [`crate::FilterSet::new`]), rule
    /// info will be limited.
    pub filter: Option<FilterRuleDebugInfo>,
    /// Represents any matched exception rule.
    /// If `Some`, this means that there was a match, but the request should
    /// _not_ be blocked.
    ///
    /// If debugging was _not_ enabled (see [`crate::FilterSet::new`]), rule
    /// info will be limited.
    pub exception: Option<FilterRuleDebugInfo>,
    /// Important is used to signal that a rule with the `important` option
    /// matched. An `important` match means that exceptions should not apply
    /// and no further checking is neccesary--the request should be blocked
    /// (empty body or cancelled).
    ///
    /// Brave Browser keeps multiple instances of [`Blocker`], so `important`
    /// here is used to correct behaviour between them: checking should stop
    /// instead of moving to the next instance iff an `important` rule matched.
    pub important: bool,
    /// Specifies what to load instead of the original request, rather than
    /// just blocking it outright. This can come from a filter with a `redirect`
    /// or `redirect-rule` option. If present, the field will contain the body
    /// of the redirect to be injected.
    ///
    /// Note that the presence of a redirect does _not_ imply that the request
    /// should be blocked. The `redirect-rule` option can produce a redirection
    /// that's only applied if another blocking filter matches a request.
    pub redirect: Option<String>,
    /// `removeparam` may remove URL parameters. If the original request URL was
    /// modified at all, the new version will be here. This should be used
    /// as long as the request is not blocked.
    pub rewritten_url: Option<String>,
    pub rewritten_rules: Vec<FilterRuleDebugInfo>,
}

impl BlockerResult {
    /// Should the request be blocked?
    pub fn should_block(&self) -> bool {
        self.important || (self.filter.is_some() && self.exception.is_none())
    }
}

// only check for tags in tagged and exception rule buckets,
// pass empty set for the rest
fn get_no_tags() -> &'static HashSet<String> {
    static NO_TAGS: OnceLock<HashSet<String>> = OnceLock::new();
    NO_TAGS.get_or_init(&HashSet::new)
}

/// Stores network filters for efficient querying.
pub struct Blocker {
    // Enabled tags are not serialized - when deserializing, tags of the existing
    // instance (the one we are recreating lists into) are maintained
    pub(crate) tags_enabled: HashSet<String>,
    // Not serialized
    #[cfg(feature = "single-thread")]
    pub(crate) regex_manager: std::cell::RefCell<RegexManager>,
    #[cfg(not(feature = "single-thread"))]
    pub(crate) regex_manager: std::sync::Mutex<RegexManager>,

    pub(crate) filter_data_context: FilterDataContextRef,
}

#[cfg(feature = "single-thread")]
pub(crate) type RegexManagerRef<'a> = std::cell::RefMut<'a, RegexManager>;
#[cfg(not(feature = "single-thread"))]
pub(crate) type RegexManagerRef<'a> = std::sync::MutexGuard<'a, RegexManager>;

impl Blocker {
    /// Decide if a network request (usually from WebRequest API) should be
    /// blocked, redirected or allowed.
    pub fn check(&self, request: &Request, resources: &ResourceStorage) -> BlockerResult {
        self.check_parameterised(request, resources, false, false)
    }

    pub(crate) fn get_list(&self, id: NetworkFilterListId) -> NetworkFilterList<'_> {
        NetworkFilterList {
            list: self
                .filter_data_context
                .memory
                .root()
                .network_rules()
                .get(id as usize),
            filter_data_context: &self.filter_data_context,
        }
    }

    pub(crate) fn csp(&self) -> NetworkFilterList<'_> {
        self.get_list(NetworkFilterListId::Csp)
    }

    pub(crate) fn exceptions(&self) -> NetworkFilterList<'_> {
        self.get_list(NetworkFilterListId::Exceptions)
    }

    pub(crate) fn importants(&self) -> NetworkFilterList<'_> {
        self.get_list(NetworkFilterListId::Importants)
    }

    pub(crate) fn redirects(&self) -> NetworkFilterList<'_> {
        self.get_list(NetworkFilterListId::Redirects)
    }

    pub(crate) fn removeparam(&self) -> NetworkFilterList<'_> {
        self.get_list(NetworkFilterListId::RemoveParam)
    }

    pub(crate) fn filters(&self) -> NetworkFilterList<'_> {
        self.get_list(NetworkFilterListId::Filters)
    }

    pub(crate) fn generic_hide(&self) -> NetworkFilterList<'_> {
        self.get_list(NetworkFilterListId::GenericHide)
    }

    pub(crate) fn tagged_filters_all(&self) -> NetworkFilterList<'_> {
        self.get_list(NetworkFilterListId::TaggedFiltersAll)
    }

    /// Borrow mutable reference to the regex manager for the ['Blocker`].
    /// Only one caller can borrow the regex manager at a time.
    pub(crate) fn borrow_regex_manager(&self) -> RegexManagerRef<'_> {
        #[cfg(feature = "single-thread")]
        #[allow(unused_mut)]
        let mut manager = self.regex_manager.borrow_mut();
        #[cfg(not(feature = "single-thread"))]
        let mut manager = self.regex_manager.lock().unwrap();

        #[cfg(not(target_arch = "wasm32"))]
        manager.update_time();

        manager
    }

    pub fn check_generic_hide(&self, hostname_request: &Request) -> bool {
        let mut regex_manager = self.borrow_regex_manager();
        self.generic_hide()
            .check(hostname_request, &HashSet::new(), &mut regex_manager)
            .is_some()
    }

    #[cfg(test)]
    pub(crate) fn check_exceptions(&self, request: &Request) -> bool {
        let mut regex_manager = self.borrow_regex_manager();
        self.exceptions()
            .check(request, &HashSet::new(), &mut regex_manager)
            .is_some()
    }

    pub fn check_parameterised(
        &self,
        request: &Request,
        resources: &ResourceStorage,
        matched_rule: bool,
        force_check_exceptions: bool,
    ) -> BlockerResult {
        let mut regex_manager = self.borrow_regex_manager();
        if !request.is_supported {
            return BlockerResult::default();
        }

        // Check the filters in the following order:
        // 1. $important (not subject to exceptions)
        // 2. redirection ($redirect=resource)
        // 3. normal filters - if no match by then
        // 4. exceptions - if any non-important match of forced

        // Always check important filters
        let important_filter = self
            .importants()
            .check(request, get_no_tags(), &mut regex_manager);

        // only check the rest of the rules if not previously matched
        let filter = if important_filter.is_none() && !matched_rule {
            self.tagged_filters_all()
                .check(request, &self.tags_enabled, &mut regex_manager)
                .or_else(|| {
                    self.filters()
                        .check(request, get_no_tags(), &mut regex_manager)
                })
        } else {
            important_filter
        };

        let exception = match filter.as_ref() {
            // if no other rule matches, only check exceptions if forced to
            None if matched_rule || force_check_exceptions => {
                self.exceptions()
                    .check(request, &self.tags_enabled, &mut regex_manager)
            }
            None => None,
            // If matched an important filter, exceptions don't atter
            Some(f) if f.filter_mask.is_important() => None,
            Some(_) => self
                .exceptions()
                .check(request, &self.tags_enabled, &mut regex_manager),
        };

        let redirect_filters =
            self.redirects()
                .check_all(request, get_no_tags(), regex_manager.deref_mut());

        // Extract the highest priority redirect directive.
        // 1. Exceptions - can bail immediately if found
        // 2. Find highest priority non-exception redirect
        let redirect_resource = {
            let mut exceptions = vec![];
            for redirect_filter in redirect_filters.iter() {
                if redirect_filter.filter_mask.is_exception()
                    && let Some(redirect) = redirect_filter.modifier_option.as_ref()
                {
                    exceptions.push(redirect_token_and_priority(redirect).0);
                }
            }
            let mut resource_and_priority = None;
            for redirect_filter in redirect_filters.iter() {
                if !redirect_filter.filter_mask.is_exception()
                    && let Some(redirect) = redirect_filter.modifier_option.as_ref()
                    && !exceptions.contains(&"")
                    && !exceptions.contains(&redirect_token_and_priority(redirect).0)
                {
                    let (resource, priority) = redirect_token_and_priority(redirect);
                    if resource != "none" && !resources.has_redirect_resource(resource) { continue; }
                    if let Some((_, p1)) = resource_and_priority {
                        if priority > p1 {
                            resource_and_priority = Some((resource, priority));
                        }
                    } else {
                        resource_and_priority = Some((resource, priority));
                    }
                }
            }
            resource_and_priority.map(|(r, _)| r)
        };

        let redirect: Option<String> = redirect_resource.and_then(|resource_name| {
            resources.get_redirect_resource(resource_name).or({
                // It's acceptable to pass no redirection if no matching resource is loaded.
                // TODO - it may be useful to return a status flag to indicate that this occurred.
                #[cfg(test)]
                eprintln!("Matched rule with redirect option but did not find corresponding resource to send");
                None
            })
        });

        let important = filter.is_some()
            && filter
                .as_ref()
                .map(|f| f.filter_mask.is_important())
                .unwrap_or_else(|| false);

        let mut rewritten_rules = Vec::new();
        let rewritten_url = if important {
            None
        } else {
            Self::apply_removeparam(&self.removeparam(), request, regex_manager.deref_mut(), &mut rewritten_rules)
        };

        // If something has already matched before but we don't know what, still return a match
        let fallback_match = if matched_rule {
            Some(FilterRuleDebugInfo::default())
        } else {
            None
        };

        BlockerResult {
            filter: filter
                .map(|f| f.debug_data.unwrap_or(FilterRuleDebugInfo::default()))
                .or(fallback_match),
            exception: exception.map(|f| f.debug_data.unwrap_or(FilterRuleDebugInfo::default())),
            important,
            redirect,
            rewritten_url,
            rewritten_rules,
        }
    }

    fn apply_removeparam(
        removeparam_filters: &NetworkFilterList,
        request: &Request,
        regex_manager: &mut RegexManager,
        matched_rules: &mut Vec<FilterRuleDebugInfo>,
    ) -> Option<String> {
        // Preserve each original query slice, including escaping and whether
        // an equals sign was present.
        struct QParam<'a>(&'a str);

        impl std::fmt::Display for QParam<'_> {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                f.write_str(self.0)
            }
        }

        let url = &request.original_url;
        // Only check for removeparam if there's a query string in the request URL
        if let Some(i) = find_char(b'?', url.as_bytes()) {
            let filters = removeparam_filters.check_all(request, get_no_tags(), regex_manager);
            // The usual request does not match a removal rule. Avoid splitting
            // and allocating its query parameters, including for an all-query
            // exception. Keep this before any per-parameter work.
            if !filters.iter().any(|filter| !filter.filter_mask.is_exception())
                || filters.iter().any(|filter| {
                    filter.filter_mask.is_exception()
                        && filter.modifier_option.as_ref().is_some_and(|selector| selector.is_empty())
                })
            {
                return None;
            }
            // String indexing safety: indices come from `.len()` or `find_char` on individual ASCII
            // characters (1 byte each), some plus 1.
            let params_start = i + 1;
            let hash_index = if let Some(j) = find_char(b'#', &url.as_bytes()[params_start..]) {
                params_start + j
            } else {
                url.len()
            };
            let qparams = &url[params_start..hash_index];
            let mut params: Vec<(QParam, bool)> = qparams
                .split('&')
                .map(QParam)
                .map(|param| (param, true))
                .collect();

            let mut rewrite = false;
            // Matching stays in the indexed network-filter engine. Exception
            // selectors protect parameters regardless of filter list order.
            for (param, include) in &mut params {
                // Decode once, without rebuilding the raw query pair.
                let (key, value) = url::form_urlencoded::parse(param.0.as_bytes()).next()
                    .unwrap_or_default();
                let mut regex_pair = None;
                let mut matches_key = |selector: &str| {
                    let inverted = selector.starts_with('~');
                    let selector = selector.strip_prefix('~').unwrap_or(selector);
                    let matched = if selector.starts_with('/') {
                        let pair = regex_pair.get_or_insert_with(|| format!("{key}={value}"));
                        let Some(matched) = regex_manager.matches_removeparam(selector, pair) else {
                            return false;
                        };
                        matched
                    } else {
                        selector.is_empty() || selector == key
                    };
                    matched != inverted
                };
                let excepted = filters.iter().any(|filter| {
                    filter.filter_mask.is_exception()
                        && filter.modifier_option.as_ref().is_some_and(|selector| matches_key(selector))
                });
                if excepted {
                    continue;
                }
                if let Some(filter) = filters.iter().find(|filter| !filter.filter_mask.is_exception()
                    && filter.modifier_option.as_ref().is_some_and(|selector| matches_key(selector))) {
                        *include = false;
                        rewrite = true;
                        if let Some(info) = &filter.debug_data {
                            if !matched_rules.iter().any(|old| old.raw_line == info.raw_line && old.source_location == info.source_location) {
                                matched_rules.push(info.clone());
                            }
                        }
                }
            }
            if rewrite {
                let p = itertools::join(
                    params
                        .into_iter()
                        .filter(|(_, include)| *include)
                        .map(|(param, _)| param),
                    "&",
                );
                let new_param_str = if p.is_empty() {
                    String::from("")
                } else {
                    format!("?{p}")
                };
                Some(format!(
                    "{}{}{}",
                    &url[0..i],
                    new_param_str,
                    &url[hash_index..]
                ))
            } else {
                None
            }
        } else {
            None
        }
    }

    /// Given a "main_frame" or "subdocument" request, check if some content security policies
    /// should be injected in the page.
    pub fn get_csp_directives(&self, request: &Request) -> Option<String> {
        self.get_csp_details(request).map(|(policy, _)| policy)
    }

    pub fn get_csp_details(&self, request: &Request) -> Option<(String, Vec<FilterRuleDebugInfo>)> {
        use crate::request::RequestType;
        if !matches!(request.request_type, RequestType::Document | RequestType::Subdocument) { return None; }
        let mut regex_manager = self.borrow_regex_manager();
        let filters = self.csp().check_all(request, &self.tags_enabled, &mut regex_manager);
        let mut disabled = HashSet::new();
        for filter in &filters {
            if filter.filter_mask.is_exception() {
                match &filter.modifier_option {
                    Some(value) => { disabled.insert(value.as_str()); },
                    None => return None,
                }
            }
        }
        let mut directives = std::collections::BTreeSet::new();
        let mut rules = Vec::new();
        for filter in &filters {
            if filter.filter_mask.is_exception() { continue; }
            if let Some(value) = &filter.modifier_option {
                if !disabled.contains(value.as_str()) {
                    directives.insert(value.as_str());
                    if let Some(info) = &filter.debug_data { rules.push(info.clone()); }
                }
            }
        }
        if directives.is_empty() { None } else { Some((directives.into_iter().collect::<Vec<_>>().join(","), rules)) }
    }

    pub(crate) fn from_context(filter_data_context: FilterDataContextRef) -> Self {
        Self {
            filter_data_context,
            tags_enabled: HashSet::new(),
            regex_manager: Default::default(),
        }
    }

    #[cfg(test)]
    pub fn new(network_filters: impl IntoIterator<Item = impl AsRef<str>>) -> Self {
        let mut filter_set = crate::FilterSet::new(false);
        filter_set.add_filters(network_filters, Default::default());
        let engine = crate::engine::Engine::new_with_filter_set(filter_set);
        Self::from_context(engine.filter_data_context())
    }

    #[cfg(test)]
    pub fn new_debug(network_filters: impl IntoIterator<Item = impl AsRef<str>>) -> Self {
        let mut filter_set = crate::FilterSet::new(true);
        filter_set.add_filters(network_filters, Default::default());
        let engine = crate::engine::Engine::new_with_filter_set(filter_set);
        Self::from_context(engine.filter_data_context())
    }

    #[cfg(test)]
    pub fn new_with_list_text(text: String) -> Self {
        let mut filter_set = crate::FilterSet::new(false);
        filter_set.add_filter_list(text, Default::default());
        let engine = crate::engine::Engine::new_with_filter_set(filter_set);
        Self::from_context(engine.filter_data_context())
    }

    pub fn use_tags(&mut self, tags: &[&str]) {
        let tag_set: HashSet<String> = tags.iter().map(|&t| String::from(t)).collect();
        self.tags_with_set(tag_set);
    }

    pub fn enable_tags(&mut self, tags: &[&str]) {
        let tag_set: HashSet<String> = tags
            .iter()
            .map(|&t| String::from(t))
            .collect::<HashSet<_>>()
            .union(&self.tags_enabled)
            .cloned()
            .collect();
        self.tags_with_set(tag_set);
    }

    pub fn disable_tags(&mut self, tags: &[&str]) {
        let tag_set: HashSet<String> = self
            .tags_enabled
            .difference(&tags.iter().map(|&t| String::from(t)).collect())
            .cloned()
            .collect();
        self.tags_with_set(tag_set);
    }

    fn tags_with_set(&mut self, tags_enabled: HashSet<String>) {
        self.tags_enabled = tags_enabled;
    }

    pub fn tags_enabled(&self) -> Vec<String> {
        self.tags_enabled.iter().cloned().collect()
    }

    pub fn set_regex_discard_policy(&self, new_discard_policy: RegexManagerDiscardPolicy) {
        let mut regex_manager = self.borrow_regex_manager();
        regex_manager.set_discard_policy(new_discard_policy);
    }

    #[cfg(feature = "debug-info")]
    pub fn discard_regex(&self, regex_id: u64) {
        let mut regex_manager = self.borrow_regex_manager();
        regex_manager.discard_regex(regex_id);
    }

    #[cfg(feature = "debug-info")]
    pub fn get_regex_debug_info(&self) -> crate::regex_manager::RegexDebugInfo {
        let regex_manager = self.borrow_regex_manager();
        regex_manager.get_debug_info()
    }
}

#[cfg(test)]
#[path = "../tests/unit/blocker.rs"]
mod unit_tests;
