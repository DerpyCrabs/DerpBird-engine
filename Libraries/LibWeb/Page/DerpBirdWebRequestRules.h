#pragma once

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibURL/Parser.h>
#include <LibWeb/Page/DerpBirdRegexResponseReplacement.h>
#include <LibWeb/Page/DerpBirdScriptURLPolicy.h>

namespace Web {

// Shared compiled representation for @webRequest and runtime registration.
// Registration replaces a complete rule set atomically. Matching never runs
// user JavaScript or consults a page-modifiable RegExp implementation.
class DerpBirdWebRequestRules {
public:
    bool is_empty() const { return m_rules.is_empty(); }
    enum class Type { SubFrame, Script, XHR, WebSocket, Other };
    struct Decision {
        bool cancel { false };
        Optional<URL::URL> redirect;
        JsonObject event;
    };
    static ErrorOr<DerpBirdWebRequestRules> compile(JsonValue const& input)
    {
        if (!input.is_array() || input.as_array().size() > 128 || input.serialized().byte_count() > 65536)
            return Error::from_string_literal("Invalid or oversized webRequest rules");
        DerpBirdWebRequestRules result;
        for (auto const& raw : input.as_array().values()) {
            if (!raw.is_object()) return Error::from_string_literal("Invalid webRequest rule");
            auto const& object = raw.as_object();
            auto selector = object.get("selector"sv);
            auto action = object.get("action"sv);
            if (!selector.has_value() || !action.has_value()) return Error::from_string_literal("Missing webRequest selector/action");
            Vector<String> includes, matches, excludes;
            auto strings = [](JsonValue const& value, Vector<String>& target) -> ErrorOr<void> {
                if (value.is_string()) {
                    if (value.as_string().is_empty()) return Error::from_string_literal("Empty webRequest selector");
                    target.append(value.as_string());
                } else if (value.is_array()) {
                    if (value.as_array().size() > 128) return Error::from_string_literal("Too many webRequest selectors");
                    for (auto const& item : value.as_array().values()) {
                        if (!item.is_string() || item.as_string().is_empty()) return Error::from_string_literal("Invalid webRequest selector");
                        target.append(item.as_string());
                    }
                } else return Error::from_string_literal("Invalid webRequest selector");
                return {};
            };
            if (selector->is_string()) TRY(strings(*selector, includes));
            else if (selector->is_object()) {
                auto const& options = selector->as_object();
                if (auto value = options.get("include"sv); value.has_value()) TRY(strings(*value, includes));
                if (auto value = options.get("match"sv); value.has_value()) TRY(strings(*value, matches));
                if (auto value = options.get("exclude"sv); value.has_value()) TRY(strings(*value, excludes));
            } else return Error::from_string_literal("Invalid webRequest selector");
            if (includes.is_empty() && matches.is_empty()) return Error::from_string_literal("Missing positive webRequest selector");
            auto policy = TRY(DerpBirdScriptURLPolicy::create(matches, {}, false, includes, excludes));
            bool cancel = false;
            Optional<String> target;
            Optional<JsonArray> replacement;
            if (action->is_string()) {
                if (action->as_string() != "cancel"sv) return Error::from_string_literal("Unknown webRequest action");
                cancel = true;
            } else if (action->is_object()) {
                auto const& options = action->as_object();
                if (options.has("cancel"sv) && !options.get_bool("cancel"sv).has_value()) return Error::from_string_literal("Invalid cancel action");
                cancel = options.get_bool("cancel"sv).value_or(false);
                if (auto redirect = options.get("redirect"sv); redirect.has_value()) {
                    if (cancel) return Error::from_string_literal("Conflicting webRequest actions");
                    if (redirect->is_string()) target = redirect->as_string();
                    else if (redirect->is_object()) {
                        auto from = redirect->as_object().get_string("from"sv);
                        auto to = redirect->as_object().get_string("to"sv);
                        if (!from.has_value() || !to.has_value() || from->bytes().size() > 4096 || to->bytes().size() > 8192)
                            return Error::from_string_literal("Invalid webRequest redirect expression");
                        JsonObject operation;
                        operation.set("pattern"sv, *from);
                        operation.set("replacement"sv, *to);
                        operation.set("flags"sv, ""sv);
                        JsonArray operations;
                        operations.must_append(move(operation));
                        // Compile and enforce the same bounded regex policy at
                        // registration, before replacing the last good rule set.
                        (void)TRY(replace_derpbird_regex_response(Utf16String {}, operations));
                        replacement = move(operations);
                    } else return Error::from_string_literal("Invalid redirect action");
                }
                if (!cancel && !target.has_value() && !replacement.has_value()) return Error::from_string_literal("Empty webRequest action");
            } else return Error::from_string_literal("Invalid webRequest action");
            if (target.has_value() && !valid_target(*target).has_value()) return Error::from_string_literal("Invalid webRequest redirect URL");
            result.m_rules.append(Rule { move(policy), cancel, move(target), move(replacement), object });
        }
        return result;
    }

    ErrorOr<Optional<Decision>> evaluate(URL::URL const& url, Type type, DerpBirdScriptURLPolicy const& script_scope,
        DerpBirdScriptMatchBudget& budget, u64& replacement_budget) const
    {
        if (type == Type::Other) return Optional<Decision> {};
        for (auto const& rule : m_rules) {
            auto match = rule.selector.evaluate(url, true, false, budget);
            if (match == DerpBirdScriptEligibility::LimitExceeded) return Error::from_string_literal("webRequest match budget exceeded");
            if (match != DerpBirdScriptEligibility::Allowed) continue;
            Decision decision;
            decision.cancel = rule.cancel;
            decision.event.set("rule"sv, rule.original);
            decision.event.set("url"sv, url.serialize());
            decision.event.set("info"sv, rule.cancel ? "cancel"sv : "redirect"sv);
            decision.event.set("message"sv, "ok"sv);
            if (!rule.cancel) {
                String address;
                if (rule.target.has_value()) address = *rule.target;
                else address = TRY(replace_derpbird_regex_response(Utf16String::from_utf8(url.serialize()), *rule.replacement, &replacement_budget)).to_utf8();
                auto target = valid_target(address);
                if (!target.has_value() || script_scope.evaluate(*target, true, false, budget) != DerpBirdScriptEligibility::Allowed) {
                    decision.event.set("message"sv, "error"sv);
                    decision.event.set("description"sv, "Redirect destination is outside the installed script scope"sv);
                } else if (*target != url) {
                    decision.redirect = *target;
                    decision.event.set("redirect_url"sv, target->serialize());
                }
            }
            return Optional<Decision> { move(decision) };
        }
        return Optional<Decision> {};
    }
private:
    static Optional<URL::URL> valid_target(String const& address)
    {
        if (address.bytes().size() > 8192) return {};
        auto url = URL::Parser::basic_parse(address);
        if (!url.has_value() || !url->scheme().is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv)
            || !url->username().is_empty() || !url->password().is_empty()) return {};
        return url;
    }
    struct Rule {
        DerpBirdScriptURLPolicy selector;
        bool cancel;
        Optional<String> target;
        Optional<JsonArray> replacement;
        JsonObject original;
    };
    Vector<Rule> m_rules;
};
}
