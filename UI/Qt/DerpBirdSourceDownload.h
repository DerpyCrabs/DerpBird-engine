#pragma once
#include <AK/JsonObject.h>
#include <UI/Qt/HubConnection.h>
#include <QPointer>
#include <QTimer>
namespace Ladybird {
class DerpBirdSourceDownload final : public QObject {
public:
    using Completion = Function<void(ErrorOr<JsonValue>)>;
    DerpBirdSourceDownload(QObject* parent, String address, Completion completion)
        : QObject(parent), m_address(move(address)), m_completion(move(completion)) { }
    ~DerpBirdSourceDownload() override { finish(Error::from_string_literal("Source download cancelled"), false); }
    void cancel() { finish(Error::from_string_literal("Source download cancelled")); }
    void start()
    {
        JsonObject payload; payload.set("url"sv, m_address); payload.set("limit"sv, 2 * MiB);
        m_request = HubConnection::the().request_derpbird(0, "source.fetch"_string, move(payload), [self = QPointer(this)](ErrorOr<JsonValue> result) {
            if (!self) return;
            self->m_request = 0; self->finish(move(result));
        });
    }
private:
    void finish(ErrorOr<JsonValue> result, bool retire = true)
    {
        if (m_done) return;
        m_done = true;
        if (m_request && HubConnection::the_if_connected()) HubConnection::the().cancel_derpbird_request(exchange(m_request, 0));
        auto completion = move(m_completion);
        if (retire) deleteLater();
        completion(move(result));
    }
    String m_address;
    Completion m_completion;
    u64 m_request { 0 };
    bool m_done { false };
};
}
