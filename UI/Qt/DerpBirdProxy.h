#pragma once
#include <AK/JsonArray.h>
#include <LibWebView/Application.h>
#include <LibWebView/Profile.h>
#include <UI/Qt/StringUtils.h>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QDir>

namespace Ladybird {
class DerpBirdProxy {
public:
    static QString path() { return qstring_from_ak_string(WebView::Application::profile().paths().config) + "/DerpBirdProxy.json"; }
    static ErrorOr<JsonObject> state()
    {
        QFile file(path());
        if (!file.exists()) {
            JsonObject options;
            options.set("schemaVersion"sv, 2);
            JsonObject result;
            result.set("options"sv, move(options));
            result.set("active"sv, "system"sv);
            result.set("revision"sv, 0);
            return result;
        }
        if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024) return Error::from_string_literal("Could not read proxy settings");
        auto bytes = file.readAll();
        auto value = TRY(JsonValue::from_string(StringView(bytes.constData(), bytes.size())));
        if (!value.is_object()) return Error::from_string_literal("Invalid proxy settings");
        return value.as_object();
    }
    static ErrorOr<JsonValue> replace(JsonObject const& request)
    {
        auto previous = TRY(state());
        auto revision = request.get_u32("revision"sv);
        if (!revision.has_value() || revision != previous.get_u32("revision"sv)) return Error::from_string_literal("Proxy settings changed in another window. Reopen the editor.");
        auto options = request.get_object("options"sv);
        auto active = request.get_string("active"sv);
        if (!options.has_value() || !active.has_value()) return Error::from_string_literal("Invalid proxy settings");
        auto configuration = WebView::Application::request_server_options().proxy_configuration;
        TRY(configuration.set_omega(*options, *active));
        JsonObject next;
        next.set("options"sv, *options);
        next.set("active"sv, *active);
        next.set("revision"sv, *revision + 1);
        auto bytes = JsonValue(next).serialized().to_byte_string();
        if (bytes.length() > 16 * 1024 * 1024) return Error::from_string_literal("Proxy settings are too large");
        QDir().mkpath(QFileInfo(path()).absolutePath());
        QSaveFile file(path());
        if (!file.open(QIODevice::WriteOnly)) return Error::from_string_literal("Could not save proxy settings");
        if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) return Error::from_string_literal("Could not protect proxy settings");
        if (file.write(bytes.characters(), bytes.length()) != static_cast<qint64>(bytes.length()) || !file.commit()) return Error::from_string_literal("Could not save proxy settings");
        WebView::Application::the().set_proxy_configuration(move(configuration));
        return JsonValue(move(next));
    }
};
}
