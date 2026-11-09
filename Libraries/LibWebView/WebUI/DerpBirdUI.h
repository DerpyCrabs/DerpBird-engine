#pragma once

#include <AK/HashTable.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebView/WebUI.h>

namespace WebView {

class DerpBirdUI final : public WebUI {
    WEB_UI(DerpBirdUI);

private:
    virtual void register_interfaces() override;
    void request(JsonValue const&);
    bool is_current() const;
    Optional<Web::HTML::CrossProcessId> m_document_id;
    HashTable<u64> m_pending;
};

} // namespace WebView
