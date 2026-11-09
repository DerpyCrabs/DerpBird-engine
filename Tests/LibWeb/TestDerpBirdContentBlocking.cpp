#include <AK/JsonObject.h>
#include <AK/JsonArray.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <LibCore/ElapsedTimer.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Loader/ContentBlocker.h>

TEST_CASE(derpbird_content_blocking_resource_substitution)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    MUST(blocker.set_patterns(Vector<String> {
        "||tracker.example/direct$redirect=noop.json"_string,
        "@@||tracker.example/direct/allowed"_string,
        "||tracker.example/rule$redirect-rule=noop.js"_string,
        "||tracker.example/blocked-rule"_string,
        "||tracker.example/blocked-rule$redirect-rule=noop.json"_string,
        "||tracker.example/unknown$redirect=does-not-exist"_string,
        "||tracker.example/alias$redirect=noopjson"_string,
        "||tracker.example/empty$redirect=empty"_string,
        "||tracker.example/priority$redirect=noop.js:1"_string,
        "||tracker.example/priority$redirect=noop.json:2"_string,
    }));
    auto source = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    auto check = [&](StringView path) {
        auto url = URL::Parser::basic_parse(MUST(String::formatted("https://tracker.example/{}", path))).release_value();
        return blocker.check(url, source, Web::ContentBlocker::ResourceType::XMLHttpRequest, "GET"sv);
    };
    for (auto path : { "direct"sv, "blocked-rule"sv, "alias"sv, "priority"sv }) {
        auto result = check(path);
        EXPECT(result.blocked);
        VERIFY(result.substitute.has_value());
        EXPECT_EQ(result.substitute->mime_type, "application/json"sv);
        EXPECT_EQ(StringView { result.substitute->body.bytes() }, "{}"sv);
    }
    for (auto path : { "direct/allowed"sv, "rule"sv }) {
        auto result = check(path);
        EXPECT(!result.blocked);
        EXPECT(!result.substitute.has_value());
    }
    auto missing = check("unknown"sv);
    EXPECT(missing.blocked);
    EXPECT(!missing.substitute.has_value());
    auto empty = check("empty"sv);
    EXPECT(empty.blocked);
    VERIFY(empty.substitute.has_value());
    EXPECT(empty.substitute->body.is_empty());
}

TEST_CASE(derpbird_content_blocking_uses_actual_http_method)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    MUST(blocker.set_patterns(Vector<String> {
        "||tracker.example/collect$method=post"_string,
        "||tracker.example/get-only$method=get"_string,
        "||tracker.example/head-only$method=head"_string,
        "@@||tracker.example/collect/allowed$method=post"_string,
        "||tracker.example/always"_string,
    }));
    auto source = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    auto matches = [&](StringView path, StringView method) {
        auto address = URL::Parser::basic_parse(MUST(String::formatted("https://tracker.example/{}", path))).release_value();
        return blocker.is_filtered(address, source, Web::ContentBlocker::ResourceType::XMLHttpRequest, method);
    };
    EXPECT(matches("collect"sv, "POST"sv));
    EXPECT(!matches("collect"sv, "GET"sv));
    EXPECT(!matches("collect"sv, "HEAD"sv));
    EXPECT(!matches("collect"sv, ""sv)); // Speculation has no method; do not invent GET.
    EXPECT(!matches("collect/allowed"sv, "POST"sv));
    EXPECT(matches("get-only"sv, "GET"sv));
    EXPECT(!matches("get-only"sv, "POST"sv));
    EXPECT(matches("head-only"sv, "HEAD"sv));
    EXPECT(!matches("head-only"sv, "GET"sv));
    EXPECT(matches("always"sv, "POST"sv));
    EXPECT(matches("always"sv, "GET"sv));
    EXPECT(matches("always"sv, ""sv));
}

TEST_CASE(derpbird_content_blocking_returns_query_rewrite)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    MUST(blocker.set_patterns(Vector<String> {
        "||tracker.example^$removeparam=tracking"_string,
        "||tracker.example/blocked"_string,
    }));
    auto source = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    auto url = URL::Parser::basic_parse("https://tracker.example/path?keep=%26&tracking=one&tracking=two#anchor"sv).release_value();
    auto result = blocker.check(url, source, Web::ContentBlocker::ResourceType::XMLHttpRequest, "GET"sv);
    EXPECT(!result.blocked);
    VERIFY(result.rewritten_url.has_value());
    EXPECT_EQ(result.rewritten_url->serialize(), "https://tracker.example/path?keep=%26#anchor"sv);
    EXPECT(!blocker.check(*result.rewritten_url, source, Web::ContentBlocker::ResourceType::XMLHttpRequest, "GET"sv).rewritten_url.has_value());
    auto blocked = URL::Parser::basic_parse("https://tracker.example/blocked?tracking=one"sv).release_value();
    result = blocker.check(blocked, source, Web::ContentBlocker::ResourceType::XMLHttpRequest, "GET"sv);
    EXPECT(result.blocked);
    EXPECT(!result.rewritten_url.has_value());
}

TEST_CASE(derpbird_content_blocking_removeparam_exceptions)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto source = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    auto check = [&](StringView input, StringView expected) {
        auto url = URL::Parser::basic_parse(input).release_value();
        auto result = blocker.check(url, source, Web::ContentBlocker::ResourceType::XMLHttpRequest, "GET"sv);
        EXPECT(!result.blocked);
        EXPECT_EQ(result.rewritten_url.value_or(url).serialize(), expected);
    };
    MUST(blocker.set_patterns(Vector<String> {
        "||tracker.example^$removeparam=tracking"_string,
        "@@||tracker.example/allowed$removeparam=tracking"_string,
    }));
    check("https://tracker.example/path?tracking=&keep=yes&tracking#frag"sv, "https://tracker.example/path?keep=yes#frag"sv);
    check("https://tracker.example/allowed?tracking=one&tracking=&tracking"sv, "https://tracker.example/allowed?tracking=one&tracking=&tracking"sv);
    check("https://tracker.example/path?Tracking=yes&keep=no"sv, "https://tracker.example/path?Tracking=yes&keep=no"sv);
    MUST(blocker.set_patterns(Vector<String> {
        "@@||tracker.example^$removeparam=keep"_string,
        "||tracker.example^$removeparam"_string,
        "@@||tracker.example/allowed$removeparam"_string,
        "||tracker.example/blocked"_string,
        "@@||tracker.example/blocked$removeparam"_string,
    }));
    check("https://tracker.example/path?tracking=x&keep=&bare&keep=value#frag"sv, "https://tracker.example/path?keep=&keep=value#frag"sv);
    check("https://tracker.example/path?tracking=x&bare#frag"sv, "https://tracker.example/path#frag"sv);
    check("https://tracker.example/allowed?tracking=x&bare"sv, "https://tracker.example/allowed?tracking=x&bare"sv);
    auto blocked = URL::Parser::basic_parse("https://tracker.example/blocked?tracking=x"sv).release_value();
    EXPECT(blocker.check(blocked, source, Web::ContentBlocker::ResourceType::XMLHttpRequest, "GET"sv).blocked);
}

BENCHMARK_CASE(derpbird_removeparam_unaffected_requests)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto source = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    for (bool exempt : { false, true }) {
        MUST(blocker.set_patterns(exempt ? Vector<String> {
            "||page.example^$removeparam"_string,
            "@@||page.example^$removeparam"_string,
        } : Vector<String> {
            "||tracker.example^$removeparam=tracking"_string,
            "||ads.example^"_string,
        }));
        for (size_t count : { 16u, 256u }) {
            StringBuilder address;
            address.append("https://page.example/resource?"sv);
            for (size_t index = 0; index < count; ++index)
                address.appendff("{}p{}=value%26value", index ? "&"sv : ""sv, index);
            auto url = URL::Parser::basic_parse(address.string_view()).release_value();
            for (size_t sample = 0; sample < 7; ++sample) {
                auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
                bool changed = false;
                for (size_t iteration = 0; iteration < 10000; ++iteration) {
                    auto result = blocker.check(url, source, Web::ContentBlocker::ResourceType::XMLHttpRequest, "GET"sv);
                    changed |= result.blocked || result.rewritten_url.has_value();
                }
                auto microseconds = timer.elapsed_time().to_microseconds();
                EXPECT(!changed);
                outln("removeparam exempt={} params={} sample={} requests=10000 microseconds={}", exempt, count, sample, microseconds);
            }
        }
    }
}

TEST_CASE(derpbird_content_blocking_inverted_and_encoded_parameters)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto source = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    auto check = [&](StringView input, StringView expected) {
        auto url = URL::Parser::basic_parse(input).release_value();
        auto result = blocker.check(url, source, Web::ContentBlocker::ResourceType::XMLHttpRequest, "GET"sv);
        EXPECT(!result.blocked);
        EXPECT_EQ(result.rewritten_url.value_or(url).serialize(), expected);
    };
    MUST(blocker.set_patterns(Vector<String> {
        "||tracker.example^$removeparam=~keep"_string,
        "@@||tracker.example^$removeparam=protected"_string,
    }));
    check("https://tracker.example/path?%6Beep=one&tracking=two&protected=yes&bare&keep=#frag"sv,
        "https://tracker.example/path?%6Beep=one&protected=yes&keep=#frag"sv);
    MUST(blocker.set_patterns(Vector<String> {
        "||tracker.example^$removeparam"_string,
        "@@||tracker.example^$removeparam=~tracking"_string,
    }));
    check("https://tracker.example/path?keep=one&%74racking=two&tracking=&bare"sv,
        "https://tracker.example/path?keep=one&bare"sv);
    MUST(blocker.set_patterns(Vector<String> { "||tracker.example^$removeparam=tracking"_string }));
    check("https://tracker.example/path?%74racking=one&trac%6Bing=two&trac+king=three&%2574racking=four"sv,
        "https://tracker.example/path?trac+king=three&%2574racking=four"sv);
    MUST(blocker.set_patterns(Vector<String> { "||tracker.example^$removeparam=~"_string }));
    check("https://tracker.example/path?keep=one&tracking=two"sv,
        "https://tracker.example/path?keep=one&tracking=two"sv);
}

TEST_CASE(derpbird_content_blocking_regex_parameters)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto source = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    auto check = [&](StringView input, StringView expected) {
        auto url = URL::Parser::basic_parse(input).release_value();
        auto result = blocker.check(url, source, Web::ContentBlocker::ResourceType::XMLHttpRequest, "GET"sv);
        EXPECT(!result.blocked);
        EXPECT_EQ(result.rewritten_url.value_or(url).serialize(), expected);
    };
    MUST(blocker.set_patterns(Vector<String> {
        R"(||tracker.example^$removeparam=/^utm_[a-z]+=.*$/)"_string,
        R"(@@||tracker.example^$removeparam=/^utm_keep=/)"_string,
    }));
    check("https://tracker.example/path?utm_source=secret&%75tm_id=x&utm_keep=yes&keep=one"sv,
        "https://tracker.example/path?utm_keep=yes&keep=one"sv);
    MUST(blocker.set_patterns(Vector<String> {
        R"(||tracker.example^$xmlhttprequest,removeparam=/^event=one,two$/i,_,method=get,___)"_string,
    }));
    check("https://tracker.example/path?event=ONE%2Ctwo&event=other&keep=1"sv,
        "https://tracker.example/path?event=other&keep=1"sv);
    MUST(blocker.set_patterns(Vector<String> {
        R"(||tracker.example^$removeparam=~/^(keep|safe)=/)"_string,
        R"(@@||tracker.example^$removeparam=/^protected=/)"_string,
    }));
    check("https://tracker.example/path?keep=1&safe=2&bad=3&protected=4"sv,
        "https://tracker.example/path?keep=1&safe=2&protected=4"sv);
    MUST(blocker.set_patterns(Vector<String> {
        R"(||tracker.example^$removeparam=/^path=a\/b$/)"_string,
    }));
    check("https://tracker.example/path?path=a%2Fb&keep=yes"sv,
        "https://tracker.example/path?keep=yes"sv);
    MUST(blocker.set_patterns(Vector<String> {
        R"(*$removeparam=tracking)"_string,
    }));
    check("https://tracker.example/path?%74racking=yes&keep=1"sv,
        "https://tracker.example/path?keep=1"sv);
    MUST(blocker.set_patterns(Vector<String> {
        R"(/tracker\.example\/path/$removeparam=/^drop=/)"_string,
    }));
    check("https://tracker.example/path?drop=yes&keep=1"sv,
        "https://tracker.example/path?keep=1"sv);
    check("https://tracker.example/other?drop=yes&keep=1"sv,
        "https://tracker.example/other?drop=yes&keep=1"sv);
    for (auto invalid : { R"(||tracker.example^$removeparam=~/[/)"sv,
             R"(||tracker.example^$removeparam=~/x/z)"sv,
             R"(||tracker.example^$removeparam=~/(x/)"sv }) {
        MUST(blocker.set_patterns(Vector<String> { MUST(String::from_utf8(invalid)) }));
        check("https://tracker.example/path?keep=1&bad=2"sv,
            "https://tracker.example/path?keep=1&bad=2"sv);
    }
}

TEST_CASE(derpbird_removeparam_regex_cache_eviction)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    Vector<String> rules;
    for (size_t index = 0; index < 80; ++index)
        rules.append(MUST(String::formatted("||site{}.example^$removeparam=/^drop{}=/", index, index)));
    MUST(blocker.set_patterns(rules));
    auto source = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    // More expressions than cache slots, then revisit evicted expressions.
    for (size_t pass = 0; pass < 2; ++pass) {
        for (size_t index = 0; index < 80; ++index) {
            auto url = URL::Parser::basic_parse(MUST(String::formatted("https://site{}.example/?drop{}=yes&keep=1", index, index))).release_value();
            auto result = blocker.check(url, source, Web::ContentBlocker::ResourceType::XMLHttpRequest, "GET"sv);
            VERIFY(result.rewritten_url.has_value());
            EXPECT_EQ(result.rewritten_url->query().value(), "keep=1"sv);
        }
    }
}

BENCHMARK_CASE(derpbird_removeparam_cached_regex)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    MUST(blocker.set_patterns(Vector<String> {
        R"(||tracker.example^$removeparam=/^utm_[a-z]+=.*$/)"_string,
        R"(@@||tracker.example^$removeparam=/^utm_keep=/)"_string,
    }));
    auto source = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    auto url = URL::Parser::basic_parse("https://tracker.example/?utm_source=one&utm_id=two&utm_keep=yes&keep=ok"sv).release_value();
    VERIFY(blocker.check(url, source, Web::ContentBlocker::ResourceType::XMLHttpRequest, "GET"sv).rewritten_url.has_value());
    for (size_t sample = 0; sample < 7; ++sample) {
        auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
        size_t rewritten = 0;
        for (size_t index = 0; index < 10000; ++index)
            rewritten += blocker.check(url, source, Web::ContentBlocker::ResourceType::XMLHttpRequest, "GET"sv).rewritten_url.has_value();
        auto microseconds = timer.elapsed_time().to_microseconds();
        EXPECT_EQ(rewritten, 10000u);
        outln("removeparam cached-regex sample={} requests=10000 microseconds={}", sample, microseconds);
    }
}

TEST_CASE(derpbird_scriptlet_arguments_preserve_quotes_and_json_strings)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto url = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    MUST(blocker.set_patterns(Vector<String> {
        R"FILTER(page.example##+js(set, "quote\"Probe", true))FILTER"_string,
        R"FILTER(page.example##+js(set, 'single\'Probe', true))FILTER"_string,
        R"FILTER(page.example##+js(set, `backtick\`Probe`, true))FILTER"_string,
        R"FILTER(page.example##+js(set, trailingWhitespaceProbe, "true"   ))FILTER"_string,
        R"FILTER(page.example##+js(abort-on-property-read, '{"jsonProbe":1}'))FILTER"_string,
    }));
    auto output = blocker.scriptlets_for_url(url, false).to_utf8();
    EXPECT(output.contains(R"JS(setConstant("quote\"Probe", "true"))JS"sv));
    EXPECT(output.contains(R"JS(setConstant("single'Probe", "true"))JS"sv));
    EXPECT(output.contains(R"JS(setConstant("backtick`Probe", "true"))JS"sv));
    EXPECT(output.contains(R"JS(setConstant("trailingWhitespaceProbe", "true"))JS"sv));
    EXPECT(output.contains(R"JS(abortOnPropertyRead("{\"jsonProbe\":1}"))JS"sv));
}

TEST_CASE(derpbird_scriptlets_select_worlds_and_preserve_exceptions)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto url = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    MUST(blocker.set_patterns(Vector<String> {
        "page.example##+js(set, mainWorldProbe, true)"_string,
        "page.example##+js(set-attr, #isolatedWorldProbe, data-probe, 1)"_string,
        "page.example##+js(trusted-set-attr, #trustedWorldProbe, data-probe, secret)"_string,
    }));
    auto main = blocker.scriptlets_for_url(url, false);
    auto isolated = blocker.scriptlets_for_url(url, true);
    EXPECT(main.utf16_view().contains("mainWorldProbe"_utf16));
    EXPECT(!main.utf16_view().contains("isolatedWorldProbe"_utf16));
    EXPECT(isolated.utf16_view().contains("isolatedWorldProbe"_utf16));
    EXPECT(!isolated.utf16_view().contains("mainWorldProbe"_utf16));
    EXPECT(!isolated.utf16_view().contains("trustedWorldProbe"_utf16));
    MUST(blocker.set_patterns(Vector<String> {
        "page.example##+js(set, mainWorldProbe, true)"_string,
        "page.example##+js(set-attr, #isolatedWorldProbe, data-probe, 1)"_string,
        "page.example#@#+js(set-attr, #isolatedWorldProbe, data-probe, 1)"_string,
    }));
    EXPECT(!blocker.scriptlets_for_url(url, false).is_empty());
    EXPECT(blocker.scriptlets_for_url(url, true).is_empty());
    MUST(blocker.set_patterns(Vector<String> {
        "page.example##+js(set, mainWorldProbe, true)"_string,
        "page.example##+js(set-attr, #isolatedWorldProbe, data-probe, 1)"_string,
        "page.example#@#+js()"_string,
    }));
    EXPECT(blocker.scriptlets_for_url(url, false).is_empty());
    EXPECT(blocker.scriptlets_for_url(url, true).is_empty());
    auto local = URL::Parser::basic_parse("file:///page.example/index.html"sv).release_value();
    EXPECT(blocker.scriptlets_for_url(local, false).is_empty());
    EXPECT(blocker.scriptlets_for_url(local, true).is_empty());
}

TEST_CASE(derpbird_procedural_css_avoids_script_injection)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto url = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    MUST(blocker.set_patterns(Vector<String> { "page.example##.card:style(color: red !important)"_string }));
    EXPECT(blocker.scriptlets_for_url(url, true).is_empty());
    EXPECT(!blocker.cosmetic_style_sheet_for_url(url).is_empty());
    MUST(blocker.set_patterns(Vector<String> { "page.example##.card:has-text(Sponsored)"_string }));
    EXPECT(!blocker.scriptlets_for_url(url, true).is_empty());
    EXPECT(blocker.scriptlets_for_url(url, false).is_empty());
}

TEST_CASE(derpbird_procedural_removal_patterns_survive_compilation)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto url = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    auto elsewhere = URL::Parser::basic_parse("https://elsewhere.example/"sv).release_value();
    for (auto rule : {
             "page.example##.card:remove-attr(/^data-ad-/i)"sv,
             "page.example##.card:remove-class(/^ad-/)"sv,
             "page.example##.card:remove-attr(\"data-ad\")"sv,
             "page.example##.card:remove-class('advert')"sv }) {
        MUST(blocker.set_patterns(Vector<String> { MUST(String::from_utf8(rule)) }));
        EXPECT(!blocker.scriptlets_for_url(url, true).is_empty());
        EXPECT(blocker.scriptlets_for_url(elsewhere, true).is_empty());
        EXPECT(blocker.scriptlets_for_url(url, false).is_empty());
    }
    MUST(blocker.set_patterns(Vector<String> {
        "page.example##.card:remove-class(/^ad-/)"_string,
        "page.example#@#.card:remove-class(/^ad-/)"_string,
    }));
    EXPECT(blocker.scriptlets_for_url(url, true).is_empty());
}

TEST_CASE(derpbird_cosmetic_regex_scopes_keep_positive_negative_and_exception_rules)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    MUST(blocker.set_patterns(Vector<String> {
        R"RULE(/^news[0-9]{1,3}\.example$/##.regex-scope)RULE"_string,
        R"RULE(example,~/^private\./##.mixed-scope)RULE"_string,
        "/^news/##.excepted"_string,
        "news1.example#@#.excepted"_string,
        "news1.example##.ordinary"_string,
        "/^news/#@#.ordinary"_string,
        "/^news/##.card:remove-class(/^ad-/)"_string,
        "/^news2/#@#.card:remove-class(/^ad-/)"_string,
    }));
    auto first = URL::Parser::basic_parse("https://news1.example/"sv).release_value();
    auto sheet = blocker.cosmetic_style_sheet_for_url(first);
    EXPECT(sheet.contains(".regex-scope"sv));
    EXPECT(sheet.contains(".mixed-scope"sv));
    EXPECT(!sheet.contains(".excepted"sv));
    EXPECT(!sheet.contains(".ordinary"sv));
    EXPECT(!blocker.scriptlets_for_url(first, true).is_empty());
    auto second = URL::Parser::basic_parse("https://news2.example/"sv).release_value();
    EXPECT(blocker.scriptlets_for_url(second, true).is_empty());
    auto excluded = URL::Parser::basic_parse("https://private.example/"sv).release_value();
    EXPECT(!blocker.cosmetic_style_sheet_for_url(excluded).contains(".mixed-scope"sv));
    auto unrelated = URL::Parser::basic_parse("https://unrelated.test/"sv).release_value();
    EXPECT(blocker.cosmetic_style_sheet_for_url(unrelated).is_empty());
}

TEST_CASE(derpbird_cosmetic_actions_are_only_parsed_at_selector_top_level)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto url = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    for (auto rule : {
             R"RULE(page.example##.card:has-text("literal :style("))RULE"sv,
             R"RULE(page.example##.card:has-text("literal :remove-class("))RULE"sv,
             R"RULE(page.example##.card:has-text(/:remove-attr\(/):remove-class(/^ad-/))RULE"sv,
             R"RULE(page.example##input:matches-prop(checked=true))RULE"sv,
             R"RULE(page.example##input:matches-prop(classList.length=0))RULE"sv }) {
        MUST(blocker.set_patterns(Vector<String> { MUST(String::from_utf8(rule)) }));
        EXPECT(!blocker.scriptlets_for_url(url, true).is_empty());
    }
    MUST(blocker.set_patterns(Vector<String> {
        R"RULE(page.example##[data-label=":remove-class("]:style(color: red))RULE"_string,
    }));
    EXPECT(!blocker.cosmetic_style_sheet_for_url(url).is_empty());
}

TEST_CASE(derpbird_specific_generic_cosmetics_are_global_and_obey_generichide)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    MUST(blocker.set_patterns(Vector<String> {
        "*##.always"_string,
        "*,~private.example##.outside-private"_string,
        "*##.card:has-text(Sponsored)"_string,
        "allowed.example#@#.always"_string,
        "allowed.example#@#.card:has-text(Sponsored)"_string,
        "@@||disabled.example^$generichide"_string,
    }));
    auto address = [](StringView host) { return URL::Parser::basic_parse(MUST(String::formatted("https://{}/", host))).release_value(); };
    EXPECT(blocker.cosmetic_style_sheet_for_url(address("news.example"sv)).contains(".always"sv));
    EXPECT(!blocker.scriptlets_for_url(address("news.example"sv), true).is_empty());
    EXPECT(!blocker.cosmetic_style_sheet_for_url(address("private.example"sv)).contains(".outside-private"sv));
    EXPECT(!blocker.cosmetic_style_sheet_for_url(address("allowed.example"sv)).contains(".always"sv));
    EXPECT(blocker.scriptlets_for_url(address("allowed.example"sv), true).is_empty());
    EXPECT(blocker.cosmetic_style_sheet_for_url(address("disabled.example"sv)).is_empty());
    EXPECT(blocker.scriptlets_for_url(address("disabled.example"sv), true).is_empty());
}

TEST_CASE(derpbird_csp_selection_is_document_scoped_and_honors_exceptions)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto source = URL::Parser::basic_parse("https://parent.example/"sv).release_value();
    auto get = [&](StringView path, Web::ContentBlocker::ResourceType type = Web::ContentBlocker::ResourceType::Document) {
        auto url = URL::Parser::basic_parse(MUST(String::formatted("https://target.example/{}", path))).release_value();
        return blocker.csp_for_response(url, source, type, "GET"sv);
    };
    MUST(blocker.set_patterns(Vector<String> {
        "||target.example^$csp=script-src 'none'"_string,
        "||target.example^$csp=img-src 'none'"_string,
        "@@||target.example/exact$csp=script-src 'none'"_string,
        "@@||target.example/all$csp"_string,
    }));
    auto both = get("page"sv);
    EXPECT(both.contains("script-src 'none'"sv));
    EXPECT(both.contains("img-src 'none'"sv));
    EXPECT_EQ(get("exact"sv), "img-src 'none'"sv);
    EXPECT(get("all"sv).is_empty());
    EXPECT(!get("page"sv, Web::ContentBlocker::ResourceType::Subdocument).is_empty());
    EXPECT(get("page"sv, Web::ContentBlocker::ResourceType::Script).is_empty());
    EXPECT(get("page"sv, Web::ContentBlocker::ResourceType::XMLHttpRequest).is_empty());
    auto local = URL::Parser::basic_parse("file:///target.example/page"sv).release_value();
    EXPECT(blocker.csp_for_response(local, source, Web::ContentBlocker::ResourceType::Document).is_empty());
    MUST(blocker.set_patterns(Vector<String> { "||target.example^$csp=script-src 'none',domain=parent.example"_string }));
    EXPECT(!get("page"sv).is_empty());
    source = URL::Parser::basic_parse("https://unrelated.example/"sv).release_value();
    EXPECT(get("page"sv).is_empty());
}

TEST_CASE(derpbird_response_headers_scope_exceptions_and_security_allowlist)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    using Type = Web::ContentBlocker::ResourceType;
    auto mask = [&](StringView host, Type type = Type::Document) {
        auto url = URL::Parser::basic_parse(MUST(String::formatted("https://{}/", host))).release_value();
        return blocker.response_header_removals(url, type);
    };
    MUST(blocker.set_patterns(Vector<String> {
        "target.example##^responseheader(location)"_string,
        "target.example##^responseheader(refresh)"_string,
        "target.example##^responseheader(report-to)"_string,
        "target.example##^responseheader(set-cookie)"_string,
        "allowed.target.example#@#^responseheader(set-cookie)"_string,
        "target.example##^responseheader(content-security-policy)"_string,
        "target.example##^responseheader(Refresh)"_string,
    }));
    EXPECT_EQ(mask("target.example"sv), 15);
    EXPECT_EQ(mask("sub.target.example"sv, Type::Subdocument), 15);
    EXPECT_EQ(mask("allowed.target.example"sv), 7);
    EXPECT_EQ(mask("target.example"sv, Type::Script), 0);
    EXPECT_EQ(mask("other.example"sv), 0);
    auto url = URL::Parser::basic_parse("https://target.example/"sv).release_value();
    EXPECT(blocker.cosmetic_style_sheet_for_url(url).is_empty());
    EXPECT(blocker.scriptlets_for_url(url, true).is_empty());
    MUST(blocker.set_patterns(Vector<String> {
        "target.*,~excluded.target.example##^responseheader(refresh)"_string,
        "#@#^responseheader(location)"_string,
    }));
    EXPECT_EQ(mask("target.example"sv), 2);
    EXPECT_EQ(mask("excluded.target.example"sv), 0);
    MUST(blocker.set_patterns(Vector<String> {
        R"RULE(/^target[#]?\.example$/##^responseheader(refresh))RULE"_string,
        R"RULE(/^news[0-9]{1,3}\.example$/##^responseheader(set-cookie))RULE"_string,
        "/^news2/#@#^responseheader(set-cookie)"_string,
    }));
    EXPECT_EQ(mask("target.example"sv), 2);
    EXPECT_EQ(mask("news1.example"sv), 8);
    EXPECT_EQ(mask("news2.example"sv), 0);
    EXPECT_EQ(mask("other.example"sv), 0);
}

TEST_CASE(derpbird_response_header_conditions)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto url = URL::Parser::basic_parse("https://tracker.example/pixel"sv).release_value();
    auto source = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    auto headers = HTTP::HeaderList::create();
    headers->append({ "X-Marker", "one, two" });
    auto type = Web::ContentBlocker::ResourceType::XMLHttpRequest;
    auto matches = [&] { return blocker.should_block_response(url, source, type, "GET"sv, *headers); };
    MUST(blocker.set_patterns(Vector<String> { "||tracker.example^$xhr,header=x-marker"_string }));
    EXPECT(blocker.has_header_filters());
    EXPECT_EQ(blocker.check_response(url, source, type, "GET"sv, *headers).matched_rule, "||tracker.example^$xhr,header=x-marker"sv);
    EXPECT(matches());
    EXPECT(!blocker.check(url, source, type, "GET"sv).blocked);
    EXPECT(!blocker.should_block_response(url, source, Web::ContentBlocker::ResourceType::Image, "GET"sv, *headers));
    MUST(blocker.set_patterns(Vector<String> { "||tracker.example^$header=x-marker:one\\, two,method=get"_string }));
    EXPECT(matches());
    EXPECT(!blocker.should_block_response(url, source, type, "POST"sv, *headers));
    MUST(blocker.set_patterns(Vector<String> { "||tracker.example^$header=x-marker:/^one,\\s+two$/"_string }));
    EXPECT(matches());
    MUST(blocker.set_patterns(Vector<String> { "||tracker.example^$header=x-marker:~different"_string }));
    EXPECT(matches());
    MUST(blocker.set_patterns(Vector<String> { "||tracker.example^$header=absent:~different"_string }));
    EXPECT(!matches());
    MUST(blocker.set_patterns(Vector<String> { "||tracker.example^$header=x-marker"_string, "@@||tracker.example^$header"_string }));
    EXPECT(!matches());
    MUST(blocker.set_patterns(Vector<String> { "||tracker.example^$header=x-marker,important"_string, "@@||tracker.example^$header"_string }));
    EXPECT(matches());
    MUST(blocker.set_patterns(Vector<String> { "||tracker.example^$header=x-marker"_string, "@@||tracker.example^"_string }));
    EXPECT(!matches());
    MUST(blocker.set_patterns(Vector<String> { "||tracker.example^$header=x-marker"_string, "@@||tracker.example^$header=x-marker:different"_string }));
    EXPECT(matches());
    MUST(blocker.set_patterns(Vector<String> { "||tracker.example^$header=x-marker:/[/"_string }));
    EXPECT(!matches());
    url = URL::Parser::basic_parse("https://tracker.example/tracker/path"sv).release_value();
    MUST(blocker.set_patterns(Vector<String> { "/tracker/path$header=x-marker"_string }));
    EXPECT(matches());
}

TEST_CASE(derpbird_diagnostic_rule_text_is_bounded)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    StringBuilder path;
    path.append("/bounded-"sv);
    for (size_t i = 0; i < 5000; ++i) path.append('a');
    auto rule = MUST(path.to_string());
    auto source = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    auto url = URL::Parser::basic_parse(MUST(String::formatted("https://tracker.example{}", rule))).release_value();
    MUST(blocker.set_patterns(Vector<String> { rule }));
    auto decision = blocker.check(url, source, Web::ContentBlocker::ResourceType::XMLHttpRequest, "GET"sv);
    EXPECT(decision.blocked);
    EXPECT(decision.matched_rule.bytes().size() <= 4096);
    EXPECT(decision.matched_rule.bytes_as_string_view().ends_with("..."sv));
    MUST(blocker.set_patterns(Vector<String> { MUST(String::formatted("{}$header=x-test", rule)) }));
    auto headers = HTTP::HeaderList::create();
    headers->append({ "X-Test", "present" });
    decision = blocker.check_response(url, source, Web::ContentBlocker::ResourceType::XMLHttpRequest, "GET"sv, *headers);
    EXPECT(decision.blocked);
    EXPECT(decision.matched_rule.bytes().size() <= 4096);
    EXPECT(decision.matched_rule.bytes_as_string_view().ends_with("..."sv));
}

TEST_CASE(derpbird_html_filters_are_separate_and_scoped)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    MUST(blocker.set_patterns(Vector<String> {
        "target.example##^script:has-text(advertisement)"_string,
        "target.example##^div.sponsor"_string,
        "allowed.target.example#@#^script:has-text(advertisement)"_string,
        "target.example##^responseheader(set-cookie)"_string,
    }));
    auto rules = [&](StringView host) {
        return blocker.html_filter_rules(URL::Parser::basic_parse(MUST(String::formatted("https://{}/", host))).release_value()).to_utf8_but_should_be_ported_to_utf16();
    };
    auto matching = rules("target.example"sv);
    EXPECT(matching.contains("advertisement"sv));
    EXPECT(matching.contains("sponsor"sv));
    EXPECT(!matching.contains("responseheader"sv));
    auto excepted = rules("allowed.target.example"sv);
    EXPECT(!excepted.contains("advertisement"sv));
    EXPECT(excepted.contains("sponsor"sv));
    EXPECT(rules("unrelated.example"sv).is_empty());
}

TEST_CASE(derpbird_filter_source_permissions_and_attribution)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto page = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    auto target = URL::Parser::basic_parse("https://tracker.example/provenance"sv).release_value();
    MUST(blocker.set_rules_from_bytes(R"JSON({"derpbirdFilterBundle":1,"lists":[
        {"name":"Untrusted subscription","trusted":false,"text":"page.example##+js(trusted-set-attr, #deniedProbe, data-x, secret)"},
        {"name":"My filters","trusted":true,"text":"page.example##+js(trusted-set-attr, #allowedProbe, data-x, secret)\n||tracker.example/provenance"}
    ]})JSON"sv.bytes()));
    auto isolated = blocker.scriptlets_for_url(page, true);
    EXPECT(isolated.utf16_view().contains("allowedProbe"_utf16));
    EXPECT(!isolated.utf16_view().contains("deniedProbe"_utf16));
    auto decision = blocker.check(target, page, Web::ContentBlocker::ResourceType::Script, "GET"sv);
    EXPECT(decision.blocked);
    EXPECT(decision.matched_rule.bytes_as_string_view().contains("My filters"sv));
    EXPECT(decision.matched_rule.bytes_as_string_view().contains("line 2"sv));
    EXPECT(blocker.set_rules_from_bytes(R"({"derpbirdFilterBundle":99,"lists":[]})"sv.bytes()).is_error());
}

TEST_CASE(derpbird_canonical_url_exemptions_do_not_allow_visible_requests)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    MUST(blocker.set_patterns(Vector<String> {
        "||visible.example^"_string,
        "||tracker.example^$third-party"_string,
        "@@||visible.example/cname$cname,domain=page.example"_string,
        "@@||visible.example/ordinary"_string,
    }));
    auto source = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    auto other = URL::Parser::basic_parse("https://other.example/"sv).release_value();
    auto cname = URL::Parser::basic_parse("https://visible.example/cname"sv).release_value();
    auto ordinary = URL::Parser::basic_parse("https://visible.example/ordinary"sv).release_value();
    auto blocked = URL::Parser::basic_parse("https://visible.example/blocked"sv).release_value();
    auto alias = URL::Parser::basic_parse("https://tracker.example/blocked"sv).release_value();
    auto type = Web::ContentBlocker::ResourceType::XMLHttpRequest;
    EXPECT(blocker.canonical_url_exempt(cname, source, type, "GET"sv));
    EXPECT(!blocker.canonical_url_exempt(cname, other, type, "GET"sv));
    EXPECT(blocker.canonical_url_exempt(ordinary, source, type, "GET"sv));
    EXPECT(!blocker.canonical_url_exempt(blocked, source, type, "GET"sv));
    EXPECT(blocker.check(cname, source, type, "GET"sv).blocked);
    EXPECT(!blocker.check(ordinary, source, type, "GET"sv).blocked);
    EXPECT(blocker.check(alias, source, type, "GET"sv).blocked);
}

TEST_CASE(derpbird_cached_canonical_hosts_recheck_current_rules)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto url = URL::Parser::basic_parse("https://visible.example/resource"sv).release_value();
    auto source = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    auto type = Web::ContentBlocker::ResourceType::XMLHttpRequest;
    MUST(blocker.set_patterns({}));
    EXPECT(!blocker.check_canonical_hosts(url, source, type, "GET"sv, "tracker.example\n"sv).blocked);
    MUST(blocker.set_patterns(Vector<String> { "||tracker.example^"_string }));
    auto decision = blocker.check_canonical_hosts(url, source, type, "GET"sv, "tracker.example\n"sv);
    EXPECT(decision.blocked);
    EXPECT(decision.matched_rule.bytes_as_string_view().contains("CNAME: tracker.example"sv));
    MUST(blocker.set_patterns(Vector<String> { "||tracker.example^"_string, "@@||visible.example^$cname"_string }));
    EXPECT(!blocker.check_canonical_hosts(url, source, type, "GET"sv, "tracker.example\n"sv).blocked);
}

TEST_CASE(derpbird_ecmascript_filter_regex)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto source = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    auto type = Web::ContentBlocker::ResourceType::XMLHttpRequest;
    auto check = [&](StringView input) {
        return blocker.check(URL::Parser::basic_parse(input).release_value(), source, type, "GET"sv);
    };
    MUST(blocker.set_patterns(Vector<String> { R"(/(?<=tracker\.example\/)ad(?=vert)/)"_string }));
    EXPECT(check("https://tracker.example/advert"sv).blocked);
    EXPECT(!check("https://tracker.example/advice"sv).blocked);
    MUST(blocker.set_patterns(Vector<String> { R"(/\/(track)\1$/)"_string }));
    EXPECT(check("https://tracker.example/tracktrack"sv).blocked);
    EXPECT(!check("https://tracker.example/trackother"sv).blocked);
    MUST(blocker.set_patterns(Vector<String> { R"(||tracker.example^$removeparam=/^(drop)=\1$/i)"_string }));
    auto rewritten = check("https://tracker.example/?drop=DROP&keep=yes"sv);
    EXPECT(rewritten.rewritten_url.has_value());
    if (rewritten.rewritten_url.has_value())
        EXPECT_EQ(rewritten.rewritten_url->serialize(), "https://tracker.example/?keep=yes"sv);
    auto headers = HTTP::HeaderList::create();
    headers->append({ "X-Marker", "tracktrack" });
    auto url = URL::Parser::basic_parse("https://tracker.example/"sv).release_value();
    MUST(blocker.set_patterns(Vector<String> { R"(||tracker.example^$header=x-marker:/^(track)\1$/)"_string }));
    EXPECT(blocker.should_block_response(url, source, type, "GET"sv, *headers));
    MUST(blocker.set_patterns(Vector<String> { R"(||tracker.example^$header=x-marker:~/^(track)\1$/)"_string }));
    EXPECT(!blocker.should_block_response(url, source, type, "GET"sv, *headers));
    // Exhaustion must never become a successful inverted comparison.
    headers = HTTP::HeaderList::create();
    headers->append({ "X-Marker", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!" });
    MUST(blocker.set_patterns(Vector<String> { R"(||tracker.example^$header=x-marker:~/^(?=a)(a+)+$/)"_string }));
    auto timer = Core::ElapsedTimer::start_new();
    EXPECT(!blocker.should_block_response(url, source, type, "GET"sv, *headers));
    EXPECT(timer.elapsed_milliseconds() < 1000);
    MUST(blocker.set_patterns(Vector<String> { R"(||tracker.example^$removeparam=~/^(?=a)(a+)+$/)"_string }));
    EXPECT(!check("https://tracker.example/?aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!=1"sv).rewritten_url.has_value());
    // Each decision receives a fresh budget after a pathological expression.
    MUST(blocker.set_patterns(Vector<String> { R"(/(?=tracker)tracker/)"_string }));
    EXPECT(check("https://tracker.example/"sv).blocked);
}

TEST_CASE(derpbird_ecmascript_semantics_when_rust_also_accepts_pattern)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto source = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    auto url = URL::Parser::basic_parse("https://tracker.example/advert"sv).release_value();
    auto type = Web::ContentBlocker::ResourceType::XMLHttpRequest;
    // In ECMAScript, && is two ordinary characters, not class intersection.
    MUST(blocker.set_patterns(Vector<String> { R"(/[a&&b]dvert/)"_string }));
    EXPECT(blocker.check(url, source, type, "GET"sv).blocked);
    auto header_matches = [&](StringView rule, ByteString value) {
        MUST(blocker.set_patterns(Vector<String> { MUST(String::from_utf8(rule)) }));
        auto headers = HTTP::HeaderList::create();
        headers->append({ "X-Marker", move(value) });
        return blocker.should_block_response(url, source, type, "GET"sv, *headers);
    };
    // HTTP header bytes are isomorphically decoded, as in the Fetch Headers API.
    // 0xE9 becomes U+00E9, 0xA0 becomes NBSP; UTF-8 C3 A9 remains two characters.
    EXPECT(header_matches(R"(||tracker.example^$header=x-marker:/^.$/)"sv, "\xe9"));
    EXPECT(!header_matches(R"(||tracker.example^$header=x-marker:/^..$/)"sv, "\xe9"));
    EXPECT(header_matches(R"(||tracker.example^$header=x-marker:/^\s$/)"sv, "\xa0"));
    EXPECT(header_matches(R"(||tracker.example^$header=x-marker:/^..$/)"sv, "\xc3\xa9"));
    EXPECT(!header_matches(R"(||tracker.example^$header=x-marker:/^.$/)"sv, "\xc3\xa9"));
    // JS non-Unicode identity escape, versus Rust absolute-end assertion.
    EXPECT(header_matches(R"(||tracker.example^$header=x-marker:/^\z$/)"sv, "z"));
    EXPECT(!header_matches(R"(||tracker.example^$header=x-marker:/^\z$/)"sv, ""));
    EXPECT(header_matches(R"(||tracker.example^$header=x-marker:/^[a&&b]$/)"sv, "a"));
    EXPECT(!header_matches(R"(||tracker.example^$header=x-marker:/^[a&&b]$/)"sv, "c"));
}

TEST_CASE(derpbird_trusted_urlskip_chains_and_exceptions)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto source = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    auto set = [&](StringView rules, bool trusted = true) {
        JsonObject item;
        item.set("name"sv, "My filters"sv);
        item.set("trusted"sv, trusted);
        item.set("text"sv, rules);
        JsonArray lists;
        MUST(lists.append(move(item)));
        JsonObject bundle;
        bundle.set("derpbirdFilterBundle"sv, 1);
        bundle.set("lists"sv, move(lists));
        auto json = bundle.serialized();
        MUST(blocker.set_rules_from_bytes(json.bytes()));
    };
    auto check = [&](StringView input, StringView expected) {
        auto url = URL::Parser::basic_parse(input).release_value();
        auto result = blocker.check(url, source, Web::ContentBlocker::ResourceType::XMLHttpRequest, "GET"sv);
        EXPECT_EQ(result.rewritten_url.value_or(url).serialize(), expected);
        return result;
    };
    set("||tracker.example^$urlskip=?to"sv, false);
    check("https://tracker.example/?to=https%3A%2F%2Ftarget.example%2F"sv, "https://tracker.example/?to=https%3A%2F%2Ftarget.example%2F"sv);
    set("||tracker.example^$urlskip=?to"sv);
    EXPECT(check("https://tracker.example/?to=https%3A%2F%2Ftarget.example%2F"sv, "https://target.example/"sv).trusted_url_skip);
    set("||tracker.example^$urlskip=?to ?next -uricomponent"sv);
    check("https://tracker.example/?to=https%3A%2F%2Fmiddle.example%2F%3Fnext%3Dhttps%253A%252F%252Ftarget.example%252F"sv, "https://target.example/"sv);
    set("||tracker.example^$urlskip=# -safebase64"sv);
    check("https://tracker.example/#aHR0cHM6Ly90YXJnZXQuZXhhbXBsZS8"sv, "https://target.example/"sv);
    set("||tracker.example^$urlskip=&1 +https"sv);
    check("https://tracker.example/?target.example%2F"sv, "https://target.example/"sv);
    set(R"(||tracker.example^$urlskip=/(?<=jump\/)(target\.example\/)/ +https)"sv);
    check("https://tracker.example/jump/target.example/"sv, "https://target.example/"sv);
    set("||tracker.example^$urlskip=?to\n@@||tracker.example^$urlskip=?to"sv);
    check("https://tracker.example/?to=https://target.example/"sv, "https://tracker.example/?to=https://target.example/"sv);
    set("||tracker.example^$urlskip=?to\n@@||tracker.example^$urlskip"sv);
    check("https://tracker.example/?to=https://target.example/"sv, "https://tracker.example/?to=https://target.example/"sv);
    set("||tracker.example^\n||tracker.example^$urlskip=?to"sv);
    EXPECT(check("https://tracker.example/?to=https://target.example/"sv, "https://tracker.example/?to=https://target.example/"sv).blocked);
    set("||tracker.example^\n||tracker.example^$urlskip=?to -blocked"sv);
    EXPECT(!check("https://tracker.example/?to=https://target.example/"sv, "https://target.example/"sv).blocked);
    set("||tracker.example^$urlskip=?to"sv);
    check("https://tracker.example/?to=javascript:alert(1)"sv, "https://tracker.example/?to=javascript:alert(1)"sv);
    set("||tracker.example^$urlskip=?to -uricomponent"sv);
    check("https://tracker.example/?to=%25XX"sv, "https://tracker.example/?to=%25XX"sv);
    set("||tracker.example^$urlskip=?to\n||tracker.example^$urlskip=?to,badfilter"sv);
    check("https://tracker.example/?to=https://target.example/"sv, "https://tracker.example/?to=https://target.example/"sv);
}

TEST_CASE(derpbird_filter_conditions_are_applied_before_every_engine)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    MUST(blocker.set_rules_from_bytes(R"(!#if (ext_ublock && !env_chromium)
||active.example^
!#if false
||nested-inactive.example^
!#else
||nested-active.example^
!#endif
!#else
||inactive.example^
!#endif
!#if unknown_vendor
||unknown-first.example^
!#else
||unknown-second.example^
!#endif
!#if cap_missing
||missing-cap.example^
!#endif
)"sv.bytes()));
    auto source = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    auto blocked = [&](StringView host) {
        auto url = URL::Parser::basic_parse(MUST(String::formatted("https://{}/", host))).release_value();
        return blocker.check(url, source, Web::ContentBlocker::ResourceType::Script, "GET"sv).blocked;
    };
    for (auto host : { "active.example"sv, "nested-active.example"sv, "unknown-first.example"sv, "unknown-second.example"sv }) EXPECT(blocked(host));
    for (auto host : { "inactive.example"sv, "nested-inactive.example"sv, "missing-cap.example"sv }) EXPECT(!blocked(host));
    EXPECT(blocker.set_rules_from_bytes("!#if false\n||broken.example^"sv.bytes()).is_error());
    EXPECT(blocked("active.example"sv)); // Failed replacement keeps the previous engine.
    MUST(blocker.set_rules_from_bytes(R"JSON({"derpbirdFilterBundle":1,"lists":[{"name":"My filters","trusted":true,"text":"!#if false\n||page.example^$urlskip=?to\npage.example##+js(trusted-set-attr, #forbidden, x, y)\npage.example##^script\n!#else\n||active.example^\n!#endif"}]})JSON"sv.bytes()));
    auto target = URL::Parser::basic_parse("https://page.example/?to=https://other.example/"sv).release_value();
    EXPECT(!blocker.check(target, source, Web::ContentBlocker::ResourceType::Document, "GET"sv).rewritten_url.has_value());
    auto scripts = blocker.scriptlets_for_url(source, true);
    EXPECT(!scripts.utf16_view().contains("forbidden"_utf16));
    EXPECT(blocker.html_filter_rules(source).is_empty());
    EXPECT(blocked("active.example"sv));
}

#include <LibWeb/Page/DerpBirdResponseReplacement.h>

TEST_CASE(derpbird_browser_package_filter_actions)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto source = URL::Parser::basic_parse("https://page.example/"sv).release_value();
    auto target = URL::Parser::basic_parse("https://track.example/request"sv).release_value();
    MUST(blocker.set_patterns(Vector<String> { "||track.example^$method=PUT|PATCH|DELETE|OPTIONS"_string }));
    for (auto method : { "PUT"sv, "PATCH"sv, "DELETE"sv, "OPTIONS"sv })
        EXPECT(blocker.check(target, source, Web::ContentBlocker::ResourceType::XMLHttpRequest, method).blocked);
    EXPECT(!blocker.check(target, source, Web::ContentBlocker::ResourceType::XMLHttpRequest, "GET"sv).blocked);
    MUST(blocker.set_rules_from_bytes(R"JSON({"derpbirdFilterBundle":1,"lists":[{"name":"My filters","trusted":true,"user_owned":true,"text":"!#derpbird-dynamic * * 3p-script block\n!#derpbird-dynamic page.example track.example * noop\n||track.example/blocked$script\n"}]})JSON"sv.bytes()));
    EXPECT(!blocker.check(target, source, Web::ContentBlocker::ResourceType::Script, "GET"sv).blocked);
    auto other = URL::Parser::basic_parse("https://other.example/request"sv).release_value();
    EXPECT(blocker.check(other, source, Web::ContentBlocker::ResourceType::Script, "GET"sv).blocked);
    MUST(blocker.set_rules_from_bytes(R"JSON({"derpbirdFilterBundle":1,"lists":[{"name":"My filters","trusted":true,"text":"||page.example^$permissions=camera=()|microphone=()\n||page.example^$replace=/foo/bar/g\n"}]})JSON"sv.bytes()));
    auto policy = blocker.response_modifiers(source, source, Web::ContentBlocker::ResourceType::Document, "GET"sv, true);
    EXPECT_EQ(policy, "camera=(), microphone=()"sv);
    auto operations = blocker.response_replacements(source, source, Web::ContentBlocker::ResourceType::Document, "GET"sv);
    EXPECT_EQ(operations.size(), 1u);
    auto changed = MUST(Web::replace_derpbird_response("foo foo"_utf16, operations));
    EXPECT_EQ(changed, "bar bar"_utf16);
}

TEST_CASE(derpbird_response_replacement_capture_and_zero_length)
{
    auto operations = MUST(JsonValue::from_string(R"JSON([{"pattern":"(a)(b)","replacement":"$2$1-$$-$&","flags":"g"}])JSON"sv));
    EXPECT_EQ(MUST(Web::replace_derpbird_response("ab ab"_utf16, operations.as_array())), "ba-$-ab ba-$-ab"_utf16);
    auto empty = MUST(JsonValue::from_string(R"JSON([{"pattern":"(?=a)","replacement":"x","flags":"g"}])JSON"sv));
    EXPECT_EQ(MUST(Web::replace_derpbird_response("aa"_utf16, empty.as_array())), "xaxa"_utf16);
}

TEST_CASE(derpbird_modifier_except_all_preserves_important_rules)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto rules = R"JSON({"derpbirdFilterBundle":1,"lists":[{"name":"trusted","trusted":true,"text":"||example.org^$permissions=camera=(),important\n||example.org^$permissions=geolocation=()\n@@||example.org^$permissions\n||example.org^$replace=/foo/bar/g,important\n||example.org^$replace=/foo/baz/g\n@@||example.org^$replace\n"}]})JSON"sv;
    MUST(blocker.set_rules_from_bytes(rules.bytes()));
    auto url = URL::Parser::basic_parse("https://example.org/page"sv).release_value();
    EXPECT_EQ(blocker.response_modifiers(url, url, Web::ContentBlocker::ResourceType::Document, "GET"sv, true), "camera=()"sv);
    auto replacements = blocker.response_replacements(url, url, Web::ContentBlocker::ResourceType::Document, "GET"sv);
    EXPECT_EQ(replacements.size(), 1u);
    if (replacements.size() == 1)
        EXPECT_EQ(replacements.at(0).as_object().get_string("replacement"sv).value(), "bar"sv);
}

TEST_CASE(derpbird_dynamic_rules_use_top_context_and_static_rules_keep_frame_context)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    MUST(blocker.set_rules_from_bytes(R"JSON({"derpbirdFilterBundle":1,"lists":[{"name":"My filters","trusted":true,"user_owned":true,"text":"!#derpbird-dynamic top.example * 3p-script block\n!#derpbird-dynamic top.example * inline-script block\n!#derpbird-dynamic safe.example * * allow\n||top.example/static$script,domain=frame.other\n||frame.other/headers$requestheader=x-test:yes\n||frame.other/headers$header=x-test:yes\n||frame.other^$permissions=camera=()\n"}]})JSON"sv.bytes()));
    auto top = URL::Parser::basic_parse("https://top.example/"sv).release_value();
    auto safe = URL::Parser::basic_parse("https://safe.example/"sv).release_value();
    auto frame = URL::Parser::basic_parse("https://frame.other/"sv).release_value();
    auto local = URL::Parser::basic_parse("https://top.example/script.js"sv).release_value();
    auto remote = URL::Parser::basic_parse("https://frame.other/script.js"sv).release_value();
    auto scoped = URL::Parser::basic_parse("https://top.example/static"sv).release_value();
    using Type = Web::ContentBlocker::ResourceType;
    EXPECT(!blocker.check(local, frame, Type::Script, "GET"sv, top).blocked);
    EXPECT(blocker.check(remote, frame, Type::Script, "GET"sv, top).blocked);
    EXPECT(!blocker.check(remote, frame, Type::Script, "GET"sv).blocked);
    EXPECT(blocker.check(scoped, frame, Type::Script, "GET"sv, top).blocked);
    EXPECT(!blocker.check(scoped, top, Type::Script, "GET"sv, top).blocked);
    EXPECT(!blocker.csp_for_response(frame, frame, Type::Subdocument, "GET"sv, top).is_empty());
    EXPECT(blocker.csp_for_response(frame, frame, Type::Subdocument, "GET"sv, safe).is_empty());
    auto headers = HTTP::HeaderList::create(); headers->append({ "x-test"sv, "yes"sv });
    auto target = URL::Parser::basic_parse("https://frame.other/headers"sv).release_value();
    EXPECT(blocker.check_request_headers(target, frame, Type::XMLHttpRequest, "GET"sv, *headers, top).blocked);
    EXPECT(!blocker.check_request_headers(target, frame, Type::XMLHttpRequest, "GET"sv, *headers, safe).blocked);
    EXPECT(blocker.check_response(target, frame, Type::XMLHttpRequest, "GET"sv, *headers, top).blocked);
    EXPECT(!blocker.check_response(target, frame, Type::XMLHttpRequest, "GET"sv, *headers, safe).blocked);
    EXPECT(blocker.canonical_url_exempt(remote, frame, Type::Script, "GET"sv, safe));
    EXPECT(blocker.response_modifiers(frame, frame, Type::Subdocument, "GET"sv, true, safe).is_empty());
}

TEST_CASE(derpbird_static_top_destination_and_denyallow_constraints)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto top = URL::Parser::basic_parse("https://news.example/"sv).release_value();
    auto excluded_top = URL::Parser::basic_parse("https://private.news.example/"sv).release_value();
    auto frame = URL::Parser::basic_parse("https://frame.example/"sv).release_value();
    auto target = URL::Parser::basic_parse("https://cdn.ads.example/asset"sv).release_value();
    auto other = URL::Parser::basic_parse("https://allowed.example/asset"sv).release_value();
    using Type = Web::ContentBlocker::ResourceType;
    MUST(blocker.set_patterns(Vector<String> { "*$script,domain=frame.example,top=news.example|~private.news.example,to=ads.example"_string }));
    EXPECT(blocker.check(target, frame, Type::Script, "GET"sv, top).blocked);
    EXPECT(!blocker.check(target, frame, Type::Script, "GET"sv, excluded_top).blocked);
    EXPECT(!blocker.check(target, frame, Type::Script, "GET"sv, frame).blocked);
    EXPECT(!blocker.check(target, top, Type::Script, "GET"sv, top).blocked);
    EXPECT(!blocker.check(other, frame, Type::Script, "GET"sv, top).blocked);
    MUST(blocker.set_patterns(Vector<String> { "*$script,denyallow=allowed.example,top=news.example"_string }));
    EXPECT(blocker.check(target, frame, Type::Script, "GET"sv, top).blocked);
    EXPECT(!blocker.check(other, frame, Type::Script, "GET"sv, top).blocked);
    MUST(blocker.set_patterns(Vector<String> { "*$script,top=news.example"_string, "@@*$script,to=ads.example"_string }));
    EXPECT(!blocker.check(target, frame, Type::Script, "GET"sv, top).blocked);
    EXPECT(blocker.check(other, frame, Type::Script, "GET"sv, top).blocked);
    MUST(blocker.set_patterns(Vector<String> { "*$script,top=news.example"_string, "*$script,top=news.example,badfilter"_string }));
    EXPECT(!blocker.check(target, frame, Type::Script, "GET"sv, top).blocked);
    MUST(blocker.set_patterns(Vector<String> { "*$script,domain=/^frame[.]example$/,top=/^(news|other)[.]example$/,to=/[.]ads[.]example$/"_string }));
    EXPECT(blocker.check(target, frame, Type::Script, "GET"sv, top).blocked);
    EXPECT(!blocker.check(target, frame, Type::Script, "GET"sv, excluded_top).blocked);
    MUST(blocker.set_patterns(Vector<String> { "*$script,top=unused.example|/^news[.]example$/,to=ads.example|~/(allowed|safe)/"_string }));
    EXPECT(blocker.check(target, frame, Type::Script, "GET"sv, top).blocked);
    auto entity_top = URL::Parser::basic_parse("https://news.example.co.uk/"sv).release_value();
    MUST(blocker.set_patterns(Vector<String> { "*$script,top=example.*"_string }));
    EXPECT(blocker.check(target, frame, Type::Script, "GET"sv, entity_top).blocked);
    EXPECT(!blocker.check(target, frame, Type::Script, "GET"sv, excluded_top).blocked);
    MUST(blocker.set_patterns(Vector<String> { "*$xhr,top=news.example,requestheader=cookie:transport_probe=1"_string }));
    auto headers = HTTP::HeaderList::create(); headers->append({ "Cookie"sv, "transport_probe=1"sv });
    EXPECT(blocker.check_request_headers(target, frame, Type::XMLHttpRequest, "GET"sv, *headers, top).blocked);
    EXPECT(!blocker.check_request_headers(target, frame, Type::XMLHttpRequest, "GET"sv, *headers, frame).blocked);
}

TEST_CASE(derpbird_json_and_jsonl_response_replacements)
{
    auto vm = JS::VM::create();
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto url = URL::Parser::basic_parse("https://data.example/api"sv).release_value();
    using Type = Web::ContentBlocker::ResourceType;
    auto select = [&](StringView rules) {
        JsonObject list; list.set("name"sv, "trusted"sv); list.set("trusted"sv, true); list.set("text"sv, rules);
        JsonArray lists; lists.must_append(move(list));
        JsonObject bundle; bundle.set("derpbirdFilterBundle"sv, 1); bundle.set("lists"sv, move(lists));
        auto text = JsonValue(move(bundle)).serialized();
        MUST(blocker.set_rules_from_bytes(text.bytes()));
        return blocker.response_replacements(url, url, Type::XMLHttpRequest, "GET"sv);
    };
    auto removal = select("||data.example^$xhr,replace=json:..ads"sv);
    EXPECT_EQ(removal.size(), 1u);
    auto output = MUST(Web::replace_derpbird_response(R"({"ads":[1],"nested":{"ads":2,"keep":3}})"_utf16, removal));
    EXPECT_EQ(output, R"({"nested":{"keep":3}})"_utf16);
    auto assignment = select("||data.example^$replace=json:.payload={\"a\":1,\"b\":2},xhr"sv);
    EXPECT_EQ(assignment.size(), 1u);
    EXPECT_EQ(MUST(Web::replace_derpbird_response(R"({"payload":0})"_utf16, assignment)), R"({"payload":{"a":1,"b":2}})"_utf16);
    auto lines = select("||data.example^$replace=jsonl:.ads,xhr"sv);
    EXPECT_EQ(MUST(Web::replace_derpbird_response("{\"ads\":1,\"keep\":2}\r\n\n{\"ads\":3}\n"_utf16, lines)), "{\"keep\":2}\r\n\n{}\n"_utf16);
    auto regex = select("||data.example^$replace=json:./^ad/,xhr"sv);
    EXPECT_EQ(MUST(Web::replace_derpbird_response(R"({"adOne":1,"keep":2,"adTwo":3})"_utf16, regex)), R"({"keep":2})"_utf16);
    auto excepted = select("||data.example^$replace=json:.ads,xhr\n@@||data.example^$replace"sv);
    EXPECT(excepted.is_empty());
    EXPECT_EQ(MUST(Web::replace_derpbird_response("not JSON"_utf16, removal)), "not JSON"_utf16);
}

TEST_CASE(derpbird_popunder_fallback_requires_a_hostname_match)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    auto source = URL::Parser::basic_parse("https://reader.example/"sv).release_value();
    auto target = URL::Parser::basic_parse("https://ads.example/advert/path"sv).release_value();
    using Type = Web::ContentBlocker::ResourceType;
    MUST(blocker.set_patterns(Vector<String> { "||ads.example^"_string }));
    EXPECT(blocker.check(target, source, Type::Document, "GET"sv).blocked);
    for (auto type : { Type::Popup, Type::Popunder })
        EXPECT(!blocker.check(target, source, type, "GET"sv).blocked);
    EXPECT(blocker.csp_for_response(target, source, Type::Document, "GET"sv).is_empty());
    for (auto rule : { "*$popup"sv, "/advert/$popup"sv, "/^https:/$popup"sv }) {
        MUST(blocker.set_patterns(Vector<String> { MUST(String::from_utf8(rule)) }));
        EXPECT(blocker.check(target, source, Type::Popup, "GET"sv).blocked);
        EXPECT(!blocker.check(target, source, Type::Popunder, "GET"sv).blocked);
    }
    for (auto rule : { "||ads.example^$popup"sv, "ads.example$popup"sv, "/ads[.]example/$popup"sv,
             "/(?<=https:\\/\\/)ads[.]example/$popup"sv, "||ads.example^$popunder"sv }) {
        MUST(blocker.set_patterns(Vector<String> { MUST(String::from_utf8(rule)) }));
        EXPECT(blocker.check(target, source, Type::Popunder, "GET"sv).blocked);
    }
    MUST(blocker.set_patterns(Vector<String> { "||ads.example^$popup"_string, "@@||ads.example^$popup,domain=reader.example"_string }));
    EXPECT(!blocker.check(target, source, Type::Popunder, "GET"sv).blocked);
    MUST(blocker.set_patterns(Vector<String> { "||ads.example^$popup,important"_string, "@@||ads.example^$popup"_string }));
    EXPECT(blocker.check(target, source, Type::Popunder, "GET"sv).blocked);
    MUST(blocker.set_patterns(Vector<String> { "||ads.example^|$popup"_string }));
    EXPECT(blocker.check(target, source, Type::Popunder, "GET"sv).blocked);
}

TEST_CASE(derpbird_action_rule_source_attribution)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    using Type = Web::ContentBlocker::ResourceType;
    auto url = URL::Parser::basic_parse("https://target.example/page?drop=1&keep=2"sv).release_value();
    auto set = [&](StringView extra) {
        auto rules = MUST(String::formatted("! comment\n||target.example^$csp=script-src 'none'\n||target.example^$permissions=camera=()\n||target.example^$removeparam=drop\ntarget.example##^responseheader(refresh)\nother.example##^responseheader(refresh)\n{}", extra));
        JsonObject item; item.set("name"sv, "Attribution list"sv); item.set("trusted"sv, true); item.set("text"sv, rules);
        JsonArray lists; lists.must_append(move(item));
        JsonObject bundle; bundle.set("derpbirdFilterBundle"sv, 1); bundle.set("lists"sv, move(lists));
        auto text = bundle.serialized();
        MUST(blocker.set_rules_from_bytes(text.bytes()));
    };
    set(""sv);
    auto expect_rule = [&](u8 action, StringView expected, StringView line) {
        auto rules = blocker.action_rules(url, url, Type::Document, "GET"sv, {}, action);
        EXPECT_EQ(rules.size(), 1u);
        if (!rules.is_empty()) {
            EXPECT(rules[0].contains(expected));
            EXPECT(rules[0].contains(line));
        }
    };
    expect_rule(0, "$csp=script-src 'none'"sv, "Attribution list · line 2"sv);
    expect_rule(1, "$permissions=camera=()"sv, "Attribution list · line 3"sv);
    expect_rule(3, "target.example##^responseheader(refresh)"sv, "Attribution list · line 5"sv);
    auto rewrite = blocker.check(url, url, Type::Document, "GET"sv);
    EXPECT(rewrite.rewritten_url.has_value());
    EXPECT(rewrite.matched_rule.contains("$removeparam=drop"sv));
    EXPECT(rewrite.matched_rule.contains("Attribution list · line 4"sv));
    set("@@||target.example^$csp\n@@||target.example^$permissions\n@@||target.example^$removeparam=drop\ntarget.example#@#^responseheader(refresh)"sv);
    for (u8 action : { 0, 1, 3 })
        EXPECT(blocker.action_rules(url, url, Type::Document, "GET"sv, {}, action).is_empty());
    EXPECT(!blocker.check(url, url, Type::Document, "GET"sv).rewritten_url.has_value());
}

TEST_CASE(derpbird_address_context_reaches_network_and_response_actions)
{
    auto& blocker = Web::ContentBlocker::the();
    ScopeGuard clear = [&] { MUST(blocker.set_patterns({})); };
    using Type = Web::ContentBlocker::ResourceType;
    auto url = URL::Parser::basic_parse("https://target.example/page?drop=1&keep=2"sv).release_value();
    auto source = URL::Parser::basic_parse("https://reader.example/"sv).release_value();
    auto install = [&](StringView rules) {
        JsonObject list;
        list.set("name"sv, "Address stages"sv);
        list.set("trusted"sv, true);
        list.set("text"sv, rules);
        JsonArray lists;
        lists.must_append(move(list));
        JsonObject bundle;
        bundle.set("derpbirdFilterBundle"sv, 1);
        bundle.set("lists"sv, move(lists));
        auto serialized = bundle.serialized();
        MUST(blocker.set_rules_from_bytes(serialized.bytes()));
        EXPECT(blocker.has_ip_address_filters());
    };
    install("!#if cap_ipaddress\n||target.example^$script,ipaddress=192.168.*\n!#endif"sv);
    EXPECT(!blocker.check(url, source, Type::Script, "GET"sv).blocked);
    EXPECT(!blocker.check(url, source, Type::Script, "GET"sv, {}, "203.0.113.1"sv).blocked);
    EXPECT(blocker.check(url, source, Type::Script, "GET"sv, {}, "192.168.1.2"sv).blocked);
    install("||target.example^$xhr,ipaddress=lan,redirect=noop.json"sv);
    auto redirected = blocker.check(url, source, Type::XMLHttpRequest, "GET"sv, {}, "192.168.1.2"sv);
    EXPECT(redirected.blocked);
    EXPECT(redirected.substitute.has_value());
    install("||target.example^$ipaddress=192.168.*,removeparam=drop"sv);
    EXPECT(!blocker.check(url, source, Type::Document, "GET"sv).rewritten_url.has_value());
    auto rewritten = blocker.check(url, source, Type::Document, "GET"sv, {}, "192.168.1.2"sv);
    VERIFY(rewritten.rewritten_url.has_value());
    EXPECT_EQ(rewritten.rewritten_url->query().value_or(""_string), "keep=2"sv);
    install("||target.example^$ipaddress=192.168.*,csp=script-src 'none'\n||target.example^$ipaddress=192.168.*,permissions=camera=()\n||target.example^$ipaddress=192.168.*,replace=/advert/clean/g"sv);
    EXPECT(blocker.csp_for_response(url, source, Type::Document, "GET"sv).is_empty());
    EXPECT(!blocker.csp_for_response(url, source, Type::Document, "GET"sv, {}, "192.168.1.2"sv).is_empty());
    EXPECT(blocker.response_modifiers(url, source, Type::Document, "GET"sv, true).is_empty());
    EXPECT(!blocker.response_modifiers(url, source, Type::Document, "GET"sv, true, {}, "192.168.1.2"sv).is_empty());
    EXPECT(blocker.response_replacements(url, source, Type::Document, "GET"sv).is_empty());
    EXPECT_EQ(blocker.response_replacements(url, source, Type::Document, "GET"sv, {}, "192.168.1.2"sv).size(), 1u);
    EXPECT(!blocker.action_rules(url, source, Type::Document, "GET"sv, {}, 0, "192.168.1.2"sv).is_empty());
    install("||target.example^$ipaddress=/^192\\.168\\.[0-9]{1,3}\\./,requestheader=x-ad:yes\n||target.example^$ipaddress=192.168.*,header=x-ad:yes"sv);
    auto headers = HTTP::HeaderList::create({ { "X-Ad"sv, "yes"sv } });
    EXPECT(!blocker.check_request_headers(url, source, Type::Script, "GET"sv, *headers).blocked);
    EXPECT(blocker.check_request_headers(url, source, Type::Script, "GET"sv, *headers, {}, "192.168.1.2"sv).blocked);
    EXPECT(!blocker.check_response(url, source, Type::Script, "GET"sv, *headers).blocked);
    EXPECT(blocker.check_response(url, source, Type::Script, "GET"sv, *headers, {}, "192.168.1.2"sv).blocked);
}
