#include <AK/JsonObject.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebUI/DerpBirdUI.h>

namespace WebView {

void DerpBirdUI::register_interfaces()
{
    if (auto* page = client().page(page_id()))
        m_document_id = page->traversable().active_document().id();
    register_interface("derpbird.request"sv,
        [this](auto const& data) { request(data); });
}

bool DerpBirdUI::is_current() const
{
    if (!has_client() || !m_document_id.has_value()) return false;
    auto* page = client().page(page_id());
    if (!page || !page->is_open() || page->web_ui().ptr() != this || !page->displays_tab()) return false;
    if (page->traversable().active_document().id() != *m_document_id) return false;
    auto& view = page->view();
    return &view.page() == page && view.on_derpbird_request && view.url().serialize() == "about:derpbird"sv;
}

void DerpBirdUI::request(JsonValue const& data)
{
    if (!data.is_object() || !is_current()) return;
    auto const& object = data.as_object();
    auto request_id = object.get_integer<u64>("requestId"sv);
    auto payload = object.get("payload"sv);
    if (!request_id.has_value() || *request_id == 0 || *request_id > 9007199254740991ULL || !payload.has_value() || !payload->is_object())
        return;

    // Native admission is independent of the renderer's pending-request bound.
    // Duplicate IDs never replace a completion that is already in flight.
    if (m_pending.contains(*request_id)) return;
    if (m_pending.size() >= 128) {
        JsonObject response;
        response.set("requestId"sv, *request_id);
        response.set("ok"sv, false);
        response.set("error"sv, "Too many pending browser requests"sv);
        async_send_message("derpbird.response"sv, move(response));
        return;
    }
    // Match the downloads WebUI: opening a local file requires a user gesture.
    auto command = payload->as_object().get_object("command"sv);
    if (command.has_value() && command->get_string("type"sv) == "download.action"sv) {
        auto action = command->get_string("action"sv);
        if (action.has_value() && action->is_one_of("open"sv, "reveal"sv) && !client().page(page_id())->consume_transient_user_activation()) {
            JsonObject response;
            response.set("requestId"sv, *request_id);
            response.set("ok"sv, false);
            response.set("error"sv, "Click the file action to open it"sv);
            async_send_message("derpbird.response"sv, move(response));
            return;
        }
    }
    m_pending.set(*request_id);
    // Retain this channel until completion, but never deliver into a replacement
    // document, even a navigation back to the same URL in the same view.
    client().page(page_id())->view().on_derpbird_request(*payload,
        [self = NonnullRefPtr { *this }, id = *request_id](ErrorOr<JsonValue> result) {
            if (!self->m_pending.remove(id) || !self->is_current()) return;
            JsonObject response;
            response.set("requestId"sv, id);
            if (result.is_error()) {
                response.set("ok"sv, false);
                response.set("error"sv, MUST(String::formatted("{}", result.error())));
            } else {
                response.set("ok"sv, true);
                response.set("value"sv, result.release_value());
            }
            self->async_send_message("derpbird.response"sv, move(response));
        });
}

} // namespace WebView
