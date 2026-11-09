//! Compiled regexes can take up large amounts of memory. To reduce the overall memory footprint of
//! the [`crate::Engine`], infrequently used regexes can be discarded. The [`RegexManager`] is
//! responsible for managing the storage of regexes used by filters.

use crate::filters::network::{NetworkFilterMask, NetworkFilterMaskHelper};

use regex::{
    Regex, bytes::Regex as BytesRegex, bytes::RegexBuilder as BytesRegexBuilder,
    bytes::RegexSet as BytesRegexSet, bytes::RegexSetBuilder as BytesRegexSetBuilder,
};
use std::sync::{LazyLock, OnceLock};
use std::cell::Cell;

use std::collections::HashMap;
use std::fmt;
use std::time::Duration;

#[cfg(test)]
#[cfg(not(target_arch = "wasm32"))]
use mock_instant::thread_local::Instant;
#[cfg(not(test))]
#[cfg(not(target_arch = "wasm32"))]
use std::time::Instant;

#[cfg(target_arch = "wasm32")]
#[derive(Clone, Copy)]
pub struct Instant;
#[cfg(target_arch = "wasm32")]
impl Instant {
    pub fn now() -> Self {
        Self
    }
}

/// `*const NetworkFilter` could technically leak across threads through `RegexDebugEntry::id`, but
/// it's disguised as a unique identifier and not intended to be dereferenced.
unsafe impl Send for RegexManager {}

const DEFAULT_CLEAN_UP_INTERVAL: Duration = Duration::from_secs(30);
const DEFAULT_DISCARD_UNUSED_TIME: Duration = Duration::from_secs(180);

// Modifiers use a separate namespace from URL regex IDs.
const MODIFIER_CACHE_CAPACITY: usize = 64;

/// Return the closing slash, respecting escapes and character classes.
pub(crate) fn regex_literal_end(value: &str) -> Option<usize> {
    if !value.starts_with('/') { return None; }
    let mut escaped = false;
    let mut in_class = false;
    for (index, byte) in value.bytes().enumerate().skip(1) {
        if escaped { escaped = false; continue; }
        match byte {
            b'\\' => escaped = true,
            b'[' => in_class = true,
            b']' => in_class = false,
            b'/' if !in_class => return Some(index),
            _ => {}
        }
    }
    None
}

/// The host supplies Ladybird's bounded ECMAScript VM for unsupported patterns.
/// Results: 1 match/valid, 0 no match, negative invalid or exhausted.
pub type HostRegexEvaluator = unsafe extern "C" fn(*const u8, usize, bool, *const u8, usize, u8, *mut u64, *mut u8, usize, *mut usize) -> i8;
static HOST_REGEX: OnceLock<HostRegexEvaluator> = OnceLock::new();
thread_local! { static HOST_BUDGET: Cell<u64> = const { Cell::new(200_000) }; }
pub fn install_host_regex(evaluator: HostRegexEvaluator) { let _ = HOST_REGEX.set(evaluator); }
pub fn reset_host_regex_budget() { HOST_BUDGET.with(|b| b.set(200_000)); }

// Rust-only syntax must not acquire different semantics merely because the
// linear engine can compile it. This deliberately conservative subset retains
// the fast path for ordinary ASCII expressions; the host owns everything else.
fn ascii_ecmascript_fast_path(pattern: &str) -> bool {
    if !pattern.is_ascii() { return false; }
    let bytes = pattern.as_bytes();
    let mut index = 0;
    let mut in_class = false;
    while index < bytes.len() {
        let byte = bytes[index];
        if byte == b'\\' {
            index += 1;
            let Some(&escaped) = bytes.get(index) else { return false; };
            if !b"dDsSwWbBtnrf\\/.*+?()[]{}^$|-".contains(&escaped) { return false; }
            // In a JS character class \b is backspace, not a boundary.
            if in_class && matches!(escaped, b'b' | b'B') { return false; }
        } else if byte == b'[' {
            if in_class { return false; }
            in_class = true;
        } else if byte == b']' {
            if !in_class { return false; }
            in_class = false;
        } else if in_class {
            if matches!(byte, b'&' | b'~' | b'-') && bytes.get(index + 1) == Some(&byte) { return false; }
        } else {
            if matches!(byte, b'{' | b'}') { return false; }
            if byte == b'(' && bytes.get(index + 1) == Some(&b'?') && bytes.get(index + 2) != Some(&b':') { return false; }
            // Rust accepts possessive-looking repeated quantifiers that do not
            // share ECMAScript parsing rules.
            if matches!(byte, b'*' | b'+' | b'?') && bytes.get(index + 1) == Some(&b'+') { return false; }
        }
        index += 1;
    }
    !in_class
}

#[derive(Debug, Clone)]
pub struct CompatibleRegex { pattern: String, ignore_case: bool, fast: Option<BytesRegex> }
impl CompatibleRegex {
    fn compile(pattern: &str, ignore_case: bool) -> Option<Self> {
        if pattern.len() > 16384 { return None; }
        let fast = BytesRegexBuilder::new(pattern).unicode(false).case_insensitive(ignore_case)
            .size_limit(128 * 1024).dfa_size_limit(128 * 1024).nest_limit(128).build().ok();
        let mut result = Self { pattern: pattern.to_owned(), ignore_case, fast };
        if HOST_REGEX.get().is_some() {
            if !ascii_ecmascript_fast_path(pattern) {
                result.fast = None;
                if result.host_match("", true) != Some(true) { return None; }
            } else if result.fast.is_none() && result.host_match("", true) != Some(true) {
                return None;
            }
            Some(result)
        } else if result.fast.is_some() { Some(result) } else { None }
    }
    fn host_match(&self, input: &str, validate: bool) -> Option<bool> {
        let evaluator = HOST_REGEX.get()?;
        HOST_BUDGET.with(|budget| {
            let mut remaining = budget.get();
            // The host borrows these slices and the budget only for this call.
            let result = unsafe { evaluator(self.pattern.as_ptr(), self.pattern.len(), self.ignore_case,
                input.as_ptr(), input.len(), u8::from(validate), &mut remaining, std::ptr::null_mut(), 0, std::ptr::null_mut()) };
            budget.set(remaining);
            match result { 0 => Some(false), 1 => Some(true), _ => None }
        })
    }
    fn fast_for_input(&self, input: &str) -> Option<&BytesRegex> {
        // Byte matching differs from JS UTF-16 for non-ASCII input, and JS
        // dot/end assertions differ for line terminators. The standalone crate
        // retains its Rust behavior when no host evaluator is installed.
        if HOST_REGEX.get().is_some() && (!input.is_ascii() || input.contains(['\r', '\n'])) { return None; }
        self.fast.as_ref()
    }
    fn first_range(&self, input: &str) -> Option<(usize, usize)> {
        if let Some(regex) = self.fast_for_input(input) { return regex.find(input.as_bytes()).map(|m| (m.start(), m.end())); }
        let evaluator = HOST_REGEX.get()?;
        HOST_BUDGET.with(|budget| {
            let mut remaining = budget.get();
            let mut bytes = [0u8; 16]; let mut length = 0usize;
            let result = unsafe { evaluator(self.pattern.as_ptr(), self.pattern.len(), self.ignore_case,
                input.as_ptr(), input.len(), 3, &mut remaining, bytes.as_mut_ptr(), bytes.len(), &mut length) };
            budget.set(remaining);
            if result != 1 || length != 16 { return None; }
            let start = u64::from_le_bytes(bytes[..8].try_into().ok()?) as usize;
            let end = u64::from_le_bytes(bytes[8..].try_into().ok()?) as usize;
            (start <= end && end <= input.len()).then_some((start, end))
        })
    }
    pub(crate) fn matches(&self, input: &str) -> Option<bool> {
        self.fast_for_input(input).map(|r| r.is_match(input.as_bytes())).or_else(|| self.host_match(input, false))
    }
}

pub(crate) fn compile_removeparam_regex(value: &str) -> Option<CompatibleRegex> {
    if value.len() > 16384 { return None; }
    let end = regex_literal_end(value)?;
    let flags = &value[end + 1..];
    if flags != "" && flags != "i" { return None; }
    CompatibleRegex::compile(&value[1..end].replace(r"\/", "/"), flags == "i")
}

/// Reports [`RegexManager`] metrics that may be useful for creating an optimized
/// [`RegexManagerDiscardPolicy`].
#[cfg(feature = "debug-info")]
pub struct RegexDebugInfo {
    /// Information about each regex contained in the [`RegexManager`].
    pub regex_data: Vec<RegexDebugEntry>,
    /// Total count of compiled regexes.
    pub compiled_regex_count: usize,
}

/// Describes metrics about a single regex from the [`RegexManager`].
#[cfg(feature = "debug-info")]
pub struct RegexDebugEntry {
    /// Id for this particular regex, which is constant and unique for its lifetime.
    ///
    /// Note that there are no guarantees about a particular id's constancy or uniqueness beyond
    /// the lifetime of a corresponding regex.
    pub id: u64,
    /// A string representation of this regex, if available. It may be `None` if the regex has been
    /// cleaned up to conserve memory.
    pub regex: Option<String>,
    /// When this regex was last used.
    pub last_used: Instant,
    /// How many times this regex has been used.
    pub usage_count: usize,
}

#[derive(Debug, Clone)]
pub enum CompiledRegex {
    Compiled(BytesRegex),
    CompiledSet(BytesRegexSet),
    Compatible(Vec<CompatibleRegex>),
    MatchAll,
    RegexParsingError(regex::Error),
}

impl CompiledRegex {
    pub(crate) fn first_range(&self, input: &str) -> Option<(usize, usize)> {
        match self {
            Self::Compiled(r) => r.find(input.as_bytes()).map(|m| (m.start(), m.end())),
            Self::Compatible(rs) => rs.iter().filter_map(|r| r.first_range(input)).min_by_key(|r| r.0),
            _ => None,
        }
    }
    pub fn is_match(&self, pattern: &str) -> bool {
        match &self {
            CompiledRegex::MatchAll => true, // simple case for matching everything, e.g. for empty filter
            CompiledRegex::RegexParsingError(_e) => false, // no match if regex didn't even compile
            CompiledRegex::Compiled(r) => r.is_match(pattern.as_bytes()),
            CompiledRegex::Compatible(rs) => rs.iter().any(|r| r.matches(pattern) == Some(true)),
            CompiledRegex::CompiledSet(r) => {
                // let matches: Vec<_> = r.matches(pattern).into_iter().collect();
                // println!("Matching {} against RegexSet: {:?}", pattern, matches);
                r.is_match(pattern.as_bytes())
            }
        }
    }
}

impl fmt::Display for CompiledRegex {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match &self {
            CompiledRegex::MatchAll => write!(f, ".*"), // simple case for matching everything, e.g. for empty filter
            CompiledRegex::RegexParsingError(_e) => write!(f, "ERROR"), // no match if regex didn't even compile
            CompiledRegex::Compiled(r) => write!(f, "{}", r.as_str()),
            CompiledRegex::Compatible(rs) => write!(f, "{}", rs.iter().map(|r| r.pattern.as_str()).collect::<Vec<_>>().join(" | ")),
            CompiledRegex::CompiledSet(r) => write!(f, "{}", r.patterns().join(" | ")),
        }
    }
}

struct RegexEntry {
    regex: Option<CompiledRegex>,
    last_used: Instant,
    usage_count: usize,
}

/// Used for customization of regex discarding behavior in the [`RegexManager`].
pub struct RegexManagerDiscardPolicy {
    /// The [`RegexManager`] will check for and cleanup unused filters on this interval.
    pub cleanup_interval: Duration,
    /// The [`RegexManager`] will discard a regex if it hasn't been used for this much time.
    pub discard_unused_time: Duration,
}

impl Default for RegexManagerDiscardPolicy {
    fn default() -> Self {
        Self {
            cleanup_interval: DEFAULT_CLEAN_UP_INTERVAL,
            discard_unused_time: DEFAULT_DISCARD_UNUSED_TIME,
        }
    }
}

type RandomState = std::hash::BuildHasherDefault<seahash::SeaHasher>;

/// A manager that creates and stores all regular expressions used by filters.
/// Rarely used entries are discarded to save memory.
///
/// The [`RegexManager`] is not thread safe, so any access to it must be synchronized externally.
pub struct RegexManager {
    map: HashMap<u64, RegexEntry, RandomState>,
    compiled_regex_count: usize,
    modifier_map: HashMap<String, (Option<CompatibleRegex>, u64)>,
    modifier_clock: u64,
    now: Instant,
    #[cfg_attr(target_arch = "wasm32", allow(unused))]
    last_cleanup: Instant,
    discard_policy: RegexManagerDiscardPolicy,
}

impl Default for RegexManager {
    fn default() -> Self {
        Self {
            map: Default::default(),
            compiled_regex_count: 0,
            modifier_map: HashMap::new(),
            modifier_clock: 0,
            now: Instant::now(),
            last_cleanup: Instant::now(),
            discard_policy: Default::default(),
        }
    }
}

fn make_regexp<'a, FiltersIter>(mask: NetworkFilterMask, filters: FiltersIter) -> CompiledRegex
where
    FiltersIter: Iterator<Item = &'a str> + ExactSizeIterator,
{
    compile_regex(
        filters,
        mask.is_right_anchor(),
        mask.is_left_anchor(),
        mask.is_complete_regex(),
    )
}

/// Compiles a filter pattern to a regex. This is only performed *lazily* for
/// filters containing at least a * or ^ symbol. Because Regexes are expansive,
/// we try to convert some patterns to plain filters.
#[allow(clippy::trivial_regex)]
pub(crate) fn compile_regex<'a, I>(
    filters: I,
    is_right_anchor: bool,
    is_left_anchor: bool,
    is_complete_regex: bool,
) -> CompiledRegex
where
    I: Iterator<Item = &'a str> + ExactSizeIterator,
{
    // Escape special regex characters: |.$+?{}()[]\
    static SPECIAL_RE: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"([\|\.\$\+\?\{\}\(\)\[\]])").unwrap());
    // * can match anything
    static WILDCARD_RE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"\*").unwrap());
    // ^ can match any separator or the end of the pattern
    static ANCHOR_RE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"\^(.)").unwrap());
    // ^ can match any separator or the end of the pattern
    static ANCHOR_RE_EOL: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"\^$").unwrap());

    let mut escaped_patterns = Vec::with_capacity(filters.len());
    for filter_str in filters {
        // If any filter is empty, the entire set matches anything
        if filter_str.is_empty() {
            return CompiledRegex::MatchAll;
        }
        if is_complete_regex {
            // unescape unrecognised escaping sequences, otherwise a normal regex
            let unescaped = filter_str[1..filter_str.len() - 1]
                .replace("\\/", "/")
                .replace("\\:", ":");

            escaped_patterns.push(unescaped);
        } else {
            let repl = SPECIAL_RE.replace_all(filter_str, "\\$1");
            let repl = WILDCARD_RE.replace_all(&repl, ".*");
            // in adblock rules, '^' is a separator.
            // The separator character is anything but a letter, a digit, or one of the following: _ - . %
            let repl = ANCHOR_RE.replace_all(&repl, "(?:[^\\w\\d\\._%-])$1");
            let repl = ANCHOR_RE_EOL.replace_all(&repl, "(?:[^\\w\\d\\._%-]|$)");

            // Should match start or end of url
            let left_anchor = if is_left_anchor { "^" } else { "" };
            let right_anchor = if is_right_anchor { "$" } else { "" };
            let filter = format!("{left_anchor}{repl}{right_anchor}");

            escaped_patterns.push(filter);
        }
    }

    if escaped_patterns.is_empty() {
        CompiledRegex::MatchAll
    } else if is_complete_regex && HOST_REGEX.get().is_some() {
        // Raw filter expressions need the same dialect/input selection as
        // modifiers and headers, including optimizer-fused expressions.
        CompiledRegex::Compatible(escaped_patterns.iter()
            .filter_map(|pattern| CompatibleRegex::compile(pattern, false)).collect())
    } else if escaped_patterns.len() == 1 {
        let pattern = &escaped_patterns[0];
        match BytesRegexBuilder::new(pattern).unicode(false).build() {
            Ok(compiled) => CompiledRegex::Compiled(compiled),
            Err(e) => {
                CompatibleRegex::compile(pattern, false).map(|r| CompiledRegex::Compatible(vec![r]))
                    .unwrap_or(CompiledRegex::RegexParsingError(e))
            }
        }
    } else {
        match BytesRegexSetBuilder::new(&escaped_patterns)
            .unicode(false)
            .build()
        {
            Ok(compiled) => CompiledRegex::CompiledSet(compiled),
            Err(e) => {
                // One unsupported member must not discard every fused rule.
                let compiled: Vec<_> = escaped_patterns.iter().filter_map(|p| CompatibleRegex::compile(p, false)).collect();
                if compiled.is_empty() { CompiledRegex::RegexParsingError(e) } else { CompiledRegex::Compatible(compiled) }
            },
        }
    }
}

impl RegexManager {
    pub(crate) fn matches_removeparam(&mut self, selector: &str, pair: &str) -> Option<bool> {
        self.modifier_clock = self.modifier_clock.wrapping_add(1);
        if !self.modifier_map.contains_key(selector) {
            if self.modifier_map.len() >= MODIFIER_CACHE_CAPACITY {
                if let Some(oldest) = self.modifier_map.iter()
                    .min_by_key(|(_, (_, tick))| *tick)
                    .map(|(key, _)| key.clone())
                {
                    self.modifier_map.remove(&oldest);
                }
            }
            self.modifier_map.insert(selector.to_owned(), (compile_removeparam_regex(selector), self.modifier_clock));
        }
        let (compiled, used) = self.modifier_map.get_mut(selector).unwrap();
        *used = self.modifier_clock;
        // Invalid inverted regexes must not match everything.
        compiled.as_ref().and_then(|regex| regex.matches(pair))
    }

    /// Check whether or not a regex network filter matches a certain URL pattern, using the
    /// [`RegexManager`]'s managed regex storage.
    pub fn matches<'a, FiltersIter>(
        &mut self,
        mask: NetworkFilterMask,
        filters: FiltersIter,
        key: u64,
        pattern: &str,
    ) -> bool
    where
        FiltersIter: Iterator<Item = &'a str> + ExactSizeIterator,
    {
        if !mask.is_regex() && !mask.is_complete_regex() {
            return true;
        }
        use std::collections::hash_map::Entry;
        match self.map.entry(key) {
            Entry::Occupied(mut e) => {
                let v = e.get_mut();
                v.usage_count += 1;
                v.last_used = self.now;
                if v.regex.is_none() {
                    // A discarded entry, recreate it:
                    v.regex = Some(make_regexp(mask, filters));
                    self.compiled_regex_count += 1;
                }
                v.regex.as_ref().unwrap().is_match(pattern)
            }
            Entry::Vacant(e) => {
                self.compiled_regex_count += 1;
                let new_entry = RegexEntry {
                    regex: Some(make_regexp(mask, filters)),
                    last_used: self.now,
                    usage_count: 1,
                };
                e.insert(new_entry)
                    .regex
                    .as_ref()
                    .unwrap()
                    .is_match(pattern)
            }
        }
    }

    /// The [`RegexManager`] is just a struct and doesn't manage any worker threads, so this method
    /// must be called periodically to ensure that it can track usage patterns of regexes over
    /// time. This method will handle periodically discarding filters if necessary.
    #[cfg(not(target_arch = "wasm32"))]
    pub fn update_time(&mut self) {
        self.now = Instant::now();
        if !self.discard_policy.cleanup_interval.is_zero()
            && self.now - self.last_cleanup >= self.discard_policy.cleanup_interval
        {
            self.last_cleanup = self.now;
            self.cleanup();
        }
    }

    #[cfg(not(target_arch = "wasm32"))]
    pub(crate) fn cleanup(&mut self) {
        let now = self.now;
        for v in self.map.values_mut() {
            if now - v.last_used >= self.discard_policy.discard_unused_time {
                // Discard the regex to save memory.
                v.regex = None;
            }
        }
    }

    /// Customize the discard behavior of this [`RegexManager`].
    pub fn set_discard_policy(&mut self, new_discard_policy: RegexManagerDiscardPolicy) {
        self.discard_policy = new_discard_policy;
    }

    /// Discard one regex, identified by its id from a [`RegexDebugEntry`].
    #[cfg(feature = "debug-info")]
    pub fn discard_regex(&mut self, regex_id: u64) {
        self.map
            .iter_mut()
            .filter(|(k, _)| { **k } == regex_id)
            .for_each(|(_, v)| {
                v.regex = None;
            });
    }

    #[cfg(feature = "debug-info")]
    pub(crate) fn get_debug_regex_data(&self) -> Vec<RegexDebugEntry> {
        use itertools::Itertools;
        self.map
            .iter()
            .map(|(k, e)| RegexDebugEntry {
                id: { *k },
                regex: e.regex.as_ref().map(|x| x.to_string()),
                last_used: e.last_used,
                usage_count: e.usage_count,
            })
            .collect_vec()
    }

    #[cfg(feature = "debug-info")]
    pub(crate) fn get_compiled_regex_count(&self) -> usize {
        self.compiled_regex_count
    }

    /// Collect metrics that may be useful for creating an optimized [`RegexManagerDiscardPolicy`].
    #[cfg(feature = "debug-info")]
    pub fn get_debug_info(&self) -> RegexDebugInfo {
        RegexDebugInfo {
            regex_data: self.get_debug_regex_data(),
            compiled_regex_count: self.get_compiled_regex_count(),
        }
    }
}

#[cfg(test)]
#[path = "../tests/unit/regex_manager.rs"]
mod unit_tests;

/// Capture extraction uses the same bounded ECMAScript VM and per-operation budget.
pub(crate) fn capture_first(pattern: &str, input: &str) -> Option<String> {
    let evaluator = HOST_REGEX.get()?;
    HOST_BUDGET.with(|budget| {
        let mut remaining = budget.get();
        let mut output = vec![0u8; 16384];
        let mut length = 0usize;
        let result = unsafe { evaluator(pattern.as_ptr(), pattern.len(), false, input.as_ptr(), input.len(),
            2, &mut remaining, output.as_mut_ptr(), output.len(), &mut length) };
        budget.set(remaining);
        if result != 1 || length > output.len() { return None; }
        output.truncate(length);
        String::from_utf8(output).ok()
    })
}
