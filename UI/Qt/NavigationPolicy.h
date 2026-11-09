#pragma once

#include <LibURL/URL.h>

namespace Ladybird {

inline bool is_public_navigation_url(URL::URL const& url)
{
    if (url.scheme().is_one_of("http"sv, "https"sv, "file"sv))
        return true;
    return url.scheme() == "about"sv
        && url.serialize_path().is_one_of("blank"sv, "history"sv, "downloads"sv,
            "bookmarks"sv, "settings"sv, "version"sv, "newtab"sv);
}

}
