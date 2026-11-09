#pragma once
#include <AK/RefCounted.h>
#include <AK/HashMap.h>
#include <AK/NumericLimits.h>
#include <AK/NeverDestroyed.h>
#include <AK/QuickSort.h>
#include <AK/Weakable.h>
#include <LibGC/Root.h>
#include <LibGC/Weak.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/Page/DerpBirdScriptRegistry.h>
#include <LibWeb/Page/DerpBirdWebRequestRules.h>

namespace Web {
class DerpBirdWebRequestRegistration final : public RefCounted<DerpBirdWebRequestRegistration>, public Weakable<DerpBirdWebRequestRegistration> {
public:
    using Rules = DerpBirdWebRequestRules;
    ~DerpBirdWebRequestRegistration() { close(); }
    static ErrorOr<NonnullRefPtr<DerpBirdWebRequestRegistration>> create(DOM::Document& document, DerpBirdScriptDefinition const& definition) {
        auto json = TRY(JsonValue::from_string(definition.configuration));
        auto copy = TRY(DerpBirdScriptDefinition::decode(json.as_object()));
        auto registration = adopt_ref(*new DerpBirdWebRequestRegistration(document, definition.id, definition.configuration, move(copy.url_policy)));
        JsonArray initial;
        auto metadata = json.as_object().get_object("metadata"sv);
        if (auto entries = metadata->get_array("entries"sv); entries.has_value()) {
            for (auto const& entry : entries->values()) {
                if (!entry.is_object() || entry.as_object().get_string("key"sv) != "webRequest"sv) continue;
                auto text = entry.as_object().get_string("value"sv);
                if (!text.has_value()) return Error::from_string_literal("Invalid @webRequest metadata");
                auto rules = TRY(JsonValue::from_string(*text));
                if (rules.is_array()) {
                    for (auto const& rule : rules.as_array().values()) initial.must_append(rule);
                } else initial.must_append(move(rules));
                if (initial.size() > 128) return Error::from_string_literal("Too many @webRequest rules");
            }
        }
        registration->m_serialized_rules = JsonValue { move(initial) };
        registration->m_rules = TRY(Rules::compile(registration->m_serialized_rules));
        registrations().remove_all_matching([](auto const& item) { return !item || !item->active(); });
        if (registrations().size() >= 4096) return Error::from_string_literal("Too many webRequest registrations");
        registrations().append(registration->make_weak_ptr());
        local_index().ensure(document.id()).set(definition.id, registration->make_weak_ptr());
        quick_sort(registrations(), [](auto const& a, auto const& b) {
            return before(a->m_document->id(), a->m_script, b->m_document->id(), b->m_script);
        });
        registration->publish(false);
        return registration;
    }
    ErrorOr<void> replace(JsonValue const& rules) {
        if (!active()) return Error::from_string_literal("webRequest registration revoked");
        if (revision == NumericLimits<u64>::max()) return Error::from_string_literal("webRequest revision exhausted");
        if (m_publications.size() >= 32) return Error::from_string_literal("Too many pending webRequest registrations");
        auto compiled = TRY(Rules::compile(rules));
        m_rules = move(compiled);
        m_serialized_rules = rules;
        ++revision;
        publish(false);
        return {};
    }
    void close() {
        if (m_closed) return;
        m_closed = true;
        if (auto document = local_index().find(m_document->id()); document != local_index().end()) {
            auto item = document->value.find(m_script);
            if (item != document->value.end() && item->value.ptr() == this) document->value.remove(item);
            if (document->value.is_empty()) local_index().remove(document);
        }
        mirrors().remove_all_matching([&](auto const& item) { return item.document == m_document->id() && item.script == m_script; });
        on_event = nullptr;
        m_publications.clear();
        publish(true);
    }
    bool active() const { return !m_closed && m_document->is_fully_active(); }
    u64 revision { 0 };
    size_t pending_events { 0 };
    Function<void(JsonObject)> on_event;
    static bool has_rules(Page const& page) {
        for (auto const& owner : registrations())
            if (owner && owner->active() && owner->same_tab(page) && !owner->m_rules.is_empty()) return true;
        for (auto const& mirror : mirrors())
            if (mirror.page.ptr().ptr() == &page && !mirror.rules.is_empty() && !local_owner(mirror.document, mirror.script)) return true;
        return false;
    }
    // Browser-authorized snapshots are installed atomically; a bad snapshot
    // leaves the previous policy intact. GC weak pages do not retain closed tabs.
    static ErrorOr<void> receive_snapshot(Page& page, JsonValue const& snapshot) {
        if (!snapshot.is_array() || snapshot.as_array().size() > 4096)
            return Error::from_string_literal("Invalid webRequest snapshot");
        Vector<Mirror> next;
        for (auto const& raw : snapshot.as_array().values()) {
            if (!raw.is_object()) return Error::from_string_literal("Invalid webRequest record");
            auto const& object = raw.as_object();
            auto ns = object.get_u64("namespace"sv), id = object.get_u64("document"sv), version = object.get_u64("revision"sv);
            auto script = object.get_string("script"sv);
            auto rules = object.get("rules"sv);
            auto scope = object.get_object("scope"sv);
            if (!ns.has_value() || !id.has_value() || !version.has_value() || !script.has_value() || !rules.has_value() || !scope.has_value())
                return Error::from_string_literal("Incomplete webRequest record");
            auto strings = [&](StringView key) -> ErrorOr<Vector<String>> {
                auto values = scope->get_array(key);
                if (!values.has_value() || values->size() > 1024) return Error::from_string_literal("Invalid webRequest scope");
                Vector<String> result;
                for (auto const& value : values->values()) {
                    if (!value.is_string()) return Error::from_string_literal("Invalid webRequest scope pattern");
                    result.append(value.as_string());
                }
                return result;
            };
            auto matches = TRY(strings("matches"sv)), exclusions = TRY(strings("excludeMatches"sv));
            auto includes = TRY(strings("includes"sv)), excludes = TRY(strings("excludes"sv));
            auto policy = TRY(DerpBirdScriptURLPolicy::create(matches, exclusions, false, includes, excludes, scope->get_bool("contextMenu"sv).value_or(false)));
            next.append(Mirror { GC::Weak<Page> { page }, { *ns, *id }, *script, *version, TRY(Rules::compile(*rules)), move(policy) });
        }
        mirrors().remove_all_matching([&](auto const& item) { return !item.page || item.page.ptr().ptr() == &page; });
        mirrors().extend(move(next));
        quick_sort(mirrors(), [](auto const& a, auto const& b) {
            return before(a.document, a.script, b.document, b.script);
        });
        return {};
    }
    static void acknowledge(HTML::CrossProcessId document, String const& script, u64 version, bool accepted) {
        auto owner = local_owner(document, script);
        if (!owner) return;
        auto pending = owner->m_publications.take(version);
        if (!pending.has_value()) return;
        if (accepted) {
            owner->m_committed_rules = move(*pending);
            owner->m_committed_revision = version;
        } else if (version == owner->revision) {
            auto restored = Rules::compile(owner->m_committed_rules);
            if (restored.is_error()) { owner->close(); return; }
            owner->m_rules = restored.release_value();
            owner->m_serialized_rules = owner->m_committed_rules;
            if (owner->on_event) {
                JsonObject event;
                event.set("info"sv, "registration"sv);
                event.set("message"sv, "error"sv);
                event.set("error"sv, "Native webRequest registration was rejected"sv);
                owner->on_event(move(event));
            }
        }
    }
    static void receive_event(HTML::CrossProcessId document, String const& script, u64 version, JsonValue event) {
        auto owner = local_owner(document, script);
        if (owner && (owner->revision == version || owner->m_committed_revision == version) && owner->on_event && event.is_object())
            owner->on_event(event.as_object());
    }
    static ErrorOr<Optional<Rules::Decision>> evaluate(Page const& page, URL::URL const& url, Rules::Type type) {
        if (type == Rules::Type::Other) return Optional<Rules::Decision> {};
        // Own a stable list: event delivery is deferred by the script binding.
        auto owners = registrations();
        DerpBirdScriptMatchBudget budget;
        u64 replacement_budget = 4'000'000;
        // Merge the sorted local and remote tables. Precedence must not depend
        // on which process happens to host the requesting frame.
        size_t local = 0, remote = 0;
        for (;;) {
            while (local < owners.size() && (!owners[local] || !owners[local]->active() || !owners[local]->same_tab(page))) ++local;
            while (remote < mirrors().size() && (mirrors()[remote].page.ptr().ptr() != &page || local_owner(mirrors()[remote].document, mirrors()[remote].script))) ++remote;
            if (local == owners.size() && remote == mirrors().size()) break;
            bool use_local = remote == mirrors().size() || (local < owners.size()
                && before(owners[local]->m_document->id(), owners[local]->m_script, mirrors()[remote].document, mirrors()[remote].script));
            if (use_local) {
                NonnullRefPtr owner { *owners[local++] };
                auto decision = TRY(owner->m_rules.evaluate(url, type, owner->m_scope, budget, replacement_budget));
                if (!decision.has_value()) continue;
                if (owner->on_event) owner->on_event(decision->event);
                if (decision->cancel || decision->redirect.has_value()) return decision;
            } else {
                auto const& mirror = mirrors()[remote++];
                auto decision = TRY(mirror.rules.evaluate(url, type, mirror.scope, budget, replacement_budget));
                if (!decision.has_value()) continue;
                page.client().derpbird_web_request_event(mirror.document, mirror.script, mirror.revision, JsonValue { decision->event });
                if (decision->cancel || decision->redirect.has_value()) return decision;
            }
        }
        return Optional<Rules::Decision> {};
    }
private:
    struct Mirror {
        GC::Weak<Page> page;
        HTML::CrossProcessId document;
        String script;
        u64 revision;
        Rules rules;
        DerpBirdScriptURLPolicy scope;
    };
    static WEB_API Vector<Mirror>& mirrors();
    static bool before(HTML::CrossProcessId a, String const& script_a, HTML::CrossProcessId b, String const& script_b) {
        if (a.namespace_id != b.namespace_id) return a.namespace_id < b.namespace_id;
        if (a.local_id != b.local_id) return a.local_id < b.local_id;
        return script_a.bytes_as_string_view() < script_b.bytes_as_string_view();
    }
    static WEB_API HashMap<HTML::CrossProcessId, HashMap<String, WeakPtr<DerpBirdWebRequestRegistration>>>& local_index();
    static RefPtr<DerpBirdWebRequestRegistration> local_owner(HTML::CrossProcessId document, String const& script) {
        auto by_document = local_index().find(document);
        if (by_document == local_index().end()) return {};
        auto by_script = by_document->value.find(script);
        if (by_script == by_document->value.end()) return {};
        auto owner = by_script->value.strong_ref();
        return owner && owner->active() ? owner : RefPtr<DerpBirdWebRequestRegistration> {};
    }
    void publish(bool removed) {
        if (removed && !m_published) return;
        if (!removed && !m_published && m_rules.is_empty()) return;
        if (!m_document->page().client().is_connection_open()) return;
        m_published = !removed;
        if (!removed) m_publications.set(revision, m_serialized_rules);
        m_document->page().client().publish_derpbird_web_requests(m_document->id(), m_script, m_configuration, revision, removed ? JsonValue { JsonArray {} } : m_serialized_rules, removed);
    }
    bool same_tab(Page const& page) const {
        auto const& owner = m_document->page();
        return owner.has_top_level_traversable() && page.has_top_level_traversable()
            && owner.top_level_traversable()->id() == page.top_level_traversable()->id();
    }
    DerpBirdWebRequestRegistration(DOM::Document& document, String script, String configuration, DerpBirdScriptURLPolicy scope)
        : m_document(document), m_script(move(script)), m_configuration(move(configuration)), m_scope(move(scope)) { }
    static WEB_API Vector<WeakPtr<DerpBirdWebRequestRegistration>>& registrations();
    GC::Root<DOM::Document> m_document;
    String m_script;
    String m_configuration;
    JsonValue m_serialized_rules;
    JsonValue m_committed_rules { JsonArray {} };
    u64 m_committed_revision { 0 };
    HashMap<u64, JsonValue> m_publications;
    DerpBirdScriptURLPolicy m_scope;
    Rules m_rules;
    bool m_closed { false };
    bool m_published { false };
};
}
