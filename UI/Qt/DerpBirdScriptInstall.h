#pragma once

#include <AK/ByteBuffer.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibHTTP/Cache/CacheMode.h>
#include <LibHTTP/Cookie/IncludeCredentials.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <AK/Base64.h>
#include <QCryptographicHash>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QThread>
#include <memory>
#include <UI/Qt/DerpBirdLocalDependencies.h>

namespace Ladybird {

// Cache integrity is checked against the original URL, including after redirects.
// Tampermonkey chooses the last supported hash in a comma/semicolon list.
inline ErrorOr<void> verify_derpbird_dependency_fragment(StringView fragment, ReadonlyBytes bytes)
{
    if (fragment.is_empty()) return {};
    auto parts = QString::fromUtf8(fragment.characters_without_null_termination(), fragment.length()).split(QRegularExpression("[,;]"));
    Optional<QCryptographicHash::Algorithm> algorithm;
    QByteArray expected;
    for (auto const& part : parts) {
        auto match = QRegularExpression("^(md5|sha1|sha256|sha384|sha512)[=-](.+)$").match(part);
        if (!match.hasMatch()) continue;
        auto name = match.captured(1);
        algorithm = name == "md5" ? QCryptographicHash::Md5 : name == "sha1" ? QCryptographicHash::Sha1
            : name == "sha256" ? QCryptographicHash::Sha256 : name == "sha384" ? QCryptographicHash::Sha384 : QCryptographicHash::Sha512;
        expected = match.captured(2).toLatin1();
    }
    if (!algorithm.has_value())
        return Error::from_string_literal("Unsupported userscript dependency integrity fragment");
    auto digest = QCryptographicHash::hash(QByteArrayView(reinterpret_cast<char const*>(bytes.data()), bytes.size()), *algorithm);
    if (expected.toLower() != digest.toHex() && expected != digest.toBase64())
        return Error::from_string_literal("Userscript dependency integrity mismatch");
    return {};
}

inline ErrorOr<void> verify_derpbird_dependency(StringView address, ReadonlyBytes bytes)
{
    auto url = URL::Parser::basic_parse(address);
    if (!url.has_value() || (url->scheme() != "https"sv && url->scheme() != "http"sv)
        || !url->username().is_empty() || !url->password().is_empty())
        return Error::from_string_literal("Userscript dependencies require an HTTP(S) URL without credentials");
    return verify_derpbird_dependency_fragment(url->fragment().value_or(StringView {}), bytes);
}

// One bounded install transaction. Network work uses Ladybird's RequestServer,
// without cookies or HTTP cache. No page navigation performs dependency I/O.
class DerpBirdScriptInstall final : public QObject {
public:
    using Completion = Function<void(ErrorOr<JsonArray>)>;
    struct Transport {
        Function<u64(JsonValue, Function<void(ErrorOr<JsonValue>)>)> request;
        Function<void(u64)> cancel;
    };
    DerpBirdScriptInstall(QObject* parent, JsonArray items, Completion completion, Transport transport, QString folder_config = {})
        : QObject(parent), m_items(move(items)), m_completion(move(completion)), m_folder_config(move(folder_config)), m_transport(move(transport))
    {
        m_timeout.setSingleShot(true);
        connect(&m_timeout, &QTimer::timeout, this, [this] { fail("Userscript dependency download timed out"sv); });
    }
    ~DerpBirdScriptInstall() override
    {
        if (m_request) m_transport.cancel(exchange(m_request, 0));
        if (!m_done) m_completion(Error::from_string_literal("Userscript installation cancelled"));
    }
    void start()
    {
        m_timeout.start(25000);
        // Account for all resident code before accepting additional downloads.
        for (auto const& item : m_items.values()) {
            m_bytes += item.as_object().get_string("source"sv)->bytes().size();
            if (auto cache = item.as_object().get_array("requiredSources"sv); cache.has_value())
                for (auto const& dependency : cache->values())
                    m_bytes += dependency.as_object().get_string("source"sv)->bytes().size();
        }
        for (auto const& item : m_items.values()) {
            if (auto cache = item.as_object().get_array("cachedResources"sv); cache.has_value())
                for (auto const& resource : cache->values())
                    m_bytes += resource.as_object().get_string("text"sv)->bytes().size() + resource.as_object().get_string("dataUrl"sv)->bytes().size();
        }
        next();
    }
private:
    void fail(StringView message)
    {
        finish(Error::from_string_view(message));
    }
    void finish(ErrorOr<JsonArray> result)
    {
        if (m_done) return;
        m_done = true;
        m_timeout.stop();
        if (m_request) m_transport.cancel(exchange(m_request, 0));
        m_completion(move(result));
        deleteLater();
    }
    void next()
    {
        if (m_done) return;
        if (m_bytes > max_total) { fail("Userscript registry exceeds 8 MiB"sv); return; }
        while (m_item < m_items.size()) {
            auto& record = m_items.values()[m_item].as_object();
            auto metadata = record.get_object("metadata"sv).value();
            auto required = metadata.get_array("requires"sv).value();
            auto resources = metadata.get_array("resources"sv).value();
            m_loading_resource = record.has("requiredSources"sv) || required.size() == 0;
            auto count = m_loading_resource ? resources.size() : required.size();
            auto cache_key = m_loading_resource ? "cachedResources"sv : "requiredSources"sv;
            if (m_loading_resource && (record.has(cache_key) || count == 0)) { ++m_item; continue; }
            if (count > 32) { fail("Too many userscript dependencies (maximum 32 per kind)"sv); return; }
            if (m_sources.size() == count) {
                record.set(cache_key, move(m_sources));
                m_sources = {};
                continue;
            }
            if (m_loading_resource) {
                auto resource = resources.at(m_sources.size()).as_object();
                m_original = resource.get_string("url"sv).value();
                m_resource_name = resource.get_string("name"sv).value();
            } else {
                m_original = required.at(m_sources.size()).as_string();
            }
            auto local_url = QUrl(derpbird_dependency_text(m_original), QUrl::StrictMode);
            if (local_url.isRelative() && local_url.host().isEmpty()) {
                fetch_local();
                return;
            }
            auto url = URL::Parser::basic_parse(m_original);
            if (!url.has_value() || (url->scheme() != "http"sv && url->scheme() != "https"sv)
                || !url->username().is_empty() || !url->password().is_empty()) {
                fail("Userscript dependencies require an HTTP(S) URL without credentials"sv); return;
            }
            m_url = url.release_value();
            m_url.set_fragment({});
            m_redirects = 0;
            fetch();
            return;
        }
        finish(move(m_items));
    }
    void fetch_local()
    {
        struct LocalResult {
            DerpBirdLocalScript script;
            DerpBirdLocalDependency dependency;
        };
        struct WorkerResult { Optional<ErrorOr<LocalResult>> result; };
        auto worker = std::make_shared<WorkerResult>();
        auto cached_script = m_local_item == m_item ? m_local_script : Optional<DerpBirdLocalScript> {};
        auto id = derpbird_dependency_text(m_items.at(m_item).as_object().get_string("id"sv).value());
        auto address = derpbird_dependency_text(m_original);
        auto serial = ++m_serial;
        // Capture values only: timeout or owner destruction must not leave the
        // worker accessing the installer, registry or Qt widgets.
        auto* thread = QThread::create([worker, config = m_folder_config, id, address, cached_script] {
            worker->result = [&]() -> ErrorOr<LocalResult> {
                auto script = cached_script.has_value() ? *cached_script : TRY(derpbird_local_script(config, id));
                auto dependency = TRY(derpbird_read_local_dependency(script, address));
                return LocalResult { move(script), move(dependency) };
            }();
        });
        connect(thread, &QThread::finished, this, [this, worker, serial, address] {
            if (m_done || serial != m_serial) return;
            VERIFY(worker->result.has_value());
            auto result = worker->result.release_value();
            if (result.is_error()) { finish(result.release_error()); return; }
            auto local = result.release_value();
            m_local_script = move(local.script);
            m_local_item = m_item;
            m_body.clear();
            append(ReadonlyBytes { reinterpret_cast<u8 const*>(local.dependency.bytes.constData()), static_cast<size_t>(local.dependency.bytes.size()) });
            if (m_done) return;
            auto fragment = derpbird_dependency_string(QUrl(address, QUrl::StrictMode).fragment());
            auto integrity = verify_derpbird_dependency_fragment(fragment, m_body.bytes());
            if (integrity.is_error()) { finish(integrity.release_error()); return; }
            m_mime = derpbird_dependency_string(local.dependency.mime);
            accept_dependency();
        });
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    }
    void fetch()
    {
        if (m_done) return;
        m_body.clear();
        m_mime = "application/octet-stream"_string;
        JsonObject payload; payload.set("url"sv, m_url.serialize()); payload.set("limit"sv, max_dependency);
        auto serial = ++m_serial;
        m_request = m_transport.request(move(payload), [self = QPointer(this), serial](ErrorOr<JsonValue> response) {
            if (!self || self->m_done || self->m_serial != serial) return;
            self->m_request = 0;
            if (response.is_error()) { self->finish(response.release_error()); return; }
            if (!response.value().is_object()) { self->fail("Invalid dependency response"sv); return; }
            auto const& result = response.value().as_object();
            auto encoded = result.get_string("body"sv);
            if (!encoded.has_value()) { self->fail("Invalid dependency response"sv); return; }
            auto bytes = decode_base64(*encoded);
            if (bytes.is_error()) { self->finish(bytes.release_error()); return; }
            self->append(bytes.value().bytes());
            if (self->m_done) return;
            if (auto type = result.get_string("contentType"sv); type.has_value()) {
                auto mime = type->bytes_as_string_view().substring_view(0, type->find_byte_offset(';').value_or(type->bytes().size())).trim_whitespace();
                bool valid = !mime.is_empty() && mime.length() <= 128;
                for (auto ch : mime) valid &= is_ascii_alphanumeric(ch) || ch == '/' || ch == '-' || ch == '+' || ch == '.' || ch == '_';
                if (valid) self->m_mime = MUST(String::from_utf8(mime));
            }
            auto integrity = verify_derpbird_dependency(self->m_original, self->m_body.bytes());
            if (integrity.is_error()) { self->finish(integrity.release_error()); return; }
            self->accept_dependency();
        });
    }
    void accept_dependency()
    {
        JsonObject dependency;
        dependency.set("url"sv, m_original);
        if (m_loading_resource) {
            auto text = String::from_utf8_with_replacement_character(StringView { m_body.bytes() });
            auto base64 = QByteArray(reinterpret_cast<char const*>(m_body.data()), m_body.size()).toBase64();
            auto data_url = MUST(String::formatted("data:{};base64,{}", m_mime, StringView(base64.data(), base64.size())));
            m_bytes += text.bytes().size() + data_url.bytes().size();
            dependency.set("name"sv, m_resource_name);
            dependency.set("text"sv, move(text));
            dependency.set("dataUrl"sv, move(data_url));
        } else {
            auto source = String::from_utf8(StringView { m_body.bytes() });
            if (source.is_error()) { fail("Userscript dependency is not valid UTF-8"sv); return; }
            dependency.set("source"sv, source.release_value());
            m_bytes += m_body.size();
        }
        m_sources.must_append(move(dependency));
        ++m_serial;
        QTimer::singleShot(0, this, [this] { next(); });
    }
    void append(ReadonlyBytes bytes)
    {
        if (bytes.size() > max_dependency - m_body.size() || bytes.size() > max_total - m_bytes - m_body.size()) {
            fail("Userscript dependency exceeds code size limit"sv); return;
        }
        auto result = m_body.try_append(bytes);
        if (result.is_error()) finish(result.release_error());
    }
    static constexpr size_t max_dependency = 2 * 1024 * 1024;
    static constexpr size_t max_total = 8 * 1024 * 1024;
    JsonArray m_items;
    Completion m_completion;
    QString m_folder_config;
    Optional<DerpBirdLocalScript> m_local_script;
    size_t m_local_item { 0 };
    QTimer m_timeout;
    Transport m_transport;
    u64 m_request { 0 };
    JsonArray m_sources;
    ByteBuffer m_body;
    URL::URL m_url;
    String m_original;
    String m_resource_name;
    String m_mime;
    bool m_loading_resource { false };
    size_t m_item { 0 }, m_bytes { 0 };
    unsigned m_redirects { 0 }, m_serial { 0 };
    bool m_ok { false }, m_done { false };
};
}
