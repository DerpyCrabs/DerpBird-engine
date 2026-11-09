#pragma once
#include <AK/JsonValue.h>
#include "DerpBirdNotificationImage.h"
#include <LibWeb/Page/DerpBirdValueProtocol.h>
#include <LibWebView/Application.h>
#include <LibWebView/NotificationBackend.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebContentClient.h>
#include <LibURL/Parser.h>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <memory>

namespace Ladybird {
class DerpBirdScriptNotifications final : public QObject {
public:
    using Protocol=Web::DerpBirdValueProtocol;
    using Completion=Function<void(ErrorOr<ByteBuffer>)>;
    Function<NonnullOwnPtr<WebView::NotificationBackend>()> create_backend;
    Function<void(WebView::WebContentPage&,URL::URL const&)> open_url;
    Function<void(WebView::WebContentPage&)> highlight;
    explicit DerpBirdScriptNotifications(QObject* parent):QObject(parent) { }
    ~DerpBirdScriptNotifications() override { shutdown(); }
    void shutdown() {
        auto entries=m_entries;m_entries.clear();
        if(m_backend)for(auto const& entry:entries)if(!entry->done)m_backend->close(entry->id);
    }
    void close_session(QString const& prefix) {
        auto keys=m_entries.keys();
        for(auto const& key:keys){
            auto entry=m_entries.value(key);if(!entry||!entry->scope.startsWith(prefix))continue;
            m_entries.remove(key);
            if(m_backend&&!entry->done)m_backend->close(entry->id);
        }
    }
    void prune() {
        auto keys=m_entries.keys();
        for(auto const& key:keys){
            auto entry=m_entries.value(key);if(!entry||entry->valid())continue;
            if(!entry->done&&entry->persistent&&entry->keep_alive&&entry->keep_alive()){
                entry->pending=nullptr;
                if(entry->wait){entry->wait->deleteLater();entry->wait=nullptr;}
                continue;
            }
            m_entries.remove(key);if(m_backend&&!entry->done)m_backend->close(entry->id);
        }
    }
    ErrorOr<void> dispatch(WebView::WebContentPage& page,QString const& token,QString scope,String const& script_name,
        Protocol::Request const& request,Function<bool()> valid,Completion completion,Function<bool()> keep_alive = {})
    {
        prune();
        auto key=token+":"+QString::fromUtf8(request.key.bytes_as_string_view().characters_without_null_termination(),request.key.bytes().size());
        if(request.method==Protocol::Method::Notify || request.method==Protocol::Method::NotifyLegacy){
            if(m_entries.size()>=64||m_entries.contains(key))return Error::from_string_literal("Too many or duplicate userscript notifications");
            auto json=TRY(JsonValue::from_string(request.updates[0].value->bytes()));
            if(!json.is_object())return Error::from_string_literal("Invalid notification options");
            auto const& options=json.as_object();
            auto text=options.get_string("text"sv), title=options.get_string("title"sv),tag=options.get_string("tag"sv);
            if(!text.has_value()||(text->is_empty()&&!options.get_bool("highlight"sv).value_or(false))||text->bytes().size()>8192||!title.has_value()||title->bytes().size()>512||!tag.has_value()||tag->bytes().size()>512)
                return Error::from_string_literal("Invalid notification text/title/tag");
            auto image=options.get_string("image"sv).value_or(""_string);
            Optional<URL::URL> action_url;
            auto address=options.get_string("url"sv).value_or(""_string);
            if(!address.is_empty()){
                action_url=URL::Parser::basic_parse(address);
                if(!action_url.has_value()||!(action_url->scheme()=="http"sv||action_url->scheme()=="https"sv)||!action_url->username().is_empty()||!action_url->password().is_empty())
                    return Error::from_string_literal("Invalid notification action URL");
            }
            auto timeout=options.get_integer<u32>("timeout"sv);
            if(!timeout.has_value()||*timeout>2147483647u)return Error::from_string_literal("Invalid notification timeout");
            auto entry=std::make_shared<Entry>(move(valid));entry->id=m_next_id++;entry->scope=move(scope);entry->tag=*tag;entry->keep_alive=move(keep_alive);entry->persistent=!tag->is_empty()||action_url.has_value();
            if(action_url.has_value())entry->activate=[this,url=*action_url,client=page.client().make_weak_ptr<WebView::WebContentClient>(),session=page.client().session().make_weak_ptr(),page_id=page.id()]{
                if(!session)return;
                auto* page=client?client->page(page_id):nullptr;
                if(!page||!page->is_open()){
                    WebView::Application::the().open_url_in_new_tab_from_notification(url,page_id,nullptr,session->is_private());
                    return;
                }
                if(open_url)open_url(*page,url);
                else WebView::Application::the().open_url_in_new_tab_from_notification(url,page_id,&page->traversable(),client->session().is_private());
            };
            WebView::PlatformNotification notification;
            notification.id=entry->id;notification.page_id=page.id();
            notification.attribution=TRY(String::formatted("DerpBird · {}",script_name));
            notification.data.title=Utf16String::from_utf8(title->is_empty()?script_name.bytes_as_string_view():title->bytes_as_string_view());
            notification.data.body=Utf16String::from_utf8(text->bytes_as_string_view());
            notification.data.tag=Utf16String::from_utf8(tag->bytes_as_string_view());
            notification.data.silent=options.get_bool("silent"sv).value_or(false);
            notification.data.require_interaction=*timeout==0;
            if (text->is_empty()) {
                // Highlight-only notifications have no desktop notification to
                // dismiss; deliver ondone through the normal observer protocol.
                entry->done=true;
                m_entries.insert(key,entry);
                auto lifetime=QPointer(this);
                if(highlight)highlight(page);
                if(!lifetime)return {};
                schedule_delivery();
                completion(Protocol::encode(Protocol::Response{Protocol::Kind::Revision,{},{},{},0}));return {};
            }
            ensure_backend();
            if(!tag->is_empty())for(auto const& old:m_entries){if(!old->done&&old->scope==entry->scope&&old->tag==*tag){
                notification.replaces=old->id;
                old->done=true;
                old->clicked=false;
                old->awaiting_action=false;
                old->activate=nullptr;
                if(old->decision){old->decision->stop();old->decision->deleteLater();old->decision=nullptr;}
                if(old->expiry){old->expiry->stop();old->expiry->deleteLater();old->expiry=nullptr;}
                break;
            }}
            m_entries.insert(key,entry);
            auto show=[self=QPointer(this),weak=std::weak_ptr<Entry>(entry),notification=move(notification),timeout=*timeout](RefPtr<Gfx::Bitmap const> icon) mutable {
                auto entry=weak.lock();if(!self||!entry||entry->done||(!entry->valid()&&!(entry->keep_alive&&entry->keep_alive()&&entry->persistent)))return;
                notification.icon=move(icon);self->m_backend->show(notification);
                if(!self||entry->done)return;
                if(timeout){
                    auto* timer=new QTimer(self);timer->setSingleShot(true);entry->expiry=timer;
                    connect(timer,&QTimer::timeout,timer,[self,weak]{if(auto item=weak.lock();self&&item&&!item->done)self->m_backend->close(item->id);});
                    timer->start(static_cast<int>(timeout));
                }
            };
            auto lifetime=QPointer(this);
            if(options.get_bool("highlight"sv).value_or(false)&&highlight)highlight(page);
            if(!lifetime)return {};
            if(image.is_empty())show({});
            else{auto* loader=new DerpBirdNotificationImage(this,page,move(show));entry->image=loader;loader->start(image);}
            if(!lifetime)return {};
            schedule_delivery();
            completion(Protocol::encode(Protocol::Response{Protocol::Kind::Revision,{},{},{},0}));return {};
        }
        auto entry=m_entries.value(key);
        if(!entry)return Error::from_string_literal("Unknown notification handle");
        if(request.method==Protocol::Method::NotifyActivate){
            if(!entry->awaiting_action){
                if(!entry->click_received)return Error::from_string_literal("Notification has no pending click action");
                // An expired/repeated acknowledgement completes harmlessly: it
                // cannot reopen the URL, and the script can still observe done.
                completion(Protocol::encode(Protocol::Response{Protocol::Kind::Revision,{},{},{},0}));return {};
            }
            entry->awaiting_action=false;
            if(entry->decision){entry->decision->stop();entry->decision->deleteLater();entry->decision=nullptr;}
            schedule_delivery();
            if(request.updates[0].value->data()[0] && entry->activate)entry->activate();
            completion(Protocol::encode(Protocol::Response{Protocol::Kind::Revision,{},{},{},0}));return {};
        }
        if(entry->pending)return Error::from_string_literal("Notification already has an observer");
        entry->pending=move(completion);
        if(deliver(key,entry))return {};
        auto* timer=new QTimer(this);timer->setSingleShot(true);entry->wait=timer;
        connect(timer,&QTimer::timeout,timer,[this,key,weak=std::weak_ptr<Entry>(entry)]{if(auto item=weak.lock())deliver(key,item,true);});
        timer->start(20000);return {};
    }
private:
    struct Entry {
        explicit Entry(Function<bool()> valid):valid(move(valid)) { }
        ~Entry(){if(image)delete image.data();if(expiry)expiry->deleteLater();if(wait)wait->deleteLater();if(decision)decision->deleteLater();}
        Function<bool()> valid,keep_alive;u64 id{0};QString scope;String tag;
        bool clicked{false},done{false},failed{false},persistent{false};
        QPointer<QTimer> expiry,wait,decision;QPointer<DerpBirdNotificationImage> image;Completion pending;
        Function<void()> activate;bool awaiting_action{false},click_received{false};
    };
    void ensure_backend(){
        if(m_backend)return;
        m_backend=create_backend?create_backend():WebView::Application::the().create_notification_backend();
        m_backend->on_shown=[](u64){};
        m_backend->on_clicked=[this](u64 id){notification_event(id,true,false,false);};
        m_backend->on_failed=[this](u64 id){notification_event(id,false,true,true);};
        m_backend->on_closed=[this](u64 id,auto){notification_event(id,false,true,false);};
    }
    void notification_event(u64 id,bool clicked,bool done,bool failed){
        prune();
        auto lifetime=QPointer(this);
        // Activation may open/close tabs and revoke capabilities synchronously.
        // Own the entries while invoking it instead of retaining map references.
        auto entries=m_entries.values();
        for(auto const& entry:entries)if(entry->id==id){
            if(entry->done)break;
            if(!entry->valid()){
                if(clicked&&!entry->click_received){entry->click_received=true;if(entry->activate)entry->activate();}
                entry->done|=done;entry->failed|=failed;
                break;
            }
            if(clicked&&!entry->click_received){
                entry->click_received=true;entry->clicked=true;entry->awaiting_action=!!entry->activate;
                if(entry->awaiting_action){
                    auto* timer=new QTimer(this);timer->setSingleShot(true);entry->decision=timer;
                    connect(timer,&QTimer::timeout,timer,[this,weak=std::weak_ptr<Entry>(entry)]{if(auto item=weak.lock()){item->awaiting_action=false;schedule_delivery();}});
                    timer->start(5000); // Missing renderer acknowledgement never opens a URL.
                }
            }
            entry->done|=done;entry->failed|=failed;break;
        }
        if(!lifetime)return;
        prune();
        schedule_delivery();
    }
    void schedule_delivery(){
        if (m_delivery_queued)
            return;
        m_delivery_queued = true;
        QTimer::singleShot(0,this,[this]{
            m_delivery_queued=false;
            auto keys=m_entries.keys();
            auto lifetime=QPointer(this);
            for(auto const& key:keys){
                if(!lifetime)return;
                if(auto entry=m_entries.value(key))deliver(key,entry);
            }
        });
    }
    bool deliver(QString const& key,std::shared_ptr<Entry> const& entry,bool force=false){
        if(!entry->pending||(!force&&!entry->clicked&&(!entry->done||entry->awaiting_action)))return false;
        if(!entry->valid()){prune();return true;}
        if(entry->wait){entry->wait->stop();entry->wait->deleteLater();entry->wait=nullptr;}
        JsonObject state;state.set("clicked"sv,entry->clicked);state.set("done"sv,entry->done&&!entry->awaiting_action);state.set("needsAction"sv,entry->awaiting_action);state.set("failed"sv,entry->failed);
        entry->clicked=false;auto done=move(entry->pending);if(entry->done&&!entry->awaiting_action)m_entries.remove(key);
        auto json=JsonValue(move(state)).serialized();done(Protocol::encode(Protocol::Response{Protocol::Kind::Network,{},MUST(ByteBuffer::copy(json.bytes())),{},0}));return true;
    }
    OwnPtr<WebView::NotificationBackend> m_backend;
    QHash<QString,std::shared_ptr<Entry>> m_entries;
    u64 m_next_id{1};bool m_delivery_queued{false};
};
}
