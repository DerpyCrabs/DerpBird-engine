#pragma once

#include <AK/ByteBuffer.h>
#include <AK/RefCounted.h>
#include <AK/Utf16StringBuilder.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/HTML/HTMLLinkElement.h>
#include <LibWeb/HTML/Parser/HTMLEncodingDetection.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/Loader/ContentBlocker.h>
#include <LibWeb/Page/DerpBirdScriptWorld.h>

namespace Web {

// Only documents with applicable HTML rules use this buffered path. The normal
// incremental parser remains untouched for every other navigation.
class DerpBirdHTMLFiltering final : public RefCounted<DerpBirdHTMLFiltering> {
public:
    static bool start(GC::Ref<DOM::Document> document, Fetch::Infrastructure::ResponseView const& response, GC::Ptr<DOM::Document> container)
    {
        if (!document->content_blocking_enabled() || !response.body()) return false;
        auto rules = ContentBlocker::the().html_filter_rules(document->url());
        if (rules.is_empty()) return false;
        auto state = adopt_ref(*new DerpBirdHTMLFiltering(document, container, response.header_list(), move(rules)));
        auto& realm = document->relevant_settings_object().realm();
        response.body()->incrementally_read(realm,
            GC::create_function(GC::Heap::the(), [state](ByteBuffer bytes) {
                if (state->m_failed) return;
                if (bytes.size() > maximum_body_size - state->m_bytes.size()) {
                    state->m_failed = true;
                    state->m_bytes.clear();
                    return;
                }
                if (state->m_bytes.try_append(bytes).is_error()) state->m_failed = true;
            }),
            GC::create_function(GC::Heap::the(), [state] { state->finish(); }),
            GC::create_function(GC::Heap::the(), [state](JS::Value) { state->m_failed = true; state->finish(); }),
            GC::Ref { realm.global_object() });
        return true;
    }

private:
    static constexpr size_t maximum_body_size = 16 * MiB;
    DerpBirdHTMLFiltering(GC::Ref<DOM::Document> document, GC::Ptr<DOM::Document> container, NonnullRefPtr<HTTP::HeaderList> headers, Utf16String rules)
        : m_document(document), m_container(container), m_headers(move(headers)), m_rules(move(rules)) { }

    ErrorOr<Utf16String> filter(Utf16String input)
    {
        auto world = TRY(DerpBirdScriptWorld::create(*m_document));
        auto& realm = world->realm();
        realm.global_object().define_direct_property("__derpbirdHTMLInput"_utf16_fly_string,
            JS::PrimitiveString::create(realm.vm(), move(input)), JS::default_attributes);
        Utf16StringBuilder script;
        script.append(ContentBlocker::html_filter_runtime());
        script.append_ascii("\n__derpbirdFilterHTML(__derpbirdHTMLInput,"sv);
        script.append(m_rules);
        script.append_ascii(");"sv);
        auto result = TRY(world->run(script.view()));
        if (result.is_abrupt() || !result.value().is_string())
            return Error::from_string_literal("HTML response filtering failed");
        return result.value().as_string().utf16_string();
    }

    void finish()
    {
        if (m_finished) return;
        m_finished = true;
        auto state = NonnullRefPtr { *this };
        auto parse = [state] {
            auto& document = *state->m_document;
            if (!document.is_fully_active()) return;
            Utf16String source = "<!doctype html><title>Page unavailable</title><h1>Unable to load this page</h1><p>HTML response filtering failed or the response exceeded its size limit.</p>"_utf16;
            if (!state->m_failed) {
                auto mime = Fetch::Infrastructure::extract_mime_type(state->m_headers);
                auto encoding = HTML::run_encoding_sniffing_algorithm(document, state->m_bytes, mime, state->m_container.ptr());
                auto decoder = TextCodec::decoder_for(encoding.encoding);
                if (decoder.has_value()) {
                    auto decoded = TextCodec::decode_to_utf16(state->m_bytes, *decoder);
                    if (!decoded.is_error()) {
                        document.set_encoding(Utf16String::from_utf8(encoding.encoding.view()));
                        auto filtered = state->filter(decoded.release_value());
                        if (!filtered.is_error()) source = filtered.release_value();
                    }
                }
            }
            state->m_bytes.clear();
            document.set_source(source);
            auto source_encoding = document.encoding().value_or("UTF-8"_utf16);
            auto parser = HTML::HTMLParser::create_for_decoded_string(document, source.utf16_view(), HTML::ParserScriptingMode::Normal, source_encoding.utf16_view());
            parser->set_allow_declarative_shadow_roots(HTML::HTMLParser::AllowDeclarativeShadowRoots::Yes);
            parser->run(document.url());
            HTML::HTMLLinkElement::process_link_headers(document, state->m_headers, document.url(), HTML::HTMLLinkElement::LinkHeaderPhase::Media);
        };
        if (m_document->ready_to_run_scripts()) parse();
        else m_document->set_deferred_parser_start(GC::create_function(GC::Heap::the(), move(parse)));
    }

    GC::Root<DOM::Document> m_document;
    GC::Root<DOM::Document> m_container;
    NonnullRefPtr<HTTP::HeaderList> m_headers;
    Utf16String m_rules;
    ByteBuffer m_bytes;
    bool m_failed { false };
    bool m_finished { false };
};

}
