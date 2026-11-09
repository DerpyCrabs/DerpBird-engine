#pragma once

#include <AK/Function.h>
#include <AK/OwnPtr.h>
#include <AK/Variant.h>
#include <LibWebView/WebUI/DerpBirdValueStore.h>
#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QObject>
#include <QThread>
#include <memory>
#include <atomic>
#include <deque>

namespace Ladybird {

// The host authorizes each operation before submitting it. No renderer-facing
// API accepts free storage identities. Own on the UI thread, close asynchronously
// during normal shutdown; the destructor drains accepted writes as a fallback.
class DerpBirdValueService final : public QObject {
public:
    using Store = WebView::DerpBirdValueStore;
    enum class Operation { Snapshot, Get, Keys, Apply, Selected };
    struct Request {
        Operation operation;
        String script_id;
        String key;
        Vector<Store::Update> updates;
        Optional<i64> expected_revision;
        String origin_token {};
    };
    using Response = Variant<Store::Snapshot, Optional<ByteBuffer>, Vector<String>, i64>;
    using Completion = Function<void(ErrorOr<Response>)>;
    // Called on the owning thread, after COMMIT and before that write's reply.
    // The authority supplies the origin token; scripts never choose a scope here.
    using ChangeCallback = Function<void(String const&, String const&, Store::Commit const&, u64)>;
    void set_change_callback(ChangeCallback callback)
    {
        VERIFY(QThread::currentThread() == thread());
        m_change_callback = callback ? std::make_shared<ChangeCallback>(move(callback)) : nullptr;
    }

    u64 last_sequence() const { VERIFY(QThread::currentThread() == thread()); return m_sequence; }

    // An empty directory is permitted only for a private, memory-backed session.
    explicit DerpBirdValueService(QString directory, bool is_private, QObject* parent = nullptr)
        : QObject(parent)
        , m_state(std::make_shared<State>(move(directory), is_private))
        , m_worker(new QObject)
    {
        m_worker->moveToThread(&m_thread);
        connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        m_thread.setObjectName("DerpBird GM values");
        m_thread.start();
    }

    ~DerpBirdValueService() override
    {
        VERIFY(QThread::currentThread() == thread());
        // Drain accepted writes even when the UI event loop has already stopped.
        // Discard replies here so draining cannot accumulate unbounded results.
        m_discard_replies = true;
        while (!m_waiting.empty()) {
            auto job = move(m_waiting.front());
            m_waiting.pop_front();
            job.dispatch();
        }
        queue_worker_close({});
        m_thread.wait();
    }

    ErrorOr<void> submit(Request request, Completion completion)
    {
        VERIFY(QThread::currentThread() == thread());
        if (m_closing)
            return Error::from_string_literal("Userscript value service is closing");
        if (!completion || request.origin_token.bytes().size() > 128 || request.script_id.bytes().size() > 128 || request.key.bytes().size() > 1024 || request.updates.size() > 256)
            return Error::from_string_literal("Invalid userscript value request");
        size_t bytes = request.script_id.bytes().size() + request.key.bytes().size();
        for (auto const& update : request.updates) {
            if (update.key.bytes().size() > 1024 || (update.value.has_value() && update.value->size() > Store::max_value_bytes))
                return Error::from_string_literal("Userscript value request exceeds limits");
            bytes += update.key.bytes().size() + (update.value.has_value() ? update.value->size() : 0);
        }
        if (bytes - request.script_id.bytes().size() - request.key.bytes().size() > Store::max_scope_bytes)
            return Error::from_string_literal("Userscript value request exceeds limits");
        auto mutation_bytes = bytes - request.script_id.bytes().size() - request.key.bytes().size();
        bool capture_changes = request.operation == Operation::Apply && !!m_change_callback;
        if (capture_changes)
            bytes += mutation_bytes + min(Store::max_scope_bytes, request.updates.size() * (Store::max_value_bytes + 1024));
        bytes += request.origin_token.bytes().size();
        auto input_bytes = request.script_id.bytes().size() + request.key.bytes().size() + mutation_bytes + request.origin_token.bytes().size();
        // Reserve for input AND the largest possible reply while the UI is busy.
        switch (request.operation) {
        case Operation::Selected: bytes += min(Store::max_scope_bytes, request.updates.size() * (Store::max_value_bytes + 1024)); break;
        case Operation::Snapshot: bytes += Store::max_scope_bytes + Store::max_keys * 1024; break;
        case Operation::Get: bytes += Store::max_value_bytes; break;
        case Operation::Keys: bytes += Store::max_keys * 1024; break;
        case Operation::Apply: break;
        default: return Error::from_string_literal("Unknown userscript value operation");
        }
        if (m_pending >= 128 || input_bytes > max_queued_bytes - m_input_bytes || bytes > max_queued_bytes || m_sequence == NumericLimits<u64>::max())
            return Error::from_string_literal("Userscript value queue is full");
        auto sequence = ++m_sequence;
        ++m_pending;
        m_input_bytes += input_bytes;
        // Queue small requests without reserving every hypothetical 10 MiB reply
        // at admission. Only dispatched jobs hold reply reservations; FIFO keeps
        // writes and their notifications ordered across tabs.
        m_waiting.push_back({ bytes, [this, request = move(request), completion = move(completion), bytes, input_bytes, capture_changes, sequence]() mutable {
            m_reserved_bytes += bytes;
            bool queued = QMetaObject::invokeMethod(m_worker, [this, state = m_state, request = move(request), completion = move(completion), bytes, input_bytes, capture_changes, sequence]() mutable {
                auto result = state->execute(request, capture_changes);
                if (m_discard_replies.load()) return;
                QMetaObject::invokeMethod(this, [this, completion = move(completion), result = move(result), bytes, input_bytes, script_id = move(request.script_id), origin = move(request.origin_token), sequence]() mutable {
                    --m_pending;
                    m_reserved_bytes -= bytes;
                    m_input_bytes -= input_bytes;
                    if (result.is_error()) {
                        completion(result.release_error());
                    } else {
                        auto outcome = result.release_value();
                        auto callback = m_change_callback;
                        if (outcome.commit.has_value() && !outcome.commit->changes.is_empty() && callback)
                            (*callback)(script_id, origin, *outcome.commit, sequence);
                        completion(move(outcome.response));
                    }
                    dispatch_waiting();
                    if (m_closing && !m_pending)
                        queue_worker_close(move(m_close_completion));
                }, Qt::QueuedConnection);
            }, Qt::QueuedConnection);
            VERIFY(queued); // The owned worker lives until the destructor joins it.
        } });
        dispatch_waiting();
        return {};
    }

    ErrorOr<void> close(Function<void()> completion)
    {
        VERIFY(QThread::currentThread() == thread());
        if (m_closing)
            return Error::from_string_literal("Userscript value service is already closing");
        begin_close(move(completion));
        return {};
    }
    size_t pending() const { return m_pending; }

private:
    struct Outcome { Response response; Optional<Store::Commit> commit; };
    struct State {
        State(QString directory, bool is_private) : directory(move(directory)), is_private(is_private) { }
        QString directory;
        bool is_private;
        bool attempted { false };
        Optional<Error> initialization_error;
        OwnPtr<Store> store;

        ErrorOr<void> initialize()
        {
            Database::Database::Options options;
            options.synchronous = Database::Database::Synchronous::Full;
            options.foreign_keys = Database::Database::ForeignKeys::Yes;
            if (is_private) {
                store = TRY(Store::create(TRY(Database::Database::create_memory_backed(options))));
                return {};
            }
            if (directory.isEmpty() || !QDir::isAbsolutePath(directory))
                return Error::from_string_literal("Userscript value directory must be absolute");
            if (!QDir().mkpath(directory) || !QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
                return Error::from_string_literal("Cannot create private userscript value directory");
            QFile file(directory + "/Values.db");
            if (!file.exists()) {
                if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                    return Error::from_string_literal("Cannot create userscript value database");
            }
            if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
                return Error::from_string_literal("Cannot restrict userscript value database permissions");
            file.close();
            auto bytes = directory.toUtf8();
            store = TRY(Store::create(TRY(Database::Database::create(ByteString(bytes.constData()), "Values"sv, options))));
            return {};
        }
        ErrorOr<Outcome> execute(Request const& request, bool capture_changes)
        {
            if (!attempted) {
                attempted = true;
                auto result = initialize();
                if (result.is_error()) initialization_error = result.release_error();
            }
            if (initialization_error.has_value())
                return Error::copy(*initialization_error);
            switch (request.operation) {
            case Operation::Selected: return Outcome { Response { TRY(store->selected(request.script_id, request.updates)) }, {} };
            case Operation::Snapshot: return Outcome { Response { TRY(store->snapshot(request.script_id)) }, {} };
            case Operation::Get: return Outcome { Response { TRY(store->get(request.script_id, request.key)) }, {} };
            case Operation::Keys: return Outcome { Response { TRY(store->keys(request.script_id)) }, {} };
            case Operation::Apply:
                if (capture_changes) {
                    auto commit = TRY(store->apply_with_changes(request.script_id, request.updates, request.expected_revision));
                    auto revision = commit.revision;
                    return Outcome { Response { revision }, move(commit) };
                }
                return Outcome { Response { TRY(store->apply(request.script_id, request.updates, request.expected_revision)) }, {} };
            }
            return Error::from_string_literal("Unknown userscript value operation");
        }
    };
    void dispatch_waiting()
    {
        while (!m_waiting.empty() && m_waiting.front().bytes <= max_queued_bytes - m_reserved_bytes) {
            auto job = move(m_waiting.front());
            m_waiting.pop_front();
            job.dispatch();
        }
    }
    void begin_close(Function<void()> completion)
    {
        m_closing = true;
        m_close_completion = move(completion);
        if (!m_pending) queue_worker_close(move(m_close_completion));
    }
    void queue_worker_close(Function<void()> completion)
    {
        if (m_worker_closing) return;
        m_worker_closing = true;
        QMetaObject::invokeMethod(m_worker, [this, state = m_state, completion = move(completion)]() mutable {
            state->store = nullptr; // Database closes on its owning thread, after all accepted writes.
            QThread::currentThread()->quit();
            QMetaObject::invokeMethod(this, [completion = move(completion)]() mutable {
                if (completion) completion();
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    }
    static constexpr size_t max_queued_bytes = 64 * 1024 * 1024;
    std::shared_ptr<State> m_state;
    std::shared_ptr<ChangeCallback> m_change_callback;
    QThread m_thread;
    QObject* m_worker;
    u64 m_sequence { 0 };
    size_t m_pending { 0 };
    size_t m_reserved_bytes { 0 };
    struct WaitingJob { size_t bytes; Function<void()> dispatch; };
    std::deque<WaitingJob> m_waiting;
    size_t m_input_bytes { 0 };
    Function<void()> m_close_completion;
    std::atomic<bool> m_discard_replies { false };
    bool m_worker_closing { false };
    bool m_closing { false };
};

}
