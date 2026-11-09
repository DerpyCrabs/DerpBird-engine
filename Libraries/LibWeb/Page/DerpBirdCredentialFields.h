#pragma once

#include <AK/CharacterTypes.h>
#include <AK/Function.h>
#include <LibGC/Root.h>
#include <LibGC/Weak.h>
#include <LibURL/Origin.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/UIEvents/InputEvent.h>

namespace Web {

// Renderer-side field selection, not permission to disclose a saved credential.
// A native host must authorize the exact document/origin and user's selection
// before invoking fill. This object holds weak fields and never a stored secret.
class DerpBirdCredentialFields {
public:
    enum class Kind { Login, NewPassword };
    struct Values { String username; String password; };
    struct SubmittedValues {
        HTML::CrossProcessId document_id;
        String origin;
        Values values;
        Kind kind;
    };
    // Snapshot only: the host must still authorize prompting/storage. In
    // particular, a submitted form does not prove successful authentication.
    static WEB_API ErrorOr<SubmittedValues> capture_form(HTML::HTMLFormElement&, bool require_visible = false);
    static ErrorOr<DerpBirdCredentialFields> select(HTML::HTMLInputElement& password, bool require_visible = false)
    {
        return select_impl(password, require_visible, false);
    }
    static ErrorOr<DerpBirdCredentialFields> select_from_focus(HTML::HTMLInputElement& focused, bool require_visible = false)
    {
        if (focused.type_state() == HTML::HTMLInputElement::TypeAttributeState::Password)
            return select(focused, require_visible);
        if (!focused.document().is_fully_active() || focused.document().origin().is_opaque()
            || !focused.document().url().scheme().is_one_of("http"sv, "https"sv) || !eligible(focused) || !username_type(focused) || TRY(token(focused, "one-time-code"sv)))
            return Error::from_string_literal("Not a login field");
        auto root = focused.get_root_node();
        size_t visited = 0, candidates = 0;
        Optional<DerpBirdCredentialFields> selected;
        for (auto* node = root.ptr(); node; node = node->next_in_pre_order(root.ptr())) {
            if (++visited > 65536) return Error::from_string_literal("Credential field scan exceeds limit");
            auto* password = as_if<HTML::HTMLInputElement>(node);
            if (!password || password->form() != focused.form() || password->type_state() != HTML::HTMLInputElement::TypeAttributeState::Password) continue;
            if (++candidates > 8) return Error::from_string_literal("Too many password fields");
            auto fields = select(*password, require_visible);
            if (fields.is_error() || fields.value().kind() != Kind::Login || fields.value().m_username.ptr().ptr() != &focused) continue;
            if (selected.has_value()) return Error::from_string_literal("Ambiguous login fields");
            selected = fields.release_value();
        }
        if (!selected.has_value() && candidates == 0 && TRY(token(focused, "username"sv)) && (!require_visible || visible(focused)))
            return DerpBirdCredentialFields(focused.document(), nullptr, &focused, Kind::Login, require_visible);
        if (!selected.has_value()) return Error::from_string_literal("No matching login form");
        return selected.release_value();
    }
private:
    static ErrorOr<DerpBirdCredentialFields> select_impl(HTML::HTMLInputElement& password, bool require_visible, bool validated_password_group)
    {
        auto& document = password.document();
        if (!document.is_fully_active() || document.origin().is_opaque()
            || !document.url().scheme().is_one_of("http"sv, "https"sv)
            || !eligible(password) || password.type_state() != HTML::HTMLInputElement::TypeAttributeState::Password)
            return Error::from_string_literal("Password field is unavailable");
        auto kind = TRY(token(password, "new-password"sv)) ? Kind::NewPassword : Kind::Login;
        if (TRY(token(password, "one-time-code"sv)))
            return Error::from_string_literal("One-time codes are not saved passwords");
        auto root = password.get_root_node();
        HTML::HTMLInputElement* explicit_username = nullptr;
        HTML::HTMLInputElement* preceding = nullptr;
        bool passed_password = false;
        size_t passwords = 0, visited = 0, explicit_count = 0;
        for (auto* node = root.ptr(); node; node = node->next_in_pre_order(root.ptr())) {
            if (++visited > 65536) return Error::from_string_literal("Credential field scan exceeds limit");
            if (node == &password) passed_password = true;
            if (!is<HTML::HTMLInputElement>(*node)) continue;
            auto& input = as<HTML::HTMLInputElement>(*node);
            if (input.form() != password.form() || !eligible(input)) continue;
            if (input.type_state() == HTML::HTMLInputElement::TypeAttributeState::Password) ++passwords;
            if (!username_type(input)) continue;
            if (TRY(token(input, "username"sv))) { explicit_username = &input; ++explicit_count; }
            else if (!passed_password && !TRY(token(input, "one-time-code"sv))) preceding = &input;
        }
        if (explicit_count > 1 || (!explicit_count && passwords > 1 && !validated_password_group))
            return Error::from_string_literal("Ambiguous credential fields");
        auto* username = explicit_username ? explicit_username : preceding;
        if (require_visible && (!visible(password) || (username && !visible(*username))))
            return Error::from_string_literal("Credential fields are not visible");
        return DerpBirdCredentialFields(document, &password, username, kind, require_visible);
    }
public:
    Kind kind() const { return m_kind; }
    String const& origin() const { return m_origin; }
    HTML::CrossProcessId document_id() const { return m_document_id; }
    ErrorOr<Values> capture() const
    {
        TRY(validate());
        if (!m_password) return Error::from_string_literal("No password field to capture");
        if ((m_username && m_username->value().length_in_code_units() > 1024)
            || m_password->value().length_in_code_units() > 65536)
            return Error::from_string_literal("Credential values exceed limits");
        auto username = m_username ? m_username->value().to_utf8() : String {};
        auto password = m_password->value().to_utf8();
        if (username.bytes().size() > 1024 || password.bytes().size() > 65536)
            return Error::from_string_literal("Credential values exceed limits");
        return Values { move(username), move(password) };
    }
    ErrorOr<void> fill(String const& username, String const& password, Function<bool()> const& authorized = {})
    {
        if (m_kind != Kind::Login) return Error::from_string_literal("Do not fill saved credentials into a new-password field");
        if (username.bytes().size() > 1024 || password.bytes().size() > 65536)
            return Error::from_string_literal("Credential values exceed limits");
        TRY(validate());
        if (authorized && !authorized()) return Error::from_string_literal("Credential operation was revoked");
        // Native setters avoid page-overridden JS properties. Root fields across
        // synchronous input handlers, which may detach/adopt nodes or run GC.
        GC::Root password_field { m_password.ptr() };
        GC::Root username_field { m_username.ptr() };
        if (username_field) {
            auto result = username_field->set_value(Utf16String::from_utf8(username));
            if (result.is_exception()) return Error::from_string_literal("Cannot fill username");
            notify_input(*username_field);
            TRY(validate());
        }
        if (authorized && !authorized()) return Error::from_string_literal("Credential operation was revoked");
        if (!password_field) return {};
        auto result = password_field->set_value(Utf16String::from_utf8(password));
        if (result.is_exception()) return Error::from_string_literal("Cannot fill password");
        notify_input(*password_field);
        return {};
    }
private:
    static WEB_API bool visible(HTML::HTMLInputElement&);
    static WEB_API void notify_input(HTML::HTMLInputElement&);
    static bool eligible(HTML::HTMLInputElement const& input)
    {
        return input.is_connected() && input.enabled() && input.is_mutable()
            && !input.has_attribute(HTML::AttributeNames::hidden);
    }
    static bool username_type(HTML::HTMLInputElement const& input)
    {
        using Type = HTML::HTMLInputElement::TypeAttributeState;
        return input.type_state() == Type::Text || input.type_state() == Type::Email;
    }
    static ErrorOr<bool> token(HTML::HTMLInputElement const& input, StringView expected)
    {
        auto attribute = input.get_attribute_value(HTML::AttributeNames::autocomplete);
        if (attribute.length_in_code_units() > 1024) return Error::from_string_literal("Autocomplete attribute exceeds limit");
        auto text = attribute.to_utf8();
        for (auto part : text.bytes_as_string_view().split_view_if([](char ch) { return is_ascii_space(ch); }))
            if (part.equals_ignoring_ascii_case(expected)) return true;
        return false;
    }
    ErrorOr<void> validate() const
    {
        if (m_username_only) {
            if (!m_document || !m_username || !m_document->is_fully_active() || m_document->id() != m_document_id
                || m_document->origin().serialize() != m_origin || &m_username->document() != m_document.ptr().ptr()
                || !eligible(*m_username) || !username_type(*m_username) || m_username->form() != m_form.ptr().ptr()
                || m_username->get_root_node().ptr() != m_root.ptr().ptr() || !TRY(token(*m_username, "username"sv))
                || TRY(token(*m_username, "one-time-code"sv)) || (m_require_visible && !visible(*m_username)))
                return Error::from_string_literal("Username field changed or document expired");
            return {};
        }
        if (!m_document || !m_password || !m_document->is_fully_active() || m_document->id() != m_document_id
            || m_document->origin().serialize() != m_origin || &m_password->document() != m_document.ptr().ptr()
            || !eligible(*m_password) || m_password->type_state() != HTML::HTMLInputElement::TypeAttributeState::Password
            || m_password->form() != m_form.ptr().ptr() || m_password->get_root_node().ptr() != m_root.ptr().ptr()
            || (m_had_form && (!m_form || !m_form->is_connected()))
            || (m_had_username && (!m_username || !eligible(*m_username) || !username_type(*m_username)
                || &m_username->document() != m_document.ptr().ptr() || m_username->form() != m_form.ptr().ptr()
                || m_username->get_root_node().ptr() != m_root.ptr().ptr())))
            return Error::from_string_literal("Credential fields changed or document expired");
        if (m_require_visible && (!visible(*m_password) || (m_username && !visible(*m_username))))
            return Error::from_string_literal("Credential fields are not visible");
        if ((TRY(token(*m_password, "new-password"sv)) ? Kind::NewPassword : Kind::Login) != m_kind
            || TRY(token(*m_password, "one-time-code"sv)))
            return Error::from_string_literal("Credential field purpose changed");
        return {};
    }
    DerpBirdCredentialFields(DOM::Document& document, HTML::HTMLInputElement* password, HTML::HTMLInputElement* username, Kind kind, bool require_visible)
        : m_document(&document), m_password(password), m_username(username), m_form((password ? password : username)->form())
        , m_root((password ? password : username)->get_root_node().ptr()), m_document_id(document.id()), m_origin(document.origin().serialize())
        , m_kind(kind), m_had_username(username), m_had_form((password ? password : username)->form()), m_require_visible(require_visible), m_username_only(!password) { }
    GC::Weak<DOM::Document> m_document;
    GC::Weak<HTML::HTMLInputElement> m_password;
    GC::Weak<HTML::HTMLInputElement> m_username;
    GC::Weak<HTML::HTMLFormElement> m_form;
    GC::Weak<DOM::Node> m_root;
    HTML::CrossProcessId m_document_id;
    String m_origin;
    Kind m_kind;
    bool m_had_username;
    bool m_had_form;
    bool m_require_visible;
    bool m_username_only;
};

}
