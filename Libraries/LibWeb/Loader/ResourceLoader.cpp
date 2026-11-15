/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/Debug.h>
#include <AK/NeverDestroyed.h>
#include <LibCore/Directory.h>
#include <LibCore/MimeData.h>
#include <LibCore/Resource.h>
#include <LibCore/System.h>
#include <LibGC/Function.h>
#include <LibHTTP/Cookie/Cookie.h>
#include <LibHTTP/HSTSPreloadData.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Engine/NetworkTransportHandle.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/NetworkPartitionKey.h>
#include <LibWeb/Loader/ContentBlocker.h>
#include <LibWeb/Loader/GeneratedPagesLoader.h>
#include <LibWeb/Loader/LoadRequest.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Page/DerpBirdWebRequestRegistration.h>
#include <LibWeb/Page/DerpBirdResponseFiltering.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/Timer.h>
#include <LibWebCommon/Loader/UserAgent.h>

namespace Web {

static RefPtr<ResourceLoader>& resource_loader()
{
    static thread_local NeverDestroyed<RefPtr<ResourceLoader>> resource_loader;
    return *resource_loader;
}

// NB: This is set from the command line, before any agent starts.
static Optional<URL::URL>& proxy_autoconfig_url_storage()
{
    static Optional<URL::URL>& proxy_autoconfig_url = *new Optional<URL::URL>;
    return proxy_autoconfig_url;
}

void ResourceLoader::set_proxy_autoconfig_url(URL::URL url)
{
    proxy_autoconfig_url_storage() = move(url);
}

Optional<URL::URL> const& ResourceLoader::proxy_autoconfig_url()
{
    return proxy_autoconfig_url_storage();
}

void ResourceLoader::initialize(GC::Heap& heap)
{
    resource_loader() = adopt_ref(*new ResourceLoader(heap));
}

void ResourceLoader::shut_down()
{
    resource_loader() = nullptr;
}

void ResourceLoader::stop_all_requests()
{
    // NB: A stopped request removes itself from the set.
    auto active_requests = m_active_requests;
    for (auto const& request : active_requests)
        request->stop();
}

bool ResourceLoader::is_initialized()
{
    return resource_loader() != nullptr;
}

ResourceLoader& ResourceLoader::the()
{
    if (!resource_loader()) {
        dbgln("Web::ResourceLoader was not initialized");
        VERIFY_NOT_REACHED();
    }
    return *resource_loader();
}

ResourceLoader::ResourceLoader(GC::Heap& heap)
    : m_heap(heap)
{
}

Atomic<NavigatorCompatibilityMode, AK::MemoryOrder::memory_order_relaxed> ResourceLoader::s_navigator_compatibility_mode { default_navigator_compatibility_mode };

void ResourceLoader::set_navigator_compatibility_mode(NavigatorCompatibilityMode mode)
{
    s_navigator_compatibility_mode = mode;

    // A script reads two strings to tell engines apart that no navigator attribute covers: the frames of an error's
    // stack, and the source text of a built-in function. Both follow the engine the navigator claims.
    if (mode == NavigatorCompatibilityMode::Chrome)
        JS::VM::set_source_text_syntaxes(JS::VM::StackFrameSyntax::Indented, JS::VM::NativeFunctionSourceSyntax::SingleLine);
    else
        JS::VM::set_source_text_syntaxes(JS::VM::StackFrameSyntax::AtSign, JS::VM::NativeFunctionSourceSyntax::MultiLine);
}

void ResourceLoader::set_client(Optional<Utf16String> const& top_level_site, NonnullRefPtr<Requests::RequestClient> request_client)
{
    request_client->on_request_server_died = [this, top_level_site, disconnected_client = request_client->make_weak_ptr<Requests::RequestClient>()]() {
        auto& client = ensure_client_for(top_level_site);
        if (client.ptr() == disconnected_client.ptr())
            client = nullptr;
    };
    ensure_client_for(top_level_site) = move(request_client);
}

RefPtr<Requests::RequestClient> ResourceLoader::request_client_for(Optional<HTTP::NetworkIsolationKey> const& network_isolation_key) const
{
    if (!network_isolation_key.has_value())
        return m_request_client;
    return request_client_for_top_level_site(network_isolation_key->top_level_site);
}

RefPtr<Requests::RequestClient> ResourceLoader::request_client_for_top_level_site(Optional<Utf16String> const& top_level_site) const
{
    if (top_level_site.has_value()) {
        if (auto it = m_site_request_clients.find(*top_level_site); it != m_site_request_clients.end())
            return it->value;
    }
    return m_request_client;
}

RefPtr<Requests::RequestClient>& ResourceLoader::ensure_client_for(Optional<Utf16String> const& top_level_site)
{
    if (!top_level_site.has_value())
        return m_request_client;
    return m_site_request_clients.ensure(*top_level_site);
}

Vector<Utf16String> ResourceLoader::top_level_sites() const
{
    Vector<Utf16String> top_level_sites;
    for (auto const& entry : m_site_request_clients)
        top_level_sites.append(entry.key);
    return top_level_sites;
}

RefPtr<Requests::RequestClient> ResourceLoader::take_client_for_testing(Optional<Utf16String> const& top_level_site)
{
    return move(ensure_client_for(top_level_site));
}

// Speculation warms up the RequestServer that the document's requests go through.
static Optional<HTTP::NetworkIsolationKey> speculative_network_isolation_key(GC::Ptr<DOM::Document> document)
{
    if (!document)
        return {};
    return Fetch::Infrastructure::determine_the_network_partition_key(document->relevant_settings_object());
}

void ResourceLoader::prefetch_dns(GC::Ptr<DOM::Document> document, URL::URL const& url, URL::URL const& source_url)
{
    if (url.scheme().is_one_of("file"sv, "data"sv))
        return;

    if (!document || document->content_blocking_enabled()) {
        if (ContentBlocker::the().check(url, source_url, ContentBlocker::ResourceType::Other, {}, document ? document->relevant_settings_object().top_level_creation_url : Optional<URL::URL> {}).blocked) {
            dbgln("ResourceLoader: Refusing to prefetch DNS for '{}': \033[31;1mURL was filtered\033[0m", url);
            return;
        }
    }

    // Speculation would resolve and connect without the proxy that the PAC file would have picked.
    if (proxy_autoconfig_url().has_value())
        return;

    // FIXME: We could put this request in a queue until the client connection is re-established.
    if (auto request_client = request_client_for(speculative_network_isolation_key(document)))
        request_client->ensure_connection(url, RequestServer::CacheLevel::ResolveOnly);
}

void ResourceLoader::preconnect(GC::Ptr<DOM::Document> document, URL::URL const& url, URL::URL const& source_url)
{
    if (url.scheme().is_one_of("file"sv, "data"sv))
        return;

    if (!document || document->content_blocking_enabled()) {
        if (ContentBlocker::the().check(url, source_url, ContentBlocker::ResourceType::Other, {}, document ? document->relevant_settings_object().top_level_creation_url : Optional<URL::URL> {}).blocked) {
            dbgln("ResourceLoader: Refusing to pre-connect to '{}': \033[31;1mURL was filtered\033[0m", url);
            return;
        }
    }

    // Speculation would resolve and connect without the proxy that the PAC file would have picked.
    if (proxy_autoconfig_url().has_value())
        return;

    // FIXME: We could put this request in a queue until the client connection is re-established.
    if (auto request_client = request_client_for(speculative_network_isolation_key(document)))
        request_client->ensure_connection(url, RequestServer::CacheLevel::CreateConnection);
}

static ByteString sanitized_url_for_logging(URL::URL const& url)
{
    if (url.scheme() == "data"sv)
        return "[data URL]"sv;
    return url.to_byte_string();
}

// https://www.rfc-editor.org/rfc/rfc6797#section-8.2
bool ResourceLoader::is_known_hsts_host(Page& page, String const& host)
{
    if (HTTP::HSTSPreloadData::the().is_known_preloaded_hsts_host(host.to_ascii_lowercase()))
        return true;

    return page.client().page_did_is_known_hsts_host(host);
}

static NonnullRefPtr<HTTP::HeaderList> response_headers_for_file(StringView path, size_t content_length, Optional<time_t> const& modified_time)
{
    // For file:// and resource:// URLs, we have to guess the MIME type, since there's no HTTP header to tell us what
    // it is. We insert a fake Content-Type header here, so that clients can use it to learn the MIME type.
    auto mime_type = Core::guess_mime_type_based_on_filename(path);

    auto response_headers = HTTP::HeaderList::create({
        { "Access-Control-Allow-Origin"sv, "null"sv },
        { "Content-Type"sv, mime_type },
        { "Content-Length"sv, ByteString::number(content_length) },
    });

    if (modified_time.has_value()) {
        auto const datetime = AK::UnixDateTime::from_seconds_since_epoch(modified_time.value());
        response_headers->set({ "Last-Modified"sv, datetime.to_byte_string("%a, %d %b %Y %H:%M:%S GMT"sv, AK::UnixDateTime::LocalTime::No) });
    }

    return response_headers;
}

static void log_request_start(LoadRequest const& request)
{
    auto url_for_logging = sanitized_url_for_logging(*request.url());

    dbgln_if(SPAM_DEBUG, "ResourceLoader: Starting load of: \"{}\"", url_for_logging);
}

static void log_success(LoadRequest const& request)
{
    auto url_for_logging = sanitized_url_for_logging(*request.url());
    auto load_time_ms = request.load_time().to_milliseconds();

    dbgln_if(SPAM_DEBUG, "ResourceLoader: Finished load of: \"{}\", Duration: {}ms", url_for_logging, load_time_ms);
}

template<typename ErrorType>
static void log_failure(LoadRequest const& request, ErrorType const& error)
{
    auto url_for_logging = sanitized_url_for_logging(*request.url());
    auto load_time_ms = request.load_time().to_milliseconds();

    dbgln("ResourceLoader: Failed load of: \"{}\", \033[31;1mError: {}\033[0m, Duration: {}ms", url_for_logging, error, load_time_ms);
}

// What kind of resource a blocked request was for, as the browser names it to the user.
static StringView blocked_resource_kind(LoadRequest const& request)
{
    using Destination = Fetch::Infrastructure::Request::Destination;
    if (!request.destination().has_value())
        return "Request"sv;
    switch (*request.destination()) {
    case Destination::Script:
    case Destination::AudioWorklet:
    case Destination::PaintWorklet:
    case Destination::ServiceWorker:
    case Destination::SharedWorker:
    case Destination::Worker:
        return "Script"sv;
    case Destination::Image:
        return "Image"sv;
    case Destination::Style:
    case Destination::XSLT:
        return "Stylesheet"sv;
    case Destination::Font:
        return "Font"sv;
    case Destination::Audio:
    case Destination::Track:
    case Destination::Video:
        return "Media"sv;
    case Destination::Document:
    case Destination::Embed:
    case Destination::Frame:
    case Destination::IFrame:
    case Destination::Object:
        return "Frame"sv;
    case Destination::JSON:
    case Destination::Manifest:
    case Destination::Report:
    case Destination::Text:
    case Destination::WebIdentity:
        return "Request"sv;
    }
    VERIFY_NOT_REACHED();
}

static ContentBlocker::Decision filter_request(LoadRequest const& request)
{
    auto const& url = request.url().value();

    auto is_port_blocked = [](int port) {
        static constexpr auto ports = to_array({ 1, 7, 9, 11, 13, 15, 17, 19, 20, 21, 22, 23, 25, 37, 42,
            43, 53, 77, 79, 87, 95, 101, 102, 103, 104, 109, 110, 111, 113, 115, 117, 119, 123, 135, 139,
            143, 179, 389, 465, 512, 513, 514, 515, 526, 530, 531, 532, 540, 556, 563, 587, 601, 636,
            993, 995, 2049, 3659, 4045, 6000, 6379, 6665, 6666, 6667, 6668, 6669 });

        return ports.first_index_of(port).has_value();
    };

    if (is_port_blocked(url.port_or_default())) {
        log_failure(request, ByteString::formatted("Port #{} is blocked", url.port_or_default()));
        return { .blocked = true };
    }

    if (auto page = request.page(); page && DerpBirdWebRequestRegistration::has_rules(*page)) {
        using Type = DerpBirdWebRequestRules::Type;
        auto kind = ContentBlocker::resource_type_from_fetch_metadata(request.destination(), request.initiator_type(), request.request_mode());
        auto type = request.derpbird_subframe_navigation || kind == ContentBlocker::ResourceType::Subdocument ? Type::SubFrame
            : kind == ContentBlocker::ResourceType::Script ? Type::Script
            : kind == ContentBlocker::ResourceType::XMLHttpRequest ? Type::XHR : Type::Other;
        auto result = DerpBirdWebRequestRegistration::evaluate(*page, url, type);
        if (result.is_error()) return { .blocked = true, .matched_rule = "Userscript request policy limit"_string };
        if (result.value().has_value()) {
            auto decision = result.release_value().release_value();
            if (decision.cancel || decision.redirect.has_value())
                return { .blocked = decision.cancel, .matched_rule = "Userscript webRequest"_string, .rewritten_url = move(decision.redirect) };
        }
    }

    if (request.derpbird_approved_navigation) return {};

    if (auto page = request.page(); !page || page->content_blocking_enabled_for_url(request.top_level_creation_url())) {
        if (request.derpbird_popup_source.has_value()) {
            auto popup = ContentBlocker::the().check(url, *request.derpbird_popup_source, request.derpbird_popunder ? ContentBlocker::ResourceType::Popunder : ContentBlocker::ResourceType::Popup, request.method(), request.derpbird_popup_top);
            if (popup.blocked) {
                if (page) page->client().page_did_block_resource(url, request.derpbird_popunder ? "Popunder"sv : "Popup"sv, popup.matched_rule);
                return popup;
            }
        }
        auto source_url = request.source_url().value_or(url);
        auto type = ContentBlocker::resource_type_from_fetch_metadata(request.destination(), request.initiator_type(), request.request_mode());
        if (request.derpbird_subframe_navigation) type = ContentBlocker::ResourceType::Subdocument;
        auto decision = ContentBlocker::the().check(url, source_url, type, request.method(), request.top_level_creation_url());
        if (decision.blocked && page)
            page->client().page_did_block_resource(url, blocked_resource_kind(request), decision.matched_rule);
        else if (page && decision.rewritten_url.has_value())
            page->client().page_did_block_resource(url, "Filter: rewrite"sv, decision.matched_rule);
        else if (page && !decision.matched_rule.is_empty())
            page->client().page_did_block_resource(url, "Filter: allow"sv, decision.matched_rule);
        return decision;
    }

    return {};
}

template<typename FileHandler, typename ErrorHandler>
void ResourceLoader::handle_file_load_request(LoadRequest& request, FileHandler on_file, ErrorHandler on_error)
{
    auto page = request.page();
    if (!page) {
        auto const error_message = ByteString("INTERNAL ERROR: No Page for file scheme request"sv);
        on_error(error_message);
        return;
    }

    auto const& url = request.url().value();

    FileRequest file_request(url.file_path(), [request, on_file, on_error, url](ErrorOr<i32> file_or_error) mutable {
        if (file_or_error.is_error()) {
            auto const message = ByteString::formatted("{}", file_or_error.error());
            on_error(message);
            return;
        }

        auto const fd = file_or_error.value();

        auto maybe_is_valid_directory = Core::Directory::is_valid_directory(fd);
        if (!maybe_is_valid_directory.is_error() && maybe_is_valid_directory.value()) {
            auto maybe_response = load_file_directory_page(url);
            if (maybe_response.is_error()) {
                auto const message = ByteString::formatted("{}", maybe_response.error());
                on_error(message);
                return;
            }

            FileLoadResult load_result {
                .data = maybe_response.value().bytes(),
                .response_headers = HTTP::HeaderList::create({ { "Content-Type"sv, "text/html"sv } }),
            };
            on_file(load_result);
            return;
        }

        auto maybe_file = Core::File::adopt_fd(fd, Core::File::OpenMode::Read);
        if (maybe_file.is_error()) {
            on_error(ByteString::formatted("{}", maybe_file.error()));
            return;
        }

        auto file = maybe_file.release_value();

        auto st_or_error = file->stat();
        if (st_or_error.is_error()) {
            on_error(ByteString::formatted("{}", st_or_error.error()));
            return;
        }
        auto maybe_data = file->read_until_eof();
        if (maybe_data.is_error()) {
            on_error(ByteString::formatted("{}", maybe_data.error()));
            return;
        }

        FileLoadResult load_result {
            .data = maybe_data.value().bytes(),
            .response_headers = response_headers_for_file(request.url()->file_path(), maybe_data.value().size(), st_or_error.value().st_mtime),
        };
        on_file(load_result);
    });

    page->client().request_file(move(file_request));
}

template<typename ResourceHandler>
void ResourceLoader::handle_about_load_request(LoadRequest const& request, ResourceHandler on_resource, GC::Ref<GC::Function<void(ByteString const&)>> on_error)
{
    auto const& url = request.url().value();

    dbgln_if(SPAM_DEBUG, "Loading about: URL {}", url);

    auto response_headers = HTTP::HeaderList::create({
        { "Content-Type"sv, "text/html; charset=UTF-8"sv },
    });

    // FIXME: Implement timing info for about requests.
    Requests::RequestTimingInfo timing_info {};

    auto serialized_path = URL::percent_decode(url.serialize_path());
    auto target_file = ByteString::formatted("{}.html", serialized_path);

    auto about_directory = MUST(Core::Resource::load_from_uri("resource://ladybird/about-pages"_string));
    if (about_directory->children().contains_slow(target_file.view())) {
        auto resource = Core::Resource::load_from_uri(ByteString::formatted("resource://ladybird/about-pages/{}", target_file));
        if (!resource.is_error()) {
            auto const& buffer = resource.value()->data();
            ReadonlyBytes data(buffer.data(), buffer.size());
            on_resource(data, timing_info, response_headers);
            return;
        }
    }

    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(
        m_heap, GC::captures(on_error, ByteString::formatted("No such about page: {}", serialized_path)), [](auto& on_error, auto& error_message) {
            on_error->function()(error_message);
        }));
}

template<typename ResourceHandler, typename ErrorHandler>
void ResourceLoader::handle_resource_load_request(LoadRequest const& request, ResourceHandler on_resource, ErrorHandler on_error)
{
    auto const& url = request.url().value();

    auto resource = Core::Resource::load_from_uri(url.serialize());
    if (resource.is_error()) {
        on_error(ByteString::formatted("{}", resource.error()));
        return;
    }

    auto resource_value = resource.release_value();

    // When resource URI is a directory use file directory loader to generate response
    if (resource_value->is_directory()) {
        auto directory_url = URL::Parser::basic_parse(resource_value->file_url());
        VERIFY(directory_url.has_value());

        auto maybe_response = load_file_directory_page(directory_url.release_value());
        if (maybe_response.is_error()) {
            on_error(ByteString::formatted("{}", maybe_response.error()));
            return;
        }

        FileLoadResult load_result {
            .data = maybe_response.value().bytes(),
            .response_headers = HTTP::HeaderList::create({ { "Content-Type"sv, "text/html"sv } }),
        };
        on_resource(load_result);
        return;
    }

    // FIXME: Implement timing info for resource requests.
    Requests::RequestTimingInfo timing_info {};

    FileLoadResult load_result {
        .data = resource_value->data(),
        .response_headers = response_headers_for_file(url.file_path(), resource_value->data().size(), resource_value->modified_time()),
        .timing_info = timing_info,
    };
    on_resource(load_result);
}

RefPtr<Fetch::Engine::NetworkTransportHandle> ResourceLoader::load(LoadRequest& request, GC::Root<OnHeadersReceived> on_headers_received, GC::Root<OnDataReceived> on_data_received, GC::Root<OnCachedBodyAvailable> on_cached_body_available, GC::Root<OnComplete> on_complete, Requests::RequestClient::TransferLease transfer_lease)
{
    auto const& url = request.url().value();
    if (request.method() != "HEAD"sv && (!request.page() || request.page()->content_blocking_enabled_for_url(request.top_level_creation_url()))) {
        auto type = ContentBlocker::resource_type_from_fetch_metadata(request.destination(), request.initiator_type(), request.request_mode());
        if (request.derpbird_subframe_navigation) type = ContentBlocker::ResourceType::Subdocument;
        DerpBirdResponseFiltering::wrap({}, on_headers_received, on_data_received, on_cached_body_available, on_complete,
            [request](JsonArray const& operations) {
                if (auto page = request.page()) {
                    for (auto const& operation : operations.values()) {
                        if (!operation.is_object()) continue;
                        if (auto rule = operation.as_object().get_string("matched_rule"sv); rule.has_value())
                            page->client().page_did_block_resource(*request.url(), "Filter: response"sv, *rule);
                    }
                }
            }, [request, type](Fetch::Engine::NetworkTransportHandle* transport) {
                if (auto page = request.page(); page && !page->content_blocking_enabled_for_url(request.top_level_creation_url())) return JsonArray {};
                StringView addresses = transport ? transport->policy_ip_addresses().bytes_as_string_view() : StringView {};
                return ContentBlocker::the().response_replacements(*request.url(), request.source_url().value_or(*request.url()), type, request.method(), request.top_level_creation_url(), addresses);
            });
    }

    log_request_start(request);
    request.start_timer();

    auto decision = filter_request(request);
    auto deliver_filter_response = [request, on_headers_received, on_data_received, on_complete](ContentBlocker::Decision& decision) {
        auto local_origin = request.headers().get("Origin"sv).value_or("*"sv);
        if (decision.substitute.has_value()) {
            // A local resource response, not a navigation to a data: URL. Fetch still
            // applies its normal CORS, MIME, integrity and CSP checks.
            auto& resource = *decision.substitute;
            if (request.derpbird_navigation_state) request.derpbird_navigation_state->click_to_load = resource.click_to_load;
            auto length = ByteString::number(resource.body.size());
            auto headers = HTTP::HeaderList::create({
                { "Content-Type"sv, resource.mime_type.bytes_as_string_view() },
                { "Content-Length"sv, length },
                { "Access-Control-Allow-Origin"sv, local_origin },
                { "Access-Control-Allow-Credentials"sv, "true"sv },
                { "Cache-Control"sv, "no-store"sv },
                { "X-Content-Type-Options"sv, "nosniff"sv },
            });
            on_headers_received->function()(nullptr, headers, 200u, {}, Requests::CacheState::NotCached);
            if (request.method() != "HEAD"sv && !resource.body.is_empty())
                on_data_received->function()(nullptr, Requests::ResponseData::from_bytes(resource.body.bytes()));
            on_complete->function()(true, {}, {}, {});
            return true;
        }
        if (decision.rewritten_url.has_value()) {
            // Go through Fetch's redirect machinery: preserve POST bodies, respect
            // redirect mode/limits, and reapply the normal security checks.
            auto location = decision.rewritten_url->serialize();
            // This response is browser-generated. The redirected network response
            // still has to pass its own CORS checks before any body is exposed.
            auto headers = HTTP::HeaderList::create({
                { "Location"sv, location.bytes_as_string_view() },
                { "Access-Control-Allow-Origin"sv, local_origin },
                { "Access-Control-Allow-Credentials"sv, "true"sv },
                { "Cache-Control"sv, "no-store"sv },
            });
            on_headers_received->function()(nullptr, headers, 307u, {}, Requests::CacheState::NotCached);
            on_complete->function()(true, {}, {}, {});
            return true;
        }
        return false;
    };
    if (deliver_filter_response(decision)) return nullptr;
    if (decision.blocked) {
        on_complete->function()(false, {}, "Request was blocked"sv, {});
        return nullptr;
    }

    if (url.scheme() == "about"sv) {
        handle_about_load_request(
            request,
            [on_headers_received = move(on_headers_received), on_data_received = move(on_data_received), on_complete, request](ReadonlyBytes data, Requests::RequestTimingInfo const& timing_info, HTTP::HeaderList const& response_headers) {
                log_success(request);
                on_headers_received->function()(nullptr, response_headers, {}, {}, Requests::CacheState::NotCached);
                on_data_received->function()(nullptr, Requests::ResponseData::from_bytes(data));
                on_complete->function()(true, timing_info, {}, {});
            },
            GC::create_function(m_heap, GC::captures(on_complete, request), [](auto& on_complete, auto& request, ByteString const& message) {
                log_failure(request, message);
                Requests::RequestTimingInfo fixme_implement_timing_info {};
                on_complete->function()(false, fixme_implement_timing_info, StringView(message), {});
            }));
        return nullptr;
    }

    if (url.scheme() == "resource"sv) {
        handle_resource_load_request(
            request,
            [on_headers_received = move(on_headers_received), on_data_received = move(on_data_received), on_complete](FileLoadResult const& load_result) {
                on_headers_received->function()(nullptr, load_result.response_headers, {}, {}, Requests::CacheState::NotCached);
                on_data_received->function()(nullptr, Requests::ResponseData::from_bytes(load_result.data));
                on_complete->function()(true, load_result.timing_info, {}, {});
            },
            [on_complete](ByteString const& message) {
                Requests::RequestTimingInfo fixme_implement_timing_info {};
                on_complete->function()(false, fixme_implement_timing_info, StringView(message), {});
            });
        return nullptr;
    }

    if (url.scheme() == "file"sv) {
        handle_file_load_request(
            request,
            [request, on_headers_received = move(on_headers_received), on_data_received = move(on_data_received), on_complete](FileLoadResult const& load_result) {
                log_success(request);
                on_headers_received->function()(nullptr, load_result.response_headers, {}, {}, Requests::CacheState::NotCached);
                on_data_received->function()(nullptr, Requests::ResponseData::from_bytes(load_result.data));
                on_complete->function()(true, load_result.timing_info, {}, {});
            },
            [on_complete, request](ByteString const& message) {
                log_failure(request, message);
                on_complete->function()(false, {}, StringView(message), {});
            });

        return nullptr;
    }

    if (!url.scheme().is_one_of("http"sv, "https"sv)) {
        auto not_implemented_error = ByteString::formatted("Protocol not implemented: {}", url.scheme());
        log_failure(request, not_implemented_error);
        on_complete->function()(false, {}, not_implemented_error, {});
        return nullptr;
    }

    // We cannot evaluate PAC files yet, and connecting directly would ignore the proxy that was asked for.
    if (proxy_autoconfig_url().has_value()) {
        dbgln("FIXME: Evaluate the PAC file to find the proxy for {}; failing the request", url);
        auto error = "PAC proxy configurations are not supported"sv;
        log_failure(request, error);
        on_complete->function()(false, {}, error, {});
        return nullptr;
    }

    // RequestServer evaluates header rules after cookies, validators and
    // generated headers exist, including disk-cache hits.
    auto protocol_request = start_network_request(request, transfer_lease);
    if (!protocol_request) {
        on_complete->function()(false, {}, "Failed to start network request"sv, {});
        return nullptr;
    }

    auto network_transport = Fetch::Engine::NetworkTransportHandle::create(*protocol_request);
    struct CanonicalAction final : RefCounted<CanonicalAction> {
        Optional<ContentBlocker::Decision> decision;
    };
    auto canonical_action = adopt_ref(*new CanonicalAction);
    protocol_request->set_canonical_url_policy([request, canonical_action](Vector<URL::URL> const& aliases, Vector<String> const& addresses) {
        if (auto page = request.page(); page && !page->content_blocking_enabled_for_url(request.top_level_creation_url())) return true;
        auto type = ContentBlocker::resource_type_from_fetch_metadata(request.destination(), request.initiator_type(), request.request_mode());
        if (request.derpbird_subframe_navigation) type = ContentBlocker::ResourceType::Subdocument;
        for (auto const& address : addresses) {
            auto decision = ContentBlocker::the().check(*request.url(), request.source_url().value_or(*request.url()), type, request.method(), request.top_level_creation_url(), address);
            if (!decision.blocked && !decision.rewritten_url.has_value()) continue;
            if (auto page = request.page()) page->client().page_did_block_resource(*request.url(), "IP address"sv, decision.matched_rule);
            canonical_action->decision = move(decision);
            return false;
        }
        if (aliases.is_empty()) return true;
        StringBuilder address_context;
        for (auto const& address : addresses) {
            if (!address_context.is_empty()) address_context.append('\n');
            address_context.append(address);
        }
        StringBuilder names;
        for (auto const& alias : aliases) {
            if (!names.is_empty()) names.append('\n');
            names.append(alias.serialized_host());
        }
        auto decision = ContentBlocker::the().check_canonical_hosts(*request.url(), request.source_url().value_or(*request.url()), type, request.method(), names.string_view(), request.top_level_creation_url(), address_context.string_view());
        if (!decision.blocked && !decision.rewritten_url.has_value()) return true;
        if (auto page = request.page()) page->client().page_did_block_resource(*request.url(), "CNAME"sv, decision.matched_rule);
        canonical_action->decision = move(decision);
        // Terminate the transport before any cookies/body reach the destination.
        // Its completion callback supplies the same local/307 response as URL rules.
        return false;
    });


    auto protocol_headers_received = [on_headers_received = move(on_headers_received), request, &protocol_request = *protocol_request, network_transport](auto const& response_headers, auto status_code, auto const& reason_phrase, auto cache_state) {
        // Never mutate transport/cache-owned headers or replace the site's CSP.
        // Recheck policy at response time, including the top-level site switch.
        NonnullRefPtr<HTTP::HeaderList const> effective_headers = response_headers;
        auto page = request.page();
        bool enabled = !page || page->content_blocking_enabled_for_url(request.top_level_creation_url());
        auto type = ContentBlocker::resource_type_from_fetch_metadata(request.destination(), request.initiator_type(), request.request_mode());
        if (request.derpbird_subframe_navigation) type = ContentBlocker::ResourceType::Subdocument;
        auto const& ip_addresses = protocol_request.response_policy_ip_addresses();
        auto policy = enabled ? ContentBlocker::the().csp_for_response(*request.url(), request.source_url().value_or(*request.url()), type, request.method(), request.top_level_creation_url(), ip_addresses) : String {};
        auto removals = protocol_request.response_header_policy().value_or_lazy_evaluated([&] {
            return enabled ? ContentBlocker::the().response_header_removals(*request.url(), type) : u8 { 0 };
        });
        auto permissions = enabled ? ContentBlocker::the().response_modifiers(*request.url(), request.source_url().value_or(*request.url()), type, request.method(), true, request.top_level_creation_url(), ip_addresses) : String {};
        if (!policy.is_empty() || !permissions.is_empty() || removals) {
            auto headers = HTTP::HeaderList::create(response_headers->headers());
            headers->set_canonical_host_names(response_headers->canonical_host_names());
            // Derived policy must not survive a later rule change in Fetch cache.
            headers->set({ "Cache-Control"sv, "no-store"sv });
            Array names { "location"sv, "refresh"sv, "report-to"sv, "set-cookie"sv };
            for (size_t i = 0; i < names.size(); ++i)
                if (removals & (1 << i)) headers->delete_(names[i]);
            if (!policy.is_empty()) headers->append({ "Content-Security-Policy"sv, policy.bytes_as_string_view() });
            if (!permissions.is_empty()) headers->append({ "Permissions-Policy"sv, permissions.bytes_as_string_view() });
            effective_headers = move(headers);
            if (page) {
                auto log_rules = [&](u8 action, StringView label, String const& fallback) {
                    auto rules = ContentBlocker::the().action_rules(*request.url(), request.source_url().value_or(*request.url()), type, request.method(), request.top_level_creation_url(), action, ip_addresses);
                    if (rules.is_empty()) page->client().page_did_block_resource(*request.url(), label, fallback);
                    else for (auto const& rule : rules) page->client().page_did_block_resource(*request.url(), label, rule);
                };
                if (!policy.is_empty()) log_rules(0, "Filter: CSP"sv, policy);
                if (!permissions.is_empty()) log_rules(1, "Filter: permissions"sv, permissions);
                for (size_t i = 0; i < names.size(); ++i) {
                    if ((removals & (1 << i)) && response_headers->contains(names[i]))
                        log_rules(i + 2, "Filter: response"sv, MUST(String::formatted("Removed response header: {}", names[i])));
                }
            }
        }

        on_headers_received->function()(network_transport.ptr(), effective_headers, move(status_code), reason_phrase, cache_state);
    };

    auto protocol_data_received = [on_data_received = move(on_data_received), network_transport](auto data) {
        on_data_received->function()(network_transport.ptr(), move(data));
    };

    auto protocol_complete = [this, on_complete = move(on_complete), request, &protocol_request = *protocol_request, network_transport, canonical_action, deliver_filter_response = move(deliver_filter_response)](u64 total_size, Requests::RequestTimingInfo const& timing_info, Optional<Requests::NetworkError> const& network_error) {
        finish_network_request(protocol_request);

        NetworkRequestEnd network_request_end {
            .request_id = network_transport->request_id(),
            .total_size = total_size,
            .network_error = network_error,
        };
        if (network_error.has_value() && canonical_action->decision.has_value()
            && deliver_filter_response(*canonical_action->decision))
            return;
        if (!network_error.has_value()) {
            log_success(request);
            on_complete->function()(true, timing_info, {}, network_request_end);
        } else {
            auto error_description = MUST(String::formatted(
                "Request finished with error: {}",
                network_error_to_string(*network_error)));
            log_failure(request, error_description);
            on_complete->function()(false, timing_info, error_description, network_request_end);
        }
    };

    auto protocol_cached_body_available = [on_cached_body_available = move(on_cached_body_available)](auto data) {
        on_cached_body_available->function()(move(data));
    };

    protocol_request->set_unbuffered_request_callbacks(move(protocol_headers_received), move(protocol_data_received), move(protocol_cached_body_available), move(protocol_complete));
    protocol_request->set_stop_callback([this, &protocol_request = *protocol_request] {
        finish_network_request(protocol_request);
    });
    return network_transport;
}

static NonnullRefPtr<HTTP::HeaderList const> headers_with_normalized_values(HTTP::HeaderList const& headers)
{
    if (all_of(headers, [](auto const& header) { return HTTP::is_header_value(header.value); }))
        return headers;

    auto normalized_headers = HTTP::HeaderList::create();
    for (auto const& header : headers)
        normalized_headers->append_with_normalized_name({ header.name, HTTP::normalize_header_value(header.value) });
    return normalized_headers;
}

RefPtr<Requests::Request> ResourceLoader::start_network_request(LoadRequest const& request, Requests::RequestClient::TransferLease transfer_lease)
{
    // FIXME: We could put this request in a queue until the client connection is re-established.
    auto request_client = request_client_for(request.network_isolation_key());
    if (!request_client) {
        log_failure(request, "RequestServer is currently unavailable"sv);
        return nullptr;
    }

    auto cache_miss_notification = request.destination() == Fetch::Infrastructure::Request::Destination::Font
        ? Requests::RequestClient::CacheMissNotification::Yes
        : Requests::RequestClient::CacheMissNotification::No;
    // NB: Workers have a Page shim without a navigable or a browser page ID. Their
    //     requests are attributed through the worker's browser-level owners.
    auto originating_page_id = request.page() && request.page()->has_top_level_traversable() ? request.page()->client().id().value() : 0;
    auto headers = headers_with_normalized_values(request.headers());
    auto response_type = ContentBlocker::resource_type_from_fetch_metadata(request.destination(), request.initiator_type(), request.request_mode());
    if (request.derpbird_subframe_navigation) response_type = ContentBlocker::ResourceType::Subdocument;
    u8 query_response_policy = (ContentBlocker::the().has_header_filters() || response_type == ContentBlocker::ResourceType::Document || response_type == ContentBlocker::ResourceType::Subdocument) ? 1 : 0;
    if (!request.page() || request.page()->content_blocking_enabled_for_url(request.top_level_creation_url())) query_response_policy |= 2;
    if (ContentBlocker::the().has_ip_address_filters() && (query_response_policy & 2)) query_response_policy |= 9;
    if (ContentBlocker::the().has_request_header_filters() && (!request.page() || request.page()->content_blocking_enabled_for_url(request.top_level_creation_url()))) query_response_policy |= 4;
    bool suppress_response_cookies = false;
    if (!request.page() || request.page()->content_blocking_enabled_for_url(request.top_level_creation_url())) {
        auto type = ContentBlocker::resource_type_from_fetch_metadata(request.destination(), request.initiator_type(), request.request_mode());
        suppress_response_cookies = ContentBlocker::the().response_header_removals(*request.url(), type) & 8;
    }
    auto protocol_request = request_client->start_request(request.method(), request.url().value(), request.same_site_context(), *headers, request.body(), request.cache_mode(), request.include_credentials(), transfer_lease, {}, cache_miss_notification, originating_page_id, request.network_isolation_key(), suppress_response_cookies, query_response_policy);
    if (!protocol_request) {
        log_failure(request, "Failed to initiate load"sv);
        return nullptr;
    }

    protocol_request->set_response_header_policy([request, response_type](HTTP::HeaderList const& headers, StringView ip_addresses) -> u8 {
        if (auto page = request.page(); page && !page->content_blocking_enabled_for_url(request.top_level_creation_url())) return 0;
        auto& blocker = ContentBlocker::the();
        auto mask = blocker.response_header_removals(*request.url(), response_type);
        auto decision = blocker.check_response(*request.url(), request.source_url().value_or(*request.url()), response_type, request.method(), headers, request.top_level_creation_url(), ip_addresses);
        if (decision.blocked) {
            mask |= 16;
            if (auto page = request.page()) page->client().page_did_block_resource(*request.url(), "Response header"sv, decision.matched_rule);
        }
        return mask;
    });
    protocol_request->set_request_header_policy([request, response_type](HTTP::HeaderList const& headers, StringView ip_addresses) {
        if (auto page = request.page(); page && !page->content_blocking_enabled_for_url(request.top_level_creation_url())) return true;
        auto decision = ContentBlocker::the().check_request_headers(*request.url(), request.source_url().value_or(*request.url()), response_type, request.method(), headers, request.top_level_creation_url(), ip_addresses);
        if (decision.blocked) {
            if (auto page = request.page()) page->client().page_did_block_resource(*request.url(), "Request header"sv, decision.matched_rule);
        }
        return !decision.blocked;
    });




    m_active_requests.set(*protocol_request);
    return protocol_request;
}

void ResourceLoader::finish_network_request(NonnullRefPtr<Requests::Request> protocol_request)
{
    if (!m_active_requests.remove(protocol_request))
        warnln("ResourceLoader: finish_network_request() called for request {}, which is not active", protocol_request->id());
}

}
