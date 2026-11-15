#pragma once
#include <LibJS/Runtime/ArrayBuffer.h>

#include <AK/HashMap.h>
#include <AK/RefCounted.h>
#include <AK/Weakable.h>
#include <AK/WeakPtr.h>
#include <LibGC/Root.h>
#include <LibGC/RootVector.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/Page/DerpBirdValueCodec.h>
#include <LibWeb/Page/DerpBirdValueProtocol.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/Timer.h>
#include <memory>

namespace Web {

// Native-owned state for one script world. JS closures and transport callbacks
// hold weak references; close releases promise/default roots without invoking JS.
class DerpBirdScriptValues : public RefCounted<DerpBirdScriptValues>, public Weakable<DerpBirdScriptValues> {
public:
    AK_ALLOC_WITH_KMALLOC;
    using Protocol = DerpBirdValueProtocol;
    using Completion = Function<void(ErrorOr<ByteBuffer>)>;
    using Send = Function<ErrorOr<void>(ByteBuffer, Completion)>;
    static NonnullRefPtr<DerpBirdScriptValues> create(JS::Realm& realm, String script_id, Send send, Function<bool()> active)
    {
        return adopt_ref(*new DerpBirdScriptValues(realm, move(script_id), move(send), move(active)));
    }
    void install(JS::Object& gm, Vector<String> const& grants)
    {
        struct Binding { Utf16FlyString name; StringView grant; Protocol::Method method; i32 length; };
        for (auto const& binding : Array {
                 Binding { "__audio"_utf16_fly_string, "GM.audio"sv, Protocol::Method::Audio, 2 },
                 Binding { "__audioLegacy"_utf16_fly_string, "GM_audio"sv, Protocol::Method::AudioLegacy, 2 },
                 Binding { "__windowClose"_utf16_fly_string, "window.close"sv, Protocol::Method::WindowClose, 0 },
                 Binding { "__windowFocus"_utf16_fly_string, "window.focus"sv, Protocol::Method::WindowFocus, 0 },
                 Binding { "__networkRead"_utf16_fly_string, "GM.xmlHttpRequest"sv, Protocol::Method::NetworkRead, 1 },
                 Binding { "__networkRead"_utf16_fly_string, "GM_xmlhttpRequest"sv, Protocol::Method::NetworkRead, 1 },
                 Binding { "__cookie"_utf16_fly_string, "GM.cookie"sv, Protocol::Method::Cookie, 2 },
                 Binding { "__cookieLegacy"_utf16_fly_string, "GM_cookie"sv, Protocol::Method::CookieLegacy, 2 },
                 Binding { "getTab"_utf16_fly_string, "GM.getTab"sv, Protocol::Method::TabGet, 0 },
                 Binding { "__getTabLegacy"_utf16_fly_string, "GM_getTab"sv, Protocol::Method::TabGetLegacy, 0 },
                 Binding { "saveTab"_utf16_fly_string, "GM.saveTab"sv, Protocol::Method::TabSave, 1 },
                 Binding { "__saveTabLegacy"_utf16_fly_string, "GM_saveTab"sv, Protocol::Method::TabSaveLegacy, 1 },
                 Binding { "getTabs"_utf16_fly_string, "GM.getTabs"sv, Protocol::Method::TabsGet, 0 },
                 Binding { "__getTabsLegacy"_utf16_fly_string, "GM_getTabs"sv, Protocol::Method::TabsGetLegacy, 0 },
                 Binding { "__menuRegister"_utf16_fly_string, "GM.registerMenuCommand"sv, Protocol::Method::MenuRegister, 2 },
                 Binding { "__menuRegisterLegacy"_utf16_fly_string, "GM_registerMenuCommand"sv, Protocol::Method::MenuRegisterLegacy, 2 },
                 Binding { "__menuRemove"_utf16_fly_string, "GM.unregisterMenuCommand"sv, Protocol::Method::MenuRemove, 1 },
                 Binding { "__menuRemoveLegacy"_utf16_fly_string, "GM_unregisterMenuCommand"sv, Protocol::Method::MenuRemoveLegacy, 1 },
                 Binding { "__menuWait"_utf16_fly_string, "GM.registerMenuCommand"sv, Protocol::Method::MenuWait, 1 },
                 Binding { "__menuWait"_utf16_fly_string, "GM_registerMenuCommand"sv, Protocol::Method::MenuWait, 1 },
                 Binding { "__notifyActivate"_utf16_fly_string, "GM.notification"sv, Protocol::Method::NotifyActivate, 2 },
                 Binding { "__notifyActivate"_utf16_fly_string, "GM_notification"sv, Protocol::Method::NotifyActivate, 2 },
                 Binding { "__notify"_utf16_fly_string, "GM.notification"sv, Protocol::Method::Notify, 2 },
                 Binding { "__notifyLegacy"_utf16_fly_string, "GM_notification"sv, Protocol::Method::NotifyLegacy, 2 },
                 Binding { "__notifyWait"_utf16_fly_string, "GM.notification"sv, Protocol::Method::NotifyWait, 1 },
                 Binding { "__notifyWait"_utf16_fly_string, "GM_notification"sv, Protocol::Method::NotifyWait, 1 },
                 Binding { "__downloadChunk"_utf16_fly_string, "GM.download"sv, Protocol::Method::DownloadChunk, 2 },
                 Binding { "__downloadChunk"_utf16_fly_string, "GM_download"sv, Protocol::Method::DownloadChunk, 2 },
                 Binding { "__downloadFinish"_utf16_fly_string, "GM.download"sv, Protocol::Method::DownloadFinish, 1 },
                 Binding { "__downloadFinish"_utf16_fly_string, "GM_download"sv, Protocol::Method::DownloadFinish, 1 },
                 Binding { "__download"_utf16_fly_string, "GM.download"sv, Protocol::Method::Download, 2 },
                 Binding { "__downloadLegacy"_utf16_fly_string, "GM_download"sv, Protocol::Method::DownloadLegacy, 2 },
                 Binding { "__downloadAbort"_utf16_fly_string, "GM.download"sv, Protocol::Method::DownloadAbort, 1 },
                 Binding { "__downloadAbort"_utf16_fly_string, "GM_download"sv, Protocol::Method::DownloadAbort, 1 },
                 Binding { "__downloadWatch"_utf16_fly_string, "GM.download"sv, Protocol::Method::DownloadWatch, 1 },
                 Binding { "__downloadWatch"_utf16_fly_string, "GM_download"sv, Protocol::Method::DownloadWatch, 1 },
                 Binding { "__tabOpen"_utf16_fly_string, "GM.openInTab"sv, Protocol::Method::TabOpen, 2 },
                 Binding { "__tabOpenLegacy"_utf16_fly_string, "GM_openInTab"sv, Protocol::Method::TabOpenLegacy, 2 },
                 Binding { "__tabClose"_utf16_fly_string, "GM.openInTab"sv, Protocol::Method::TabClose, 1 },
                 Binding { "__tabClose"_utf16_fly_string, "GM_openInTab"sv, Protocol::Method::TabClose, 1 },
                 Binding { "__tabWatch"_utf16_fly_string, "GM.openInTab"sv, Protocol::Method::TabWatch, 1 },
                 Binding { "__tabWatch"_utf16_fly_string, "GM_openInTab"sv, Protocol::Method::TabWatch, 1 },
                 Binding { "__clipboard"_utf16_fly_string, "GM.setClipboard"sv, Protocol::Method::Clipboard, 2 },
                 Binding { "__clipboardLegacy"_utf16_fly_string, "GM_setClipboard"sv, Protocol::Method::ClipboardLegacy, 2 },
                 Binding { "__network"_utf16_fly_string, "GM.xmlHttpRequest"sv, Protocol::Method::Network, 2 },
                 Binding { "__networkLegacy"_utf16_fly_string, "GM_xmlhttpRequest"sv, Protocol::Method::NetworkLegacy, 2 },
                 Binding { "__networkAbort"_utf16_fly_string, "GM.xmlHttpRequest"sv, Protocol::Method::NetworkAbort, 1 },
                 Binding { "__networkAbort"_utf16_fly_string, "GM_xmlhttpRequest"sv, Protocol::Method::NetworkAbort, 1 },
                 Binding { "getValue"_utf16_fly_string, "GM.getValue"sv, Protocol::Method::Get, 1 },
                 Binding { "setValue"_utf16_fly_string, "GM.setValue"sv, Protocol::Method::Set, 2 },
                 Binding { "deleteValue"_utf16_fly_string, "GM.deleteValue"sv, Protocol::Method::Delete, 1 },
                 Binding { "listValues"_utf16_fly_string, "GM.listValues"sv, Protocol::Method::List, 0 },
                 Binding { "getValues"_utf16_fly_string, "GM.getValues"sv, Protocol::Method::GetMany, 1 },
                 Binding { "setValues"_utf16_fly_string, "GM.setValues"sv, Protocol::Method::SetMany, 1 },
                 Binding { "deleteValues"_utf16_fly_string, "GM.deleteValues"sv, Protocol::Method::DeleteMany, 1 },
                 Binding { "addValueChangeListener"_utf16_fly_string, "GM.addValueChangeListener"sv, Protocol::Method::Subscribe, 2 },
                 Binding { "removeValueChangeListener"_utf16_fly_string, "GM.removeValueChangeListener"sv, Protocol::Method::Unsubscribe, 1 } }) {
            if (!grants.contains_slow(binding.grant)) continue;
            auto function = JS::NativeFunction::create(*m_realm, GC::create_function(GC::Heap::the(), GC::captures(make_weak_ptr(), binding.method), [](auto& weak, auto& method, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                if (!weak)
                    return WebIDL::create_rejected_promise(*vm.current_realm(), WebIDL::InvalidStateError::create("Userscript world was revoked"_utf16))->promise();
                NonnullRefPtr owner { *weak };
                return owner->invoke(method, vm);
            }), binding.length, binding.name, GC::Ptr<JS::Realm> { m_realm.ptr() });
            gm.define_direct_property(binding.name, function, JS::default_attributes);
        }
    }
    void install_legacy(Vector<String> const& grants, JS::Object* destination = nullptr)
    {
        struct Binding { Utf16FlyString name; StringView grant; Protocol::Method method; i32 length; };
        for (auto const& binding : Array {
                 Binding { "__derpbirdNetworkSync"_utf16_fly_string, "GM_xmlhttpRequest"sv, Protocol::Method::NetworkLegacy, 2 },
                 Binding { "GM_getValue"_utf16_fly_string, "GM_getValue"sv, Protocol::Method::Get, 1 },
                 Binding { "GM_setValue"_utf16_fly_string, "GM_setValue"sv, Protocol::Method::Set, 2 },
                 Binding { "GM_deleteValue"_utf16_fly_string, "GM_deleteValue"sv, Protocol::Method::Delete, 1 },
                 Binding { "GM_listValues"_utf16_fly_string, "GM_listValues"sv, Protocol::Method::List, 0 },
                 Binding { "GM_getValues"_utf16_fly_string, "GM_getValues"sv, Protocol::Method::GetMany, 1 },
                 Binding { "GM_setValues"_utf16_fly_string, "GM_setValues"sv, Protocol::Method::SetMany, 1 },
                 Binding { "GM_deleteValues"_utf16_fly_string, "GM_deleteValues"sv, Protocol::Method::DeleteMany, 1 },
                 Binding { "GM_addValueChangeListener"_utf16_fly_string, "GM_addValueChangeListener"sv, Protocol::Method::Subscribe, 2 },
                 Binding { "GM_removeValueChangeListener"_utf16_fly_string, "GM_removeValueChangeListener"sv, Protocol::Method::Unsubscribe, 1 } }) {
            if (!grants.contains_slow(binding.grant)) continue;
            auto function = JS::NativeFunction::create(*m_realm, GC::create_function(GC::Heap::the(), GC::captures(make_weak_ptr(), binding.method), [](auto& weak, auto& method, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                if (!weak) return vm.throw_completion<JS::TypeError>("Userscript world was revoked"_utf16);
                NonnullRefPtr owner { *weak };
                return WebIDL::throw_dom_exception_if_needed(vm, *vm.current_realm(), [&] { return owner->invoke_sync(method, vm); });
            }), binding.length, binding.name, GC::Ptr<JS::Realm> { m_realm.ptr() });
            (destination ? *destination : m_realm->global_object()).define_direct_property(binding.name, function, JS::default_attributes);
        }
    }
    void close()
    {
        m_closed = true;
        m_token = {};
        m_listeners.clear();
        m_pending.clear();
        m_waiting.clear();
        m_reserved = 0;
    }
    // Installed by native listener bindings, never by page JavaScript.
    Function<bool(Protocol::Event const&)> on_event;
    bool receive_event(Protocol::Event const& event)
    {
        NonnullRefPtr owner { *this };
        if (!active() || m_token.is_empty() || event.identity != m_token) return false;
        if (HTML::main_thread_event_loop().execution_paused() || m_deferred_events) {
            auto bytes = Protocol::encode(event);
            if (bytes.is_error() || m_deferred_events >= 128 || bytes.value().size() > 16 * 1024 * 1024 - m_deferred_event_bytes) return false;
            ++m_deferred_events;
            m_deferred_event_bytes += bytes.value().size();
            HTML::queue_global_task(HTML::Task::Source::Unspecified, m_realm->global_object(),
                GC::create_function(GC::Heap::the(), GC::captures(make_weak_ptr(), bytes.release_value()), [](auto& weak, auto& bytes) {
                    if (!weak) return;
                    NonnullRefPtr owner { *weak };
                    --owner->m_deferred_events;
                    owner->m_deferred_event_bytes -= bytes.size();
                    auto decoded = Protocol::decode_event(bytes.bytes());
                    if (!decoded.is_error()) owner->deliver_event(decoded.value());
                }));
            return true;
        }
        return deliver_event(event);
    }
    bool deliver_event(Protocol::Event const& event)
    {
        if (!active() || event.identity != m_token) return false;
        if (on_event) return on_event(event);
        auto found = m_listeners.find(event.subscription);
        if (found == m_listeners.end() || found->value->key != event.key) return false;
        if (event.revision <= found->value->revision) return true;
        HTML::TemporaryExecutionContext context(*m_realm);
        GC::Root callback { found->value->callback.ptr() };
        auto key = DerpBirdValueKey::decode(event.key);
        if (key.is_error()) return false;
        GC::RootVector<JS::Value> arguments;
        arguments.append(JS::PrimitiveString::create(m_realm->vm(), key.release_value()));
        for (auto const* value : { &event.old_value, &event.new_value }) {
            if (!value->has_value()) arguments.append(JS::js_undefined());
            else {
                auto decoded = decode_derpbird_value(m_realm->vm(), (*value)->bytes(), *m_realm);
                if (decoded.is_error()) return false;
                arguments.append(decoded.release_value());
            }
        }
        arguments.append(JS::Value(event.remote));
        found->value->revision = event.revision;
        auto result = JS::call(m_realm->vm(), JS::Value(callback.ptr()), JS::js_undefined(), arguments.span());
        if (result.is_error()) HTML::report_exception(result, *m_realm);
        return true;

    }
    size_t pending() const { return m_pending.size(); }

private:
    struct Listener {
        AK_ALLOC_WITH_KMALLOC;
        Listener(String key, JS::Object& callback) : key(move(key)), callback(callback) { }
        String key;
        GC::Root<JS::Object> callback;
        i64 revision { 0 };
    };
    size_t listener_slots() const
    {
        size_t count = m_listeners.size();
        for (auto const& pending : m_pending)
            if (pending.value->request.method == Protocol::Method::Subscribe) ++count;
        return count;
    }
    struct Pending {
        AK_ALLOC_WITH_KMALLOC;
        Pending(GC::Ref<WebIDL::Promise> promise, Protocol::Request request, JS::Value fallback, size_t bytes)
            : promise(promise), request(move(request)), fallback({ fallback }), bytes(bytes) { }
        GC::Root<WebIDL::Promise> promise;
        Protocol::Request request;
        GC::RootVector<JS::Value> fallback;
        size_t bytes;
    };
    DerpBirdScriptValues(JS::Realm& realm, String script_id, Send send, Function<bool()> active)
        : m_realm(realm), m_script_id(move(script_id)), m_send(move(send)), m_active(move(active)) { }
    bool active()
    {
        if (m_closed) return false;
        if (!m_active()) { close(); return false; }
        return true;
    }
    WebIDL::ExceptionOr<Protocol::Request> prepare(Protocol::Method method, JS::VM& vm, JS::Object& selection)
    {
        Protocol::Request request { method, true, {}, {}, {} };
        if (method == Protocol::Method::Audio || method == Protocol::Method::AudioLegacy) {
            auto action = TRY(vm.argument(0).to_utf16_string(vm));
            request.key = TRY_OR_THROW_OOM(vm, action.utf16_view().to_utf8());
            auto text = TRY(vm.argument(1).to_utf16_string(vm));
            auto bytes = TRY_OR_THROW_OOM(vm, text.utf16_view().to_utf8());
            if (bytes.bytes().size() > 1024) return WebIDL::QuotaExceededError::create("Audio options exceed limit"_utf16);
            request.updates.append({ "audio"_string, TRY_OR_THROW_OOM(vm, ByteBuffer::copy(bytes.bytes())) });
            return request;
        }
        if (method == Protocol::Method::WindowClose || method == Protocol::Method::WindowFocus) return request;
        if (method == Protocol::Method::TabGet || method == Protocol::Method::TabGetLegacy
            || method == Protocol::Method::TabsGet || method == Protocol::Method::TabsGetLegacy) return request;
        if (method == Protocol::Method::TabSave || method == Protocol::Method::TabSaveLegacy) {
            if (!vm.argument(0).is_object()) return JS::throw_completion(JS::TypeError::create(*m_realm, "Expected tab data object"_utf16));
            request.updates.append({ "tab-data"_string, TRY(encode_derpbird_value(vm, vm.argument(0))) });
            return request;
        }
        if (method == Protocol::Method::MenuRegister || method == Protocol::Method::MenuRegisterLegacy
            || method == Protocol::Method::MenuRemove || method == Protocol::Method::MenuRemoveLegacy || method == Protocol::Method::MenuWait) {
            bool adding = method == Protocol::Method::MenuRegister || method == Protocol::Method::MenuRegisterLegacy;
            auto id = TRY(vm.argument(adding ? 1 : 0).to_utf16_string(vm));
            request.key = TRY_OR_THROW_OOM(vm, id.utf16_view().to_utf8());
            if (adding) {
                auto options = TRY(vm.argument(0).to_utf16_string(vm));
                auto utf8 = TRY_OR_THROW_OOM(vm, options.utf16_view().to_utf8());
                if (utf8.bytes().size()>4096) return WebIDL::QuotaExceededError::create("Menu command exceeds 4 KiB"_utf16);
                request.updates.append({ "menu"_string, TRY_OR_THROW_OOM(vm, ByteBuffer::copy(utf8.bytes())) });
            }
            return request;
        }
        if (method == Protocol::Method::Cookie || method == Protocol::Method::CookieLegacy) {
            auto operation = TRY(vm.argument(0).to_utf16_string(vm));
            request.key = TRY_OR_THROW_OOM(vm, operation.utf16_view().to_utf8());
            auto text = TRY(vm.argument(1).to_utf16_string(vm));
            auto utf8 = TRY_OR_THROW_OOM(vm, text.utf16_view().to_utf8());
            if (utf8.bytes().size() > 16384) return WebIDL::QuotaExceededError::create("Cookie request exceeds 16 KiB"_utf16);
            request.updates.append({ "cookie"_string, TRY_OR_THROW_OOM(vm, ByteBuffer::copy(utf8.bytes())) });
            return request;
        }
        if (method == Protocol::Method::NotifyActivate) {
            auto id=TRY(vm.argument(0).to_utf16_string(vm));request.key=TRY_OR_THROW_OOM(vm,id.utf16_view().to_utf8());
            if(!vm.argument(1).is_boolean())return JS::throw_completion(JS::TypeError::create(*m_realm,"Expected notification action boolean"_utf16));
            u8 allow=vm.argument(1).as_bool()?1:0;
            request.updates.append({"allow"_string,TRY_OR_THROW_OOM(vm,ByteBuffer::copy(ReadonlyBytes{&allow,1}))});return request;
        }
        if (method == Protocol::Method::Notify || method == Protocol::Method::NotifyLegacy || method == Protocol::Method::NotifyWait) {
            bool adding = method != Protocol::Method::NotifyWait;
            auto id = TRY(vm.argument(adding ? 1 : 0).to_utf16_string(vm));
            request.key = TRY_OR_THROW_OOM(vm, id.utf16_view().to_utf8());
            if (adding) {
                auto text = TRY(vm.argument(0).to_utf16_string(vm));
                auto utf8 = TRY_OR_THROW_OOM(vm, text.utf16_view().to_utf8());
                if (utf8.bytes().size() > 65536) return WebIDL::QuotaExceededError::create("Notification exceeds64KiB"_utf16);
                request.updates.append({ "notification"_string, TRY_OR_THROW_OOM(vm, ByteBuffer::copy(utf8.bytes())) });
            }
            return request;
        }
        if (method == Protocol::Method::DownloadChunk || method == Protocol::Method::DownloadFinish) {
            auto id = TRY(vm.argument(0).to_utf16_string(vm));
            request.key = TRY_OR_THROW_OOM(vm, id.utf16_view().to_utf8());
            if (method == Protocol::Method::DownloadChunk) {
                if (!vm.argument(1).is_object() || !is<JS::ArrayBuffer>(vm.argument(1).as_object()))
                    return JS::throw_completion(JS::TypeError::create(*m_realm, "Expected download ArrayBuffer"_utf16));
                auto& buffer = static_cast<JS::ArrayBuffer&>(vm.argument(1).as_object());
                if (buffer.is_detached() || buffer.is_shared_array_buffer() || buffer.byte_length() > 131072)
                    return WebIDL::QuotaExceededError::create("Invalid or oversized download chunk"_utf16);
                request.updates.append({ "chunk"_string, TRY_OR_THROW_OOM(vm, buffer.copy_to_byte_buffer()) });
            }
            return request;
        }
        if (method == Protocol::Method::Download || method == Protocol::Method::DownloadLegacy
            || method == Protocol::Method::DownloadAbort || method == Protocol::Method::DownloadWatch) {
            bool opening = method == Protocol::Method::Download || method == Protocol::Method::DownloadLegacy;
            auto id = TRY(vm.argument(opening ? 1 : 0).to_utf16_string(vm));
            request.key = TRY_OR_THROW_OOM(vm, id.utf16_view().to_utf8());
            if (opening) {
                auto options = TRY(vm.argument(0).to_utf16_string(vm));
                auto utf8 = TRY_OR_THROW_OOM(vm, options.utf16_view().to_utf8());
                if (utf8.bytes().size() > 65536) return WebIDL::QuotaExceededError::create("Download options exceed 64 KiB"_utf16);
                request.updates.append({ "download"_string, TRY_OR_THROW_OOM(vm, ByteBuffer::copy(utf8.bytes())) });
            }
            return request;
        }
        if (method == Protocol::Method::TabOpen || method == Protocol::Method::TabOpenLegacy
            || method == Protocol::Method::TabClose || method == Protocol::Method::TabWatch) {
            bool opening = method == Protocol::Method::TabOpen || method == Protocol::Method::TabOpenLegacy;
            auto id = TRY(vm.argument(opening ? 1 : 0).to_utf16_string(vm));
            request.key = TRY_OR_THROW_OOM(vm, id.utf16_view().to_utf8());
            if (opening) {
                auto options = TRY(vm.argument(0).to_utf16_string(vm));
                auto utf8 = TRY_OR_THROW_OOM(vm, options.utf16_view().to_utf8());
                if (utf8.bytes().size() > 65536) return WebIDL::QuotaExceededError::create("Tab options exceed 64 KiB"_utf16);
                request.updates.append({ "tab"_string, TRY_OR_THROW_OOM(vm, ByteBuffer::copy(utf8.bytes())) });
            }
            return request;
        }
        if (method == Protocol::Method::Clipboard || method == Protocol::Method::ClipboardLegacy) {
            auto data = TRY(vm.argument(0).to_utf16_string(vm));
            auto utf8 = TRY_OR_THROW_OOM(vm, data.utf16_view().to_utf8());
            if (utf8.bytes().size() > Protocol::max_value) return WebIDL::QuotaExceededError::create("Clipboard exceeds 1 MiB"_utf16);
            auto type = TRY(vm.argument(1).to_utf16_string(vm));
            request.key = TRY_OR_THROW_OOM(vm, type.utf16_view().to_utf8());
            request.updates.append({ "clipboard"_string, TRY_OR_THROW_OOM(vm, ByteBuffer::copy(utf8.bytes())) });
            return request;
        }
        if (method == Protocol::Method::Network || method == Protocol::Method::NetworkLegacy || method == Protocol::Method::NetworkAbort || method == Protocol::Method::NetworkRead) {
            auto key = TRY(vm.argument((method == Protocol::Method::NetworkAbort || method == Protocol::Method::NetworkRead) ? 0 : 1).to_utf16_string(vm));
            request.key = TRY_OR_THROW_OOM(vm, key.utf16_view().to_utf8());
            if (method != Protocol::Method::NetworkAbort && method != Protocol::Method::NetworkRead) {
                auto json = TRY(vm.argument(0).to_utf16_string(vm));
                auto utf8 = TRY_OR_THROW_OOM(vm, json.utf16_view().to_utf8());
                if (utf8.bytes().size() > Protocol::max_value) return WebIDL::QuotaExceededError::create("Userscript request exceeds 1 MiB"_utf16);
                request.updates.append({ "request"_string, TRY_OR_THROW_OOM(vm, ByteBuffer::copy(utf8.bytes())) });
            }
            return request;
        }
        if (method == Protocol::Method::List) return request;
        if (method == Protocol::Method::Subscribe) {
            if (!vm.argument(1).is_function()) return JS::throw_completion(JS::TypeError::create(*m_realm, "Expected a value listener callback"_utf16));
            request.key = TRY(encode_derpbird_value_key(vm, vm.argument(0)));
            return request;
        }
        if (method == Protocol::Method::Unsubscribe) {
            auto text = TRY(vm.argument(0).to_utf16_string(vm));
            if (text.utf16_view().length_in_code_units() > 16)
                return JS::throw_completion(JS::TypeError::create(*m_realm, "Invalid value listener ID"_utf16));
            request.key = TRY_OR_THROW_OOM(vm, text.utf16_view().to_utf8());
            auto id = Protocol::subscription_id(request.key);
            if (!id.has_value() || *id > 9007199254740991ULL)
                return JS::throw_completion(JS::TypeError::create(*m_realm, "Invalid value listener ID"_utf16));
            return request;
        }
        if (method == Protocol::Method::GetMany || method == Protocol::Method::SetMany || method == Protocol::Method::DeleteMany) {
            auto input = vm.argument(0);
            if (!input.is_object()) return JS::throw_completion(JS::TypeError::create(*m_realm, "Expected a values object or keys array"_utf16));
            auto array = TRY(input.is_array(vm));
            if (method == Protocol::Method::DeleteMany && !array)
                return JS::throw_completion(JS::TypeError::create(*m_realm, "Expected a keys array"_utf16));
            bool keys_array = array && method != Protocol::Method::SetMany;
            auto& object = input.as_object();
            GC::RootVector<JS::Value> keys;
            if (keys_array) {
                auto length = TRY(JS::length_of_array_like(vm, object));
                if (length > 256) return WebIDL::QuotaExceededError::create("Too many userscript value keys"_utf16);
                for (size_t i = 0; i < length; ++i) keys.append(TRY(object.get(i)));
            } else {
                for (auto key : TRY(object.internal_own_property_keys())) {
                    if (!key.is_string()) continue;
                    auto descriptor = TRY(object.internal_get_own_property(key.as_string().utf16_string()));
                    if (descriptor.has_value() && descriptor->enumerable.value_or(false)) keys.append(key);
                    if (keys.size() > 256) return WebIDL::QuotaExceededError::create("Too many userscript value keys"_utf16);
                }
            }
            HashTable<String> seen;
            size_t bytes = 0;
            for (auto key : keys) {
                auto encoded = TRY(encode_derpbird_value_key(vm, key));
                if (seen.contains(encoded)) continue;
                seen.set(encoded);
                bytes += encoded.bytes().size();
                if (method == Protocol::Method::GetMany) {
                    auto fallback = keys_array ? JS::js_undefined() : TRY(object.get(key.as_string().utf16_string()));
                    selection.define_direct_property(Utf16String::from_utf8(encoded), fallback, JS::default_attributes);
                    request.updates.append({ move(encoded), {} });
                } else {
                    Optional<ByteBuffer> value;
                    if (method == Protocol::Method::SetMany) {
                        value = TRY(encode_derpbird_value(vm, TRY(object.get(key.as_string().utf16_string()))));
                        bytes += value->size();
                    }
                    if (bytes > Protocol::max_values) return WebIDL::QuotaExceededError::create("Userscript value batch exceeds limit"_utf16);
                    request.updates.append({ move(encoded), move(value) });
                }
            }
            return request;
        }
        auto key = TRY(encode_derpbird_value_key(vm, vm.argument(0)));
        if (method == Protocol::Method::Get) request.key = move(key);
        else {
            Optional<ByteBuffer> value;
            if (method == Protocol::Method::Set) value = TRY(encode_derpbird_value(vm, vm.argument(1)));
            request.updates.append({ move(key), move(value) });
        }
        return request;
    }
    struct SyncWait final : RefCounted<SyncWait> {
        GC_HOLDS_NO_CELL_EDGES;
        bool timed_out { false };
        Optional<ErrorOr<ByteBuffer>> response;
    };
    bool wait_for_sync(RefPtr<SyncWait> response = {}, bool transport_timeout = true)
    {
        if (response ? response->response.has_value() : !m_opening) return active();
        auto timeout = adopt_ref(*new SyncWait);
        GC::Root timer = Platform::Timer::create_single_shot(GC::Heap::the(), AK::Duration::from_seconds(30),
            GC::create_function(GC::Heap::the(), GC::captures(timeout), [](auto& timeout) { timeout->timed_out = true; }));
        if (transport_timeout) timer->start();
        {
            // Follow synchronous XHR: pump platform IPC/timers, not author tasks,
            // rendering callbacks or microtasks. Other browser processes stay live.
            auto pause = HTML::main_thread_event_loop().pause(HTML::EventLoop::UpdateTheRendering::No);
            Platform::EventLoopPlugin::the().spin_until(GC::create_function(GC::Heap::the(),
                GC::captures(make_weak_ptr(), response, timeout), [](auto& owner, auto& response, auto& timeout) { return !owner || !owner->active() || timeout->timed_out || (response ? response->response.has_value() : !owner->m_opening); }));
        }
        timer->stop();
        return !timeout->timed_out && active();
    }
    WebIDL::ExceptionOr<JS::Value> invoke_sync(Protocol::Method method, JS::VM& vm)
    {
        if (!active()) return WebIDL::InvalidStateError::create("Userscript world was revoked"_utf16);
        GC::Root selection = JS::Object::create(*m_realm, nullptr);
        auto request = TRY(prepare(method, vm, *selection));
        // Conversion can execute getters, recursively call APIs or revoke us.
        if (!active()) return WebIDL::InvalidStateError::create("Userscript world was revoked"_utf16);
        if (method == Protocol::Method::Subscribe && listener_slots() >= 128)
            return WebIDL::QuotaExceededError::create("Too many value listeners"_utf16);
        GC::RootVector<JS::Value> fallback;
        fallback.append((method == Protocol::Method::Get || method == Protocol::Method::Subscribe) ? vm.argument(1)
            : method == Protocol::Method::GetMany ? JS::Value(selection.ptr()) : JS::js_undefined());
        if (method == Protocol::Method::GetMany && request.updates.is_empty())
            return JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
        if (m_token.is_empty()) {
            if (!m_opening) open();
            if (!wait_for_sync() || m_token.is_empty())
                return WebIDL::InvalidStateError::create("Userscript storage could not open"_utf16);
        }
        request.asynchronous = false;
        request.identity = m_token;
        auto encoded = Protocol::encode(request);
        if (encoded.is_error()) return WebIDL::QuotaExceededError::create("Invalid userscript storage request"_utf16);
        // Shared state outlives a timeout/revocation and any late IPC completion.
        auto response = adopt_ref(*new SyncWait);
        auto sent = m_send(encoded.release_value(), [response](ErrorOr<ByteBuffer> result) { response->response = move(result); });
        // Network lifetime is controlled by details.timeout, abort and authority
        // revocation in the native provider, not the storage RPC deadline.
        bool network = method == Protocol::Method::Network || method == Protocol::Method::NetworkLegacy;
        if (sent.is_error() || !wait_for_sync(response, !network) || response->response->is_error())
            return WebIDL::InvalidStateError::create("Userscript storage request failed"_utf16);
        auto decoded = Protocol::decode_response(response->response->value().bytes());
        if (decoded.is_error()) return WebIDL::InvalidStateError::create("Invalid userscript storage reply"_utf16);
        return result_value(request, fallback[0], decoded.release_value());
    }
    JS::Value invoke(Protocol::Method method, JS::VM& vm)
    {
        auto promise = WebIDL::create_promise(*m_realm);
        if (!active()) return promise->promise();
        if (m_pending.size() >= 128 || m_next_id == NumericLimits<u64>::max()) {
            WebIDL::reject_promise(*m_realm, promise, WebIDL::QuotaExceededError::create("Userscript value queue is full"_utf16));
            return promise->promise();
        }
        GC::Root selection = JS::Object::create(*m_realm, nullptr);
        auto prepared = prepare(method, vm, *selection);
        // Both ToString and structured serialization can invoke getters that
        // close/revoke the world. Never dispatch after either reentrant operation.
        if (!active()) return promise->promise();
        if (prepared.is_error()) {
            WebIDL::reject_promise_with_exception(*m_realm, promise, prepared.release_error());
            return promise->promise();
        }
        // Argument conversion can recursively enqueue more requests.
        if (m_pending.size() >= 128 || m_next_id == NumericLimits<u64>::max()) {
            WebIDL::reject_promise(*m_realm, promise, WebIDL::QuotaExceededError::create("Userscript value queue is full"_utf16));
            return promise->promise();
        }
        if (method == Protocol::Method::Subscribe && listener_slots() >= 128) {
            WebIDL::reject_promise(*m_realm, promise, WebIDL::QuotaExceededError::create("Too many value listeners"_utf16));
            return promise->promise();
        }
        auto request = prepared.release_value();
        if (method == Protocol::Method::GetMany && request.updates.is_empty()) {
            WebIDL::resolve_promise(*m_realm, promise, JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype()));
            return promise->promise();
        }
        size_t bytes = 256 + request.key.bytes().size();
        for (auto const& entry : request.updates) bytes += entry.key.bytes().size() + (entry.value.has_value() ? entry.value->size() : 0);
        if (bytes > 16 * 1024 * 1024 - m_reserved) {
            WebIDL::reject_promise(*m_realm, promise, WebIDL::QuotaExceededError::create("Userscript value queue exceeds memory limit"_utf16));
            return promise->promise();
        }
        auto id = m_next_id++;
        m_reserved += bytes;
        m_pending.set(id, make<Pending>(promise, move(request), (method == Protocol::Method::Get || method == Protocol::Method::Subscribe) ? vm.argument(1) : method == Protocol::Method::GetMany ? JS::Value(selection.ptr()) : JS::js_undefined(), bytes));
        if (!m_token.is_empty()) send(id);
        else {
            m_waiting.append(id);
            if (!m_opening) open();
        }
        return promise->promise();
    }
    void open()
    {
        m_opening = true;
        auto payload = Protocol::encode(Protocol::Request { Protocol::Method::Open, true, m_script_id, {}, {} });
        if (payload.is_error()) { fail_open(); return; }
        auto sent = m_send(payload.release_value(), [weak = make_weak_ptr()](ErrorOr<ByteBuffer> response) {
            if (!weak) return;
            NonnullRefPtr owner { *weak };
            if (!owner->active()) return;
            if (response.is_error()) { owner->fail_open(); return; }
            auto decoded = Protocol::decode_response(response.value().bytes());
            if (decoded.is_error() || decoded.value().kind != Protocol::Kind::Token) { owner->fail_open(); return; }
            owner->m_token = move(decoded.value().token);
            owner->m_opening = false;
            auto waiting = move(owner->m_waiting);
            owner->m_waiting = {};
            for (auto id : waiting) {
                if (!owner->active()) break;
                owner->send(id);
            }
        });
        if (sent.is_error()) fail_open();
    }
    void fail_open()
    {
        m_opening = false;
        auto waiting = move(m_waiting);
        m_waiting = {};
        for (auto id : waiting) fail(id);
    }
    void send(u64 id)
    {
        auto entry = m_pending.find(id);
        if (entry == m_pending.end() || !active()) return;
        entry->value->request.identity = m_token;
        auto bytes = Protocol::encode(entry->value->request);
        if (bytes.is_error()) { fail(id); return; }
        auto sent = m_send(bytes.release_value(), [weak = make_weak_ptr(), id](ErrorOr<ByteBuffer> response) {
            if (!weak) return;
            NonnullRefPtr owner { *weak };
            owner->finish(id, move(response));
        });
        if (sent.is_error()) fail(id);
    }
    void fail(u64 id) { finish(id, Error::from_string_literal("Userscript storage request failed")); }
    void finish(u64 id, ErrorOr<ByteBuffer> response)
    {
        auto pending = m_pending.take(id);
        if (!pending.has_value()) return;
        m_reserved -= (*pending)->bytes;
        if (!active()) return;
        HTML::TemporaryExecutionContext context(*m_realm);
        auto& promise = *(*pending)->promise;
        if (response.is_error()) {
            WebIDL::reject_promise(*m_realm, promise, WebIDL::InvalidStateError::create("Userscript storage request failed"_utf16));
            return;
        }
        auto decoded = Protocol::decode_response(response.value().bytes());
        if (decoded.is_error()) {
            WebIDL::reject_promise(*m_realm, promise, WebIDL::InvalidStateError::create("Invalid userscript storage reply"_utf16));
            return;
        }
        auto value = result_value((*pending)->request, (*pending)->fallback[0], decoded.release_value());
        if (!active()) return;
        if (value.is_error()) WebIDL::reject_promise_with_exception(*m_realm, promise, value.release_error());
        else WebIDL::resolve_promise(*m_realm, promise, value.release_value());
    }
    WebIDL::ExceptionOr<JS::Value> result_value(Protocol::Request const& request, JS::Value fallback, Protocol::Response response)
    {
        auto& vm = m_realm->vm();
        switch (request.method) {
        case Protocol::Method::TabGet: case Protocol::Method::TabGetLegacy:
            if (response.kind != Protocol::Kind::Value) break;
            if (!response.value.has_value()) return JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
            return decode_derpbird_value(vm, response.value->bytes(), *m_realm);
        case Protocol::Method::TabsGet: case Protocol::Method::TabsGetLegacy: {
            if (response.kind != Protocol::Kind::TabSnapshot) break;
            auto result = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
            for (auto const& entry : response.entries) {
                auto value = entry.value.has_value() ? TRY(decode_derpbird_value(vm, entry.value->bytes(), *m_realm))
                    : JS::Value(JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype()));
                result->define_direct_property(Utf16String::from_utf8(entry.key.bytes_as_string_view()), value, JS::default_attributes);
            }
            return result;
        }
        case Protocol::Method::TabSave: case Protocol::Method::TabSaveLegacy:
            if (response.kind == Protocol::Kind::Revision) return JS::js_undefined();
            break;
        case Protocol::Method::Cookie: case Protocol::Method::CookieLegacy:
        case Protocol::Method::Audio: case Protocol::Method::AudioLegacy:
        case Protocol::Method::NetworkRead:
        case Protocol::Method::DownloadWatch:
        case Protocol::Method::NotifyWait:
        case Protocol::Method::MenuWait:
        case Protocol::Method::TabWatch:
        case Protocol::Method::Network: case Protocol::Method::NetworkLegacy:
            if (response.kind == Protocol::Kind::Network && response.value.has_value())
                return JS::PrimitiveString::create(vm, Utf16String::from_utf8(StringView { response.value->bytes() }));
            break;
        case Protocol::Method::MenuRegister: case Protocol::Method::MenuRegisterLegacy: case Protocol::Method::MenuRemove: case Protocol::Method::MenuRemoveLegacy:
        case Protocol::Method::WindowClose: case Protocol::Method::WindowFocus:
        case Protocol::Method::TabOpen: case Protocol::Method::TabOpenLegacy: case Protocol::Method::TabClose:
        case Protocol::Method::Clipboard: case Protocol::Method::ClipboardLegacy:
        case Protocol::Method::Notify: case Protocol::Method::NotifyLegacy: case Protocol::Method::NotifyActivate:
        case Protocol::Method::Download: case Protocol::Method::DownloadLegacy: case Protocol::Method::DownloadAbort:
        case Protocol::Method::NetworkAbort:
            if (response.kind == Protocol::Kind::Revision) return JS::js_undefined();
            break;
        case Protocol::Method::DownloadChunk: case Protocol::Method::DownloadFinish:
            if (response.kind == Protocol::Kind::Revision) return JS::Value(response.revision == 0);
            break;
        case Protocol::Method::Subscribe: {
            if (response.kind != Protocol::Kind::Subscription) break;
            auto id = Protocol::subscription_id(response.token);
            if (!id.has_value() || *id > 9007199254740991ULL || m_listeners.contains(*id)) break;
            m_listeners.set(*id, make<Listener>(request.key, fallback.as_object()));
            return JS::Value(static_cast<double>(*id));
        }
        case Protocol::Method::Unsubscribe: {
            if (response.kind != Protocol::Kind::Revision) break;
            m_listeners.remove(*Protocol::subscription_id(request.key));
            return JS::js_undefined();
        }
        case Protocol::Method::Get:
            if (response.kind != Protocol::Kind::Value) break;
            if (!response.value.has_value()) return fallback;
            return decode_derpbird_value(vm, response.value->bytes(), *m_realm);
        case Protocol::Method::Set: case Protocol::Method::Delete: case Protocol::Method::SetMany: case Protocol::Method::DeleteMany:
            if (response.kind == Protocol::Kind::Revision) return JS::js_undefined();
            break;
        case Protocol::Method::GetMany: {
            if (response.kind != Protocol::Kind::Snapshot) break;
            auto& selection = fallback.as_object();
            HashMap<String, Protocol::Entry const*> stored;
            for (auto const& entry : response.entries) stored.set(entry.key, &entry);
            auto result = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
            for (auto key : TRY(selection.internal_own_property_keys())) {
                auto encoded = TRY_OR_THROW_OOM(vm, key.as_string().utf16_string_view().to_utf8());
                auto decoded = DerpBirdValueKey::decode(encoded);
                if (decoded.is_error()) return WebIDL::InvalidStateError::create("Invalid userscript selection key"_utf16);
                auto value = TRY(selection.get(key.as_string().utf16_string()));
                auto entry = stored.find(encoded);
                if (entry != stored.end() && entry->value->value.has_value())
                    value = TRY(decode_derpbird_value(vm, entry->value->value->bytes(), *m_realm));
                result->define_direct_property(decoded.release_value(), value, JS::default_attributes);
            }
            return result;
        }
        case Protocol::Method::List: {
            if (response.kind != Protocol::Kind::Keys) break;
            GC::RootVector<JS::Value> keys;
            for (auto const& entry : response.entries) {
                auto key = DerpBirdValueKey::decode(entry.key);
                if (key.is_error()) return WebIDL::InvalidStateError::create("Invalid stored userscript key"_utf16);
                keys.append(JS::PrimitiveString::create(vm, key.release_value()));
            }
            return JS::Array::create_from(*m_realm, keys.span());
        }
        default: break;
        }
        return WebIDL::InvalidStateError::create("Unexpected userscript storage reply"_utf16);
    }
    GC::Root<JS::Realm> m_realm;
    String m_script_id;
    String m_token;
    Send m_send;
    Function<bool()> m_active;
    HashMap<u64, NonnullOwnPtr<Listener>> m_listeners;
    HashMap<u64, NonnullOwnPtr<Pending>> m_pending;
    Vector<u64> m_waiting;
    u64 m_next_id { 1 };
    size_t m_reserved { 0 };
    size_t m_deferred_events { 0 };
    size_t m_deferred_event_bytes { 0 };
    bool m_closed { false };
    bool m_opening { false };
};

}
