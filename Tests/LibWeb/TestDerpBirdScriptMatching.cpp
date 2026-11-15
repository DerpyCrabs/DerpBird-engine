#include <LibTest/TestCase.h>
#include <LibWeb/Page/DerpBirdWebRequestRules.h>
#include <LibWeb/Page/DerpBirdScriptMatchPattern.h>
#include <LibWeb/Page/DerpBirdScriptURLPolicy.h>
#include <LibWeb/Page/DerpBirdScriptRegistry.h>

using Web::DerpBirdScriptMatch;
using Web::DerpBirdScriptMatchBudget;
using Web::DerpBirdScriptMatchPattern;

static bool matches(StringView pattern, StringView address)
{
    auto parsed = MUST(DerpBirdScriptMatchPattern::create(pattern));
    auto url = URL::Parser::basic_parse(address);
    VERIFY(url.has_value());
    DerpBirdScriptMatchBudget budget;
    return parsed.matches(*url, budget) == DerpBirdScriptMatch::Match;
}

TEST_CASE(script_match_patterns_keep_scheme_host_and_path_boundaries)
{
    EXPECT(matches("*://*.example.com/foo*bar"sv, "https://example.com/foobar"sv));
    EXPECT(matches("*://*.example.com/foo*bar"sv, "http://a.b.example.com/foo/x/bar#ignored"sv));
    EXPECT(!matches("*://*.example.com/*"sv, "https://badexample.com/"sv));
    EXPECT(!matches("*://*.example.com/*"sv, "https://example.com.evil.test/"sv));
    EXPECT(!matches("*://*.example.com/*"sv, "https://example.com@evil.test/"sv));
    EXPECT(!matches("*://*.example.com/*"sv, "ftp://example.com/"sv));
    EXPECT(matches("https://EXAMPLE.com/*"sv, "https://example.com:8443/"sv));
    EXPECT(matches("https://example.com:443/*"sv, "https://example.com/"sv));
    EXPECT(!matches("https://example.com:443/*"sv, "https://example.com:8443/"sv));
    EXPECT(matches("http://[::1]:8080/*"sv, "http://[::1]:8080/path"sv));
    EXPECT(matches("http://localhost:*/*"sv, "http://localhost:1234/"sv));
    EXPECT(matches("https://bücher.example/*"sv, "https://xn--bcher-kva.example/"sv));
    EXPECT(matches("https://example.com/p?q=*"sv, "https://example.com/p?q=x#f"sv));
    EXPECT(!matches("https://example.com/p"sv, "https://example.com/p?q=x"sv));
    EXPECT(!matches("https://example.com/P*"sv, "https://example.com/path"sv));
    EXPECT(matches("file:///tmp/*"sv, "file:///tmp/a.txt"sv));
    EXPECT(!matches("file:///tmp/*"sv, "file:///other/a.txt"sv));
    EXPECT(matches("<all_urls>"sv, "https://example.com/"sv));
    EXPECT(!matches("<all_urls>"sv, "about:derpbird"sv));
    EXPECT(!matches("<all_urls>"sv, "data:text/html,test"sv));
}

TEST_CASE(script_match_patterns_reject_invalid_restrictions)
{
    for (auto pattern : { ""sv, "https://example.com"sv, "https:///*"sv, "https://foo.*/*"sv,
             "https://*example.com/*"sv, "https://a@b/*"sv, "https://a b/*"sv,
             "https://example.com:65536/*"sv, "https://example.com:/*"sv,
             "https://example.com:1:2/*"sv, "https://[::1]bad/*"sv,
             "file://example.com/*"sv, "about://*/*"sv, "https://example.com/*#fragment"sv })
        EXPECT(DerpBirdScriptMatchPattern::create(pattern).is_error());
}

TEST_CASE(script_match_patterns_report_exhaustion_separately)
{
    auto pattern = MUST(DerpBirdScriptMatchPattern::create("https://example.com/*aaaaaaaaab"sv));
    auto url = URL::Parser::basic_parse("https://example.com/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"sv);
    VERIFY(url.has_value());
    DerpBirdScriptMatchBudget budget { 20 };
    EXPECT(pattern.matches(*url, budget) == DerpBirdScriptMatch::LimitExceeded);
    EXPECT_EQ(budget.remaining, 0u);
    DerpBirdScriptMatchBudget enough;
    EXPECT(pattern.matches(*url, enough) == DerpBirdScriptMatch::NoMatch);
}

TEST_CASE(context_menu_policy_ignores_legacy_selectors_but_preserves_native_boundaries)
{
    using Eligibility = Web::DerpBirdScriptEligibility;
    auto policy = MUST(Web::DerpBirdScriptURLPolicy::create({}, {}, true,
        { "https://different.example/*"_string }, { "*"_string }, true));
    auto evaluate = [&](StringView address, bool top = true, bool file_access = false) {
        auto url = URL::Parser::basic_parse(address);
        VERIFY(url.has_value());
        DerpBirdScriptMatchBudget budget;
        return policy.evaluate(*url, top, file_access, budget);
    };
    EXPECT(evaluate("https://page.example/"sv) == Eligibility::Allowed);
    EXPECT(evaluate("https://page.example/"sv, false) == Eligibility::FrameDenied);
    EXPECT(evaluate("file:///tmp/page.html"sv) == Eligibility::FileAccessDenied);
    EXPECT(evaluate("file:///tmp/page.html"sv, true, true) == Eligibility::Allowed);
    EXPECT(evaluate("about:derpbird"sv) == Eligibility::NoMatch);
    EXPECT(evaluate("resource://ladybird/derpbird/index.html"sv) == Eligibility::NoMatch);
    EXPECT(evaluate("data:text/html,test"sv) == Eligibility::NoMatch);
    policy = MUST(Web::DerpBirdScriptURLPolicy::create(
        { "https://page.example/*"_string }, { "https://page.example/private*"_string }, false,
        { "*"_string }, {}, true));
    EXPECT(evaluate("https://other.example/"sv) == Eligibility::NoMatch);
    EXPECT(evaluate("https://page.example/private"sv) == Eligibility::Excluded);
    EXPECT(evaluate("https://page.example/public"sv) == Eligibility::Allowed);
}

TEST_CASE(script_url_policy_applies_exclusions_and_document_permissions)
{
    using Eligibility = Web::DerpBirdScriptEligibility;
    auto policy = MUST(Web::DerpBirdScriptURLPolicy::create(
        { "https://one.example/*"_string, "https://*.two.example/*"_string, "file:///*"_string },
        { "https://*.two.example/private*"_string }, true));
    auto evaluate = [&](StringView address, bool top = true, bool file_access = false) {
        auto url = URL::Parser::basic_parse(address);
        VERIFY(url.has_value());
        DerpBirdScriptMatchBudget budget;
        return policy.evaluate(*url, top, file_access, budget);
    };
    EXPECT(evaluate("https://one.example/"sv) == Eligibility::Allowed);
    EXPECT(evaluate("https://a.two.example/public"sv) == Eligibility::Allowed);
    EXPECT(evaluate("https://a.two.example/private?a=b"sv) == Eligibility::Excluded);
    EXPECT(evaluate("https://three.example/"sv) == Eligibility::NoMatch);
    EXPECT(evaluate("https://one.example/"sv, false) == Eligibility::FrameDenied);
    EXPECT(evaluate("file:///tmp/a"sv) == Eligibility::FileAccessDenied);
    EXPECT(evaluate("file:///tmp/a"sv, true, true) == Eligibility::Allowed);
    EXPECT(evaluate("about:derpbird"sv) == Eligibility::NoMatch);
    auto empty = MUST(Web::DerpBirdScriptURLPolicy::create({}, {}, false));
    auto url = URL::Parser::basic_parse("https://one.example/"sv);
    DerpBirdScriptMatchBudget budget;
    EXPECT(empty.evaluate(*url, true, true, budget) == Eligibility::NoMatch);
    EXPECT(Web::DerpBirdScriptURLPolicy::create({ "<all_urls>"_string }, { "broken exclusion"_string }, false).is_error());
}

TEST_CASE(script_url_policy_never_skips_an_expensive_exclusion)
{
    auto policy = MUST(Web::DerpBirdScriptURLPolicy::create(
        { "<all_urls>"_string }, { "https://example.com/*aaaaaaaaab"_string }, false));
    auto url = URL::Parser::basic_parse("https://example.com/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"sv);
    DerpBirdScriptMatchBudget short_budget { 20 };
    EXPECT(policy.evaluate(*url, true, false, short_budget) == Web::DerpBirdScriptEligibility::LimitExceeded);
    DerpBirdScriptMatchBudget sufficient;
    EXPECT(policy.evaluate(*url, true, false, sufficient) == Web::DerpBirdScriptEligibility::Allowed);
}

TEST_CASE(script_legacy_includes_preserve_url_component_boundaries)
{
    auto test = [](StringView pattern, StringView address) {
        auto selector = MUST(Web::DerpBirdScriptIncludePattern::create(pattern));
        auto url = URL::Parser::basic_parse(address);
        VERIFY(url.has_value());
        DerpBirdScriptMatchBudget budget;
        return selector.matches(*url, budget) == DerpBirdScriptMatch::Match;
    };
    EXPECT(test("*://example.com/*"sv, "https://example.com/a#fragment"sv));
    EXPECT(!test("*://example.com/*"sv, "https://evil.test/?http://example.com/a"sv));
    EXPECT(!test("https://*example.com/*"sv, "https://evil.test/path/example.com/a"sv));
    EXPECT(test("https://*"sv, "https://example.com/"sv));
    EXPECT(test("*"sv, "https://example.com/a"sv));
    EXPECT(!test("*"sv, "about:derpbird"sv));
    EXPECT(test("https://example.com/?q=a+b"sv, "https://example.com/?q=a+b#fragment"sv));
    EXPECT(!test("https://example.com/?q=a+b"sv, "https://example.com/?q=aaab"sv));
    EXPECT(test(R"(/example\.com\/(a)\1/)"sv, "https://example.com/aa"sv));
    EXPECT(!test(R"(/^example\.com/)"sv, "https://example.com/"sv));
    EXPECT(test(R"(/^https:\/\/example\.com\/a$/)"sv, "https://example.com/a#fragment"sv));
    EXPECT(Web::DerpBirdScriptIncludePattern::create("/[broken/"sv).is_error());
    EXPECT(Web::DerpBirdScriptIncludePattern::create("/unterminated"sv).is_error());
    EXPECT(matches("http*://example.com/*"sv, "http://example.com/a"sv));
    EXPECT(matches("http*://example.com/*"sv, "https://example.com/a"sv));
    EXPECT(!matches("http*://example.com/*"sv, "ftp://example.com/a"sv));
}

TEST_CASE(script_legacy_exclusions_veto_both_positive_selector_families)
{
    auto policy = MUST(Web::DerpBirdScriptURLPolicy::create(
        { "https://one.example/*"_string }, {}, false,
        { "https://two.example/*"_string }, { "/private/"_string }));
    for (auto address : { "https://one.example/private"sv, "https://two.example/private"sv }) {
        auto url = URL::Parser::basic_parse(address);
        DerpBirdScriptMatchBudget budget;
        EXPECT(policy.evaluate(*url, true, false, budget) == Web::DerpBirdScriptEligibility::Excluded);
    }
    auto public_url = URL::Parser::basic_parse("https://two.example/public"sv);
    DerpBirdScriptMatchBudget budget;
    EXPECT(policy.evaluate(*public_url, true, false, budget) == Web::DerpBirdScriptEligibility::Allowed);
    auto expensive = MUST(Web::DerpBirdScriptURLPolicy::create(
        { "<all_urls>"_string }, {}, false, {}, { "/(a+)+b$/"_string }));
    auto long_url = URL::Parser::basic_parse("https://example.com/aaaaaaaaaaaaaaaaaaaaaaaaaaaa"sv);
    DerpBirdScriptMatchBudget short_budget { 1000 };
    EXPECT(expensive.evaluate(*long_url, true, false, short_budget) == Web::DerpBirdScriptEligibility::LimitExceeded);
}

static JsonObject script_record()
{
    return MUST(JsonValue::from_string(R"JSON({
        "id":"script-1", "name":"Fixture", "source":"globalThis.ran = true;",
        "enabled":true, "fileAccessGranted":false,
        "metadata": {
            "runAt":"document-start", "grantNone":false, "noFrames":true,
            "matches":["https://example.com/*"], "includes":[], "excludeMatches":[], "excludes":[],
            "grants":["GM_getValue"], "requires":["https://example.com/library.js"],
            "connects":["api.example.com"], "resources":[{"name":"theme","url":"https://example.com/theme.css"}]
        }
    })JSON"sv)).as_object();
}

TEST_CASE(script_registry_decodes_execution_requirements_without_granting_them)
{
    JsonArray snapshot;
    MUST(snapshot.append(script_record()));
    Web::DerpBirdScriptRegistry registry;
    EXPECT(!registry.replace(snapshot).is_error());
    VERIFY(registry.definitions().size() == 1);
    auto const& definition = registry.definitions()[0];
    EXPECT_EQ(definition.id, "script-1"sv);
    EXPECT(definition.enabled);
    EXPECT(!definition.file_access_granted);
    EXPECT(!definition.grant_none);
    EXPECT_EQ(definition.info.get_string("scriptHandler"sv).value(), "DerpBird"sv);
    auto script_info = definition.info.get_object("script"sv).value();
    EXPECT_EQ(script_info.get_string("name"sv).value(), "Fixture"sv);
    EXPECT_EQ(script_info.get_string("run-at"sv).value(), "document-start"sv);
    EXPECT_EQ(script_info.get_array("grant"sv)->at(0).as_string(), "GM_getValue"sv);
    EXPECT(definition.run_at == Web::DerpBirdScriptRunAt::DocumentStart);
    EXPECT_EQ(definition.grants[0], "GM_getValue"sv);
    EXPECT_EQ(definition.required_scripts[0], "https://example.com/library.js"sv);
    EXPECT_EQ(definition.resources[0].name, "theme"sv);
    auto url = URL::Parser::basic_parse("https://example.com/page"sv);
    DerpBirdScriptMatchBudget budget;
    EXPECT(definition.url_policy.evaluate(*url, false, false, budget) == Web::DerpBirdScriptEligibility::FrameDenied);
}

TEST_CASE(script_registry_rejects_partial_or_ambiguous_replacements)
{
    Web::DerpBirdScriptRegistry registry;
    JsonArray initial;
    MUST(initial.append(script_record()));
    MUST(registry.replace(initial));
    auto invalid = script_record();
    invalid.set("id"sv, "script-2"sv);
    invalid.set("enabled"sv, "true"sv);
    JsonArray broken;
    MUST(broken.append(script_record()));
    MUST(broken.append(invalid));
    EXPECT(registry.replace(broken).is_error());
    VERIFY(registry.definitions().size() == 1);
    EXPECT_EQ(registry.definitions()[0].id, "script-1"sv);
    JsonArray duplicates;
    MUST(duplicates.append(script_record()));
    MUST(duplicates.append(script_record()));
    EXPECT(registry.replace(duplicates).is_error());
    auto conflict = script_record();
    auto metadata = *conflict.get_object("metadata"sv);
    metadata.set("grantNone"sv, true);
    conflict.set("metadata"sv, move(metadata));
    EXPECT(Web::DerpBirdScriptDefinition::decode(conflict).is_error());
    auto missing = script_record();
    auto missing_metadata = *missing.get_object("metadata"sv);
    missing_metadata.remove("excludes"sv);
    missing.set("metadata"sv, move(missing_metadata));
    EXPECT(Web::DerpBirdScriptDefinition::decode(missing).is_error());
    JsonArray empty;
    MUST(registry.replace(empty));
    EXPECT(registry.definitions().is_empty());
}

TEST_CASE(script_registry_preserves_descriptive_info_as_data)
{
    auto record = script_record();
    auto metadata = record.get_object("metadata"sv).value();
    metadata.set("name"sv, "Metadata name"sv);
    metadata.set("namespace"sv, "https://example.com/userscripts"sv);
    metadata.set("version"sv, "1.2.3"sv);
    metadata.set("description"sv, "Quotes \" and unicode: Привет"sv);
    metadata.set("header"sv, "// ==UserScript==\n// @name Metadata name\n// ==/UserScript=="sv);
    record.set("metadata"sv, metadata);
    auto decoded = MUST(Web::DerpBirdScriptDefinition::decode(record));
    auto script = decoded.info.get_object("script"sv).value();
    EXPECT_EQ(script.get_string("name"sv).value(), "Metadata name"sv);
    EXPECT_EQ(script.get_string("version"sv).value(), "1.2.3"sv);
    EXPECT_EQ(decoded.info.get_string("scriptMetaStr"sv).value(), metadata.get_string("header"sv).value());
    metadata.set("description"sv, JsonArray {});
    record.set("metadata"sv, metadata);
    EXPECT(Web::DerpBirdScriptDefinition::decode(record).is_error());
}

TEST_CASE(script_registry_validates_native_dependency_cache)
{
    auto record = script_record();
    auto decoded = MUST(Web::DerpBirdScriptDefinition::decode(record));
    EXPECT(decoded.required_sources.is_empty()); // Legacy unresolved scripts stay disabled.
    record.set("requiredSources"sv, JsonArray {});
    EXPECT(Web::DerpBirdScriptDefinition::decode(record).is_error());
    auto sources = MUST(JsonValue::from_string(R"JSON([{"url":"https://example.com/library.js","source":"const dependency = 42;"}])JSON"sv));
    record.set("requiredSources"sv, sources);
    decoded = MUST(Web::DerpBirdScriptDefinition::decode(record));
    EXPECT_EQ(decoded.required_sources.size(), 1u);
    sources.as_array().values()[0].as_object().set("url"sv, "https://other.example/code.js"sv);
    record.set("requiredSources"sv, sources);
    EXPECT(Web::DerpBirdScriptDefinition::decode(record).is_error());
}

TEST_CASE(script_contexts_filter_array_snapshots_and_preserve_default_raw)
{
    JsonArray records;
    for (auto context : { "normal-tabs"sv, "incognito-tabs"sv, "container-missing"sv }) {
        auto record = script_record();
        record.set("id"sv, context);
        auto metadata = record.get_object("metadata"sv).value();
        JsonObject directive;
        directive.set("key"sv, "run-in"sv);
        directive.set("value"sv, context);
        JsonArray entries;
        entries.must_append(move(directive));
        metadata.set("entries"sv, move(entries));
        record.set("metadata"sv, move(metadata));
        records.must_append(move(record));
    }
    auto normal = Web::derpbird_scripts_for_context(JsonValue { records }, false);
    auto private_scripts = Web::derpbird_scripts_for_context(JsonValue { records }, true);
    EXPECT_EQ(normal.as_array().size(), 1u);
    EXPECT_EQ(private_scripts.as_array().size(), 1u);
    EXPECT_EQ(normal.as_array().values()[0].as_object().get_string("id"sv).value(), "normal-tabs"sv);
    EXPECT_EQ(private_scripts.as_array().values()[0].as_object().get_string("id"sv).value(), "incognito-tabs"sv);
    auto decoded = MUST(Web::DerpBirdScriptDefinition::decode(script_record()));
    EXPECT_EQ(decoded.sandbox, "raw"sv);
    EXPECT_EQ(decoded.info.get_string("sandboxMode"sv).value(), "raw"sv);
}

TEST_CASE(web_request_rules_enforce_redirect_scope_exclusions_and_websocket_types)
{
    using Rules = Web::DerpBirdWebRequestRules;
    auto rules = MUST(Rules::compile(MUST(JsonValue::from_string(R"JSON([
        {"selector":{"include":"https://example.com/*","exclude":"https://example.com/allowed*"},"action":"cancel"},
        {"selector":"wss://example.com/*","action":{"redirect":"wss://example.com/allowed"}},
        {"selector":"https://other.test/*","action":{"redirect":"https://outside.test/leak"}}
    ])JSON"sv))));
    auto scope = MUST(Web::DerpBirdScriptURLPolicy::create({ "https://example.com/*"_string, "wss://example.com/*"_string }, {}, false));
    auto evaluate = [&](StringView url, Rules::Type type) {
        Web::DerpBirdScriptMatchBudget budget;
        u64 replacements = 4'000'000;
        return MUST(rules.evaluate(*URL::Parser::basic_parse(url), type, scope, budget, replacements));
    };
    EXPECT(evaluate("https://example.com/blocked.js"sv, Rules::Type::Script)->cancel);
    EXPECT(!evaluate("https://example.com/allowed.js"sv, Rules::Type::Script).has_value());
    EXPECT(!evaluate("https://example.com/blocked.js"sv, Rules::Type::Other).has_value());
    auto socket = evaluate("wss://example.com/socket"sv, Rules::Type::WebSocket);
    VERIFY(socket.has_value() && socket->redirect.has_value());
    EXPECT_EQ(socket->redirect->serialize(), "wss://example.com/allowed"sv);
    auto escaped = evaluate("https://other.test/source"sv, Rules::Type::XHR);
    VERIFY(escaped.has_value());
    EXPECT(!escaped->redirect.has_value());
    EXPECT(!escaped->cancel);
    EXPECT_EQ(escaped->event.get_string("message"sv).value(), "error"sv);
    auto invalid = MUST(JsonValue::from_string(R"JSON([{"selector":"*","action":{"redirect":"javascript:alert(1)"}}])JSON"sv));
    EXPECT(Rules::compile(invalid).is_error());
}

TEST_CASE(script_info_reflects_installed_updates_and_context)
{
    auto record = script_record();
    record.set("sourceURL"sv, "https://example.com/scripts/main.user.js"sv);
    record.set("checkUpdates"sv, true);
    auto metadata = record.get_object("metadata"sv).value();
    metadata.set("entries"sv, MUST(JsonValue::from_string(R"([{"key":"updateURL","value":"../meta.js"},{"key":"downloadURL","value":"./next.js"},{"key":"sandbox","value":"JavaScript"}])"sv)));
    record.set("metadata"sv, metadata);
    auto info = MUST(Web::DerpBirdScriptDefinition::decode(record)).info;
    EXPECT(info.get_bool("scriptWillUpdate"sv).value());
    EXPECT(info.get_object("script"sv)->get_object("options"sv)->get_bool("check_for_updates"sv).value());
    EXPECT_EQ(info.get_string("scriptUpdateURL"sv).value(), "https://example.com/meta.js"sv);
    EXPECT_EQ(info.get_string("sandboxMode"sv).value(), "js"sv);
    JsonArray records;
    records.must_append(record);
    auto scoped = Web::derpbird_scripts_for_context(JsonValue { records }, true);
    auto installed_configuration = MUST(Web::DerpBirdScriptDefinition::decode(record)).configuration;
    EXPECT_EQ(MUST(Web::DerpBirdScriptDefinition::decode(scoped.as_array().at(0).as_object())).configuration, installed_configuration);
    EXPECT(MUST(Web::DerpBirdScriptDefinition::decode(scoped.as_array().at(0).as_object())).info.get_bool("isIncognito"sv).value());
    scoped = Web::derpbird_scripts_for_context(scoped, false);
    EXPECT_EQ(MUST(Web::DerpBirdScriptDefinition::decode(scoped.as_array().at(0).as_object())).configuration, installed_configuration);
    EXPECT(!MUST(Web::DerpBirdScriptDefinition::decode(scoped.as_array().at(0).as_object())).info.get_bool("isIncognito"sv).value());
    auto changed = scoped.as_array().at(0).as_object();
    changed.set("source"sv, "console.log('changed definition');"sv);
    EXPECT_NE(MUST(Web::DerpBirdScriptDefinition::decode(changed)).configuration, installed_configuration);
    record.set("checkUpdates"sv, false);
    EXPECT(!MUST(Web::DerpBirdScriptDefinition::decode(record)).info.get_bool("scriptWillUpdate"sv).value());
    record.set("checkUpdates"sv, true);
    metadata.set("entries"sv, MUST(JsonValue::from_string(R"([{"key":"downloadURL","value":"none"}])"sv)));
    record.set("metadata"sv, metadata);
    EXPECT(!MUST(Web::DerpBirdScriptDefinition::decode(record)).info.get_bool("scriptWillUpdate"sv).value());
    record.remove("sourceURL"sv);
    EXPECT(!MUST(Web::DerpBirdScriptDefinition::decode(record)).info.get_bool("scriptWillUpdate"sv).value());
}
