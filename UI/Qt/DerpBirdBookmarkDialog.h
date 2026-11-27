#pragma once
#include <UI/Qt/BookmarkStore.h>
#include <UI/Qt/DerpBirdDialog.h>
#include <QJsonObject>

namespace Ladybird {

inline void show_web_bookmark_dialog(DerpBirdDialog& dialog, QString heading, bool folder, QString name, QString url, ReadonlySpan<BookmarkItem> roots, Optional<String const&> selected, Optional<String const&> excluded, Optional<String const&> editing_id, DerpBirdDialog::Reply reply)
{
    if (dialog.is_open()) { reply(false, {}); return; }
    JsonArray choices;
    auto append = [&](auto& self, ReadonlySpan<BookmarkItem> items, QString prefix, String parent) -> void {
        for (auto const& item : items) {
            if (editing_id.has_value() && item.id == *editing_id)
                dialog.configure("folder"sv, parent);
            if (!item.is_folder() || item.id == excluded) continue;
            auto path = prefix + qstring_from_ak_string(item.folder().title.value_or("(no title)"_string));
            JsonObject choice;
            choice.set("id"sv, item.id);
            choice.set("name"sv, ak_string_from_qstring(path));
            choices.must_append(move(choice));
            self(self, item.folder().children, path + " / ", item.id);
        }
    };
    dialog.show(folder ? "bookmark-folder" : "bookmark", heading, {}, name, "Save", move(reply));
    dialog.configure("url"sv, ak_string_from_qstring(url));
    dialog.configure("folder"sv, selected.value_or(String {}));
    dialog.configure("removable"sv, editing_id.has_value());
    append(append, roots, {}, {});
    dialog.configure("folders"sv, move(choices));
}

}
