// Integration target: upstream .crate omits its unit-test sources.

    use adblock::{Engine, FilterSet, lists::ParseOptions, request::Request, ip_address::IpAddressEngine};
    #[test]
    fn shared_predicate_survives_header_modifier_and_badfilter_paths() {
        let mut filters = FilterSet::new(true);
        filters.add_filter_list("*$script,ipaddress=10.*\n*$script,ipaddress=192.168.*\n*$script,ipaddress=10.*,badfilter".to_owned(), ParseOptions::default());
        let engine = Engine::new_with_filter_set_no_optimize(filters);
        let request = Request::new("https://ads.example/a", "https://page.example/", "script", "GET").unwrap();
        assert!(!engine.check_network_request(&request).should_block());
        assert!(!engine.check_network_request(&request.clone().with_ip_addresses("10.1.2.3")).should_block());
        assert!(engine.check_network_request(&request.clone().with_ip_addresses("192.168.1.2")).should_block());
        let headers = adblock::response_headers::ResponseHeaderEngine::new(
            r"*$script,ipaddress=/^192\.168\.[0-9]{1,3}\./,header=x-ad:yes");
        let response = vec![("x-ad".to_owned(), "yes".to_owned())];
        assert!(!headers.should_block(&request, &response, false));
        assert!(headers.should_block(&request.clone().with_ip_addresses("192.168.1.2"), &response, false));
        assert!(!headers.should_block(&request.with_ip_addresses("203.0.113.1"), &response, false));
    }

    #[test]
    fn address_rules_keep_url_scope_exceptions_priority_and_original_attribution() {
        let rules = "*$script,ipaddress=192.168.*\n@@||allowed.example^$script,ipaddress=192.168.*\n||important.example^$script,important,ipaddress=lan\n@@||important.example^$script,ipaddress=lan\n*$image,ipaddress=/^203\\.0\\.113\\.[0-9]{1,3}$/";
        let engine = IpAddressEngine::new_for_sources(std::iter::once((rules, "Address fixture")));
        let request = |url, kind| Request::new(url, "https://page.example/", kind, "GET").unwrap();
        let blocked = engine.check(&request("https://ads.example/a", "script"), "192.168.1.2", false);
        assert!(blocked.0);
        assert!(blocked.1.contains("ipaddress=192.168.*"));
        assert!(blocked.1.contains("Address fixture · line 1"));
        assert!(!engine.check(&request("https://allowed.example/a", "script"), "192.168.1.2", false).0);
        assert!(engine.check(&request("https://important.example/a", "script"), "192.168.1.2", true).0);
        assert!(!engine.check(&request("https://ads.example/a", "script"), "192.168.1.2", true).0);
        assert!(!engine.check(&request("https://ads.example/a", "script"), "", false).0);
        assert!(!engine.check(&request("https://ads.example/a", "script"), "not-an-address", false).0);
        assert!(engine.check(&request("https://ads.example/a", "image"), "203.0.113.12", false).0);
        assert!(!engine.check(&request("https://ads.example/a", "image"), "203.0.112.12", false).0);
    }
    #[test]
    fn address_classes_and_badfilter_preserve_native_boundaries() {
        let rules = "*$script,ipaddress=lan\n*$image,ipaddress=loopback\n*$xhr,ipaddress=203.0.113.1\n*$xhr,ipaddress=203.0.113.1,badfilter";
        let engine = IpAddressEngine::new_for_sources(std::iter::once((rules, "")));
        let request = |kind| Request::new("https://ads.example/", "https://page.example/", kind, "GET").unwrap();
        for address in ["10.0.0.1", "172.16.0.1", "192.168.0.1", "169.254.0.1", "::1", "::", "fc00::1", "fd00::2", "::ffff:192.168.1.1"] {
            assert!(engine.check(&request("script"), address, false).0, "{address}");
        }
        for address in ["8.8.8.8", "172.32.0.1", "2001:4860:4860::8888"] {
            assert!(!engine.check(&request("script"), address, false).0, "{address}");
        }
        assert!(engine.check(&request("image"), "127.0.0.1", false).0);
        assert!(!engine.check(&request("image"), "127.0.0.2", false).0);
        assert!(!engine.check(&request("xmlhttprequest"), "203.0.113.1", false).0);
    }

#[test]
fn slash_paths_keep_modifier_regex_and_cosmetic_suffix_scopes() {
    let request = Request::new("http://127.0.0.1/body", "http://127.0.0.1/", "xmlhttprequest", "GET").unwrap().with_ip_addresses("127.0.0.1");
    let modifiers = adblock::response_modifiers::ResponseModifiers::new(std::iter::once(("/body$xhr,ipaddress=loopback,replace=/advert/clean/g", true, "fixture")));
    assert_eq!(modifiers.replacements(&request).len(), 1);
    let mut filters = FilterSet::new(true);
    filters.add_filter_list("example,~/^private\\./##.mixed\nexample##.ordinary".to_owned(), ParseOptions::default());
    let engine = Engine::new_with_filter_set_no_optimize(filters);
    let public = engine.url_cosmetic_resources_without_scriptlets("https://news1.example/");
    assert!(public.hide_selectors.contains(".mixed"));
    assert!(public.hide_selectors.contains(".ordinary"));
    let private = engine.url_cosmetic_resources_without_scriptlets("https://private.example/");
    assert!(!private.hide_selectors.contains(".mixed"));
    assert!(private.hide_selectors.contains(".ordinary"));
}
