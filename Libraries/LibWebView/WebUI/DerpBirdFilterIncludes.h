#pragma once
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/RefCounted.h>
#include <AK/StringBuilder.h>
#include <LibURL/Parser.h>
#include <LibURL/Origin.h>
#include <LibWeb/Page/DerpBirdFilterPreprocessor.h>

namespace WebView {

class DerpBirdFilterIncludes final : public RefCounted<DerpBirdFilterIncludes> {
public:
    explicit DerpBirdFilterIncludes(URL::URL root) : m_root(move(root)) { }
    static bool allowed(URL::URL const& base, URL::URL const& target)
    {
        if (!target.scheme().is_one_of("http"sv, "https"sv) || target.includes_credentials() || target.fragment().has_value()) return false;
        if (base.origin() != target.origin()) return false;
        auto directory = base.serialize_path();
        auto slash = directory.find_last('/');
        if (!slash.has_value()) return false;
        return target.serialize_path().starts_with(directory.substring_view(0, *slash + 1));
    }
    ErrorOr<void> accept(URL::URL const& key, URL::URL const& final_url, StringView text)
    {
        if (m_documents.size() >= 64 || text.length() > 64 * MiB - m_bytes) return Error::from_string_literal("Included filter lists exceed limits");
        m_bytes += text.length();
        auto processed = TRY(Web::preprocess_derpbird_filter_list(text));
        Document document { final_url, move(processed) };
        for (auto line : document.text.bytes_as_string_view().split_view('\n')) {
            auto target = TRY(include_url(final_url, line));
            if (!target.has_value()) continue;
            auto address = target->serialize();
            if (m_scheduled.contains(address)) continue;
            if (m_scheduled.size() >= 64) return Error::from_string_literal("Too many included filter lists");
            m_scheduled.set(address);
            m_pending.append(target.release_value());
        }
        m_documents.set(key.serialize(), move(document));
        return {};
    }
    Optional<URL::URL> next()
    {
        while (!m_pending.is_empty()) {
            auto url = m_pending.take_first();
            if (!m_documents.contains(url.serialize())) return url;
        }
        return {};
    }
    ErrorOr<String> finish() const
    {
        StringBuilder output;
        HashTable<String> ancestors;
        TRY(expand(m_root, ancestors, output));
        return output.to_string();
    }
private:
    static ErrorOr<Optional<URL::URL>> include_url(URL::URL const& base, StringView line)
    {
        line = line.trim_whitespace();
        if (!line.starts_with("!#include "sv) && !line.starts_with("!#include\t"sv)) return Optional<URL::URL> {};
        auto name = line.substring_view(10).trim_whitespace();
        auto separator = name.find_any_of(" \t\r"sv);
        if (separator.has_value()) name = name.substring_view(0, *separator);
        if (name.is_empty() || name.starts_with('/') || name.contains(':') || name.contains('\\')) return Error::from_string_literal("Invalid filter include path");
        auto target = URL::Parser::basic_parse(name, base);
        if (!target.has_value() || !allowed(base, *target)) return Error::from_string_literal("Filter include escapes its directory");
        return target;
    }
    ErrorOr<void> expand(URL::URL const& url, HashTable<String>& ancestors, StringBuilder& output) const
    {
        auto key = url.serialize();
        if (ancestors.size() >= 16 || ancestors.contains(key)) return Error::from_string_literal("Recursive filter include");
        auto document = m_documents.get(key);
        if (!document.has_value()) return Error::from_string_literal("Missing filter include");
        ancestors.set(key);
        for (auto line : document->text.bytes_as_string_view().split_view('\n', SplitBehavior::KeepEmpty)) {
            auto target = TRY(include_url(document->url, line));
            if (target.has_value()) TRY(expand(*target, ancestors, output));
            else { output.append(line); output.append('\n'); }
            if (output.length() > 64 * MiB) return Error::from_string_literal("Expanded filter list exceeds limit");
        }
        ancestors.remove(key);
        return {};
    }
    struct Document { URL::URL url; String text; };
    URL::URL m_root;
    size_t m_bytes { 0 };
    HashMap<String, Document> m_documents;
    HashTable<String> m_scheduled;
    Vector<URL::URL> m_pending;
};

}
