#pragma once
#include <AK/ByteBuffer.h>
#include <LibGfx/Bitmap.h>
#include <LibHTTP/Cache/CacheMode.h>
#include <LibHTTP/Cookie/IncludeCredentials.h>
#include <LibImageDecoderClient/Client.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/ImageDecoderManager.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebContentClient.h>
#include <QPointer>
#include <QTimer>

namespace Ladybird {
// Untrusted images are decoded in Ladybird's sandboxed ImageDecoder. A failed
// image must not suppress the notification. Both network and decoder have a deadline.
class DerpBirdNotificationImage final : public QObject {
public:
    using Completion = Function<void(RefPtr<Gfx::Bitmap const>)>;
    DerpBirdNotificationImage(QObject* parent, WebView::WebContentPage& page, Completion completion)
        : QObject(parent), m_client(page.client().make_weak_ptr<WebView::WebContentClient>()), m_client_child_id(page.client().child_id()), m_completion(move(completion))
    {
        m_timeout.setSingleShot(true);
        connect(&m_timeout, &QTimer::timeout, this, [this] { finish({}); });
    }
    ~DerpBirdNotificationImage() override {
        m_done = true;
        if (m_request) m_request->stop();
        m_decoder = nullptr;
        if (m_decoder_generation.has_value()) WebView::ImageDecoderManager::the().process_did_release_decoder(m_client_child_id, *m_decoder_generation);
    }
    void start(String const& address)
    {
        m_timeout.start(10000);
        auto bytes = address.bytes_as_string_view();
        if (bytes.starts_with("data:"sv)) {
            auto comma = bytes.find(',');
            if (!comma.has_value() || bytes.length() > maximum_bytes * 2) { finish({}); return; }
            auto header = bytes.substring_view(5, *comma - 5);
            auto payload = bytes.substring_view(*comma + 1);
            auto body = QByteArray::fromPercentEncoding(QByteArray(payload.characters_without_null_termination(), payload.length()));
            if (header.ends_with(";base64"sv)) {
                auto decoded = QByteArray::fromBase64Encoding(body, QByteArray::AbortOnBase64DecodingErrors);
                if (!decoded) { finish({}); return; }
                body = move(decoded.decoded);
            }
            if (static_cast<size_t>(body.size()) > maximum_bytes) { finish({}); return; }
            auto copied = ByteBuffer::copy(ReadonlyBytes(reinterpret_cast<u8 const*>(body.data()), body.size()));
            if (copied.is_error()) { finish({}); return; }
            m_body = copied.release_value(); decode(); return;
        }
        auto url = URL::Parser::basic_parse(address);
        if (!url.has_value() || !allowed(*url)) { finish({}); return; }
        m_url = url.release_value(); fetch();
    }
private:
    static constexpr size_t maximum_bytes = 1024 * 1024;
    static bool allowed(URL::URL const& url) { return (url.scheme() == "https"sv || url.scheme() == "http"sv) && url.username().is_empty() && url.password().is_empty(); }
    void finish(RefPtr<Gfx::Bitmap const> bitmap)
    {
        if (m_done) return;
        m_done = true; m_timeout.stop();
        if (m_request) m_request->stop();
        auto completion = move(m_completion);
        // Showing the notification can synchronously revoke its owner and
        // destroy this image loader. Schedule cleanup before leaving our code.
        deleteLater();
        completion(move(bitmap));
    }
    void fetch()
    {
        if (m_done || !m_client) { finish({}); return; }
        m_body.clear(); m_ok = false;
        auto client = WebView::Application::request_server_client(m_client->session().is_private());
        if (client.is_error()) { finish({}); return; }
        m_request = client.value()->start_request("GET"sv, m_url, HTTP::Cookie::SameSiteContext {}, {}, {}, HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::No);
        if (!m_request) { finish({}); return; }
        auto serial = ++m_serial;
        auto active = [self = QPointer(this), serial] { return self && !self->m_done && self->m_serial == serial; };
        m_request->set_unbuffered_request_callbacks(
            [this, active](auto headers, Optional<u32> status, auto const&, Requests::CacheState) {
                if (!active()) return;
                if (status == 301u || status == 302u || status == 303u || status == 307u || status == 308u) {
                    auto location = headers->get("Location"sv);
                    auto next = location.has_value() ? URL::Parser::basic_parse(*location, m_url) : Optional<URL::URL>{};
                    if (!next.has_value() || !allowed(*next) || ++m_redirects > 5 || (m_url.scheme() == "https"sv && next->scheme() != "https"sv)) { finish({}); return; }
                    m_url = next.release_value(); ++m_serial; m_request->stop();
                    QTimer::singleShot(0, this, [this] { fetch(); }); return;
                }
                m_ok = status.has_value() && *status >= 200 && *status < 300;
                if (!m_ok) finish({});
            },
            [this, active](Requests::ResponseData data) { if (active()) append(data.bytes()); },
            [this, active](Core::ImmutableBytes data) { if (active()) append(data.bytes()); },
            [this, active](u64, auto const&, Optional<Requests::NetworkError> error) {
                if (!active()) return;
                if (error.has_value() || !m_ok) { finish({}); return; }
                decode();
            });
    }
    void append(ReadonlyBytes bytes)
    {
        if (bytes.size() > maximum_bytes - m_body.size() || m_body.try_append(bytes).is_error()) finish({});
    }
    void decode()
    {
        if (!m_client || m_body.is_empty()) { finish({}); return; }
        auto handle = m_client->connect_new_image_decoder_client(m_decoder_generation);
        if (!handle.has_value()) { finish({}); return; }
        auto transport = handle->create_transport();
        if (transport.is_error()) { finish({}); return; }
        m_decoder = make_ref_counted<ImageDecoderClient::Client>(transport.release_value());
        m_decoder->on_death = [self = QPointer(this)] { if (self) self->finish({}); };
        auto promise = m_decoder->decode_image(m_body.bytes(), [self = QPointer(this)](auto& decoded) -> ErrorOr<void> {
            if (!self || self->m_done) return {};
            if (decoded.is_animated) self->m_decoder->stop_animation_decode(decoded.session_id);
            if (decoded.frames.is_empty()) { self->finish({}); return {}; }
            auto& source = *decoded.frames[0].bitmap;
            if (source.width() <= 0 || source.height() <= 0) { self->finish({}); return {}; }
            // The bounded 128px output costs at most 16K pixel samples on the UI thread.
            auto ratio = min(1.0, 128.0 / max(source.width(), source.height()));
            auto width = max(1, static_cast<int>(source.width() * ratio));
            auto height = max(1, static_cast<int>(source.height() * ratio));
            auto output = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, {width, height});
            if (output.is_error()) { self->finish({}); return {}; }
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
                output.value()->set_pixel(x, y, source.get_pixel(static_cast<i64>(x) * source.width() / width, static_cast<i64>(y) * source.height() / height));
            self->finish(output.release_value()); return {};
        }, [self = QPointer(this)](Error&) { if (self) self->finish({}); }, Gfx::IntSize{128,128});
    }
    WeakPtr<WebView::WebContentClient> m_client;
    u64 m_client_child_id;
    Optional<u64> m_decoder_generation;
    Completion m_completion; QTimer m_timeout;
    RefPtr<Requests::Request> m_request;
    RefPtr<ImageDecoderClient::Client> m_decoder;
    URL::URL m_url; ByteBuffer m_body;
    unsigned m_serial{0}, m_redirects{0}; bool m_done{false}, m_ok{false};
};
}
