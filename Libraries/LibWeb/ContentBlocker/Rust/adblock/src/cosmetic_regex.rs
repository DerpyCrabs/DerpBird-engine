//! Regex-scoped cosmetics retain their complete scope and action in the binary
//! cache. Ordinary hostname/entity rules keep using the existing hash indexes.
use std::collections::HashMap;
use std::sync::Arc;
use crate::cosmetic_filter_cache::ProceduralOrActionFilter;
use crate::cosmetic_filter_utils::SpecificFilterType;
use crate::filters::cosmetic::{CosmeticFilter, CosmeticFilterMask};
use crate::regex_manager::{CompatibleRegex, compile_removeparam_regex};
use crate::utils::Hash;

pub(crate) struct RegexCosmeticRule {
    rule: CosmeticFilter,
    positive: Vec<Arc<CompatibleRegex>>,
    negative: Vec<Arc<CompatibleRegex>>,
    pub kind: SpecificFilterType,
}

impl RegexCosmeticRule {
    pub fn is_generic(&self) -> bool {
        !self.rule.mask.contains(CosmeticFilterMask::SCRIPT_INJECT) &&
            (self.rule.wildcard_scope || (self.positive.is_empty() && self.rule.hostnames.is_none()
                && self.rule.entities.is_none() && self.rule.action.is_none() && self.rule.plain_css_selector().is_some()))
    }

    pub fn load<'a>(serialized: impl Iterator<Item = &'a str>) -> Vec<Self> {
        let mut cache: HashMap<String, Option<Arc<CompatibleRegex>>> = HashMap::new();
        let mut compile = |patterns: &[String]| -> Option<Vec<Arc<CompatibleRegex>>> {
            patterns.iter().map(|pattern| cache.entry(pattern.clone())
                .or_insert_with(|| {
                    crate::regex_manager::reset_host_regex_budget();
                    compile_removeparam_regex(pattern).map(Arc::new)
                }).clone()).collect()
        };
        serialized.filter_map(|json| {
            let rule: CosmeticFilter = serde_json::from_str(json).ok()?;
            if rule.selector.is_empty() { return None; }
            let positive = compile(&rule.regex_domains)?;
            let negative = compile(&rule.not_regex_domains)?;
            let kind = if rule.mask.contains(CosmeticFilterMask::SCRIPT_INJECT) {
                SpecificFilterType::InjectScript((rule.plain_css_selector()?.to_owned(), rule.permission))
            } else if let Some(selector) = rule.plain_css_selector().filter(|_| rule.action.is_none()) {
                SpecificFilterType::Hide(selector.to_owned())
            } else {
                SpecificFilterType::ProceduralOrAction(serde_json::to_string(&ProceduralOrActionFilter {
                    selector: rule.selector.clone(), action: rule.action.clone(),
                }).ok()?)
            };
            let kind = if rule.mask.contains(CosmeticFilterMask::UNHIDE) { kind.negated() } else { kind };
            Some(Self { rule, positive, negative, kind })
        }).collect()
    }

    pub fn matches(&self, hostname: &str, hosts: &[Hash], entities: &[Hash], matches: &mut HashMap<usize, Option<bool>>) -> bool {
        let mut evaluate = |regex: &Arc<CompatibleRegex>| *matches.entry(Arc::as_ptr(regex) as usize)
            .or_insert_with(|| regex.matches(hostname));
        let intersects = |scope: &Option<Vec<Hash>>, candidates: &[Hash]| scope.as_ref()
            .is_some_and(|scope| candidates.iter().any(|hash| scope.binary_search(hash).is_ok()));
        if intersects(&self.rule.not_hostnames, hosts) || intersects(&self.rule.not_entities, entities) {
            return false;
        }
        // Invalid/exhausted negative expressions cannot silently broaden a rule.
        if self.negative.iter().any(|regex| evaluate(regex) != Some(false)) { return false; }
        let unrestricted = self.positive.is_empty() && self.rule.hostnames.is_none() && self.rule.entities.is_none();
        self.rule.wildcard_scope || unrestricted || intersects(&self.rule.hostnames, hosts) || intersects(&self.rule.entities, entities)
            || self.positive.iter().any(|regex| evaluate(regex) == Some(true))
    }
}

#[cfg(test)]
mod tests {
    use crate::Engine;

    #[test]
    fn regex_scope_survives_cache_roundtrip_and_preserves_exceptions() {
        let original = Engine::new_with_list_text(r#"
/^news[0-9]{1,3}\.example$/##.regex
example,~/^private\./##.mixed
/^news/##.excepted
news1.example#@#.excepted
news1.example##.ordinary
/^news/#@#.ordinary
/^news/##.card:remove-class(/^ad-/)
/^news2/#@#.card:remove-class(/^ad-/)
"#);
        let mut restored = Engine::default();
        restored.deserialize(&original.serialize()).unwrap();
        for engine in [&original, &restored] {
            let first = engine.url_cosmetic_resources_without_scriptlets("https://news1.example/");
            assert!(first.hide_selectors.contains(".regex"));
            assert!(first.hide_selectors.contains(".mixed"));
            assert!(!first.hide_selectors.contains(".excepted"));
            assert!(!first.hide_selectors.contains(".ordinary"));
            assert_eq!(first.procedural_actions.len(), 1);
            assert!(engine.url_cosmetic_resources_without_scriptlets("https://news2.example/").procedural_actions.is_empty());
            let private = engine.url_cosmetic_resources_without_scriptlets("https://private.example/");
            assert!(!private.hide_selectors.contains(".mixed"));
            assert!(!private.hide_selectors.contains(".regex"));
            let unrelated = engine.url_cosmetic_resources_without_scriptlets("https://unrelated.test/");
            assert!(unrelated.hide_selectors.is_empty());
        }
    }

    #[test]
    fn regex_delimiters_and_invalid_exclusions_do_not_broaden_rules() {
        let engine = Engine::new_with_list_text(r#"
/^news[0-9]{1,3}\.example$/##.commas
/^news[#x]?\.example$/##.hash
example,~/[/##.invalid
"#);
        let result = engine.url_cosmetic_resources_without_scriptlets("https://news12.example/");
        assert!(result.hide_selectors.contains(".commas"));
        assert!(!result.hide_selectors.contains(".invalid"));
        assert!(engine.url_cosmetic_resources_without_scriptlets("https://newsx.example/").hide_selectors.contains(".hash"));
    }
}

#[cfg(test)]
mod policy_tests {
    use crate::Engine;
    #[test]
    fn negative_only_scopes_remain_generic_under_cosmetic_switches() {
        let engine = Engine::new_with_list_text(r#"
~/^private\./##.generic
/^news/##.specific
"#);
        let url = "https://news.example/";
        let specific_off = engine.url_cosmetic_resources_with_policy(url, 0, true, false);
        assert!(specific_off.hide_selectors.contains(".generic"));
        assert!(!specific_off.hide_selectors.contains(".specific"));
        let all_off = engine.url_cosmetic_resources_with_policy(url, 0, false, true);
        assert!(all_off.hide_selectors.is_empty());
        let generic_off = Engine::new_with_list_text(r#"
~/^private\./##.generic
/^news/##.specific
@@||news.example^$generichide
"#).url_cosmetic_resources_without_scriptlets(url);
        assert!(!generic_off.hide_selectors.contains(".generic"));
        assert!(generic_off.hide_selectors.contains(".specific"));
    }
}

#[cfg(test)]
mod wildcard_tests {
    use crate::Engine;
    #[test]
    fn specific_generic_rules_inject_without_survey_and_obey_generic_policy() {
        let engine = Engine::new_with_list_text(r#"
*##.always
*,~private.example##.scoped
*##.card:has-text(Sponsored)
allowed.example#@#.always
allowed.example#@#.card:has-text(Sponsored)
"#);
        let mut restored = Engine::default();
        restored.deserialize(&engine.serialize()).unwrap();
        for engine in [&engine, &restored] {
            let page = engine.url_cosmetic_resources_without_scriptlets("https://news.example/");
            assert!(page.hide_selectors.contains(".always"));
            assert!(page.hide_selectors.contains(".scoped"));
            assert_eq!(page.procedural_actions.len(), 1);
            let excepted = engine.url_cosmetic_resources_without_scriptlets("https://allowed.example/");
            assert!(!excepted.hide_selectors.contains(".always"));
            assert!(excepted.procedural_actions.is_empty());
            let private = engine.url_cosmetic_resources_without_scriptlets("https://private.example/");
            assert!(private.hide_selectors.contains(".always"));
            assert!(!private.hide_selectors.contains(".scoped"));
            let specific_off = engine.url_cosmetic_resources_with_policy("https://news.example/", 0, true, false);
            assert!(specific_off.hide_selectors.contains(".always"));
            assert_eq!(specific_off.procedural_actions.len(), 1);
        }
        let generic_off = Engine::new_with_list_text("*##.always\n*##.card:has-text(Sponsored)\n@@||news.example^$generichide");
        let result = generic_off.url_cosmetic_resources_without_scriptlets("https://news.example/");
        assert!(result.hide_selectors.is_empty());
        assert!(result.procedural_actions.is_empty());
    }
}
