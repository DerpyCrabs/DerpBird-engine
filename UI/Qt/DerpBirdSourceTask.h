#pragma once

#include <AK/Base64.h>
#include <AK/JsonObject.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <QPointer>
#include <QTimer>

namespace Ladybird {

// Browser-owned downloads use no site cookies and revalidate every redirect.
class DerpBirdSourceTask final : public QObject {
public:
    DerpBirdSourceTask(QObject* parent, URL::URL url, size_t limit, bool binary,
        Function<void(ErrorOr<JsonValue>)> completion)
        : QObject(parent)
        , m_url(move(url))
        , m_limit(limit)
        , m_binary(binary)
        , m_completion(move(completion))
    {
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            finish(Error::from_string_literal("Source download timed out"));
        });
    }
    ~DerpBirdSourceTask() override
    {
        auto request = move(m_request);
        if (request)
            request->stop();
        if (m_completion)
            m_completion(Error::from_string_literal("Source download cancelled"));
    }
    static bool valid(URL::URL const& url)
    {
        return url.scheme().is_one_of("http"sv, "https"sv) && !url.includes_credentials();
    }
    void start()
    {
        m_timer.start(25000);
        fetch();
    }

private:
    void fetch()
    {
        if (!m_completion)
            return;
        m_body.clear();
        m_ok = false;
        auto client = WebView::Application::request_server_client();
        if (client.is_error()) {
            finish(client.release_error());
            return;
        }
        m_request = client.value()->start_request(
            "GET"sv, m_url, HTTP::Cookie::SameSiteContext {}, {}, {},
            HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::No);
        if (!m_request) {
            finish(Error::from_string_literal("Cannot download source"));
            return;
        }
        auto serial = ++m_serial;
        auto active = [self = QPointer(this), serial] {
            return self && self->m_completion && self->m_serial == serial;
        };
        m_request->set_unbuffered_request_callbacks(
            [this, active](auto headers, Optional<u32> status, auto const&,
                Requests::CacheState) {
                if (!active())
                    return;
                if (status == 301u || status == 302u || status == 303u || status == 307u || status == 308u) {
                    auto location = headers->get("Location"sv);
                    auto target = location.has_value()
                        ? URL::Parser::basic_parse(*location, m_url)
                        : Optional<URL::URL> {};
                    if (!target.has_value() || !valid(*target) || ++m_redirects > 10 || (m_url.scheme() == "https"sv && target->scheme() != "https"sv)) {
                        finish(Error::from_string_literal("Invalid source redirect"));
                        return;
                    }
                    m_url = target.release_value();
                    m_url.set_fragment({});
                    ++m_serial;
                    m_request->stop();
                    QTimer::singleShot(0, this, [this] { fetch(); });
                    return;
                }
                auto content_type = headers->get("Content-Type"sv)
                                        .value_or("application/octet-stream"sv);
                auto parsed_content_type = String::from_utf8(content_type.view());
                if (parsed_content_type.is_error()) {
                    finish(parsed_content_type.release_error());
                    return;
                }
                m_content_type = parsed_content_type.release_value();
                m_ok = status.has_value() && *status >= 200 && *status < 300;
                if (!m_ok)
                    finish(Error::from_string_literal(
                        "Source URL returned an unsuccessful HTTP status"));
            },
            [this, active](Requests::ResponseData data) {
                if (active())
                    append(data.bytes());
            },
            [this, active](Core::ImmutableBytes data) {
                if (active())
                    append(data.bytes());
            },
            [this, active](u64, auto const&,
                Optional<Requests::NetworkError> error) {
                if (!active())
                    return;
                if (error.has_value() || !m_ok) {
                    finish(Error::from_string_literal("Source download failed"));
                    return;
                }
                auto text = m_binary ? encode_base64(m_body.bytes())
                                     : String::from_utf8(StringView { m_body.bytes() });
                if (text.is_error()) {
                    finish(text.release_error());
                    return;
                }
                JsonObject result;
                result.set(m_binary ? "body"sv : "source"sv, text.release_value());
                result.set("url"sv, m_url.serialize());
                result.set("contentType"sv, m_content_type);
                finish(JsonValue { move(result) });
            });
    }
    void append(ReadonlyBytes bytes)
    {
        if (bytes.size() > m_limit - m_body.size()) {
            finish(Error::from_string_literal("Source exceeds size limit"));
            return;
        }
        if (auto result = m_body.try_append(bytes); result.is_error())
            finish(result.release_error());
    }
    void finish(ErrorOr<JsonValue> result)
    {
        if (!m_completion)
            return;
        auto completion = move(m_completion);
        m_timer.stop();
        auto request = move(m_request);
        if (request)
            request->stop();
        deleteLater();
        completion(move(result));
    }
    String m_content_type;
    URL::URL m_url;
    size_t m_limit;
    bool m_binary;
    Function<void(ErrorOr<JsonValue>)> m_completion;
    QTimer m_timer;
    RefPtr<Requests::Request> m_request;
    ByteBuffer m_body;
    u64 m_serial { 0 };
    unsigned m_redirects { 0 };
    bool m_ok { false };
};

} // namespace Ladybird
