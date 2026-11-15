#pragma once

#include <AK/Weakable.h>
#include <AK/Utf16StringBuilder.h>
#include <AK/StringBuilder.h>
#include <LibWeb/Page/DerpBirdPageScript.h>
#include <AK/RefCounted.h>
#include <LibWeb/Page/DerpBirdScriptRegistry.h>
#include <LibWeb/Page/DerpBirdScriptWorld.h>
#include <LibWeb/Page/DerpBirdWebRequestRegistration.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>

namespace Web {

// One immutable selection per document. Native lifecycle hooks supply phases;
// updating the installed registry never changes a document's running sources.
class DerpBirdDocumentScripts : public RefCounted<DerpBirdDocumentScripts>, public Weakable<DerpBirdDocumentScripts> {
    AK_MAKE_NONCOPYABLE(DerpBirdDocumentScripts);
    AK_MAKE_NONMOVABLE(DerpBirdDocumentScripts);
public:
    AK_ALLOC_WITH_KMALLOC;
    enum class Status { Pending, Executed, Failed, UnsupportedRequirements, Cancelled };
    struct Entry {
        String id;
        u64 generation;
        Utf16String source;
        JsonObject info;
        JsonObject resources;
        DerpBirdScriptRunAt run_at;
        Status status;
        bool page_world;
        Vector<String> grants;
        OwnPtr<DerpBirdScriptWorld> world;
        Vector<OwnPtr<DerpBirdScriptWorld>> previous_invocations;
        bool javascript_sandbox { false };
        bool raw_with_grants { false };
        RefPtr<DerpBirdWebRequestRegistration> request_rules;
        Optional<String> pending_request_configuration;
    };

    static ErrorOr<NonnullRefPtr<DerpBirdDocumentScripts>> create(DOM::Document& document, DerpBirdScriptRegistry const& registry, DerpBirdScriptValues::Send send = {})
    {
        auto context = document.browsing_context();
        if (!context || !document.is_fully_active())
            return Error::from_string_literal("Userscript document is inactive");
        auto session = adopt_ref(*new DerpBirdDocumentScripts(document));
        session->m_send = move(send);
        DerpBirdScriptMatchBudget budget;
        for (auto const& definition : registry.definitions()) {
            if (!definition.enabled)
                continue;
            auto eligibility = definition.url_policy.evaluate(document.url(), context->is_top_level(), definition.file_access_granted, budget);
            // An indeterminate restriction denies this script, not every script
            // already selected for the document. Keep the shared work budget:
            // later rules may use its remainder, but never receive a fresh quota.
            if (eligibility == DerpBirdScriptEligibility::LimitExceeded)
                continue;
            if (eligibility != DerpBirdScriptEligibility::Allowed)
                continue;
            // Only wired providers qualify. @grant none never receives capabilities.
            bool unsupported_grant = false;
            for (auto const& grant : definition.grants)
                unsupported_grant |= grant != "GM_webRequest"sv && grant != "window.onurlchange"sv && grant != "unsafeWindow"sv && grant != "GM_info"sv && grant != "GM.info"sv && grant != "GM_addStyle"sv && grant != "GM.addStyle"sv
                    && grant != "GM_log"sv && grant != "GM.log"sv && grant != "GM_addElement"sv && grant != "GM.addElement"sv
                    && grant != "GM_getResourceText"sv && grant != "GM_getResourceURL"sv
                    && grant != "GM.getResourceText"sv && grant != "GM.getResourceUrl"sv && grant != "GM.getResourceURL"sv
                    && !(session->m_send && (grant == "GM.audio"sv || grant == "GM_audio"sv || grant == "window.close"sv || grant == "window.focus"sv || grant == "GM.cookie"sv || grant == "GM_cookie"sv || grant == "GM.notification"sv || grant == "GM_notification"sv || grant == "GM.download"sv || grant == "GM_download"sv || grant == "GM.getTab"sv || grant == "GM_getTab"sv || grant == "GM.saveTab"sv || grant == "GM_saveTab"sv || grant == "GM.getTabs"sv || grant == "GM_getTabs"sv || grant == "GM_getValue"sv || grant == "GM_setValue"sv || grant == "GM_deleteValue"sv || grant == "GM_listValues"sv || grant == "GM_getValues"sv || grant == "GM_setValues"sv || grant == "GM_deleteValues"sv || grant == "GM_addValueChangeListener"sv || grant == "GM_removeValueChangeListener"sv || grant == "GM.registerMenuCommand"sv || grant == "GM_registerMenuCommand"sv || grant == "GM.unregisterMenuCommand"sv || grant == "GM_unregisterMenuCommand"sv || grant == "GM.openInTab"sv || grant == "GM_openInTab"sv || grant == "GM.setClipboard"sv || grant == "GM_setClipboard"sv || grant == "GM.xmlHttpRequest"sv || grant == "GM_xmlhttpRequest"sv || grant == "GM.getValue"sv || grant == "GM.setValue"sv
                        || grant == "GM.deleteValue"sv || grant == "GM.listValues"sv
                        || grant == "GM.getValues"sv || grant == "GM.setValues"sv || grant == "GM.deleteValues"sv
                        || grant == "GM.addValueChangeListener"sv || grant == "GM.removeValueChangeListener"sv));
            bool unsupported = unsupported_grant
                || definition.required_scripts.size() != definition.required_sources.size() || definition.resources.size() != definition.resource_values.size();
            Utf16StringBuilder combined;
            if (!unsupported) {
                for (auto const& dependency : definition.required_sources) {
                    combined.append(dependency.utf16_view());
                    combined.append_ascii("\n;\n"sv);
                }
                combined.append(definition.source.utf16_view());
            }
            session->m_entries.append(Entry {
                definition.id, definition.generation, combined.to_string(), unsupported ? JsonObject {} : definition.info, unsupported ? JsonObject {} : definition.resource_values, definition.run_at,
                unsupported ? Status::UnsupportedRequirements : Status::Pending, definition.grant_none || (definition.sandbox == "raw"sv && definition.grants.is_empty()), definition.grants, {}, {}, definition.sandbox == "JavaScript"sv, definition.sandbox == "raw"sv && !definition.grant_none && !definition.grants.is_empty(), {}, {}
            });
            if (!unsupported && (definition.has_web_request_metadata || definition.grants.contains_slow("GM_webRequest"sv))) {
                if (definition.run_at == DerpBirdScriptRunAt::ContextMenu)
                    session->m_entries.last().pending_request_configuration = definition.configuration;
                else
                    session->m_entries.last().request_rules = TRY(DerpBirdWebRequestRegistration::create(document, definition));
            }
        }
        session->m_observer->set_document_became_inactive(GC::create_function(GC::Heap::the(), GC::captures(session->make_weak_ptr()), [](auto& weak) {
            if (weak)
                weak->close();
        }));
        return session;
    }

    ~DerpBirdDocumentScripts() { close(); }

    ErrorOr<void> dispatch(DerpBirdScriptRunAt phase, StringView selected_script = {}, u64 selected_generation = 0)
    {
        if (m_closed || !m_document->is_fully_active())
            return Error::from_string_literal("Userscript document session is closed");
        if (m_dispatching)
            return Error::from_string_literal("Userscript phase dispatch is already active");
        if (phase == DerpBirdScriptRunAt::ContextMenu && selected_script.is_empty())
            return Error::from_string_literal("Context-menu scripts require an explicit script command");
        m_dispatching = true;
        ScopeGuard finish = [&] {
            m_dispatching = false;
            if (m_closed)
                release_closed_worlds();
            else
                release_revoked_worlds();
        };
        for (auto& entry : m_entries) {
            if (m_closed)
                break;
            bool explicit_command = phase == DerpBirdScriptRunAt::ContextMenu;
            if (entry.run_at != phase) continue;
            if (explicit_command) {
                if (entry.id != selected_script || entry.generation != selected_generation) continue;
                if (entry.status == Status::Cancelled || entry.status == Status::UnsupportedRequirements)
                    return Error::from_string_literal("Context-menu script is unavailable");
                if (entry.previous_invocations.size() >= 31)
                    return Error::from_string_literal("Reload the page before running more script instances");
                if (entry.world) entry.previous_invocations.append(move(entry.world));
            } else if (entry.status != Status::Pending) continue;
            // Mark before invoking script so exceptions or reentrant events can
            // never cause an automatic retry with duplicate DOM side effects.
            entry.status = Status::Failed;
            if (entry.pending_request_configuration.has_value()) {
                auto configuration = TRY(JsonValue::from_string(*entry.pending_request_configuration));
                auto definition = TRY(DerpBirdScriptDefinition::decode(configuration.as_object()));
                entry.request_rules = TRY(DerpBirdWebRequestRegistration::create(*m_document, definition));
                entry.pending_request_configuration.clear();
            }
            // A function scope permits top-level return and prevents declarations
            // in independent userscripts from colliding. Explicit window writes
            // still target the selected realm, including page globals for grant none.
            Utf16StringBuilder source;
            // Keep API bindings in a per-script closure even in the shared page
            // realm. Construct JSON data as a literal, never via page JSON.parse.
            source.append_ascii("(function(GM_info,GM){GM.info=GM_info;"sv);
            if (!entry.page_world)
                source.append_ascii("const scriptIsActive=__derpbirdIsActive;"sv);
            if (entry.raw_with_grants && (entry.grants.contains_slow("window.close"sv) || entry.grants.contains_slow("window.focus"sv))) {
                source.append_ascii(R"JS(
                    const rawWindow=globalThis, windowOverrides=Object.create(null), boundWindowMethods=new Map();
                    const window=new Proxy(rawWindow, {
                        get(target,key) {
                            if (Object.prototype.hasOwnProperty.call(windowOverrides,key)) return windowOverrides[key];
                            if(key==='window'||key==='self'||key==='frames')return window;
                            const value=Reflect.get(target,key,target);
                            if(typeof value!=='function'||Object.prototype.hasOwnProperty.call(value,'prototype'))return value;
                            if(!boundWindowMethods.has(value))boundWindowMethods.set(value,value.bind(target));
                            return boundWindowMethods.get(value);
                        },
                        set(target,key,value) {
                            if(key==='close'||key==='focus'){windowOverrides[key]=value;return true;}
                            return Reflect.set(target,key,value,target);
                        }
                    });
                )JS"sv);
            }

            if (!entry.page_world) {
                if (entry.grants.contains_slow("GM_log"sv)) source.append_ascii("const GM_log=(...args)=>console.log(...args);"sv);
                if (entry.grants.contains_slow("GM.log"sv)) source.append_ascii("GM.log=(...args)=>console.log(...args);"sv);
            }
            if (!entry.page_world && (entry.grants.contains_slow("GM_addElement"sv) || entry.grants.contains_slow("GM.addElement"sv))) {
                source.append_ascii(R"JS(
                    function addElement(parent, tag, attributes) {
                        if (!scriptIsActive()) throw new Error("Userscript was revoked");
                        try {
                            if (typeof parent === 'string') { attributes=tag; tag=parent; parent=null; }
                            const element=document.createElement(String(tag));
                            for (const [key,value] of Object.entries(attributes || {})) {
                                __derpbirdConfigureElement(element,key,String(value));
                            }
                            const attach=()=>{
                                if (!scriptIsActive()) return true;
                                const target=parent || document.head || document.body || document.documentElement;
                                if (!target) return false;
                                __derpbirdInsertElement(element,target); return true;
                            };
                            if (!attach()) {
                                const observer=new MutationObserver(()=>{
                                    try { if(attach()) observer.disconnect(); }
                                    catch(error) { observer.disconnect(); console.error(error); }
                                });
                                observer.observe(document,{childList:true,subtree:true});
                            }
                            return element;
                        } catch { return null; }
                    }
                )JS"sv);
                if (entry.grants.contains_slow("GM_addElement"sv)) source.append_ascii("const GM_addElement=addElement;"sv);
                if (entry.grants.contains_slow("GM.addElement"sv)) source.append_ascii("GM.addElement=async (...args)=>addElement(...args);"sv);
            }
            if (!entry.page_world && (entry.grants.contains_slow("GM.registerMenuCommand"sv) || entry.grants.contains_slow("GM_registerMenuCommand"sv)
                || entry.grants.contains_slow("GM.unregisterMenuCommand"sv) || entry.grants.contains_slow("GM_unregisterMenuCommand"sv))) {
                source.append_ascii(R"JS(
                    const menuRegister=GM.__menuRegister,menuRegisterLegacy=GM.__menuRegisterLegacy,
                        menuRemove=GM.__menuRemove,menuRemoveLegacy=GM.__menuRemoveLegacy,menuWait=GM.__menuWait;
                    delete GM.__menuRegister;delete GM.__menuRegisterLegacy;delete GM.__menuRemove;delete GM.__menuRemoveLegacy;delete GM.__menuWait;
                    const menuCallbacks=new Map();let nextMenuId=1,menuWatching=false;
                    async function watchMenu(){
                        if(menuWatching||!menuCallbacks.size)return;
                        menuWatching=true;
                        try {
                            while(menuCallbacks.size) {
                                for(const data of JSON.parse(await menuWait('1'))) {
                                    const entry=menuCallbacks.get(data.id);if(!entry)continue;
                                    const event=data.keyboard ? new KeyboardEvent('keydown',data) : new MouseEvent('click',data);
                                    try{entry.callback(event);}catch(error){console.error(error);}
                                }
                            }
                        } finally {menuWatching=false;}
                    }
                    function registerMenu(name,callback,options,register,legacy){
                        if(typeof callback!=='function')throw new TypeError('Expected menu callback');
                        options=typeof options==='string'?{accessKey:options}:options||{};
                        const existing=options.id==null?null:String(options.id);
                        const id=existing&&menuCallbacks.has(existing)?existing:String(nextMenuId++);
                        const previous=menuCallbacks.get(id),entry={callback};menuCallbacks.set(id,entry);
                        const config={name:String(name),title:String(options.title??''),accessKey:String(options.accessKey??''),autoClose:options.autoClose!==false,clickedFrame:!!options.clickedFrame};
                        const ready=register(JSON.stringify(config),id).then(()=>{void watchMenu().catch(error=>console.error(error));return Number(id);},error=>{
                            if(menuCallbacks.get(id)===entry){if(previous)menuCallbacks.set(id,previous);else menuCallbacks.delete(id);}throw error;
                        });
                        if(legacy){void ready.catch(error=>console.error(error));return Number(id);}return ready;
                    }
                    function unregisterMenu(id,remove,legacy){
                        const key=String(id);menuCallbacks.delete(key);
                        const ready=remove(key);if(legacy){void ready.catch(error=>console.error(error));return;}return ready;
                    }
                )JS"sv);
                if(entry.grants.contains_slow("GM.registerMenuCommand"sv))source.append_ascii("GM.registerMenuCommand=(name,callback,options)=>registerMenu(name,callback,options,menuRegister,false);"sv);
                if(entry.grants.contains_slow("GM_registerMenuCommand"sv))source.append_ascii("const GM_registerMenuCommand=(name,callback,options)=>registerMenu(name,callback,options,menuRegisterLegacy,true);"sv);
                if(entry.grants.contains_slow("GM.unregisterMenuCommand"sv))source.append_ascii("GM.unregisterMenuCommand=id=>unregisterMenu(id,menuRemove,false);"sv);
                if(entry.grants.contains_slow("GM_unregisterMenuCommand"sv))source.append_ascii("const GM_unregisterMenuCommand=id=>unregisterMenu(id,menuRemoveLegacy,true);"sv);
            }
            if (!entry.page_world) {
                if (entry.grants.contains_slow("GM_getTab"sv)) source.append_ascii("const getTabLegacy=GM.__getTabLegacy;delete GM.__getTabLegacy;const GM_getTab=callback=>{if(typeof callback!=='function')throw new TypeError('Expected callback');void getTabLegacy().then(callback).catch(console.error);};"sv);
                if (entry.grants.contains_slow("GM_saveTab"sv)) source.append_ascii("const saveTabLegacy=GM.__saveTabLegacy;delete GM.__saveTabLegacy;const GM_saveTab=(tab,callback)=>{void saveTabLegacy(tab).then(()=>{if(typeof callback==='function')callback();}).catch(console.error);};"sv);
                if (entry.grants.contains_slow("GM_getTabs"sv)) source.append_ascii("const getTabsLegacy=GM.__getTabsLegacy;delete GM.__getTabsLegacy;const GM_getTabs=callback=>{if(typeof callback!=='function')throw new TypeError('Expected callback');void getTabsLegacy().then(callback).catch(console.error);};"sv);
            }
            if (!entry.page_world && (entry.grants.contains_slow("GM.cookie"sv) || entry.grants.contains_slow("GM_cookie"sv))) {
                source.append_ascii(R"JS(
                    const cookieModern=GM.__cookie,cookieLegacy=GM.__cookieLegacy;
                    delete GM.__cookie;delete GM.__cookieLegacy;
                    function cookieAPI(send){
                        const api={};
                        for(const operation of ['list','set','delete'])api[operation]=(details={},callback)=>{
                            if(!details||typeof details!=='object')throw new TypeError('Expected cookie details');
                            if(callback!==undefined&&typeof callback!=='function')throw new TypeError('Expected cookie callback');
                            const ready=send(operation,JSON.stringify(details)).then(JSON.parse).then(value=>operation==='list'?value:undefined);
                            if(callback){void ready.then(value=>operation==='list'?callback(value,null):callback(),error=>operation==='list'?callback([],String(error)):callback(String(error)));return;}
                            return ready;
                        };
                        return Object.freeze(api);
                    }
                )JS"sv);
                if(entry.grants.contains_slow("GM.cookie"sv))source.append_ascii("GM.cookie=cookieAPI(cookieModern);"sv);
                if(entry.grants.contains_slow("GM_cookie"sv))source.append_ascii("const GM_cookie=cookieAPI(cookieLegacy);"sv);
            }
            if (!entry.page_world && (entry.grants.contains_slow("GM.notification"sv) || entry.grants.contains_slow("GM_notification"sv))) {
                source.append_ascii(R"JS(
                    const notifyModern=GM.__notify,notifyLegacy=GM.__notifyLegacy,notifyWait=GM.__notifyWait,notifyActivate=GM.__notifyActivate;
                    delete GM.__notify;delete GM.__notifyLegacy;delete GM.__notifyWait;delete GM.__notifyActivate;
                    let nextNotificationId=1;
                    function notifyScript(input,title,image,onclick,start,legacy){
                        const details=typeof input==='object'&&input!==null?{...input}:{text:String(input),title,image,onclick};
                        if(typeof input==='object'&&typeof title==='function')details.ondone=title;
                        const config={text:String(details.text??''),title:String(details.title??''),tag:String(details.tag??''),
                            silent:!!details.silent,timeout:Number(details.timeout??0),highlight:!!details.highlight,image:details.image?new URL(String(details.image),document.baseURI).href:'',url:details.url?new URL(String(details.url),document.baseURI).href:''};
                        const id=String(nextNotificationId++),ready=start(JSON.stringify(config),id);
                        const call=(name,arg)=>{if(typeof details[name]==='function'){try{details[name].call(details,arg);}catch(error){console.error(error);}}};
                        const finished=ready.then(async()=>{let clicked=false;for(;;){const state=JSON.parse(await notifyWait(id));
                            if(state.clicked){clicked=true;const event=new Event('click',{cancelable:true});call('onclick',event);if(state.needsAction)await notifyActivate(id,!event.defaultPrevented);}
                            if(state.done){call('ondone',clicked);if(state.failed)console.error('Desktop notification could not be shown');return clicked;}
                        }});
                        if(legacy){void finished.catch(console.error);return;}
                        return finished;
                    }
                )JS"sv);
                if(entry.grants.contains_slow("GM.notification"sv))source.append_ascii("GM.notification=(a,b,c,d)=>notifyScript(a,b,c,d,notifyModern,false);"sv);
                if(entry.grants.contains_slow("GM_notification"sv))source.append_ascii("const GM_notification=(a,b,c,d)=>notifyScript(a,b,c,d,notifyLegacy,true);"sv);
            }
            if (!entry.page_world && (entry.grants.contains_slow("GM.download"sv) || entry.grants.contains_slow("GM_download"sv))) {
                source.append_ascii(R"JS(
                    const downloadModern=GM.__download, downloadLegacy=GM.__downloadLegacy,
                        downloadAbort=GM.__downloadAbort, downloadWatch=GM.__downloadWatch,
                        downloadChunk=GM.__downloadChunk, downloadFinish=GM.__downloadFinish;
                    delete GM.__download; delete GM.__downloadLegacy; delete GM.__downloadAbort; delete GM.__downloadWatch; delete GM.__downloadChunk; delete GM.__downloadFinish;
                    let nextDownloadId=1;
                    function downloadFile(input,name,start,legacy) {
                        const details=typeof input==='string'||input instanceof Blob ? {url:input,name} : {...input};
                        let blob=details.url instanceof Blob ? details.url : null;
                        const config={url:blob?document.URL:new URL(String(details.url),document.baseURI).href,name:details.name??(blob instanceof File?blob.name:undefined),
                            saveAs:!!details.saveAs,conflictAction:details.conflictAction ?? 'uniquify'};
                        if(details.headers!==undefined)config.headers=details.headers;
                        const id=String(nextDownloadId++);
                        let aborted=false,finished=false,timedOut=false,timer,abortRequest;
                        const ready=(async()=>{
                            if(!blob&&config.url.startsWith('blob:'))blob=await (await fetch(config.url)).blob();
                            if(blob){config.blobSize=blob.size;config.url=document.URL;}
                            await start(JSON.stringify(config),id);
                        })();
                        // Share one cancellation between the public handle,
                        // timeout, and producer failure paths. A terminal watch
                        // may already have retired the native handle.
                        const cancel=()=>abortRequest??=(ready.then(()=>downloadAbort(id)).catch(()=>{}));
                        const abort=()=>{if(finished)return;aborted=true;void cancel();};
                        const notify=(name,event)=>{if(typeof details[name]==='function'){try{details[name](event);}catch(error){console.error(error);}}};
                        const result=ready.then(async()=>{
                            if(Number(details.timeout)>0)timer=setTimeout(()=>{timedOut=true;abort();},Number(details.timeout));
                            if(aborted)await cancel();
                            if(blob&&!aborted){
                                let initial;
                                do { initial=JSON.parse(await downloadWatch(id)); } while(initial.status==='waiting');
                                if(initial.status==='failed'||initial.status==='canceled')throw Object.assign(new Error(initial.message||initial.status),{error:'not_succeeded',details:initial});
                                const waitForProducer=async()=>{
                                    while(!aborted){
                                        const state=JSON.parse(await downloadWatch(id));
                                        if(state.status==='failed'||state.status==='canceled')throw new Error(state.message||state.status);
                                        if(state.status!=='paused'&&state.status!=='waiting')return;
                                    }
                                };
                                try {
                                    for(let offset=0;offset<blob.size&&!aborted;offset+=131072){
                                        const bytes=await blob.slice(offset,offset+131072).arrayBuffer();
                                        if(aborted)break;
                                        while(!aborted && !await downloadChunk(id,bytes)) await waitForProducer();
                                        if(!aborted)notify('onprogress',{loaded:Math.min(offset+bytes.byteLength,blob.size),total:blob.size,lengthComputable:true});
                                    }
                                    while(!aborted && !await downloadFinish(id)) await waitForProducer();
                                } catch(error){await cancel();throw error;}
                            }
                            for(;;){
                                const state=JSON.parse(await downloadWatch(id));
                                if(state.status==='completed'){notify('onload',state);return state;}
                                if(state.status==='failed'||state.status==='canceled')throw Object.assign(new Error(state.message||state.status),{error:'not_succeeded',details:state});
                                notify('onprogress',state);
                            }
                        }).catch(error=>{notify(timedOut?'ontimeout':'onerror',{error:error.error||'not_succeeded',details:error.details||String(error)});throw error;})
                          .finally(()=>{finished=true;clearTimeout(timer);});
                        if(legacy){void result.catch(()=>{});return {abort};}
                        result.abort=abort;return result;
                    }
                )JS"sv);
                if (entry.grants.contains_slow("GM.download"sv)) source.append_ascii("GM.download=(input,name)=>downloadFile(input,name,downloadModern,false);"sv);
                if (entry.grants.contains_slow("GM_download"sv)) source.append_ascii("const GM_download=(input,name)=>downloadFile(input,name,downloadLegacy,true);"sv);
            }
            if (!entry.page_world && entry.grants.contains_slow("GM_webRequest"sv))
                source.append_ascii("const GM_webRequest=(rules,listener)=>{ if(listener!==undefined&&typeof listener!=='function')throw new TypeError('Expected webRequest listener'); __derpbirdWebRequest(JSON.stringify(rules), listener===undefined?undefined:text=>{const event=JSON.parse(text);const {info,message,...details}=event;listener(info,message,details);}); };"sv);
            if (!entry.page_world && (entry.grants.contains_slow("GM.audio"sv) || entry.grants.contains_slow("GM_audio"sv))) {
                source.append_ascii(R"JS(
                    function createAudioAPI(send) {
                        const listeners = new Set(), pending = new Map();
                        let running = false, previous;
                        const request = (action, value = {}) => send(action, JSON.stringify(value)).then(JSON.parse);
                        const callbackResult = (promise, callback, state = false) => {
                            if (typeof callback !== 'function') return promise;
                            void promise.then(value => state ? callback(value) : callback(),
                                error => state ? callback(undefined, String(error)) : callback(String(error)));
                        };
                        async function observe() {
                            if (running) return;
                            running = true;
                            try {
                                while (listeners.size) {
                                    const text = JSON.stringify(previous);
                                    const next = await request('watch', {previous: text});
                                    const change = {};
                                    if (next.isMuted !== previous.isMuted || next.muteReason !== previous.muteReason)
                                        change.muted = next.isMuted ? next.muteReason : false;
                                    if (next.isAudible !== previous.isAudible) change.audible = next.isAudible;
                                    previous = next;
                                    if (Object.keys(change).length) for (const listener of [...listeners]) {
                                        if (!listeners.has(listener)) continue;
                                        try { listener(change); } catch (error) { console.error(error); }
                                    }
                                }
                            } catch (error) { listeners.clear(); console.error(error); }
                            finally { running = false; }
                        }
                        return Object.freeze({
                            setMute(details, callback) {
                                const promise = Promise.resolve().then(() => {
                                    if (!details || typeof details.isMuted !== 'boolean') throw new TypeError('Expected isMuted boolean');
                                    return request('set', {isMuted: details.isMuted});
                                }).then(() => undefined);
                                return callbackResult(promise, callback);
                            },
                            getState(callback) { return callbackResult(request('get'), callback, true); },
                            addStateChangeListener(listener, callback) {
                                const promise = Promise.resolve().then(async () => {
                                    if (typeof listener !== 'function') throw new TypeError('Expected audio listener');
                                    if (listeners.has(listener)) return;
                                    if (!pending.has(listener) && listeners.size + pending.size >= 64) throw new Error('Too many audio listeners');
                                    const ticket = {};
                                    pending.set(listener, ticket);
                                    let current;
                                    try { current = await request('get'); }
                                    catch (error) { if (pending.get(listener) === ticket) pending.delete(listener); throw error; }
                                    if (pending.get(listener) !== ticket) return;
                                    pending.delete(listener);
                                    if (!running) previous = current;
                                    listeners.add(listener);
                                    void observe();
                                });
                                return callbackResult(promise, callback);
                            },
                            removeStateChangeListener(listener, callback) {
                                pending.delete(listener);
                                listeners.delete(listener);
                                const promise = (listeners.size ? Promise.resolve() : request('cancel')).then(() => undefined);
                                return callbackResult(promise, callback);
                            }
                        });
                    }
                )JS"sv);
                if (entry.grants.contains_slow("GM.audio"sv)) source.append_ascii("GM.audio=createAudioAPI(GM.__audio); delete GM.__audio;"sv);
                if (entry.grants.contains_slow("GM_audio"sv)) source.append_ascii("const GM_audio=createAudioAPI(GM.__audioLegacy); delete GM.__audioLegacy;"sv);
            }
            if (!entry.page_world && entry.grants.contains_slow("window.close"sv))
                source.append_ascii("{ const close=GM.__windowClose; delete GM.__windowClose; window.close=()=>{ void close().catch(error=>console.error(error)); }; }"sv);
            if (!entry.page_world && entry.grants.contains_slow("window.focus"sv))
                source.append_ascii("{ const focus=GM.__windowFocus; delete GM.__windowFocus; window.focus=()=>{ void focus().catch(error=>console.error(error)); }; }"sv);
            if (!entry.page_world && (entry.grants.contains_slow("GM.openInTab"sv) || entry.grants.contains_slow("GM_openInTab"sv))) {
                source.append_ascii(R"JS(
                    const tabOpen=GM.__tabOpen, tabOpenLegacy=GM.__tabOpenLegacy, tabClose=GM.__tabClose, tabWatch=GM.__tabWatch;
                    delete GM.__tabOpen; delete GM.__tabOpenLegacy; delete GM.__tabClose; delete GM.__tabWatch;
                    let nextTabId=1;
                    function openTab(url, options, open, legacy) {
                        options=typeof options==='boolean' ? {active:!options} : options || {};
                        const config={url:new URL(String(url),document.baseURI).href,
                            active:!!options.active, setParent:!!options.setParent, insert:options.insert ?? false};
                        if(options.incognito!==undefined) config.incognito=!!options.incognito;
                        const id=String(nextTabId++);
                        let closed=false;
                        const handle={close(){void ready.then(()=>tabClose(id)).catch(error=>console.error(error));},onclose:null,get closed(){return closed;}};
                        const ready=open(JSON.stringify(config),id);
                        const watch=async()=>{
                            while(!closed) {
                                if(await tabWatch(id)==='closed') {
                                    closed=true;
                                    if(typeof handle.onclose==='function') handle.onclose.call(handle);
                                }
                            }
                        };
                        void ready.then(watch,error=>{closed=true;if(legacy)console.error(error);}).catch(error=>{closed=true;console.error(error);});
                        return legacy ? handle : ready.then(()=>handle);
                    }
                )JS"sv);
                if (entry.grants.contains_slow("GM.openInTab"sv)) source.append_ascii("GM.openInTab=(url,options)=>openTab(url,options,tabOpen,false);"sv);
                if (entry.grants.contains_slow("GM_openInTab"sv)) source.append_ascii("const GM_openInTab=(url,options)=>openTab(url,options,tabOpenLegacy,true);"sv);
            }
            if (!entry.page_world && (entry.grants.contains_slow("GM.setClipboard"sv) || entry.grants.contains_slow("GM_setClipboard"sv))) {
                source.append_ascii(R"JS(
                    const clipboardModern=GM.__clipboard, clipboardLegacy=GM.__clipboardLegacy;
                    delete GM.__clipboard; delete GM.__clipboardLegacy;
                    function clipboardType(info) {
                        const type=typeof info==='string' ? info : info?.mimetype || info?.type || 'text';
                        if(type==='text'||type==='text/plain') return 'text/plain';
                        if(type==='html'||type==='text/html') return 'text/html';
                        throw new TypeError('Unsupported clipboard MIME type');
                    }
                )JS"sv);
                if (entry.grants.contains_slow("GM.setClipboard"sv)) source.append_ascii("GM.setClipboard=async (data,info)=>clipboardModern(String(data),clipboardType(info));"sv);
                if (entry.grants.contains_slow("GM_setClipboard"sv)) source.append_ascii("const GM_setClipboard=(data,info,callback)=>{const pending=clipboardLegacy(String(data),clipboardType(info)); pending.then(()=>{if(typeof callback==='function')callback();},error=>console.error(error));};"sv);
            }
            if (!entry.page_world && (entry.grants.contains_slow("GM_addStyle"sv) || entry.grants.contains_slow("GM.addStyle"sv))) {
                source.append_ascii(R"JS(
                    function addStyle(css) {
                        if (!scriptIsActive()) throw new Error("Userscript was revoked");
                        const style = document.createElement('style');
                        __derpbirdConfigureElement(style,'textContent',String(css));
                        const attach = () => {
                            if (!scriptIsActive()) return true;
                            const parent = document.head || document.documentElement;
                            if (!parent) return false;
                            if (!style.parentNode) __derpbirdInsertElement(style,parent);
                            return true;
                        };
                        if (!attach()) {
                            const observer = new MutationObserver(() => {
                                try { if (attach()) observer.disconnect(); }
                                catch (error) { observer.disconnect(); console.error(error); }
                            });
                            observer.observe(document, { childList: true, subtree: true });
                        }
                        return style;
                    }
                )JS"sv);
                if (entry.grants.contains_slow("GM_addStyle"sv))
                    source.append_ascii("const GM_addStyle=addStyle;"sv);
                if (entry.grants.contains_slow("GM.addStyle"sv))
                    source.append_ascii("GM.addStyle=async (css)=>addStyle(css);"sv);
            }
            if (!entry.page_world) {
                source.append_ascii("const resources = JSON.parse("sv);
                auto resource_json = Utf16String::from_utf8(JsonValue { entry.resources.serialized() }.serialized());
                source.append(resource_json.utf16_view());
                source.append_ascii(R"JS();
                    const resource = (name, property) => {
                        const key = String(name);
                        if (!Object.prototype.hasOwnProperty.call(resources, key)) throw new Error('Unknown userscript resource: ' + key);
                        return resources[key][property];
                    };
                )JS"sv);
                if (entry.grants.contains_slow("GM_getResourceText"sv)) source.append_ascii("const GM_getResourceText=name=>resource(name,'text');"sv);
                if (entry.grants.contains_slow("GM_getResourceURL"sv)) source.append_ascii("const GM_getResourceURL=name=>resource(name,'url');"sv);
                if (entry.grants.contains_slow("GM.getResourceText"sv)) source.append_ascii("GM.getResourceText=async name=>resource(name,'text');"sv);
                if (entry.grants.contains_slow("GM.getResourceUrl"sv)) source.append_ascii("GM.getResourceUrl=async name=>resource(name,'url');"sv);
                if (entry.grants.contains_slow("GM.getResourceURL"sv)) source.append_ascii("GM.getResourceURL=async name=>resource(name,'url');"sv);
            }
            if (!entry.page_world && (entry.grants.contains_slow("GM.xmlHttpRequest"sv) || entry.grants.contains_slow("GM_xmlhttpRequest"sv))) {
                source.append_ascii(R"JS(
                    const networkModern = GM.__network, networkLegacy = GM.__networkLegacy, networkAbort = GM.__networkAbort, networkRead = GM.__networkRead;
                    const networkSync=typeof __derpbirdNetworkSync === 'function' ? __derpbirdNetworkSync : undefined;
                    delete GM.__network; delete GM.__networkLegacy; delete GM.__networkAbort; delete GM.__networkRead;
                    let nextNetworkId = 1;
                    function networkRequest(details, nativeRequest, legacy) {
                        if (!details || typeof details !== 'object') throw new TypeError('Expected request details');
                        if(details.synchronous&&details.responseType==='stream')throw new TypeError('A response stream requires an asynchronous request');
                        if(details.synchronous&&!legacy)throw new TypeError('Synchronous requests require GM_xmlhttpRequest');
                        const id = String(nextNetworkId++);
                        const type = details.responseType || 'text';
                        if (!['text','json','arraybuffer','blob','document','stream'].includes(type)) throw new TypeError('Unsupported responseType');
                        const context = details.context;
                        const headers = Object.fromEntries(Object.entries(details.headers || {}).map(([key,value])=>[key,String(value)]));
                        const options = {url: new URL(String(details.url), document.baseURI).href, method: String(details.method || 'GET'), headers,
                            anonymous: !!details.anonymous, stream: type==='stream', timeout: Math.max(0,Math.floor(Number(details.timeout)||0))};
                        let aborted = false, settled = false;
                        const callback = (event, result) => {
                            const fn = details['on'+event];
                            if (typeof fn === 'function') { try { fn(result); } catch(error) { console.error(error); } }
                        };
                        const abort = () => {
                            if (settled || aborted) return;
                            aborted = true;
                            networkAbort(id).catch(()=>{});
                        };
                        function deliver(raw) {
                            if(type==='stream'&&raw.event==='headers'){
                                let loaded=0;
                                const result={status:raw.status,statusText:raw.statusText,finalUrl:raw.finalUrl,responseHeaders:raw.responseHeaders,readyState:2,context,loaded:0,lengthComputable:false};
                                const response=new ReadableStream({
                                    async pull(controller){
                                        try{
                                            const chunk=JSON.parse(await networkRead(id));
                                            const bytes=Uint8Array.from(atob(chunk.body||''),c=>c.charCodeAt(0));
                                            loaded+=bytes.length;result.loaded=loaded;result.readyState=3;
                                            if(bytes.length){controller.enqueue(bytes);callback('progress',result);}
                                            if(chunk.event==='data')return;
                                            settled=true;result.readyState=4;callback('readystatechange',result);
                                            if(chunk.event==='load'){controller.close();callback('load',result);callback('loadend',result);}
                                            else{controller.error(new Error('Userscript request '+chunk.event));callback(chunk.event,result);callback('loadend',result);}
                                        }catch(error){
                                            if(!settled){settled=true;result.readyState=4;result.error=error;callback('error',result);callback('loadend',result);}
                                            controller.error(error);
                                        }
                                    },
                                    cancel(){aborted=true;settled=true;result.readyState=4;callback('abort',result);callback('loadend',result);return networkAbort(id);}
                                },{highWaterMark:0});
                                result.response=response;
                                callback('readystatechange',result);callback('loadstart',result);
                                return result;
                            }
                            const binary = atob(raw.body || '');
                            const bytes = Uint8Array.from(binary,c=>c.charCodeAt(0));
                            const mime = String(details.overrideMimeType || (/^content-type:\s*([^\r\n]+)/im.exec(raw.responseHeaders)?.[1]) || 'text/plain');
                            const charset = /charset\s*=\s*["']?([^;\s"']+)/i.exec(mime)?.[1] || 'utf-8';
                            let text;
                            try { text = new TextDecoder(charset).decode(bytes); } catch { text = new TextDecoder().decode(bytes); }
                            let response = text;
                            if (type === 'arraybuffer') response=bytes.buffer;
                            if (type === 'blob') response=new Blob([bytes],{type:mime.split(';')[0]});
                            if (type === 'json') { try { response=JSON.parse(text); } catch { response=null; } }
                            if (type === 'document') response=new DOMParser().parseFromString(text,/xml/i.test(mime)?'application/xml':'text/html');
                            const result = {status:raw.status,statusText:raw.statusText,finalUrl:raw.finalUrl,responseHeaders:raw.responseHeaders,
                                readyState:4,response,responseText:text,responseXML:type==='document'?response:null,context,lengthComputable:true,loaded:bytes.length,total:bytes.length};
                            settled=true;
                            callback('readystatechange',result);
                            if (raw.event === 'load') { callback('progress',result); callback('load',result); callback('loadend',result); return result; }
                            callback(raw.event,result); callback('loadend',result);
                            throw Object.assign(new Error('Userscript request '+raw.event),{response:result,reported:true});

                        }
                        if(details.synchronous){
                            let data=details.data;
                            if(data instanceof Blob||data instanceof FormData)throw new TypeError('Synchronous uploads require text or bytes');
                            if(data!=null){
                                if(data instanceof URLSearchParams&&!Object.keys(headers).some(k=>k.toLowerCase()==='content-type'))headers['Content-Type']='application/x-www-form-urlencoded;charset=UTF-8';
                                const bytes=data instanceof ArrayBuffer?new Uint8Array(data):ArrayBuffer.isView(data)?new Uint8Array(data.buffer,data.byteOffset,data.byteLength):details.binary?Uint8Array.from(String(data),c=>c.charCodeAt(0)&255):new TextEncoder().encode(String(data));
                                if(bytes.length>700000)throw new RangeError('Userscript upload exceeds limit');
                                let binary='';for(let i=0;i<bytes.length;i+=8192)binary+=String.fromCharCode(...bytes.subarray(i,i+8192));
                                options.body=btoa(binary);
                            }
                            callback('loadstart',{readyState:1,context});
                            try{return deliver(JSON.parse(networkSync(JSON.stringify(options),id)));}
                            catch(error){if(!error?.reported){const result={error,context,readyState:4,status:0};callback('error',result);callback('loadend',result);}throw error;}
                        }
                        const promise = (async () => {
                            let data = details.data;
                            if (data != null) {
                                let bytes;
                                if (data instanceof Blob) bytes = new Uint8Array(await data.arrayBuffer());
                                else if (data instanceof ArrayBuffer) bytes = new Uint8Array(data);
                                else if (ArrayBuffer.isView(data)) bytes = new Uint8Array(data.buffer,data.byteOffset,data.byteLength);
                                else if (data instanceof FormData) {
                                    const boundary = '----DerpBird'+crypto.randomUUID();
                                    const chunks = [];
                                    for (const [name,value] of data) {
                                        const quote = value => String(value).replace(/[\r\n]/g,' ').replace(/"/g,'%22');
                                        chunks.push('--'+boundary+'\r\nContent-Disposition: form-data; name="'+quote(name)+'"');
                                        if (value instanceof Blob) chunks.push('; filename="'+quote(value.name || 'blob')+'"\r\nContent-Type: '+(value.type || 'application/octet-stream'));
                                        chunks.push('\r\n\r\n', value, '\r\n');
                                    }
                                    chunks.push('--'+boundary+'--\r\n');
                                    bytes = new Uint8Array(await new Blob(chunks).arrayBuffer());
                                    if (!Object.keys(headers).some(k=>k.toLowerCase()==='content-type')) headers['Content-Type']='multipart/form-data; boundary='+boundary;
                                } else {
                                    if (data instanceof URLSearchParams && !Object.keys(headers).some(k=>k.toLowerCase()==='content-type')) headers['Content-Type']='application/x-www-form-urlencoded;charset=UTF-8';
                                    const text = String(data);
                                    bytes = details.binary ? Uint8Array.from(text,c=>c.charCodeAt(0)&255) : new TextEncoder().encode(text);
                                }
                                if (bytes.length > 700000) throw new RangeError('Userscript upload exceeds limit');
                                let binary = '';
                                for (let i=0;i<bytes.length;i+=8192) binary += String.fromCharCode(...bytes.subarray(i,i+8192));
                                options.body=btoa(binary);
                            }
                            if (aborted) return {event:'abort',status:0,statusText:'',responseHeaders:'',finalUrl:options.url,body:''};
                            if(type!=='stream')callback('loadstart',{readyState:1,context});
                            return JSON.parse(await nativeRequest(JSON.stringify(options),id));
                        })().then(deliver).catch(error => {
                            settled=true;
                            if (!error?.reported) { const result={error,context,readyState:4,status:0}; callback('error',result); callback('loadend',result); }
                            throw error;
                        });
                        promise.abort=abort;
                        if (legacy) { promise.catch(()=>{}); return {abort}; }
                        return promise;
                    }
                )JS"sv);
                if (entry.grants.contains_slow("GM.xmlHttpRequest"sv)) source.append_ascii("GM.xmlHttpRequest=details=>networkRequest(details,networkModern,false);"sv);
                if (entry.grants.contains_slow("GM_xmlhttpRequest"sv)) source.append_ascii("const GM_xmlhttpRequest=details=>networkRequest(details,networkLegacy,true);"sv);
            }
            source.append_ascii("return (function(GM_info,GM){\n"sv);
            source.append(entry.source.utf16_view());
            source.append_ascii("\n}).call(this,GM_info,GM);}).call(this,"sv);
            auto info_source = Utf16String::from_utf8(JsonValue { entry.info }.serialized());
            source.append(info_source.utf16_view());
            source.append_ascii(entry.page_world ? ",{});"sv : entry.raw_with_grants ? ",__derpbirdGM);"sv : ",globalThis.GM ?? {});"sv);
            bool succeeded = false;
            if (entry.page_world) {
                auto result = run_derpbird_page_script(*m_document, entry.id.bytes_as_string_view(), source.view());
                if (result.is_error())
                    warnln("Userscript {} could not run in page: {}", entry.id, result.error());
                succeeded = !result.is_error() && result.value();
            } else {
                auto world = DerpBirdScriptWorld::create(*m_document);
                if (!world.is_error()) {
                    entry.world = world.release_value();
                    entry.world->install_element_insertion(entry.grants, entry.raw_with_grants);
                    if (entry.request_rules && entry.grants.contains_slow("GM_webRequest"sv)) entry.world->install_web_requests(*entry.request_rules, entry.raw_with_grants);
                    if (entry.javascript_sandbox || entry.grants.contains_slow("unsafeWindow"sv)) entry.world->expose_unsafe_window();
                    if (entry.grants.contains_slow("window.onurlchange"sv)) {
                        auto installed = entry.world->expose_url_changes(entry.raw_with_grants);
                        if (installed.is_error()) { entry.world->close(); entry.status = Status::Failed; continue; }
                    }
                    if (m_send) {
                        entry.world->install_values(entry.id, entry.grants,
                            [weak = make_weak_ptr()](ByteBuffer payload, DerpBirdScriptValues::Completion completion) -> ErrorOr<void> {
                                if (!weak || weak->m_closed)
                                    return Error::from_string_literal("Userscript session is closed");
                                NonnullRefPtr owner { *weak };
                                return owner->m_send(move(payload), move(completion));
                            }, entry.raw_with_grants);
                    }
                    auto completion = entry.raw_with_grants ? entry.world->run_raw(source.view()) : entry.world->run(source.view());
                    succeeded = !completion.is_error() && !completion.value().is_abrupt();
                    if (completion.is_error())
                        warnln("Userscript {} could not execute: {}", entry.id, completion.error());
                    else if (completion.value().is_error())
                        HTML::report_exception(completion.value(), entry.raw_with_grants ? m_document->window()->principal_realm() : entry.world->realm());
                } else {
                    warnln("Userscript {} could not create isolated world: {}", entry.id, world.error());
                }
            }
            if (succeeded && !m_closed && entry.status != Status::Cancelled)
                entry.status = Status::Executed;
            else if (!m_closed && entry.status != Status::Cancelled)
                warnln("Userscript {} failed in {} world", entry.id, entry.page_world ? "page" : "isolated");
            if (!explicit_command) {
                entry.source = {};
                entry.info = {};
                entry.resources = {};
            }
        }
        return {};
    }

    // Registry updates never inject new sources into an existing document.
    // Unchanged generations retain callbacks; changed/removed ones lose theirs.
    void reconcile(DerpBirdScriptRegistry const& registry)
    {
        if (m_closed)
            return;
        for (auto& entry : m_entries) {
            bool retained = false;
            for (auto const& definition : registry.definitions()) {
                if (definition.id == entry.id && definition.enabled && definition.generation == entry.generation) {
                    retained = true;
                    break;
                }
            }
            if (retained)
                continue;
            if (entry.request_rules) entry.request_rules->close();
            if (entry.world)
                entry.world->close();
            for (auto& invocation : entry.previous_invocations) invocation->close();
            entry.status = Status::Cancelled;
            entry.source = {};
            entry.info = {};
            entry.resources = {};
        }
        if (!m_dispatching)
            release_revoked_worlds();
        // @grant none executes in the page realm: previously registered page
        // callbacks cannot be individually detached, and carry no capabilities.
    }

    Function<void()> on_close;

    void close()
    {
        if (m_closed)
            return;
        m_closed = true;
        if (on_close) {
            auto callback = move(on_close);
            callback();
        }
        m_observer->set_document_became_inactive(nullptr);
        m_observer = {};
        for (auto& entry : m_entries) {
            if (entry.request_rules) entry.request_rules->close();
            if (entry.world)
                entry.world->close();
            for (auto& invocation : entry.previous_invocations) invocation->close();
            if (entry.status == Status::Pending)
                entry.status = Status::Cancelled;
        }
        if (!m_dispatching)
            release_closed_worlds();
    }

    bool receive_value_event(ReadonlyBytes payload)
    {
        if (m_closed || !m_document || !m_document->is_fully_active()) return false;
        auto event = DerpBirdValueProtocol::decode_event(payload);
        if (event.is_error()) return false;
        for (auto& entry : m_entries) {
            if (entry.world && entry.world->receive_value_event(event.value())) return true;
            for (auto& invocation : entry.previous_invocations)
                if (invocation->receive_value_event(event.value())) return true;
        }
        return false;
    }

    Vector<Entry> const& entries() const { return m_entries; }
    bool is_closed() const { return m_closed; }

private:
    explicit DerpBirdDocumentScripts(DOM::Document& document)
        : m_document(document)
        , m_observer(DOM::DocumentObserver::create(document))
    {
    }
    void release_revoked_worlds()
    {
        for (auto& entry : m_entries) {
            if (entry.status == Status::Cancelled) {
                entry.world = nullptr;
                entry.previous_invocations.clear();
            }
        }
    }
    void release_closed_worlds()
    {
        for (auto& entry : m_entries) {
            entry.world = nullptr;
            entry.previous_invocations.clear();
            entry.source = {};
            entry.info = {};
            entry.resources = {};
        }
        m_document = {};
    }

    GC::Root<DOM::Document> m_document;
    GC::Root<DOM::DocumentObserver> m_observer;
    Vector<Entry> m_entries;
    DerpBirdScriptValues::Send m_send;
    bool m_closed { false };
    bool m_dispatching { false };
};

}
