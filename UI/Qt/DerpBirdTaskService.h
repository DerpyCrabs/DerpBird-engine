#pragma once

#pragma push_macro("signals")
#undef signals
#include <gio/gio.h>
#pragma pop_macro("signals")
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NumericLimits.h>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <memory>

namespace Ladybird {

// UI-owned service; all secret-service calls run serially on one worker. Cancel
// settles the caller immediately but retains admission credit until the worker
// releases its request/result. A slow provider cannot grow an unbounded queue.
template<typename Request, typename Value>
class DerpBirdTaskService : public QObject {
public:
    using Result = ErrorOr<Value>;
    using Completion = Function<void(Result)>;
    using Backend = Function<Result(Request const&, GCancellable*)>;
    using Admission = Function<ErrorOr<size_t>(Request const&)>;
    static constexpr size_t max_pending = 32;
    static constexpr size_t max_bytes = 1024 * 1024;

    explicit DerpBirdTaskService(Backend backend, Admission admission, Function<void()> cleanup = {}, size_t budget = max_bytes, int timeout_ms = 30000)
        : m_worker(new QObject), m_backend(move(backend)), m_admission(move(admission)), m_cleanup(move(cleanup)), m_budget(budget), m_timeout_ms(timeout_ms)
    {
        VERIFY(m_backend && m_admission && budget && timeout_ms > 0 && timeout_ms <= 300000);
        m_worker->moveToThread(&m_thread);
        connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        connect(&m_thread, &QThread::finished, this, [this] {
            VERIFY(m_pending.is_empty());
            auto completion = move(m_closed_callback);
            if (completion) completion();
        });
        m_thread.setObjectName("DerpBird secrets");
        m_thread.start();
    }
    ~DerpBirdTaskService() override
    {
        VERIFY(QThread::currentThread() == thread());
        for (auto& entry : m_pending) {
            entry.value->timer->stop();
            g_cancellable_cancel(entry.value->cancel);
        }
        if (!m_closing) stop_worker();
        m_thread.wait();
        // QObject drops queued result delivery after destruction. No completion
        // runs from a destructor, and secrets remain in native owning values.
        m_pending.clear();
    }
    ErrorOr<u64> submit(Request request, Completion completion)
    {
        VERIFY(QThread::currentThread() == thread());
        if (m_closing || !completion) return Error::from_string_literal("Credential service is closed or invalid");
        auto bytes = TRY(m_admission(request));
        if (m_pending.size() >= max_pending || bytes > m_budget - m_bytes || m_next_id == NumericLimits<u64>::max())
            return Error::from_string_literal("Credential service queue is full");
        auto id = m_next_id++;
        auto task = std::make_shared<Task>();
        task->completion = move(completion);
        task->bytes = bytes;
        task->timer = new QTimer(this);
        task->timer->setSingleShot(true);
        connect(task->timer, &QTimer::timeout, this, [this, id] { cancel(id); });
        m_pending.set(id, task);
        m_bytes += bytes;
        task->timer->start(m_timeout_ms);
        bool queued = QMetaObject::invokeMethod(m_worker, [this, id, task, request = move(request)]() mutable {
            auto result = g_cancellable_is_cancelled(task->cancel)
                ? Result { Error::from_string_literal("Credential operation cancelled") }
                : m_backend(request, task->cancel);
            if (g_cancellable_is_cancelled(task->cancel))
                result = Error::from_string_literal("Credential operation cancelled");
            QMetaObject::invokeMethod(this, [this, id, result = move(result)]() mutable { finish(id, move(result)); }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
        if (!queued) {
            m_pending.remove(id);
            m_bytes -= bytes;
            delete task->timer;
            return Error::from_string_literal("Cannot queue credential operation");
        }
        return id;
    }
    bool cancel(u64 id)
    {
        VERIFY(QThread::currentThread() == thread());
        auto found = m_pending.find(id);
        if (found == m_pending.end() || !found->value->completion) return false;
        auto task = found->value;
        task->timer->stop();
        g_cancellable_cancel(task->cancel);
        auto completion = move(task->completion);
        completion(Error::from_string_literal("Credential operation cancelled or timed out"));
        return true;
    }
    ErrorOr<void> close(Function<void()> completion)
    {
        VERIFY(QThread::currentThread() == thread());
        if (m_closing) return Error::from_string_literal("Credential service already closing");
        m_closing = true;
        m_closed_callback = move(completion);
        // Queue the stop before calling reentrant completions: they may destroy us.
        stop_worker();
        Vector<u64> ids;
        for (auto const& entry : m_pending) ids.append(entry.key);
        QPointer guard { this };
        for (auto id : ids) {
            cancel(id);
            if (!guard) break;
        }
        return {};
    }
    size_t pending() const { return m_pending.size(); }
    size_t bytes() const { return m_bytes; }

private:
    struct Task {
        GCancellable* cancel { g_cancellable_new() };
        ~Task() { g_object_unref(cancel); }
        QTimer* timer { nullptr };
        Completion completion;
        size_t bytes { 0 };
    };
    void finish(u64 id, Result result)
    {
        auto task = m_pending.take(id);
        if (!task.has_value()) return;
        m_bytes -= (*task)->bytes;
        (*task)->timer->stop();
        (*task)->timer->deleteLater();
        auto completion = move((*task)->completion);
        if (completion) completion(move(result));
    }
    void stop_worker()
    {
        QMetaObject::invokeMethod(m_worker, [this] { if (m_cleanup) m_cleanup(); QThread::currentThread()->quit(); }, Qt::QueuedConnection);
    }
    QThread m_thread;
    QObject* m_worker;
    Backend m_backend;
    Admission m_admission;
    Function<void()> m_cleanup;
    size_t m_budget;
    int m_timeout_ms;
    HashMap<u64, std::shared_ptr<Task>> m_pending;
    u64 m_next_id { 1 };
    size_t m_bytes { 0 };
    bool m_closing { false };
    Function<void()> m_closed_callback;
};

}
