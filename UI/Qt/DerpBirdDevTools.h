#pragma once
#include <LibURL/Parser.h>
#include <UI/Qt/WebContentView.h>
#include <UI/Qt/StringUtils.h>
#include <QVBoxLayout>
#include <QPointer>
#include <QSettings>
#include <QSplitter>
#include <QTimer>
#include <QResizeEvent>
#include <cmath>

namespace Ladybird {
// Keep the tools share independent of window size and Qt's size-hint stretch weights.
class DerpBirdDevToolsSplitter final : public QSplitter {
public:
    explicit DerpBirdDevToolsSplitter(QWidget* parent) : QSplitter(parent) { setOpaqueResize(true); }
    double fraction { 0.4 };
    bool leading { false };
    void apply_fraction()
    {
        if(count()!=2) return;
        int available = qMax(0, (orientation()==Qt::Vertical ? height() : width())-handleWidth());
        int tools = qRound(available*fraction);
        setSizes(leading ? QList<int>{tools,available-tools} : QList<int>{available-tools,tools});
    }
protected:
    void resizeEvent(QResizeEvent* event) override { QSplitter::resizeEvent(event); apply_fraction(); }
};
class DerpBirdDevTools final : public QWidget {
public:
    DerpBirdDevTools(WebContentView& target, QWidget* parent, QSettings& settings)
        : QWidget(parent), m_target(&target), m_tab(target.parentWidget()), m_window(parent), m_settings(settings), m_page_minimum(target.minimumSize())
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle("Developer tools");
        m_save_preferences.setSingleShot(true); m_save_preferences.setInterval(250);
        connect(&m_save_preferences, &QTimer::timeout, this, [this] { m_settings.sync(); });
        WebContentViewInitialState initial; initial.is_private = target.is_private();
        m_ui = new WebContentView(this, {}, initial);
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->addWidget(m_ui);
        connect(&target, &QObject::destroyed, this, [this] { m_target = nullptr; close(); });
        target.on_derpbird_devtools = [self = QPointer(this)](JsonValue const& packet) {
            if (self) self->m_ui->send_derpbird_shell_message("derpbird.devtools"_string, packet);
        };
        m_ui->on_derpbird_request = [this](JsonValue const& payload, auto done) {
            if (!m_target) { done(Error::from_string_literal("Inspected tab closed")); return; }
            auto type = payload.is_object() ? payload.as_object().get_string("type"sv) : Optional<String> {};
            if (type == "devtools.dock"sv) {
                auto position = payload.as_object().get_string("position"sv);
                if (!position.has_value() || !position->is_one_of("left"sv, "right"sv, "bottom"sv, "window"sv)) { done(Error::from_string_literal("Invalid dock position")); return; }
                set_dock(qstring_from_ak_string(*position));
                done(JsonValue {}); return;
            }
            if (type == "devtools.dismiss"sv) { done(JsonValue {}); QTimer::singleShot(0, this, [this] { close(); }); return; }
            if (type == "devtools.pick"sv) {
                if (m_picking) stop_picker();
                else {
                    if (m_target->is_node_picker_active()) { done(Error::from_string_literal("Another element picker is active")); return; }
                    m_picking = true; publish_controls();
                    m_target->start_node_picker([self = QPointer(this)](auto event) {
                        if (!self || !self->m_target || !self->m_picking) return;
                        using Type = DevTools::DevToolsDelegate::NodePickerEvent::Type;
                        if (event.type == Type::Hovered && event.node_id.has_value()) self->m_target->highlight_dom_node(*event.node_id, {});
                        if (event.type != Type::Picked && event.type != Type::Canceled) return;
                        QTimer::singleShot(0, self, [self, event] {
                            if (!self || !self->m_target) return;
                            self->stop_picker();
                            if (event.type == Type::Picked && event.node_id.has_value()) {
                                JsonObject refresh; refresh.set("type"sv, "devtools.refresh"sv);
                                self->m_target->derpbird_command("devtools"_string, AK::move(refresh), [](auto) {});
                                self->inspect(event.node_id);
                            }
                        });
                    });
                    m_target->window()->raise(); m_target->window()->activateWindow(); m_target->setFocus();
                }
                done(JsonValue {}); return;
            }
            if (type == "devtools.bootstrap"sv) {
                m_target->derpbird_command("devtools"_string, payload, [self = QPointer(this), done = AK::move(done)](auto result) mutable {
                    if (!result.is_error() && self && result.value().is_object()) {
                        result.value().as_object().set("dock"sv, ak_string_from_qstring(self->m_dock));
                        result.value().as_object().set("picking"sv, self->m_picking);
                    }
                    done(AK::move(result));
                }); return;
            }
            m_target->derpbird_command("devtools"_string, payload, AK::move(done));
        };
        auto dock = settings.value("devtools/dock", "right").toString();
        set_dock(QStringList { "left", "right", "bottom", "window" }.contains(dock) ? dock : "right");
        m_ui->load(URL::Parser::basic_parse("about:derpbird"sv).release_value());
    }
    ~DerpBirdDevTools() override
    {
        save_geometry(); m_settings.sync();
        stop_picker();
        // Restore the page before disposing the splitter, which owns its widgets.
        if (m_splitter && m_tab && m_target) {
            m_target->setParent(m_tab);
            m_target->setMinimumSize(m_page_minimum);
            delete m_tab->layout()->replaceWidget(m_splitter, m_target);
            m_target->show();
            setParent(m_window);
            delete m_splitter;
        }
        if (!m_target) return;
        m_target->on_derpbird_devtools = nullptr;
        JsonObject payload; payload.set("type"sv, "devtools.close"sv);
        m_target->derpbird_command("devtools"_string, AK::move(payload), [](auto) {});
    }
    void inspect(Optional<Web::UniqueNodeID> node = {})
    {
        show(); window()->raise(); window()->activateWindow(); m_ui->setFocus();
        if (node.has_value() && m_target) {
            JsonObject payload; payload.set("type"sv, "devtools.select"sv); payload.set("id"sv, String::number(node->value()));
            m_target->derpbird_command("devtools"_string, AK::move(payload), [](auto) {});
        }
    }
protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        // The splitter already knows the final child geometry. Don't leave the
        // embedded renderer one layout turn behind its native container.
        if(m_ui) { m_ui->setGeometry(rect()); m_ui->push_viewport_size(); }
    }
private:
    void save_geometry()
    {
        if(m_dock=="window") m_settings.setValue("devtools/windowGeometry", saveGeometry());
    }
    void publish_controls()
    {
        JsonObject packet; packet.set("dock"sv, ak_string_from_qstring(m_dock)); packet.set("picking"sv, m_picking);
        m_ui->send_derpbird_shell_message("derpbird.devtools"_string, AK::move(packet));
    }
    void stop_picker()
    {
        if (!m_picking) return;
        m_picking = false;
        if (m_target) m_target->stop_node_picker();
        publish_controls();
    }
    void set_dock(QString const& position)
    {
        if (!m_target || !m_tab || position == m_dock) return;
        save_geometry();
        m_dock = position;
        if (!m_splitter) {
            m_splitter = new DerpBirdDevToolsSplitter(m_tab); m_splitter->setChildrenCollapsible(false); m_splitter->setHandleWidth(5);
            connect(m_splitter, &QSplitter::splitterMoved, this, [this] {
                if(m_dock=="window" || m_splitter->count()!=2) return;
                auto sizes=m_splitter->sizes(); int total=sizes[0]+sizes[1];
                if(total<=0) return;
                m_splitter->fraction=static_cast<double>(sizes[m_dock=="left"?0:1])/total;
                m_settings.setValue("devtools/fraction/"+m_dock, m_splitter->fraction);
                m_save_preferences.start();
                m_ui->push_viewport_size(); m_target->push_viewport_size();
            });
            m_splitter->setStyleSheet("QSplitter::handle { background: #383c40; }");
            delete m_tab->layout()->replaceWidget(m_target, m_splitter);
            m_splitter->addWidget(m_target);
        }
        if (position == "window") {
            setParent(m_window, Qt::Window); resize(1100, 720);
            restoreGeometry(m_settings.value("devtools/windowGeometry").toByteArray());
            m_target->setMinimumSize(m_page_minimum);
        } else {
            setParent(m_splitter, Qt::Widget);
            m_splitter->setOrientation(position == "bottom" ? Qt::Vertical : Qt::Horizontal);
            m_splitter->insertWidget(position == "left" ? 0 : 1, this);
            setMinimumSize(360, 180); m_target->setMinimumSize(120, 100);
            auto fraction=m_settings.value("devtools/fraction/"+position, 0.4).toDouble();
            m_splitter->fraction=std::isfinite(fraction)&&fraction>0&&fraction<1?fraction:0.4;
            m_splitter->leading=position=="left";
            m_splitter->apply_fraction();
        }
        m_splitter->show(); m_target->show(); show();
        m_settings.setValue("devtools/dock", position); m_settings.sync();
        publish_controls();
    }
    QPointer<WebContentView> m_target;
    QPointer<QWidget> m_tab;
    QPointer<QWidget> m_window;
    QPointer<DerpBirdDevToolsSplitter> m_splitter;
    QTimer m_save_preferences;
    QSettings& m_settings;
    QSize m_page_minimum;
    QString m_dock;
    bool m_picking { false };
    WebContentView* m_ui {};
};
}
