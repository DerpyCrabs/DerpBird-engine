#include "StructuredSerializeTestHelpers.h"
#include <LibJS/Script.h>
#include <LibWeb/Page/DerpBirdScriptValues.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibWebView/WebUI/DerpBirdValueStore.h>
#include <LibWeb/Page/DerpBirdValueCodec.h>
#include <LibWeb/Page/DerpBirdValueKey.h>
#include <LibWeb/Page/DerpBirdValueProtocol.h>

static_assert(WebView::DerpBirdValueStore::max_value_bytes == Web::derpbird_max_value_bytes);

static JS::Value evaluate_value_script(JS::Realm& realm, Utf16View source)
{
    auto& vm = realm.vm();
    auto& settings = Web::Bindings::principal_host_defined_environment_settings_object(realm);
    vm.push_execution_context(settings.realm_execution_context());
    ScopeGuard pop = [&] { vm.pop_execution_context(); };
    auto script = JS::Script::parse(source, realm);
    VERIFY(!script.is_error());
    auto result = vm.run(script.value());
    VERIFY(!result.is_error());
    return result.release_value();
}

TEST_CASE(derpbird_value_codec_retains_types_graphs_and_utf16)
{
    auto& realm = principal_test_realm();
    auto source = evaluate_value_script(realm, R"JS((()=>{
        const shared={ value: 7 };
        const result={ absent:undefined, nil:null, yes:true, no:false, zero:0, negativeZero:-0,
            nan:NaN, positive:Infinity, negative:-Infinity, fraction:1.25,
            text:'\u0000\ud800\udfff', array:[,undefined,null,3], first:shared, second:shared };
        result.self=result;
        Object.defineProperty(result,'__proto__',{value:{sentinel:42},enumerable:true});
        return result;
    })())JS"_utf16);
    auto encoded = MUST(Web::encode_derpbird_value(realm.vm(), source));
    VERIFY(encoded.size() >= 7);
    EXPECT(StringView(encoded.bytes().slice(0,4)) == "LBSC"sv);
    auto database = MUST(Database::Database::create_memory_backed());
    auto store = MUST(WebView::DerpBirdValueStore::create(database));
    Array updates { WebView::DerpBirdValueStore::Update { "graph"_string, MUST(ByteBuffer::copy(encoded.bytes())) } };
    MUST(store->apply("script"_string, updates));
    auto stored = MUST(store->get("script"_string, "graph"_string));
    VERIFY(stored.has_value());
    auto decoded = MUST(Web::decode_derpbird_value(realm.vm(), stored->bytes(), realm));
    realm.global_object().define_direct_property("derpDecoded"_utf16_fly_string, decoded, JS::default_attributes);
    EXPECT(evaluate_value_script(realm, R"JS((()=>{
        const x=derpDecoded;
        return Object.hasOwn(x,'absent') && x.absent===undefined && x.nil===null && x.yes===true && x.no===false
            && x.zero===0 && Object.is(x.negativeZero,-0) && Number.isNaN(x.nan)
            && x.positive===Infinity && x.negative===-Infinity && x.fraction===1.25
            && x.text.length===3 && x.text.charCodeAt(0)===0 && x.text.charCodeAt(1)===0xd800 && x.text.charCodeAt(2)===0xdfff
            && x.array.length===4 && !(0 in x.array) && (1 in x.array) && x.array[1]===undefined && x.array[2]===null
            && x.first===x.second && x.self===x && x.first.value===7
            && Object.hasOwn(x,'__proto__') && x.__proto__.sentinel===42 && Object.getPrototypeOf(x)===Object.prototype;
    })())JS"_utf16).as_bool());
    // Every read produces a new object graph, never references into an earlier read.
    evaluate_value_script(realm, "derpDecoded.first.value=99"_utf16);
    auto again = MUST(Web::decode_derpbird_value(realm.vm(), encoded.bytes(), realm));
    realm.global_object().define_direct_property("derpAgain"_utf16_fly_string, again, JS::default_attributes);
    EXPECT(evaluate_value_script(realm, "derpAgain!==derpDecoded && derpAgain.first.value===7"_utf16).as_bool());
    auto undefined = MUST(Web::encode_derpbird_value(realm.vm(), JS::js_undefined()));
    EXPECT(MUST(Web::decode_derpbird_value(realm.vm(), undefined.bytes(), realm)).is_undefined());
    Array missing_value { WebView::DerpBirdValueStore::Update { "undefined"_string, move(undefined) } };
    MUST(store->apply("script"_string, missing_value));
    auto persisted_undefined = MUST(store->get("script"_string, "undefined"_string));
    VERIFY(persisted_undefined.has_value());
    EXPECT(MUST(Web::decode_derpbird_value(realm.vm(), persisted_undefined->bytes(), realm)).is_undefined());
    EXPECT(!MUST(store->get("script"_string, "not-stored"_string)).has_value());
    // Restoring into another script realm must use that realm's objects, never
    // share source-realm prototypes or arrays through the persistence boundary.
    auto depth = realm.vm().execution_context_stack().size();
    auto target = Web::Bindings::create_a_simple_javascript_realm();
    ScopeGuard restore_context = [&] {
        while (realm.vm().execution_context_stack().size() > depth)
            realm.vm().pop_execution_context();
    };
    auto cross_realm = MUST(Web::decode_derpbird_value(realm.vm(), encoded.bytes(), *target));
    EXPECT(cross_realm.as_object().prototype() == target->intrinsics().object_prototype().ptr());
    EXPECT(property_of(cross_realm, "array"sv).as_object().prototype() == target->intrinsics().array_prototype().ptr());
    EXPECT(&cross_realm.as_object() != &source.as_object());
}

TEST_CASE(derpbird_value_codec_rejects_invalid_and_oversized_payloads)
{
    auto& realm = principal_test_realm();
    auto valid = MUST(Web::encode_derpbird_value(realm.vm(), JS::js_null()));
    for (size_t size=0; size<valid.size(); ++size)
        EXPECT(Web::decode_derpbird_value(realm.vm(), valid.bytes().slice(0,size), realm).is_error());
    auto invalid = MUST(ByteBuffer::copy(valid.bytes()));
    invalid[0]='X';
    EXPECT(Web::decode_derpbird_value(realm.vm(), invalid.bytes(), realm).is_error());
    invalid = MUST(ByteBuffer::copy(valid.bytes()));
    invalid[4]=2; // Unsupported LBSC version.
    EXPECT(Web::decode_derpbird_value(realm.vm(), invalid.bytes(), realm).is_error());
    auto huge = MUST(ByteBuffer::create_zeroed(Web::derpbird_max_value_bytes+1));
    EXPECT(Web::decode_derpbird_value(realm.vm(), huge.bytes(), realm).is_error());
    auto source = evaluate_value_script(realm, "'x'.repeat(1048577)"_utf16);
    EXPECT(Web::encode_derpbird_value(realm.vm(), source).is_error());
    for (auto script : { "(()=>{})"_utf16, "Symbol('unsupported')"_utf16, "({get value(){throw new Error('getter failure')}})"_utf16 })
        EXPECT(Web::encode_derpbird_value(realm.vm(), evaluate_value_script(realm, script)).is_error());
}

TEST_CASE(derpbird_value_keys_preserve_js_strings_across_protocol_and_sqlite)
{
    auto& realm = principal_test_realm();
    auto store = MUST(WebView::DerpBirdValueStore::create(MUST(Database::Database::create_memory_backed())));
    HashTable<String> encoded_keys;
    i32 index = 0;
    for (auto expression : { "''"_utf16, "'normal'"_utf16, "'a\\0b'"_utf16,
             "'\\ud800'"_utf16, "'\\udc00'"_utf16, "'\\ufffd'"_utf16,
             "'\\ud83d\\ude80'"_utf16, "'é'"_utf16, "'e\\u0301'"_utf16,
             "'u16:ANg='"_utf16, "'u8:normal'"_utf16 }) {
        auto key = evaluate_value_script(realm, expression).as_string().utf16_string();
        auto encoded = MUST(Web::DerpBirdValueKey::encode(key.utf16_view()));
        EXPECT(!encoded_keys.contains(encoded));
        encoded_keys.set(encoded);
        auto payload = MUST(Web::encode_derpbird_value(realm.vm(), JS::Value(index)));
        Web::DerpBirdValueProtocol::Request request { Web::DerpBirdValueProtocol::Method::Set, true, "token"_string, {}, {
            { encoded, move(payload) } } };
        auto wire = MUST(Web::DerpBirdValueProtocol::encode(request));
        auto decoded_request = MUST(Web::DerpBirdValueProtocol::decode_request(wire.bytes()));
        auto& update = decoded_request.updates[0];
        Array updates { WebView::DerpBirdValueStore::Update { move(update.key), move(update.value) } };
        MUST(store->apply("script"_string, updates));
        auto stored = MUST(store->get("script"_string, encoded));
        VERIFY(stored.has_value());
        EXPECT_EQ(MUST(Web::decode_derpbird_value(realm.vm(), stored->bytes(), realm)).as_i32(), index++);
        auto restored_key = MUST(Web::DerpBirdValueKey::decode(encoded));
        EXPECT(restored_key.utf16_view() == key.utf16_view());
    }
    EXPECT_EQ(MUST(store->keys("script"_string)).size(), encoded_keys.size());
}

TEST_CASE(derpbird_value_key_codec_is_injective_for_every_utf16_code_unit)
{
    HashTable<String> encoded_keys;
    for (u32 value = 0; value <= 0xffff; ++value) {
        char16_t unit = static_cast<char16_t>(value);
        Utf16View view { &unit, 1 };
        auto encoded = MUST(Web::DerpBirdValueKey::encode(view));
        EXPECT(!encoded_keys.contains(encoded));
        encoded_keys.set(encoded);
        auto restored_key = MUST(Web::DerpBirdValueKey::decode(encoded));
        EXPECT(restored_key.utf16_view() == view);
    }
    EXPECT_EQ(encoded_keys.size(), 65536u);
}

TEST_CASE(derpbird_value_key_codec_rejects_aliases_corruption_and_quota_overflow)
{
    for (auto invalid : { "key"sv, "u16:QQA="sv, "u16:ANg"sv, "u16: ANg="sv, "u16:AA=="sv, "u16:!!!="sv, "u16:"sv })
        EXPECT(Web::DerpBirdValueKey::decode(MUST(String::from_utf8(invalid))).is_error());
    auto& realm = principal_test_realm();
    auto boundary = evaluate_value_script(realm, "'x'.repeat(1021)"_utf16).as_string().utf16_string();
    auto encoded_boundary = MUST(Web::DerpBirdValueKey::encode(boundary.utf16_view()));
    EXPECT_EQ(encoded_boundary.bytes().size(), 1024u);
    auto oversized = evaluate_value_script(realm, "'x'.repeat(1022)"_utf16).as_string().utf16_string();
    EXPECT(Web::DerpBirdValueKey::encode(oversized.utf16_view()).is_error());
    auto surrogate_boundary = evaluate_value_script(realm, "'\\ud800'.repeat(382)"_utf16).as_string().utf16_string();
    auto encoded_surrogate_boundary = MUST(Web::DerpBirdValueKey::encode(surrogate_boundary.utf16_view()));
    EXPECT_EQ(encoded_surrogate_boundary.bytes().size(), 1024u);
    auto surrogate_overflow = evaluate_value_script(realm, "'\\ud800'.repeat(383)"_utf16).as_string().utf16_string();
    EXPECT(Web::DerpBirdValueKey::encode(surrogate_overflow.utf16_view()).is_error());
}

TEST_CASE(derpbird_value_key_js_conversion_propagates_exceptions_and_side_effects)
{
    auto& realm = principal_test_realm();
    auto key = evaluate_value_script(realm, "globalThis.keyConversions=0; ({toString(){keyConversions++; return 'x\\ud800'}})"_utf16);
    auto encoded = MUST(Web::encode_derpbird_value_key(realm.vm(), key));
    auto restored = MUST(Web::DerpBirdValueKey::decode(encoded));
    EXPECT_EQ(restored.utf16_view().length_in_code_units(), 2u);
    EXPECT_EQ(restored.utf16_view().code_unit_at(1), 0xd800);
    EXPECT_EQ(evaluate_value_script(realm, "keyConversions"_utf16).as_i32(), 1);
    for (auto expression : { "Symbol('key')"_utf16, "({toString(){throw new Error('key failure')}})"_utf16, "'x'.repeat(1022)"_utf16 })
        EXPECT(Web::encode_derpbird_value_key(realm.vm(), evaluate_value_script(realm, expression)).is_error());
}


static void value_microtask_checkpoint(JS::VM& vm)
{
    vm.save_execution_context_stack();
    ScopeGuard restore = [&] { vm.restore_execution_context_stack(); };
    Web::HTML::main_thread_event_loop().perform_a_microtask_checkpoint();
}

TEST_CASE(derpbird_value_promises_queue_in_order_and_preserve_defaults)
{
    using Protocol = Web::DerpBirdValueProtocol;
    using Values = Web::DerpBirdScriptValues;
    auto& realm = principal_test_realm();
    struct Queued { ByteBuffer payload; Values::Completion completion; };
    Vector<Queued> queue;
    bool active = true;
    auto api = Values::create(realm, "script"_string,
        [&](ByteBuffer payload, Values::Completion completion) -> ErrorOr<void> {
            queue.append({ move(payload), move(completion) });
            return {};
        }, [&] { return active; });
    auto gm = JS::Object::create(realm, realm.intrinsics().object_prototype());
    api->install(*gm, { "GM.getValue"_string, "GM.setValue"_string, "GM.deleteValue"_string, "GM.listValues"_string });
    realm.global_object().define_direct_property("derpGM"_utf16_fly_string, gm, JS::default_attributes);
    evaluate_value_script(realm, R"JS(
        globalThis.valueResults=[];
        globalThis.valueDefault={sentinel:42};
        derpGM.setValue('x\ud800',{n:NaN,z:-0,u:undefined}).then(x=>valueResults.push(x===undefined));
        derpGM.getValue('x\ud800').then(x=>valueResults.push(Number.isNaN(x.n)&&Object.is(x.z,-0)&&Object.hasOwn(x,'u')));
        derpGM.getValue('missing',valueDefault).then(x=>valueResults.push(x===valueDefault));
        derpGM.listValues().then(x=>valueResults.push(x.length===1&&x[0]==='x\ud800'));
        derpGM.deleteValue('x\ud800').then(x=>valueResults.push(x===undefined));
        derpGM.listValues().then(x=>valueResults.push(x.length===0));
    )JS"_utf16);
    EXPECT_EQ(queue.size(), 1u);
    EXPECT_EQ(api->pending(), 6u);
    auto store = MUST(WebView::DerpBirdValueStore::create(MUST(Database::Database::create_memory_backed())));
    size_t completed = 0;
    while (!queue.is_empty()) {
        auto item = queue.take_first();
        auto request = MUST(Protocol::decode_request(item.payload.bytes()));
        Protocol::Response response { Protocol::Kind::Revision, {}, {}, {}, 0 };
        switch (request.method) {
        case Protocol::Method::Open:
            EXPECT_EQ(completed, 0u);
            response.kind = Protocol::Kind::Token;
            response.token = "token"_string;
            break;
        case Protocol::Method::Set:
        case Protocol::Method::Delete: {
            EXPECT_EQ(completed, request.method == Protocol::Method::Set ? 1u : 5u);
            auto& entry = request.updates[0];
            Array updates { WebView::DerpBirdValueStore::Update { move(entry.key), move(entry.value) } };
            response.revision = MUST(store->apply("script"_string, updates));
            break;
        }
        case Protocol::Method::Get:
            response.kind = Protocol::Kind::Value;
            response.value = MUST(store->get("script"_string, request.key));
            break;
        case Protocol::Method::List:
            response.kind = Protocol::Kind::Keys;
            for (auto& key : MUST(store->keys("script"_string))) response.entries.append({ move(key), {} });
            break;
        default: VERIFY_NOT_REACHED();
        }
        item.completion(MUST(Protocol::encode(response)));
        ++completed;
    }
    value_microtask_checkpoint(realm.vm());
    EXPECT_EQ(completed, 7u);
    EXPECT_EQ(api->pending(), 0u);
    EXPECT(evaluate_value_script(realm, "valueResults.length===6 && valueResults.every(Boolean)"_utf16).as_bool());
    size_t delivered = 0;
    api->on_event = [&](Web::DerpBirdValueProtocol::Event const& event) {
        EXPECT_EQ(event.subscription, 42u);
        ++delivered;
        return true;
    };
    Protocol::Event notification { "other-token"_string, 42, 1, "u8:key"_string, true, {}, ByteBuffer {} };
    EXPECT(!api->receive_event(notification));
    notification.identity = "token"_string;
    EXPECT(api->receive_event(notification));
    EXPECT_EQ(delivered, 1u);
    // A reply after revocation must not settle the script's pending promise.
    evaluate_value_script(realm, "globalThis.lateValue=false; derpGM.getValue('x').then(()=>lateValue=true,()=>lateValue=true)"_utf16);
    EXPECT_EQ(queue.size(), 1u);
    active = false;
    api->close();
    queue.take_first().completion(Error::from_string_literal("late"));
    value_microtask_checkpoint(realm.vm());
    EXPECT_EQ(api->pending(), 0u);
    EXPECT(!evaluate_value_script(realm, "lateValue"_utf16).as_bool());
    EXPECT(!api->receive_event(notification));
    EXPECT_EQ(delivered, 1u);
    api->on_event = {};
    RefPtr<Values> closing;
    closing = Values::create(realm, "closing"_string,
        [](ByteBuffer, Values::Completion) -> ErrorOr<void> { VERIFY_NOT_REACHED(); },
        [&] { closing = nullptr; return false; });
    EXPECT(!closing->receive_event(notification));
    EXPECT(!closing);

}


TEST_CASE(derpbird_value_promises_recheck_limits_and_revocation_after_getters)
{
    using Values = Web::DerpBirdScriptValues;
    auto& realm = principal_test_realm();
    bool active = true;
    size_t sends = 0;
    auto api = Values::create(realm, "script"_string,
        [&](ByteBuffer, Values::Completion) -> ErrorOr<void> { ++sends; return {}; }, [&] { return active; });
    auto gm = JS::Object::create(realm, realm.intrinsics().object_prototype());
    api->install(*gm, { "GM.getValue"_string, "GM.setValue"_string });
    realm.global_object().define_direct_property("derpBoundedGM"_utf16_fly_string, gm, JS::default_attributes);
    evaluate_value_script(realm, R"JS(
        globalThis.valueQuota=false;
        derpBoundedGM.setValue('outer',{get x(){
            for(let i=0;i<128;i++)derpBoundedGM.getValue(i);
            return 1;
        }}).catch(e=>valueQuota=e.name==='QuotaExceededError');
    )JS"_utf16);
    value_microtask_checkpoint(realm.vm());
    EXPECT_EQ(api->pending(), 128u);
    EXPECT_EQ(sends, 1u);
    EXPECT(evaluate_value_script(realm, "valueQuota && typeof derpBoundedGM.deleteValue==='undefined'"_utf16).as_bool());
    api->close();

    auto revoked_api = Values::create(realm, "script"_string,
        [&](ByteBuffer, Values::Completion) -> ErrorOr<void> { ++sends; return {}; }, [&] { return active; });
    auto revoked_gm = JS::Object::create(realm, realm.intrinsics().object_prototype());
    revoked_api->install(*revoked_gm, { "GM.setValue"_string });
    realm.global_object().define_direct_property("derpRevokedGM"_utf16_fly_string, revoked_gm, JS::default_attributes);
    auto revoke = JS::NativeFunction::create(realm, [&](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
        active = false;
        return JS::js_undefined();
    }, 0, "revokeValueWorld"_utf16_fly_string);
    realm.global_object().define_direct_property("revokeValueWorld"_utf16_fly_string, revoke, JS::default_attributes);
    evaluate_value_script(realm, "derpRevokedGM.setValue('key',{get value(){revokeValueWorld(); return 42}})"_utf16);
    EXPECT_EQ(sends, 1u);
    EXPECT_EQ(revoked_api->pending(), 0u);
    // Remove the test closure before its stack capture expires.
    realm.global_object().define_direct_property("revokeValueWorld"_utf16_fly_string, JS::js_undefined(), JS::default_attributes);
}


TEST_CASE(derpbird_value_promises_reject_failures_and_retry_open)
{
    using Values = Web::DerpBirdScriptValues;
    using Protocol = Web::DerpBirdValueProtocol;
    auto& realm = principal_test_realm();
    Vector<Values::Completion> callbacks;
    bool immediate_failure = true;
    auto api = Values::create(realm, "script"_string,
        [&](ByteBuffer, Values::Completion completion) -> ErrorOr<void> {
            if (immediate_failure) return Error::from_string_literal("transport unavailable");
            callbacks.append(move(completion));
            return {};
        }, [] { return true; });
    auto gm = JS::Object::create(realm, realm.intrinsics().object_prototype());
    api->install(*gm, { "GM.getValue"_string });
    realm.global_object().define_direct_property("derpFailureGM"_utf16_fly_string, gm, JS::default_attributes);
    evaluate_value_script(realm, "globalThis.valueErrors=0; derpFailureGM.getValue('x').catch(()=>valueErrors++)"_utf16);
    value_microtask_checkpoint(realm.vm());
    EXPECT_EQ(api->pending(), 0u);
    EXPECT_EQ(evaluate_value_script(realm, "valueErrors"_utf16).as_i32(), 1);
    immediate_failure = false;
    evaluate_value_script(realm, "derpFailureGM.getValue('x').catch(()=>valueErrors++); derpFailureGM.getValue('y').catch(()=>valueErrors++)"_utf16);
    EXPECT_EQ(callbacks.size(), 1u);
    callbacks.take_first()(Error::from_string_literal("Open denied"));
    value_microtask_checkpoint(realm.vm());
    EXPECT_EQ(api->pending(), 0u);
    EXPECT_EQ(evaluate_value_script(realm, "valueErrors"_utf16).as_i32(), 3);
    evaluate_value_script(realm, "derpFailureGM.getValue('x').catch(()=>valueErrors++)"_utf16);
    callbacks.take_first()(MUST(Protocol::encode(Protocol::Response { Protocol::Kind::Token, "token"_string, {}, {}, 0 })));
    EXPECT_EQ(callbacks.size(), 1u);
    // A valid packet with the wrong operation kind must never resolve the promise.
    callbacks.take_first()(MUST(Protocol::encode(Protocol::Response { Protocol::Kind::Revision, {}, {}, {}, 0 })));
    value_microtask_checkpoint(realm.vm());
    EXPECT_EQ(api->pending(), 0u);
    EXPECT_EQ(evaluate_value_script(realm, "valueErrors"_utf16).as_i32(), 4);
    api->close();
}


TEST_CASE(derpbird_value_batch_promises_types_defaults_and_atomic_preparation)
{
    using Protocol = Web::DerpBirdValueProtocol;
    using Values = Web::DerpBirdScriptValues;
    auto& realm = principal_test_realm();
    struct Queued { ByteBuffer payload; Values::Completion completion; };
    Vector<Queued> queue;
    auto api = Values::create(realm, "batch"_string,
        [&](ByteBuffer payload, Values::Completion completion) -> ErrorOr<void> {
            queue.append({ move(payload), move(completion) }); return {};
        }, [] { return true; });
    auto gm = JS::Object::create(realm, realm.intrinsics().object_prototype());
    api->install(*gm, { "GM.getValues"_string, "GM.setValues"_string, "GM.deleteValues"_string });
    realm.global_object().define_direct_property("batchGM"_utf16_fly_string, gm, JS::default_attributes);
    evaluate_value_script(realm, R"JS(
        globalThis.batchResult='pending';
        (async()=>{
            const values=Object.create({inherited:1});
            values.a={n:NaN,z:-0}; values.u=undefined; values['x\ud800']=42;
            Object.defineProperty(values,'__proto__',{value:{safe:true},enumerable:true});
            Object.defineProperty(values,'hidden',{value:1}); values[Symbol('ignore')]=2;
            await batchGM.setValues(values);
            const fallback={identity:1};
            const defaults={a:0,u:7,missing:fallback};
            const promise=batchGM.getValues(defaults);
            defaults.missing='mutated';
            const read=await promise;
            if(!Number.isNaN(read.a.n)||!Object.is(read.a.z,-0)||read.u!==undefined||read.missing!==fallback)throw Error('defaults');
            const special=await batchGM.getValues(['__proto__','x\ud800','absent','__proto__']);
            if(!Object.hasOwn(special,'__proto__')||!special.__proto__.safe||Object.getPrototypeOf(special)!==Object.prototype||special['x\ud800']!==42||!Object.hasOwn(special,'absent')||special.absent!==undefined)throw Error('keys');
            const filtered=await batchGM.getValues(['hidden','inherited']);
            if(filtered.hidden!==undefined||filtered.inherited!==undefined)throw Error('enumeration');
            let rejected=0;
            for(const operation of [()=>batchGM.setValues({atomic:1,bad:()=>{}}),()=>batchGM.setValues({get bad(){throw Error('getter')}}),()=>batchGM.deleteValues({a:1}),()=>batchGM.getValues(null),()=>batchGM.deleteValues(Array(257).fill('a'))]) {
                try{await operation()}catch(e){rejected++}
            }
            if(rejected!==5||(await batchGM.getValues(['atomic'])).atomic!==undefined)throw Error('atomic');
            await batchGM.deleteValues(['a','u','x\ud800','__proto__','a']);
            const deleted=await batchGM.getValues({a:9,u:8});
            if(deleted.a!==9||deleted.u!==8)throw Error('delete');
            await batchGM.setValues({});await batchGM.deleteValues([]);
            if(Object.keys(await batchGM.getValues([])).length!==0)throw Error('empty');
            batchResult='passed';
        })().catch(e=>batchResult=String(e));
    )JS"_utf16);
    auto store = MUST(WebView::DerpBirdValueStore::create(MUST(Database::Database::create_memory_backed())));
    size_t requests = 0;
    while (!queue.is_empty()) {
        VERIFY(++requests < 30);
        auto item = queue.take_first();
        auto request = MUST(Protocol::decode_request(item.payload.bytes()));
        Protocol::Response response { Protocol::Kind::Revision, {}, {}, {}, 0 };
        if (request.method == Protocol::Method::Open) {
            response.kind = Protocol::Kind::Token; response.token = "token"_string;
        } else if (request.method == Protocol::Method::GetMany) {
            Vector<WebView::DerpBirdValueStore::Update> selection;
            for (auto const& entry : request.updates) selection.append({ entry.key, {} });
            VERIFY(!selection.is_empty());
            auto snapshot = MUST(store->selected("batch"_string, selection));
            response.kind = Protocol::Kind::Snapshot; response.revision = snapshot.revision;
            for (auto& entry : snapshot.values) response.entries.append({ move(entry.key), move(entry.value) });
        } else {
            VERIFY(request.method == Protocol::Method::SetMany || request.method == Protocol::Method::DeleteMany);
            Vector<WebView::DerpBirdValueStore::Update> updates;
            for (auto& entry : request.updates) updates.append({ move(entry.key), move(entry.value) });
            response.revision = MUST(store->apply("batch"_string, updates));
        }
        item.completion(MUST(Protocol::encode(response)));
        value_microtask_checkpoint(realm.vm());
    }
    EXPECT(evaluate_value_script(realm, "batchResult==='passed'"_utf16).as_bool());
    EXPECT_EQ(api->pending(), 0u);
    EXPECT(MUST(store->keys("batch"_string)).is_empty());
    api->close();
}

TEST_CASE(derpbird_value_listeners_deliver_remove_and_revoke)
{
    using Protocol = Web::DerpBirdValueProtocol;
    using Values = Web::DerpBirdScriptValues;
    auto& realm = principal_test_realm();
    size_t subscriptions = 0, removals = 0;
    bool active = true;
    auto key = MUST(Web::DerpBirdValueKey::encode(u"x\xd800"sv));
    auto api = Values::create(realm, "listeners"_string,
        [&](ByteBuffer payload, Values::Completion completion) -> ErrorOr<void> {
            auto request = MUST(Protocol::decode_request(payload.bytes()));
            Protocol::Response response { Protocol::Kind::Revision, {}, {}, {}, 0 };
            if (request.method == Protocol::Method::Open) {
                response.kind = Protocol::Kind::Token; response.token = "listener-token"_string;
            } else if (request.method == Protocol::Method::Subscribe) {
                EXPECT_EQ(request.key, key);
                response.kind = Protocol::Kind::Subscription;
                response.token = MUST(String::formatted("{}", ++subscriptions));
            } else if (request.method == Protocol::Method::Unsubscribe) {
                EXPECT(Protocol::subscription_id(request.key).has_value()); ++removals;
            } else VERIFY_NOT_REACHED();
            completion(MUST(Protocol::encode(response)));
            return {};
        }, [&] { return active; });
    auto gm = JS::Object::create(realm, realm.intrinsics().object_prototype());
    api->install(*gm, { "GM.addValueChangeListener"_string, "GM.removeValueChangeListener"_string });
    realm.global_object().define_direct_property("listenerGM"_utf16_fly_string, gm, JS::default_attributes);
    evaluate_value_script(realm, R"JS(
        globalThis.listenerEvents=[];globalThis.listenerId=0;globalThis.invalidListener=false;
        listenerGM.addValueChangeListener('x\ud800',(key,oldValue,newValue,remote)=>listenerEvents.push([key,oldValue,newValue,remote])).then(id=>listenerId=id);
        listenerGM.addValueChangeListener('x\ud800',42).catch(()=>invalidListener=true);
    )JS"_utf16);
    value_microtask_checkpoint(realm.vm());
    EXPECT(evaluate_value_script(realm, "listenerId===1 && invalidListener"_utf16).as_bool());
    EXPECT_EQ(subscriptions, 1u);
    auto new_value = MUST(Web::encode_derpbird_value(realm.vm(), evaluate_value_script(realm, "({x:NaN,z:-0})"_utf16)));
    Protocol::Event event { "listener-token"_string, 1, 1, key, true, {}, move(new_value) };
    EXPECT(api->receive_event(event));
    EXPECT(api->receive_event(event)); // Duplicate revision is acknowledged without replaying JS.
    EXPECT(evaluate_value_script(realm, R"JS(listenerEvents.length===1 && listenerEvents[0][0]==='x\ud800' && listenerEvents[0][1]===undefined && Number.isNaN(listenerEvents[0][2].x) && Object.is(listenerEvents[0][2].z,-0) && listenerEvents[0][3]===true)JS"_utf16).as_bool());
    event.key = "u8:wrong"_string;
    EXPECT(!api->receive_event(event));
    event.key = key;
    evaluate_value_script(realm, "listenerGM.removeValueChangeListener(listenerId)"_utf16);
    EXPECT_EQ(removals, 1u);
    event.revision = 2;
    EXPECT(!api->receive_event(event));
    evaluate_value_script(realm, "listenerGM.addValueChangeListener('x\\ud800',()=>{listenerGM.removeValueChangeListener(2);listenerEvents.push('self')})"_utf16);
    event.subscription = 2;
    EXPECT(api->receive_event(event));
    EXPECT(!api->receive_event(event));
    EXPECT_EQ(removals, 2u);
    evaluate_value_script(realm, "listenerGM.addValueChangeListener('x\\ud800',()=>listenerEvents.push('late'))"_utf16);
    event.subscription = 3;
    active = false;
    EXPECT(!api->receive_event(event));
    EXPECT(evaluate_value_script(realm, "listenerEvents.length===2 && listenerEvents[1]==='self'"_utf16).as_bool());
}

TEST_CASE(derpbird_value_listener_roots_pending_quota_and_reentrant_close)
{
    using Protocol = Web::DerpBirdValueProtocol;
    using Values = Web::DerpBirdScriptValues;
    auto& realm = principal_test_realm();
    struct Queued { ByteBuffer payload; Values::Completion completion; };
    Vector<Queued> queue;
    auto api = Values::create(realm, "listener-roots"_string,
        [&](ByteBuffer payload, Values::Completion completion) -> ErrorOr<void> { queue.append({ move(payload), move(completion) }); return {}; },
        [] { return true; });
    auto gm = JS::Object::create(realm, realm.intrinsics().object_prototype());
    api->install(*gm, { "GM.addValueChangeListener"_string });
    realm.global_object().define_direct_property("rootedListenerGM"_utf16_fly_string, gm, JS::default_attributes);
    auto revoke = JS::NativeFunction::create(realm, [weak = api->make_weak_ptr()](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        if (weak) weak->close();
        vm.heap().collect_garbage();
        return JS::js_undefined();
    }, 0, "revokeListenerApi"_utf16_fly_string, GC::Ptr<JS::Realm> { &realm });
    realm.global_object().define_direct_property("revokeListenerApi"_utf16_fly_string, revoke, JS::default_attributes);
    evaluate_value_script(realm, R"JS(
        globalThis.listenerQuota=0;globalThis.listenerAfterClose=0;
        for(let i=0;i<129;i++){
            const captured={number:i+42};
            rootedListenerGM.addValueChangeListener('root',()=>{
                revokeListenerApi();
                listenerAfterClose=captured.number;
            }).catch(()=>listenerQuota++);
        }
    )JS"_utf16);
    EXPECT_EQ(queue.size(), 1u);
    EXPECT_EQ(api->pending(), 128u);
    realm.vm().heap().collect_garbage();
    size_t next = 0;
    while (!queue.is_empty()) {
        auto item = queue.take_first();
        auto request = MUST(Protocol::decode_request(item.payload.bytes()));
        Protocol::Response response { Protocol::Kind::Token, "root-token"_string, {}, {}, 0 };
        if (request.method == Protocol::Method::Subscribe) {
            response.kind = Protocol::Kind::Subscription;
            response.token = MUST(String::formatted("{}", ++next));
        } else EXPECT(request.method == Protocol::Method::Open);
        item.completion(MUST(Protocol::encode(response)));
    }
    value_microtask_checkpoint(realm.vm());
    EXPECT_EQ(next, 128u);
    EXPECT(evaluate_value_script(realm, "listenerQuota===1"_utf16).as_bool());
    realm.vm().heap().collect_garbage();
    Protocol::Event event { "root-token"_string, 1, 1, "u8:root"_string, false, {}, MUST(Web::encode_derpbird_value(realm.vm(), JS::Value(1))) };
    EXPECT(api->receive_event(event));
    EXPECT(evaluate_value_script(realm, "listenerAfterClose===42"_utf16).as_bool());
    event.subscription = 2;
    EXPECT(!api->receive_event(event));
}

TEST_CASE(derpbird_sync_values_fail_retry_preserve_defaults_and_revoke)
{
    using Protocol = Web::DerpBirdValueProtocol;
    using Values = Web::DerpBirdScriptValues;
    auto& realm = principal_test_realm();
    bool active = true;
    bool fail = true;
    bool malformed = false;
    size_t requests = 0;
    auto api = Values::create(realm, "sync-script"_string,
        [&](ByteBuffer bytes, Values::Completion completion) -> ErrorOr<void> {
            ++requests;
            if (fail) return Error::from_string_literal("offline");
            auto request = TRY(Protocol::decode_request(bytes.bytes()));
            if (request.method == Protocol::Method::Open) {
                completion(TRY(Protocol::encode(Protocol::Response { Protocol::Kind::Token, "sync-token"_string, {}, {}, 0 })));
                return {};
            }
            EXPECT(!request.asynchronous);
            EXPECT(request.identity == "sync-token"sv);
            EXPECT(request.method == Protocol::Method::Get);
            if (request.key == "u8:revoke"sv) active = false;
            if (malformed) completion(ByteBuffer {});
            else completion(TRY(Protocol::encode(Protocol::Response { Protocol::Kind::Value, {}, {}, {}, 0 })));
            return {};
        }, [&] { return active; });
    api->install_legacy({ "GM_getValue"_string });
    auto throws = "(()=>{try{GM_getValue('x');return false;}catch(e){return e.name==='InvalidStateError';}})()"_utf16;
    EXPECT(evaluate_value_script(realm, throws).as_bool());
    fail = false;
    EXPECT(evaluate_value_script(realm, R"JS((()=>{
        const fallback={get then(){throw Error('assimilated default');}};
        return GM_getValue('x',fallback)===fallback && typeof GM_setValue==='undefined';
    })())JS"_utf16).as_bool());
    malformed = true;
    EXPECT(evaluate_value_script(realm, throws).as_bool());
    malformed = false;
    EXPECT(evaluate_value_script(realm, "GM_getValue('x',42)===42"_utf16).as_bool());
    auto before = requests;
    EXPECT(evaluate_value_script(realm, R"JS((()=>{
        try { GM_getValue({toString(){throw Error('conversion');}});return false; }
        catch(e){return e.message==='conversion';}
    })())JS"_utf16).as_bool());
    EXPECT_EQ(requests, before);
    EXPECT(evaluate_value_script(realm, R"JS((()=>{
        try { GM_getValue({toString(){GM_getValue('revoke');return 'outer';}});return false; }
        catch(e){return e.name==='InvalidStateError';}
    })())JS"_utf16).as_bool());
    EXPECT_EQ(requests, before + 1);
    EXPECT(evaluate_value_script(realm, throws).as_bool());
    EXPECT_EQ(requests, before + 1);
    api->close();
}
