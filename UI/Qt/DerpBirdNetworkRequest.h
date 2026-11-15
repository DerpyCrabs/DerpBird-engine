#pragma once

#include <AK/JsonObject.h>
#include <AK/StringBuilder.h>
#include <LibHTTP/Header.h>
#include <LibHTTP/Cache/CacheMode.h>
#include <LibHTTP/Cookie/IncludeCredentials.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/RequestClient.h>
#include <LibRequests/Request.h>
#include <LibURL/Parser.h>
#include <LibURL/Site.h>
#include <LibWeb/Page/DerpBirdValueProtocol.h>
#include <LibWebView/Application.h>
#include <QByteArray>
#include <QDeadlineTimer>
#include <QPointer>
#include <QTimer>

namespace Ladybird {

// Privileged userscript requests remain outside the page's fetch/CORS context.
// Each redirect and completion rechecks native document and script authority.
class DerpBirdNetworkRequest final : public QObject {
public:
    using Completion = Function<void(ErrorOr<ByteBuffer>)>;
    using Active = Function<bool()>;
    DerpBirdNetworkRequest(QObject* parent, Vector<String> connects, URL::URL origin, WebView::IsPrivate privacy, Active active, Completion completion)
        : QObject(parent), m_connects(move(connects)), m_origin(move(origin)), m_privacy(privacy), m_active(move(active)), m_completion(move(completion))
    {
        m_timeout.setSingleShot(true);
        connect(&m_timeout, &QTimer::timeout, this, [this] {
            if (m_deadline.hasExpired()) finish("timeout"sv);
            else arm_timeout();
        });
        m_liveness.setInterval(100);
        connect(&m_liveness, &QTimer::timeout, this, [this] { if (!m_active()) finish("abort"sv); });
    }
    ~DerpBirdNetworkRequest() override { m_done = true; if (m_request) m_request->stop(); }
    void cancel() { finish("abort"sv); }
    ErrorOr<void> start(ReadonlyBytes payload)
    {
        auto value = TRY(JsonValue::from_string(StringView { payload }));
        if (!value.is_object()) return Error::from_string_literal("Invalid userscript request");
        auto const& options = value.as_object();
        auto address = options.get_string("url"sv);
        auto url = address.has_value() ? URL::Parser::basic_parse(*address) : Optional<URL::URL> {};
        if (!url.has_value()) return Error::from_string_literal("Invalid userscript request URL");
        m_url = url.release_value();
        TRY(authorize_url());
        auto method = options.get_string("method"sv).value_or("GET"_string);
        auto verb = method.to_byte_string().to_uppercase();
        if (verb != "GET" && verb != "HEAD" && verb != "POST" && verb != "PUT" && verb != "PATCH" && verb != "DELETE" && verb != "OPTIONS")
            return Error::from_string_literal("Unsupported userscript HTTP method");
        m_method = move(verb);
        m_headers = HTTP::HeaderList::create();
        if (auto headers = options.get_object("headers"sv); headers.has_value()) {
            if (headers->size() > 128) return Error::from_string_literal("Too many userscript request headers");
            Optional<Error> error;
            size_t bytes = 0;
            headers->for_each_member([&](auto const& name, auto const& value) {
                if (error.has_value()) return;
                if (!value.is_string() || !HTTP::is_header_name(name) || !HTTP::is_header_value(value.as_string())) {
                    error = Error::from_string_literal("Invalid userscript request header"); return;
                }
                HTTP::Header header { name.to_byte_string(), value.as_string().to_byte_string() };
                bytes += header.name.length() + header.value.length();
                if (bytes > 65536 || HTTP::is_forbidden_request_header(header)) {
                    error = Error::from_string_literal("Forbidden or oversized userscript request header"); return;
                }
                m_headers->append(move(header));
            });
            if (error.has_value()) return error.release_value();
        }
        if (auto body = options.get_string("body"sv); body.has_value()) {
            auto encoded = body->bytes_as_string_view();
            auto decoded = QByteArray::fromBase64Encoding(QByteArray(encoded.characters_without_null_termination(), encoded.length()), QByteArray::AbortOnBase64DecodingErrors);
            if (!decoded) return Error::from_string_literal("Invalid userscript request body");
            m_upload = TRY(ByteBuffer::copy(ReadonlyBytes { reinterpret_cast<u8 const*>(decoded.decoded.data()), static_cast<size_t>(decoded.decoded.size()) }));
        }
        if ((m_method == "GET" || m_method == "HEAD") && !m_upload.is_empty())
            return Error::from_string_literal("GET and HEAD cannot have a body");
        m_stream = options.get_bool("stream"sv).value_or(false);
        m_anonymous = options.get_bool("anonymous"sv).value_or(false);
        auto timeout = options.get_integer<u32>("timeout"sv).value_or(0);
        if (timeout) {
            m_deadline.setRemainingTime(timeout, Qt::PreciseTimer);
            arm_timeout();
        }
        m_liveness.start();
        fetch();
        return {};
    }
    ErrorOr<void> read(Completion completion)
    {
        if (!m_stream || !m_headers_sent || !m_active() || m_read)
            return Error::from_string_literal("Response stream is unavailable or already being read");
        m_read = move(completion);
        if (m_done) { deliver_stream(); return {}; }
        m_request->resume_body_delivery_up_to(65536);
        return {};
    }
private:
    void arm_timeout()
    {
        // QTimer's interval is signed 32-bit; retain the full request deadline
        // instead of shortening large unsigned XHR timeouts.
        m_timeout.start(static_cast<int>(min(m_deadline.remainingTime(), static_cast<qint64>(NumericLimits<int>::max()))));
    }
    ErrorOr<void> authorize_url() const
    {
        if (!m_active() || (m_url.scheme() != "http"sv && m_url.scheme() != "https"sv)
            || !m_url.username().is_empty() || !m_url.password().is_empty())
            return Error::from_string_literal("Userscript network authority or URL denied");
        auto host = m_url.serialized_host();
        for (auto const& entry : m_connects) {
            auto rule = entry.bytes_as_string_view();
            if (rule == "self"sv) {
                if (host.equals_ignoring_ascii_case(m_origin.serialized_host())) return {};
                continue;
            }
            if (rule == "*"sv || host.equals_ignoring_ascii_case(rule)) return {};
            if (host.length() > rule.length() && host.ends_with(rule, CaseSensitivity::CaseInsensitive)
                && host[host.length() - rule.length() - 1] == '.') return {};
        }
        // A script may always request its own host. Cross-host requests require @connect.
        if (host.equals_ignoring_ascii_case(m_origin.serialized_host())) return {};
        return Error::from_string_literal("Userscript target is not allowed by @connect");
    }
    void fetch()
    {
        if (m_done) return;
        if (authorize_url().is_error()) { finish("error"sv); return; }
        auto client = WebView::Application::request_server_client(m_privacy);
        if (client.is_error()) { finish("error"sv); return; }
        m_body.clear();
        m_status = 0;
        auto same_site = URL::Site::obtain(m_origin.origin()).is_same_site(URL::Site::obtain(m_url.origin()));
        auto cookie_context = same_site ? HTTP::Cookie::SameSiteContext::same_site() : HTTP::Cookie::SameSiteContext {};
        m_request = client.value()->start_request(m_method, m_url, cookie_context, *m_headers, m_upload.bytes(), HTTP::CacheMode::NoStore,
            m_anonymous ? HTTP::Cookie::IncludeCredentials::No : HTTP::Cookie::IncludeCredentials::Yes);
        if (!m_request) { finish("error"sv); return; }
        if (m_stream) m_request->set_body_delivery_paused(true);
        auto serial = ++m_serial;
        auto active = [self = QPointer(this), serial] { return self && !self->m_done && self->m_serial == serial; };
        m_request->set_unbuffered_request_callbacks(
            [this, active](auto headers, Optional<u32> status, Optional<String> const& reason, Requests::CacheState) {
                if (!active()) return;
                if (status == 301u || status == 302u || status == 303u || status == 307u || status == 308u) {
                    auto location = headers->get("Location"sv);
                    auto target = location.has_value() ? URL::Parser::basic_parse(*location, m_url) : Optional<URL::URL> {};
                    if (!target.has_value() || ++m_redirects > 20) { finish("error"sv); return; }
                    if (target->serialized_host() != m_url.serialized_host() || target->scheme() != m_url.scheme() || target->port() != m_url.port())
                        m_headers->delete_("Authorization"sv);
                    if ((*status == 303 && m_method != "HEAD") || ((*status == 301 || *status == 302) && m_method == "POST")) {
                        m_method = "GET";
                        m_upload.clear();
                        m_headers->delete_("Content-Type"sv);
                    }
                    m_url = target.release_value();
                    ++m_serial;
                    m_request->stop();
                    QTimer::singleShot(0, this, [this] { fetch(); });
                    return;
                }
                m_status = status.value_or(0);
                m_status_text = reason.value_or(String {});
                StringBuilder text;
                for (auto const& header : headers->headers()) {
                    if (HTTP::is_forbidden_response_header_name(header.name)) continue;
                    text.appendff("{}: {}\r\n", header.name, header.value);
                    if (text.length() > 65536) { finish("error"sv); return; }
                }
                m_response_headers = String::from_utf8_with_replacement_character(text.string_view());
                if (m_stream) {
                    m_headers_sent = true;
                    auto completion = move(m_completion);
                    completion(packet("headers"sv));
                }
            },
            [this, active](Requests::ResponseData data) { if (active()) append(data.bytes()); },
            [this, active](Core::ImmutableBytes data) { if (active()) append(data.bytes()); },
            [this, active](u64, auto const&, Optional<Requests::NetworkError> error) {
                if (!active()) return;
                finish(error.has_value() || !m_status ? "error"sv : "load"sv);
            });
    }
    void append(ReadonlyBytes bytes)
    {
        if (!m_active() || bytes.size() > (m_stream ? 65536 : 4 * 1024 * 1024) - m_body.size()) { finish("error"sv); return; }
        if (m_body.try_append(bytes).is_error()) { finish("error"sv); return; }
        if (m_stream) {
            m_request->set_body_delivery_paused(true);
            deliver_stream();
        }
    }
    ErrorOr<ByteBuffer> packet(StringView event)
    {
        // A pending stream read can outlive the authority check in read().
        // Apply the same delivery-time check to headers, buffered responses and
        // terminal events so revocation cannot expose already buffered bytes.
        if (!m_active()) {
            m_body.clear();
            m_response_headers = {};
            return Error::from_string_literal("Userscript request authority was revoked");
        }
        JsonObject response;
        response.set("event"sv, event);
        response.set("status"sv, m_status);
        response.set("statusText"sv, m_status_text);
        response.set("finalUrl"sv, m_url.serialize());
        response.set("responseHeaders"sv, m_response_headers);
        auto base64 = QByteArray(reinterpret_cast<char const*>(m_body.data()), m_body.size()).toBase64();
        response.set("body"sv, TRY(String::from_utf8(StringView(base64.data(), base64.size()))));
        auto json = response.serialized();
        using Protocol = Web::DerpBirdValueProtocol;
        return Protocol::encode(Protocol::Response { Protocol::Kind::Network, {}, TRY(ByteBuffer::copy(json.bytes())), {}, 0 });
    }
    void deliver_stream()
    {
        if (!m_read) return;
        auto completion = move(m_read);
        auto response = packet(m_done ? m_terminal_event.bytes_as_string_view() : "data"sv);
        m_body.clear();
        if (m_done) deleteLater();
        completion(move(response));
    }
    void finish(StringView event)
    {
        if (m_done) return;
        m_done = true;
        m_timeout.stop(); m_liveness.stop();
        if (m_request) m_request->stop();
        if (m_stream && m_headers_sent) {
            m_terminal_event = MUST(String::from_utf8(event));
            if (m_read) deliver_stream();
            else QTimer::singleShot(30000, this, [this] { deleteLater(); });
            return;
        }
        auto completion = move(m_completion);
        auto response = packet(event);
        deleteLater();
        completion(move(response));
    }
    Vector<String> m_connects;
    URL::URL m_origin, m_url;
    WebView::IsPrivate m_privacy;
    Active m_active;
    Completion m_completion, m_read;
    String m_terminal_event;
    bool m_stream { false }, m_headers_sent { false };
    QTimer m_timeout, m_liveness;
    QDeadlineTimer m_deadline;
    RefPtr<Requests::Request> m_request;
    RefPtr<HTTP::HeaderList> m_headers;
    ByteString m_method;
    ByteBuffer m_upload, m_body;
    String m_response_headers, m_status_text;
    u32 m_status { 0 };
    unsigned m_serial { 0 }, m_redirects { 0 };
    bool m_anonymous { false }, m_done { false };
};
}
