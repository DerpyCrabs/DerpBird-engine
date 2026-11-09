#pragma once

#include <AK/HashMap.h>
#include <LibWeb/Page/DerpBirdValueProtocol.h>
#include <LibWebView/ViewImplementation.h>

namespace Ladybird {

// Owned by a browsing session: private and regular windows never share entries.
// The native view identity survives document and WebContent-process replacement.
class DerpBirdTabData {
public:
    using Protocol = Web::DerpBirdValueProtocol;
    ErrorOr<ByteBuffer> dispatch(WebView::ViewImplementation& view, String const& script, Protocol::Request const& request)
    {
        m_tabs.remove_all_matching([](auto const&, auto const& tab) { return !tab.view; });
        auto found = m_tabs.find(view.view_id());
        if (found == m_tabs.end()) {
            if (m_tabs.size() >= 1024) return Error::from_string_literal("Userscript tab-data limit reached");
            TRY(m_tabs.try_set(view.view_id(), Tab { view.make_weak_ptr(), {} }));
            found = m_tabs.find(view.view_id());
        }
        auto& tab = found->value;
        if (!tab.values.contains(script)) {
            if (tab.values.size() >= 256) return Error::from_string_literal("Userscript tab scope limit reached");
            TRY(tab.values.try_set(script, Optional<ByteBuffer> {}));
        }
        bool save = request.method == Protocol::Method::TabSave || request.method == Protocol::Method::TabSaveLegacy;
        bool all = request.method == Protocol::Method::TabsGet || request.method == Protocol::Method::TabsGetLegacy;
        if (save) {
            auto const& value = *request.updates[0].value;
            size_t total = 0, scope = 0;
            for (auto const& item : m_tabs) {
                for (auto const& entry : item.value.values) {
                    if (item.key == view.view_id() && entry.key == script) continue;
                    if (entry.key == script) scope += 32;
                    if (!entry.value.has_value()) continue;
                    total += entry.value->size();
                    if (entry.key == script) scope += entry.value->size();
                }
            }
            if (total + value.size() > 64 * 1024 * 1024 || scope + value.size() + 32 > Protocol::max_values)
                return Error::from_string_literal("Userscript tab data exceeds quota");
            TRY(tab.values.try_set(script, TRY(ByteBuffer::copy(value.bytes()))));
            return Protocol::encode(Protocol::Response { Protocol::Kind::Revision, {}, {}, {}, 0 });
        }
        Protocol::Response response { all ? Protocol::Kind::TabSnapshot : Protocol::Kind::Value, {}, {}, {}, 0 };
        if (all) {
            for (auto const& item : m_tabs) {
                auto value = item.value.values.find(script);
                if (value == item.value.values.end()) continue;
                TRY(response.entries.try_append({ String::number(item.key), value->value.has_value() ? Optional<ByteBuffer> { TRY(ByteBuffer::copy(value->value->bytes())) } : Optional<ByteBuffer> {} }));
            }
        } else if (auto value = tab.values.find(script); value != tab.values.end() && value->value.has_value()) {
            response.value = TRY(ByteBuffer::copy(value->value->bytes()));
        }
        return Protocol::encode(response);
    }
private:
    struct Tab {
        WeakPtr<WebView::ViewImplementation> view;
        HashMap<String, Optional<ByteBuffer>> values;
    };
    HashMap<u64, Tab> m_tabs;
};
}
