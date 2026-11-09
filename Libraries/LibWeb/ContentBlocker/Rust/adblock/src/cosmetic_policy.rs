//! Network exceptions controlling cosmetic filtering without disabling scriptlets.
use crate::{Engine, FilterSet, lists::ParseOptions, request::Request};
pub struct CosmeticPolicy { all: Engine, specific: Engine }
fn select(rules: &str, names: &[&str]) -> Engine {
    let mut selected = String::new();
    for line in rules.lines().map(str::trim) {
        if !line.starts_with("@@") { continue; }
        let Some(dollar) = crate::filters::network_filter_option_start(line) else { continue; };
        let pattern = &line[..dollar];
        let Ok(options) = crate::filters::split_filter_options(&line[dollar + 1..]) else { continue; };
        if !options.iter().any(|option| names.contains(option)) { continue; }
        let mut remaining: Vec<_> = options.into_iter().filter(|option| !names.contains(option)).collect();
        if !remaining.iter().any(|option| matches!(*option, "document" | "doc")) { remaining.push("document"); }
        selected.push_str(pattern); selected.push('$'); selected.push_str(&remaining.join(",")); selected.push('\n');
    }
    let mut filters = FilterSet::new(false);
    filters.add_filter_list(selected, ParseOptions::default());
    Engine::new_with_filter_set(filters)
}
impl CosmeticPolicy {
    pub fn new(rules: &str) -> Self { Self { all: select(rules, &["elemhide", "ehide"]), specific: select(rules, &["specifichide", "shide"]) } }
    pub fn flags(&self, url: &str) -> (bool, bool) {
        let Ok(request) = Request::new(url, url, "document", "GET") else { return (false, false); };
        let all = self.all.check_network_request_subset(&request, false, true).exception.is_some();
        (all || self.specific.check_network_request_subset(&request, false, true).exception.is_some(), all)
    }
}

#[cfg(test)]
mod tests {
    use super::CosmeticPolicy;
    #[test]
    fn cosmetic_switches_retain_regex_domain_anchors_and_commas() {
        let policy = CosmeticPolicy::new(r#"
@@||news.example^$elemhide,domain=/^news\.example$/
@@/^https:\/\/news[0-9]+\.example\/$/$specifichide,domain=/^news[0-9]{1,3}\.example$/
"#);
        assert_eq!(policy.flags("https://news.example/"), (true, true));
        assert_eq!(policy.flags("https://news12.example/"), (true, false));
        assert_eq!(policy.flags("https://news1234.example/"), (false, false));
        assert_eq!(policy.flags("https://other.example/"), (false, false));
    }
}
