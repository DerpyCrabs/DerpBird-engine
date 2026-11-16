#include <UI/Qt/DerpBirdDevToolsSession.h>
#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <Hub/Application.h>
#include <Hub/ConnectionFromUI.h>
#include <Hub/DerpBird.h>
#include <Hub/DerpBirdProxy.h>
#include <Hub/Proxies.h>
#include <Hub/View.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/RequestClient.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/Profile.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebUI.h>
#include <QTimer>
#include <UI/Qt/DerpBirdSourceTask.h>
#include <UI/Qt/DerpBirdValueHost.h>

namespace Hub {

static DerpBird* s_derpbird;

static View* find_view(u64 id)
{
    auto view = WebView::ViewImplementation::find_view_by_id(id);
    return view.has_value() ? static_cast<View*>(&*view) : nullptr;
}

static String document_token(Web::HTML::CrossProcessId id) { return MUST(String::formatted("{}", id)); }

static RefPtr<WebView::CanonicalDocument> current_document(View& view, String const& token)
{
    if (!view.has_traversable() || view.crash_overlay_active())
        return {};
    RefPtr<WebView::CanonicalDocument> result;
    view.traversable().for_each_in_inclusive_subtree([&](WebView::CanonicalNavigable& navigable) {
        if (!navigable.active_session_history_entry() || !navigable.active_session_history_entry()->document_state->document)
            return IterationDecision::Continue;
        auto& document = navigable.active_document();
        auto page = document.host();
        if (document_token(document.id()) != token || !page || !page->is_open()
            || document.browsing_context().has_been_discarded()
            || document.browsing_context().active_document().ptr() != &document
            || page->hosted_document_with_id(document.id()).ptr() != &document)
            return IterationDecision::Continue;
        result = document;
        return IterationDecision::Break;
    });
    return result;
}

class CredentialTask final : public QObject {
public:
    using Completion = DerpBird::Completion;
    CredentialTask(QObject* parent, View& view, RefPtr<WebView::CanonicalDocument> document, Completion completion)
        : QObject(parent)
        , m_view(view.make_weak_ptr())
        , m_document(move(document))
        , m_page(m_document->host())
        , m_origin(m_document->origin().serialize())
        , m_completion(move(completion))
    {
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this] { finish(Error::from_string_literal("Credential request timed out")); });
    }
    ~CredentialTask() override
    {
        if (m_owns_callback)
            m_page->on_derpbird_credential_result = nullptr;
        if (m_completion)
            m_completion(Error::from_string_literal("Credential request cancelled"));
    }
    void start(String const& operation, JsonObject const& payload)
    {
        if (!valid() || m_page->on_derpbird_credential_result) {
            finish(Error::from_string_literal("Credential document unavailable or busy"));
            return;
        }
        static u64 next_id = 1;
        m_id = next_id++;
        m_page->on_derpbird_credential_result = [self = QPointer(this)](auto document_id, u64 id, JsonValue value, Optional<String> error) {
            if (!self || document_id != self->m_document->id() || id != self->m_id)
                return;
            QTimer::singleShot(0, self, [self, value = move(value), error = move(error)]() mutable {
                if (!self || !self->m_completion)
                    return;
                if (!self->valid()) {
                    self->finish(Error::from_string_literal("Credential document changed"));
                    return;
                }
                if (error.has_value()) {
                    self->finish(Error::from_string_view(error->bytes_as_string_view()));
                    return;
                }
                if (value.is_object()) {
                    value.as_object().set("document"sv, document_token(self->m_document->id()));
                    value.as_object().set("origin"sv, self->m_origin);
                }
                self->finish(move(value));
            });
        };
        m_owns_callback = true;
        m_timer.start(25000);
        if (operation == "credential.select"sv) {
            m_page->async_request_derpbird_credential_fields(m_document->id(), m_id);
            return;
        }
        auto ticket_string = payload.get_string("ticket"sv);
        auto ticket = ticket_string.has_value() ? ticket_string->to_number<u64>() : payload.get_integer<u64>("ticket"sv);
        if (!ticket.has_value() || !*ticket || payload.get_string("origin"sv) != m_origin) {
            finish(Error::from_string_literal("Invalid credential authority"));
            return;
        }
        if (operation == "credential.capture"sv) {
            m_page->async_capture_derpbird_credential_fields(m_document->id(), m_id, *ticket, m_origin);
        } else {
            auto username = payload.get_string("username"sv), password = payload.get_string("password"sv);
            if (!username.has_value() || !password.has_value() || username->bytes().size() > 1024 || password->bytes().size() > 65536) {
                finish(Error::from_string_literal("Invalid credential fields"));
                return;
            }
            m_page->async_fill_derpbird_credential_fields(m_document->id(), m_id, *ticket, m_origin, *username, *password);
        }
    }

private:
    bool valid() const
    {
        return m_view && m_page && m_page->is_open() && !m_view->crash_overlay_active()
            && m_document->host() == m_page && !m_document->browsing_context().has_been_discarded()
            && m_document->browsing_context().active_document().ptr() == m_document.ptr()
            && m_page->hosted_document_with_id(m_document->id()) == m_document
            && m_document->origin().serialize() == m_origin;
    }
    void finish(ErrorOr<JsonValue> result)
    {
        if (!m_completion)
            return;
        m_timer.stop();
        if (m_owns_callback) {
            m_page->on_derpbird_credential_result = nullptr;
            m_owns_callback = false;
        }
        auto completion = move(m_completion);
        deleteLater();
        completion(move(result));
    }
    WeakPtr<WebView::ViewImplementation> m_view;
    RefPtr<WebView::CanonicalDocument> m_document;
    RefPtr<WebView::WebContentPage> m_page;
    String m_origin;
    Completion m_completion;
    QTimer m_timer;
    u64 m_id { 0 };
    bool m_owns_callback { false };
};

DerpBird::DerpBird(ConnectionFromUI& connection)
    : m_connection(connection)
    , m_values(make<Ladybird::DerpBirdValueHost>(Application::the()))
{
    s_derpbird = this;
    m_values->tabs.on_open_async = [this](auto& page, auto const& url, JsonObject const& options, auto completion) {
        auto payload = options;
        payload.set("url"sv, url.serialize());
        request_ui(page.view().view_id(), "tab.open"_string, JsonValue { move(payload) }, [this, completion = move(completion)](ErrorOr<JsonValue> result) mutable {
            if (result.is_error()) {
                completion(result.release_error());
                return;
            }
            auto handle = result.value().is_object() ? result.value().as_object().get_string("handle"sv) : Optional<String> {};
            if (!handle.has_value() || handle->is_empty() || handle->bytes().size() > 128 || m_tabs.size() >= 256) {
                completion(Error::from_string_literal("Invalid userscript tab handle"));
                return;
            }
            auto key = QString::fromUtf8(handle->bytes_as_string_view().characters_without_null_termination(), handle->bytes().size());
            if (m_tabs.contains(key)) {
                completion(Error::from_string_literal("Duplicate userscript tab handle"));
                return;
            }
            auto* lifetime = new QObject(this);
            m_tabs.insert(key, lifetime);
            completion(Ladybird::DerpBirdScriptTabs::Handle { lifetime, [self = QPointer(this), handle = *handle] {
                                                                 if (!self)
                                                                     return;
                                                                 JsonObject payload;
                                                                 payload.set("handle"sv, handle);
                                                                 self->request_ui(0, "tab.close"_string, JsonValue { move(payload) }, [](auto) { });
                                                             } });
        });
    };
    m_values->window_action_async = [this](auto& page, bool close, auto completion) {
        request_ui(page.view().view_id(), close ? "window.close"_string : "window.focus"_string, JsonValue {}, [completion = move(completion)](auto result) mutable {
            if (result.is_error())
                completion(result.release_error());
            else
                completion({});
        });
    };
    m_values->clipboard_write_async = [this](auto& page, String const& type, ByteBuffer bytes, auto completion) {
        auto text = String::from_utf8(StringView { bytes.bytes() });
        if (text.is_error()) {
            completion(text.release_error());
            return;
        }
        JsonObject payload;
        payload.set("type"sv, type);
        payload.set("text"sv, text.release_value());
        request_ui(page.view().view_id(), "clipboard.write"_string, JsonValue { move(payload) }, [completion = move(completion)](auto result) mutable {
            if (result.is_error())
                completion(result.release_error());
            else
                completion({});
        });
    };
    m_values->notifications.highlight = [this](auto& page) { m_connection.async_derpbird_ui_event(page.view().view_id(), "tab.highlight"_string, JsonValue {}); };
    m_values->commands.on_change = [] {
        WebView::ViewImplementation::for_each_view([](auto& view) { static_cast<View&>(view).push_state_to_ui(); return IterationDecision::Continue; });
    };
}

DerpBird::~DerpBird()
{
    for (auto session : m_devtools) if (session) delete session;
    m_devtools.clear();
    s_derpbird = nullptr;
    WebView::ViewImplementation::for_each_view([](auto& view) {
        view.on_derpbird_request = nullptr;
        view.on_derpbird_credential_focus = nullptr;
        view.on_derpbird_credential_submission = nullptr;
        view.on_derpbird_filter_selector = nullptr;
        return IterationDecision::Continue;
    });
    auto tasks = move(m_tasks);
    for (auto const& entry : tasks)
        if (entry.value)
            delete entry.value.data();
}
DerpBird* DerpBird::the() { return s_derpbird; }

void DerpBird::shutdown(Function<void()> completion)
{
    m_values->shutdown(move(completion));
}

void DerpBird::request_ui(u64 view_id, String operation, JsonValue payload, Completion completion)
{
    if (m_ui_pending.size() >= 256) {
        completion(Error::from_string_literal("Too many pending UI requests"));
        return;
    }
    if (view_id) {
        auto* view = find_view(view_id);
        if (!view || !view->has_traversable()) {
            completion(Error::from_string_literal("Userscript source tab closed"));
            return;
        }
        if (!payload.is_object())
            payload = JsonObject {};
        payload.as_object().set("sourceDocument"sv, document_token(view->traversable().active_document().id()));
    }
    auto id = m_next_ui_request++;
    m_ui_pending.set(id, move(completion));
    m_connection.async_derpbird_ui_request(id, view_id, move(operation), move(payload));
    QTimer::singleShot(25000, this, [this, id] {
        if (auto completion = m_ui_pending.take(id); completion.has_value())
            (*completion)(Error::from_string_literal("UI request timed out"));
    });
}

void DerpBird::ui_response(u64 id, JsonValue result, Optional<String> error)
{
    auto completion = m_ui_pending.take(id);
    if (!completion.has_value())
        return;
    if (error.has_value())
        (*completion)(Error::from_string_view(error->bytes_as_string_view()));
    else
        (*completion)(move(result));
}
void DerpBird::shell_response(u64 id, JsonValue result, Optional<String> error)
{
    auto completion = m_shell_pending.take(id);
    if (!completion.has_value())
        return;
    if (error.has_value())
        (*completion)(Error::from_string_view(error->bytes_as_string_view()));
    else
        (*completion)(move(result));
}

void DerpBird::release_devtools(u64 view_id)
{
    if (auto session = m_devtools.take(view_id)) delete session;
}

void DerpBird::install_view_callbacks(View& view)
{
    auto id = view.view_id();
    view.on_inspect_element_request = [this, id](Web::UniqueNodeID node) {
        JsonObject event; event.set("node"sv, node.value());
        m_connection.async_derpbird_ui_event(id, "devtools.inspect"_string, AK::move(event));
    };
    view.on_derpbird_request = [this, id](JsonValue const& payload, Completion completion) {
        if (m_shell_pending.size() >= 256) {
            completion(Error::from_string_literal("Too many pending shell requests"));
            return;
        }
        auto request_id = m_next_shell_request++;
        // DerpBirdUI owns the completion and rechecks its channel/document at delivery.
        m_shell_pending.set(request_id, move(completion));
        if (auto* view = find_view(id))
            view->push_state_to_ui();
        m_connection.async_derpbird_shell_request(id, request_id, payload);
        QTimer::singleShot(30000, this, [this, request_id] {
            if (auto completion = m_shell_pending.take(request_id); completion.has_value())
                (*completion)(Error::from_string_literal("Shell request timed out"));
        });
    };
    view.on_derpbird_credential_focus = [this, id](auto& document, bool available, Gfx::IntRect bounds) {
        JsonObject event;
        event.set("document"sv, document_token(document.id()));
        event.set("origin"sv, document.origin().serialize());
        event.set("url"sv, document.creation_url().serialize());
        event.set("available"sv, available);
        JsonObject rect;
        rect.set("x"sv, bounds.x());
        rect.set("y"sv, bounds.y());
        rect.set("width"sv, bounds.width());
        rect.set("height"sv, bounds.height());
        event.set("bounds"sv, move(rect));
        m_connection.async_derpbird_ui_event(id, "credential.focus"_string, JsonValue { move(event) });
    };
    view.on_derpbird_credential_submission = [this, id](auto& document, String username, String password) {
        JsonObject event;
        event.set("document"sv, document_token(document.id()));
        event.set("origin"sv, document.origin().serialize());
        event.set("url"sv, document.creation_url().serialize());
        event.set("username"sv, move(username));
        event.set("password"sv, move(password));
        m_connection.async_derpbird_ui_event(id, "credential.submission"_string, JsonValue { move(event) });
    };
    view.on_derpbird_filter_selector = [this, id](String url, String selector) {
        JsonObject event;
        event.set("url"sv, move(url));
        event.set("selector"sv, move(selector));
        m_connection.async_derpbird_ui_event(id, "filter-selector"_string, JsonValue { move(event) });
    };
}

String DerpBird::shell_channel(View const& view) const
{
    m_channels.remove_all_matching([](auto const& id, auto const& entry) { return !entry.channel || !find_view(id); });
    auto channel = view.has_traversable() && view.traversable().display_page() ? view.page().web_ui() : RefPtr<WebView::WebUI> {};
    if (!channel || channel->host() != "derpbird"sv)
        return {};
    auto entry = m_channels.find(view.view_id());
    if (entry == m_channels.end() || entry->value.channel.ptr() != channel.ptr()) {
        m_channels.set(view.view_id(), Channel { channel, String::number(m_next_channel++) });
    }
    return m_channels.get(view.view_id())->token;
}

JsonValue DerpBird::view_state(View const& view) const
{
    JsonObject result;
    bool open = view.has_traversable() && view.traversable().display_page() && view.traversable().display_page()->is_open();
    result.set("open"sv, open);
    result.set("shellChannel"sv, shell_channel(view));
    result.set("requestLogging"sv, view.request_logging_enabled());
    result.set("document"sv, open ? document_token(view.traversable().active_document().id()) : String {});
    if (auto context = view.derpbird_context_document(); context.has_value())
        result.set("contextDocument"sv, document_token(*context));
    JsonArray log;
    for (auto const& entry : view.blocking_log()) {
        JsonObject value;
        value.set("sequence"sv, entry.sequence);
        value.set("url"sv, entry.url);
        value.set("kind"sv, entry.kind);
        value.set("rule"sv, entry.rule);
        log.must_append(move(value));
    }
    result.set("log"sv, move(log));
    if (open)
        result.set("scriptCommands"sv, m_values->list_commands(view.page(), view.derpbird_context_document()));
    else
        result.set("scriptCommands"sv, JsonArray {});
    return JsonValue { move(result) };
}

void DerpBird::finish(u64 id, ErrorOr<JsonValue> result)
{
    m_tasks.remove(id);
    if (result.is_error())
        m_connection.async_derpbird_result(id, {}, MUST(String::formatted("{}", result.error())));
    else
        m_connection.async_derpbird_result(id, result.release_value(), {});
}

void DerpBird::command(u64 request_id, u64 view_id, String command, JsonValue payload)
{
    if (!request_id || m_tasks.contains(request_id))
        return;
    if (m_tasks.size() >= 128) {
        finish(request_id, Error::from_string_literal("Too many pending browser tasks"));
        return;
    }
    if (payload.is_null())
        payload = JsonObject {};
    dispatch(request_id, view_id, command, payload);
}

void DerpBird::dispatch(u64 request_id, u64 view_id, String const& command, JsonValue const& payload)
{
    auto fail = [&](StringView message) { finish(request_id, Error::from_string_view(message)); };
    auto done = [&] { finish(request_id, JsonValue {}); };
    if (command == "profile"sv) {
        auto const& profile = Application::profile();
        auto const& paths = profile.paths();
        JsonObject result;
        result.set("config"sv, paths.config.view());
        result.set("data"sv, paths.data.view());
        result.set("identity"sv, paths.identity.view());
        result.set("temporary"sv, profile.is_temporary());
        finish(request_id, JsonValue { move(result) });
        return;
    }
    if (command == "styles.set"sv || command == "scripts.set"sv) {
        auto result = command == "styles.set"sv ? Application::the().set_derpbird_user_style_sections(payload) : Application::the().set_derpbird_user_scripts(payload);
        if (result.is_error())
            finish(request_id, result.release_error());
        else
            done();
        return;
    }
    if (command == "app.shutdown"sv) {
        shutdown([this, request_id] { finish(request_id, JsonValue {}); });
        return;
    }
    if (command == "proxy.state"sv) {
        auto result = Ladybird::DerpBirdProxy::state();
        if (result.is_error())
            finish(request_id, result.release_error());
        else
            finish(request_id, JsonValue { result.release_value() });
        return;
    }
    if (!payload.is_object()) {
        fail("Expected browser command object"sv);
        return;
    }
    auto const& object = payload.as_object();
    if (command == "proxy.replace"sv) {
        finish(request_id, Ladybird::DerpBirdProxy::replace(object));
        return;
    }
    if (command == "request.cancel"sv) {
        if (auto id = object.get_integer<u64>("requestId"sv); id.has_value()) {
            if (auto task = m_tasks.take(*id); task.has_value() && *task)
                delete task->data();
        }
        done();
        return;
    }
    if (command == "tab.closed"sv) {
        if (auto handle = object.get_string("handle"sv); handle.has_value()) {
            auto key = QString::fromUtf8(handle->bytes_as_string_view().characters_without_null_termination(), handle->bytes().size());
            if (auto lifetime = m_tabs.take(key))
                delete lifetime.data();
        }
        done();
        return;
    }
    if (command == "source.fetch"sv || command == "network.fetch"sv) {
        auto address = object.get_string("url"sv);
        auto url = address.has_value() ? URL::Parser::basic_parse(*address) : Optional<URL::URL> {};
        auto limit = object.get_integer<size_t>("limit"sv).value_or(2 * MiB);
        if (!url.has_value() || !Ladybird::DerpBirdSourceTask::valid(*url) || !limit || limit > 16 * MiB) {
            fail("Invalid source download request"sv);
            return;
        }
        auto* task = new Ladybird::DerpBirdSourceTask(this, url.release_value(), limit, command == "network.fetch"sv, [self = QPointer(this), request_id](auto result) { if (self) self->finish(request_id, move(result)); });
        m_tasks.set(request_id, task);
        task->start();
        return;
    }
    auto* view = find_view(view_id);
    if (!view || !view->has_traversable()) {
        fail("Browser view is unavailable"sv);
        return;
    }
    if (command == "proxy.site"sv) {
        auto result = [&]() -> ErrorOr<JsonValue> {
            auto requested = object.get_string("url"sv);
            auto const& url = view->url();
            if (!requested.has_value() || url.serialize() != *requested)
                return Error::from_string_literal("The active page changed; reopen the proxy switcher");
            if (!url.scheme().is_one_of("http"sv, "https"sv)) return Error::from_string_literal("Proxy rules require an HTTP or HTTPS page");
            auto state = TRY(Ladybird::DerpBirdProxy::state());
            auto configuration = HTTP::ProxyConfiguration::from_environment();
            TRY(configuration.set_omega(state.get_object("options"sv).value(), state.get_string("active"sv).value()));
            JsonObject result;
            result.set("url"sv, url.serialize()); result.set("host"sv, url.serialized_host());
            result.set("profile"sv, configuration.profile_for(url)); result.set("proxied"sv, configuration.proxy_for(url).has_value());
            JsonArray failures;
            for (auto const& [host, failure] : view->proxy_failures()) {
                auto failed_url = URL::Parser::basic_parse(failure.url);
                if (!failed_url.has_value()) continue;
                JsonObject item;
                item.set("host"sv, host); item.set("count"sv, failure.count); item.set("error"sv, failure.error);
                item.set("profile"sv, configuration.profile_for(*failed_url)); item.set("proxied"sv, configuration.proxy_for(*failed_url).has_value());
                TRY(failures.append(move(item)));
            }
            result.set("failures"sv, move(failures));
            return JsonValue(move(result));
        }();
        finish(request_id, move(result)); return;
    }
    if (command == "devtools"sv) {
        auto type = object.get_string("type"sv);
        if (type == "devtools.close"sv) { if (auto session = m_devtools.take(view_id)) delete session; done(); return; }
        auto& session = m_devtools[view_id];
        if (!session) {
            if (view->on_received_dom_tree || view->on_console_message || view->on_network_request_started) { fail("This tab already has a debugger"sv); return; }
            session = new Ladybird::DerpBirdDevToolsSession(*view, this, [this, view_id](JsonValue packet) {
                m_connection.async_derpbird_ui_event(view_id, "devtools"_string, AK::move(packet));
            });
        }
        session->request(payload, [this, request_id](auto result) { finish(request_id, AK::move(result)); });
        return;
    }
    if (command.bytes_as_string_view().starts_with("credential."sv)) {
        credential_command(request_id, *view, command, payload);
        return;
    }
    if (command == "logging.set"sv) {
        view->set_request_logging(object.get_bool("enabled"sv).value_or(false));
        view->push_state_to_ui();
        done();
        return;
    }
    if (command == "logging.clear"sv) {
        view->clear_blocking_log();
        view->push_state_to_ui();
        done();
        return;
    }
    if (command == "transparent.set"sv) {
        view->set_transparent_background(object.get_bool("enabled"sv).value_or(false));
        done();
        return;
    }
    if (command == "scripts.list"sv || command == "scripts.invoke"sv) {
        Optional<Web::HTML::CrossProcessId> clicked;
        if (auto token = object.get_string("document"sv); token.has_value() && !token->is_empty()) {
            auto document = current_document(*view, *token);
            if (!document) {
                fail("Context document changed"sv);
                return;
            }
            clicked = document->id();
        }
        if (command == "scripts.list"sv) {
            finish(request_id, JsonValue { m_values->list_commands(view->page(), clicked) });
            return;
        }
        auto id = object.get_string("id"sv);
        auto event = object.get_object("event"sv);
        if (!id.has_value()) {
            fail("Missing userscript command"sv);
            return;
        }
        auto result = m_values->invoke_command(view->page(), *id, event.has_value() ? *event : JsonObject {}, clicked);
        if (result.is_error())
            finish(request_id, result.release_error());
        else
            done();
        return;
    }
    if (command == "picker.start"sv) {
        view->start_node_picker([this, view_id](auto event) { JsonObject data; data.set("type"sv, to_underlying(event.type)); if (event.node_id.has_value()) data.set("node"sv, event.node_id->value()); m_connection.async_derpbird_ui_event(view_id, "picker"_string, JsonValue { move(data) }); });
        view->push_state_to_ui();
        done();
        return;
    }
    if (command == "picker.stop"sv) {
        view->stop_node_picker();
        view->push_state_to_ui();
        done();
        return;
    }
    if (command == "picker.clear"sv) {
        view->clear_node_picker();
        done();
        return;
    }
    if (command == "picker.remove"sv || command == "picker.highlight"sv || command == "filter.selector"sv) {
        auto node = object.get_integer<i64>("node"sv);
        if (!node.has_value()) {
            fail("Missing picker node"sv);
            return;
        }
        if (command == "picker.remove"sv)
            view->remove_dom_node(Web::UniqueNodeID { *node });
        else if (command == "picker.highlight"sv)
            view->highlight_dom_node(Web::UniqueNodeID { *node }, {});
        else
            view->page().async_derpbird_filter_selector(Web::UniqueNodeID { *node });
        done();
        return;
    }
    auto token = object.get_string("document"sv);
    if (!token.has_value() || !current_document(*view, *token) || document_token(view->traversable().active_document().id()) != *token) {
        fail("Browser document changed"sv);
        return;
    }
    if (command == "page.info"sv) {
        auto type = object.get_integer<u32>("type"sv);
        if (!type.has_value() || *type != to_underlying(WebView::PageInfoType::DerpBirdStyleStatus)) {
            fail("Unsupported internal page info"sv);
            return;
        }
        view->request_internal_page_info(WebView::PageInfoType::DerpBirdStyleStatus)->when_resolved([self = QPointer(this), request_id](auto const& result) { if (self) self->finish(request_id, JsonValue { result }); }).when_rejected([self = QPointer(this), request_id](auto& error) { if (self) self->finish(request_id, move(error)); });
        return;
    }
    if (command == "shell.message"sv || command == "shell.javascript"sv) {
        auto channel = view->page().web_ui();
        if (view->url().serialize() != "about:derpbird"sv || !channel || channel->host() != "derpbird"sv || object.get_string("channel"sv) != shell_channel(*view)) {
            fail("Trusted browser shell required"sv);
            return;
        }
        if (command == "shell.javascript"sv) {
            auto source = object.get_string("source"sv);
            if (!source.has_value()) {
                fail("Missing shell source"sv);
                return;
            }
            view->run_javascript(*source);
        } else {
            auto name = object.get_string("name"sv);
            auto data = object.get("data"sv);
            if (!name.has_value() || !data.has_value()) {
                fail("Invalid shell message"sv);
                return;
            }
            channel->async_send_message(*name, *data);
        }
        done();
        return;
    }
    fail("Unknown browser command"sv);
}

void DerpBird::credential_command(u64 request_id, View& view, String const& command, JsonValue const& payload)
{
    auto const& object = payload.as_object();
    RefPtr<WebView::CanonicalDocument> document;
    if (command == "credential.select"sv) {
        auto focused = view.traversable().focused_navigable_id();
        view.traversable().for_each_in_inclusive_subtree([&](auto& navigable) { if (focused.has_value() && navigable.id() == *focused) document = navigable.active_document(); return IterationDecision::Continue; });
        if (!document)
            document = view.traversable().active_document();
        document = current_document(view, document_token(document->id()));
    } else if (auto token = object.get_string("document"sv); token.has_value())
        document = current_document(view, *token);
    bool valid = document && !document->origin().is_opaque() && document->creation_url().scheme().is_one_of("http"sv, "https"sv);
    if (command == "credential.current"sv) {
        finish(request_id, JsonValue { valid && (!object.has("origin"sv) || object.get_string("origin"sv) == document->origin().serialize()) });
        return;
    }
    if (!valid) {
        finish(request_id, Error::from_string_literal("Credential document changed"));
        return;
    }
    if (command == "credential.cancel"sv) {
        auto text = object.get_string("ticket"sv);
        auto ticket = text.has_value() ? text->to_number<u64>() : object.get_integer<u64>("ticket"sv);
        if (ticket.has_value())
            document->host()->async_cancel_derpbird_credential_fields(document->id(), *ticket);
        finish(request_id, JsonValue {});
        return;
    }
    if (!command.is_one_of("credential.select"sv, "credential.fill"sv, "credential.capture"sv)) {
        finish(request_id, Error::from_string_literal("Unknown credential operation"));
        return;
    }
    auto* task = new CredentialTask(this, view, move(document), [self = QPointer(this), request_id](auto result) { if (self) self->finish(request_id, move(result)); });
    m_tasks.set(request_id, task);
    task->start(command, object);
}

}
