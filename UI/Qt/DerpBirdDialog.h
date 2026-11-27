#pragma once

#include <AK/JsonObject.h>
#include <UI/Qt/JavaScriptDialog.h>
#include <UI/Qt/StringUtils.h>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

namespace Ladybird {

// A pending shell dialog belongs to its originating tab. The token binds replies
// to that specific request, including when two prompts have identical text.
class DerpBirdDialog {
public:
    AK_ALLOC_WITH_KMALLOC;
    using Type = JavaScriptDialog::Type;
    using Offer = JavaScriptDialog::OfferToBlockFurtherDialogs;
    using Reply = Function<void(bool, QString const&)>;

    JavaScriptDialog* legacy { nullptr };
    Function<void()> changed;
    Function<void(QString const&)> on_edit;
    void edit(QString const& text)
    {
        m_value = text;
        if (on_edit) on_edit(text);
    }
    Function<void(Type, bool, QString const&)> on_complete;

    bool is_open() const { return legacy ? legacy->is_open() : !m_id.isEmpty(); }
    Optional<Type> type() const { return legacy ? legacy->type() : m_type; }
    bool blocks_further_dialogs() const { return legacy ? legacy->blocks_further_dialogs() : m_block; }
    void set_title_icon(QIcon const& icon) { if (legacy) legacy->set_title_icon(icon); }

    void show_alert(QString const& site, QString const& message, Offer offer)
    {
        if (legacy) return legacy->show_alert(site, message, offer);
        page(Type::Alert, "alert", site, message, {}, offer);
    }
    void show_confirm(QString const& site, QString const& message, Offer offer)
    {
        if (legacy) return legacy->show_confirm(site, message, offer);
        page(Type::Confirm, "confirm", site, message, {}, offer);
    }
    void show_prompt(QString const& site, QString const& message, QString const& value, Offer offer)
    {
        if (legacy) return legacy->show_prompt(site, message, value, offer);
        page(Type::Prompt, "prompt", site, message, value, offer);
    }
    void show_before_unload(QString const& site, bool closing_window)
    {
        if (legacy) return legacy->show_before_unload(site);
        page(Type::BeforeUnload, closing_window ? "window-close" : "leave", site, "Changes on this page may be lost.", {}, Offer::No);
    }
    void show(QString kind, QString title, QString message, QString value, QString action, Reply reply)
    {
        if (is_open()) {
            reply(false, {});
            return;
        }
        m_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_state = {};
        put("id"sv, m_id);
        put("kind"sv, kind);
        put("title"sv, title);
        put("message"sv, message);
        put("action"sv, action);
        m_reply = move(reply);
        m_block = false;
        set_value(value);
    }
    bool is_kind(StringView kind) const { return is_open() && m_state.get_string("kind"sv) == kind; }
    void configure(StringView key, JsonValue value) { m_state.set(key, move(value)); }
    JsonObject const& state() const { return m_state; }
    bool matches(String const& id) const { return is_open() && ak_string_from_qstring(m_id) == id; }
    void set_prompt_text(QString const& text)
    {
        if (legacy) return legacy->set_prompt_text(text);
        if (m_type == Type::Prompt) set_value(text);
    }
    void respond(bool accepted, QString value, bool block)
    {
        if (!is_open()) return;
        m_block = block && m_state.get_bool("offerBlock"sv).value_or(false);
        auto reply = move(m_reply);
        reset();
        if (reply) reply(accepted, value);
    }
    void accept()
    {
        if (legacy) return legacy->accept();
        respond(true, m_value, false);
    }
    void dismiss()
    {
        if (legacy) return legacy->dismiss();
        respond(m_type == Type::Alert, m_value, false);
    }
    void reset()
    {
        if (legacy) return legacy->reset();
        m_reply = {};
        on_edit = {};
        m_type.clear();
        m_id.clear();
        m_state = {};
        if (changed) changed();
    }

private:
    void put(StringView key, QString const& value) { m_state.set(key, ak_string_from_qstring(value)); }
    void set_value(QString const& value)
    {
        m_value = value;
        put("value"sv, value);
        if (changed) changed();
    }
    void page(Type type, QString kind, QString title, QString message, QString value, Offer offer)
    {
        if (is_open()) {
            m_block = false;
            on_complete(type, type == Type::Alert, {});
            return;
        }
        show(kind, title, message, value, {}, [this, type](bool accepted, QString const& text) {
            on_complete(type, accepted, text);
        });
        m_type = type;
        m_state.set("offerBlock"sv, offer == Offer::Yes);
    }
    Optional<Type> m_type;
    JsonObject m_state;
    QString m_id;
    QString m_value;
    Reply m_reply;
    bool m_block { false };
};

}
