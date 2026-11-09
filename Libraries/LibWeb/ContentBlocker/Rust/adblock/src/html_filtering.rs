//! HTML response rules use the existing domain/selector compiler in a separate
//! namespace. They must run on an inert document before the live parser receives
//! any bytes; injecting these as cosmetic rules would allow scripts to run first.
use crate::{Engine, FilterSet, lists::ParseOptions, filters::cosmetic::{CosmeticFilter, split_cosmetic_rule}};

pub struct HtmlFilterEngine { engine: Option<Engine> }
impl HtmlFilterEngine {
    pub fn new(rules: &str) -> Self {
        let mut transformed = String::new();
        for line in rules.lines().map(str::trim) {
            let Some((domains, selector, exception)) = split_cosmetic_rule(line) else { continue; };
            let Some(selector) = selector.strip_prefix('^') else { continue; };
            // Response header removal is a distinct stage. HTML filtering is
            // deliberately scoped; a missing positive domain is not a wildcard.
            if domains.is_empty() || domains.starts_with('!') || selector.starts_with("responseheader(") { continue; }
            // Only removal is meaningful for a response document. Existing parser
            // validation rejects malformed selectors and unsupported syntax.
            let converted = format!("{domains}{}{selector}:remove()", if exception { "#@#" } else { "##" });
            let Ok(parsed) = CosmeticFilter::parse(&converted, false, Default::default()) else { continue; };
            // Inspect parsed scopes: a comma inside a negated regex does not
            // introduce a positive domain or authorize a global HTML action.
            if parsed.hostnames.is_none() && parsed.entities.is_none() && parsed.regex_domains.is_empty() { continue; }
            transformed.push_str(&converted);
            transformed.push('\n');
        }
        if transformed.is_empty() { return Self { engine: None }; }
        let mut filters = FilterSet::new(false);
        filters.add_filter_list(transformed, ParseOptions::default());
        Self { engine: Some(Engine::new_with_filter_set(filters)) }
    }

    /// Compiled selector/action JSON, already scoped and exception-filtered.
    pub fn rules_for_url(&self, url: &str) -> Vec<String> {
        let Some(engine) = &self.engine else { return Vec::new(); };
        let mut rules: Vec<_> = engine.url_cosmetic_resources_without_scriptlets(url).procedural_actions.into_iter().collect();
        rules.sort_unstable();
        rules
    }
}

#[cfg(test)]
mod scope_tests {
    use super::HtmlFilterEngine;
    #[test]
    fn html_scopes_share_regex_delimiters_and_require_a_positive_scope() {
        let engine = HtmlFilterEngine::new(r#"
/^news[0-9]{1,3}\.example$/##^script.ad
/^news2/#@#^script.ad
~/^private{1,3}\./##^body
/^news[#x]?\.example$/##^div.ad
"#);
        assert_eq!(engine.rules_for_url("https://news1.example/").len(), 1);
        assert!(engine.rules_for_url("https://news2.example/").is_empty());
        assert_eq!(engine.rules_for_url("https://newsx.example/").len(), 1);
        assert!(engine.rules_for_url("https://unrelated.test/").is_empty());
    }
}
