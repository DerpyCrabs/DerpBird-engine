#pragma once
#include <AK/ByteBuffer.h>
#include <AK/JsonObject.h>
#include <LibHTTP/Cache/CacheMode.h>
#include <LibHTTP/Cookie/IncludeCredentials.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <QPointer>
#include <QTimer>

namespace Ladybird {
class DerpBirdSourceDownload final : public QObject {
public:
    using Completion = Function<void(ErrorOr<JsonValue>)>;
    DerpBirdSourceDownload(QObject* parent, String address, Completion completion)
        : QObject(parent), m_address(move(address)), m_completion(move(completion))
    {
        m_timeout.setSingleShot(true);
        connect(&m_timeout, &QTimer::timeout, this, [this] { fail("Source download timed out"sv); });
    }
    ~DerpBirdSourceDownload() override
    {
        bool pending = !m_done;
        m_done = true;
        auto request = move(m_request);
        if (request) request->stop();
        if (pending) m_completion(Error::from_string_literal("Source download cancelled"));
    }
    void cancel() { fail("Source download cancelled"sv); }
    void start()
    {
        auto url = URL::Parser::basic_parse(m_address);
        if (!url.has_value() || !valid(*url)) { fail("Source URL must be HTTP(S), without credentials"sv); return; }
        m_url = url.release_value();
        m_url.set_fragment({});
        m_timeout.start(25000);
        fetch();
    }
private:
    static bool valid(URL::URL const& url) { return (url.scheme() == "http"sv || url.scheme() == "https"sv) && url.username().is_empty() && url.password().is_empty(); }
    void fail(StringView message) { finish(Error::from_string_view(message)); }
    void finish(ErrorOr<JsonValue> result)
    {
        if (m_done) return;
        m_done = true;
        m_timeout.stop();
        auto request = move(m_request);
        if (request) request->stop();
        m_bytes.clear();
        auto completion = move(m_completion);
        // A completion can destroy the owner and this child synchronously.
        deleteLater();
        completion(move(result));
    }
    void fetch()
    {
        // Redirects are deferred to leave the previous request callback. The
        // timeout or owning panel can cancel us before that queued turn runs.
        if (m_done) return;
        m_bytes.clear();
        m_ok = false;
        auto client = WebView::Application::request_server_client();
        if (client.is_error()) { finish(client.release_error()); return; }
        auto headers = HTTP::HeaderList::create();
        m_request = client.value()->start_request("GET"sv, m_url, *headers, {}, HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::No);
        if (!m_request) { fail("Cannot download source"sv); return; }
        auto serial = ++m_serial;
        auto active = [self = QPointer(this), serial] { return self && !self->m_done && self->m_serial == serial; };
        m_request->set_unbuffered_request_callbacks(
            [this, active](auto headers, Optional<u32> status, auto const&, Requests::CacheState) {
                if (!active()) return;
                if (status == 301u || status == 302u || status == 303u || status == 307u || status == 308u) {
                    auto location = headers->get("Location"sv);
                    auto target = location.has_value() ? URL::Parser::basic_parse(*location, m_url) : Optional<URL::URL> {};
                    if (!target.has_value() || !valid(*target) || ++m_redirects > 10 || (m_url.scheme() == "https"sv && target->scheme() != "https"sv)) {
                        fail("Invalid source redirect"sv); return;
                    }
                    m_url = target.release_value();
                    m_url.set_fragment({});
                    ++m_serial;
                    m_request->stop();
                    QTimer::singleShot(0, this, [this] { fetch(); });
                    return;
                }
                m_ok = status.has_value() && *status >= 200 && *status < 300;
                if (!m_ok) fail("Source URL returned an unsuccessful HTTP status"sv);
            },
            [this, active](Requests::ResponseData data) { if (active()) append(data.bytes()); },
            [this, active](Core::ImmutableBytes data) { if (active()) append(data.bytes()); },
            [this, active](u64, auto const&, Optional<Requests::NetworkError> error) {
                if (!active()) return;
                if (error.has_value() || !m_ok) { fail("Source download failed"sv); return; }
                auto source = String::from_utf8(StringView { m_bytes.bytes() });
                if (source.is_error()) { finish(source.release_error()); return; }
                JsonObject result;
                result.set("source"sv, source.release_value());
                result.set("url"sv, m_url.serialize());
                finish(JsonValue { move(result) });
            });
    }
    void append(ReadonlyBytes bytes)
    {
        if (bytes.size() > 2 * MiB - m_bytes.size()) { fail("Source exceeds 2 MiB"sv); return; }
        auto result = m_bytes.try_append(bytes);
        if (result.is_error()) finish(result.release_error());
    }
    String m_address;
    Completion m_completion;
    QTimer m_timeout;
    RefPtr<Requests::Request> m_request;
    URL::URL m_url;
    ByteBuffer m_bytes;
    unsigned m_serial { 0 }, m_redirects { 0 };
    bool m_done { false }, m_ok { false };
};
}
