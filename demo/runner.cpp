#include "documentcontroller.h"
#include "markdownhighlighter.h"
#include "httpserver.h"
#include "filedocument.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>
#include <QProcess>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QtTest>
#include <functional>
#include <stdexcept>
#include <iostream>

class Tour : public QObject {
public:
    QQuickWindow *window = nullptr;
    QObject *overlay = nullptr;
    DocumentController *controller = nullptr;
    Mustermark::DocumentHttpServer *server = nullptr;
    QString directory, style;
    bool running = false, fast = false, autoplay = false;
    double pace = 2;
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() != QEvent::KeyPress) return false;
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_F10) std::exit(0);
        if (key->key() == Qt::Key_F8 && !running) {
            running = true;
            QTimer::singleShot(0, this, [this] { run(); });
            return true;
        }
        return false;
    }
    void wait(int ms) { QTest::qWait(qMax(20, int(ms * pace))); }
    void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
    void until(const std::function<bool()> &predicate, const char *message) {
        QElapsedTimer timer; timer.start();
        while (!predicate() && timer.elapsed() < 10000) QTest::qWait(50);
        require(predicate(), message);
    }
    void say(const QString &caption, const QString &detail = {}, int ms = 3500) {
        overlay->setProperty("caption", caption); overlay->setProperty("detail", detail);
        std::cout << caption.toStdString() << std::endl; wait(ms);
    }
    void key(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::keyClick(window, key, modifiers); wait(350);
    }
    void select(const QString &text) {
        const int pos = controller->source().indexOf(text);
        require(pos >= 0, "Demo selection is missing");
        QMetaObject::invokeMethod(window, "selectAtPosition", Q_ARG(QVariant, pos), Q_ARG(QVariant, true)); wait(300);
    }
    void type(const QString &text) {
        for (const QChar c : text) { QTest::keyClick(window, c.toLatin1()); wait(55); }
    }
    QVariant js(const QString &script) {
        overlay->setProperty("webView", QVariant::fromValue(window->findChild<QObject *>("previewView")));
        QMetaObject::invokeMethod(overlay, "evaluate", Q_ARG(QVariant, script));
        until([this] { return overlay->property("jsDone").toBool(); }, "Visual script timed out");
        return overlay->property("jsResult");
    }
    QJsonObject request(const QString &endpoint, QJsonObject body = {}) {
        QNetworkAccessManager network;
        QNetworkRequest req(QUrl(QString("http://127.0.0.1:%1/%2").arg(server->port()).arg(endpoint)));
        req.setRawHeader("X-Mustermark-Token", server->token().toUtf8());
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        auto *reply = body.isEmpty() ? network.get(req) : network.post(req, QJsonDocument(body).toJson());
        until([reply] { return reply->isFinished(); }, "API request timed out");
        auto result = QJsonDocument::fromJson(reply->readAll()).object();
        require(reply->error() == QNetworkReply::NoError && result.value("ok").toBool(), "API request failed");
        return result;
    }
    void focusWindow() {
        if (QGuiApplication::platformName() == "offscreen") return;
        QProcess probe; probe.start("hyprctl", {"-j", "clients"});
        if (!probe.waitForFinished(3000)) { probe.kill(); return; }
        for (const auto &value : QJsonDocument::fromJson(probe.readAllStandardOutput()).array()) {
            const auto client = value.toObject();
            if (client.value("pid").toInteger() != QCoreApplication::applicationPid()) continue;
            const QString address = client.value("address").toString();
            if (!address.startsWith("0x")) continue;
            QProcess focus; focus.start("hyprctl", {"dispatch", "hl.dsp.focus({ window = [[address:" + address + "]] })"});
            if (!focus.waitForFinished(3000)) focus.kill();
            if (client.value("workspace").toObject().value("id").toInt() != 2) {
                QProcess move; move.start("hyprctl", {"dispatch", "hl.dsp.window.move({ workspace = [[2]], follow = true })"});
                if (!move.waitForFinished(3000)) move.kill();
            }
        }
    }
    void run() {
        try {
            focusWindow();
            until([this]{return window->isActive();}, "Focus the demo window, then restart the runner");
            for (int count = fast ? 1 : 5; count > 0; --count) {
                say(QString("Starting in %1").arg(count), "Mustermark only · F10 stops the demo", 0);
                QTest::qWait(fast ? 20 : 1000);
            }
            say("One Markdown file. Three ways to work.", "Normal organizes · Insert writes · Visual renders", 4500);
            key(Qt::Key_Question);
            say("Help follows the selected structure.", "Press ? for the contextual keyboard guide.", 5000);
            key(Qt::Key_Question);
            select("Keep child notes together");
            say("Move a task with its children.", "Normal mode · Shift+J moves the entire subtree.");
            key(Qt::Key_J, Qt::ShiftModifier); wait(2500);
            say("Undo restores the complete structure.", "Ctrl+Z restores the task and its children.");
            key(Qt::Key_Z, Qt::ControlModifier);
            select("Map the route");
            say("Mark tasks complete without leaving Normal mode.", "Space toggles the selected task.");
            key(Qt::Key_Space); wait(2200);
            select("Plan the expedition");
            say("Change a heading's level structurally.", "Shift+H promotes the heading; Ctrl+Z puts it back.");
            key(Qt::Key_H, Qt::ShiftModifier); wait(1800); key(Qt::Key_Z, Qt::ControlModifier);
            select("Share the plan");
            say("Append to the list, with correct numbering.", "a appends · A prepends · Enter accepts the new item.");
            key(Qt::Key_A); type("Check the weather"); key(Qt::Key_Return);
            require(controller->source().contains("4. Check the weather"), "Ordered append did not renumber");
            say("Insert mode starts inside the selected item.", "i enters the text; Escape returns to Normal.");
            select("Check the weather"); key(Qt::Key_I); key(Qt::Key_End); type(" before departure"); key(Qt::Key_Escape);
            select("Capture the view");
            say("Attach a local image to a task.", "The image is copied beside the Markdown file.");
            auto node = window->property("selectedNode").toMap();
            require(controller->attachImage(node.value("identity").toString(), QUrl::fromLocalFile(directory + "/landscape.png")), "Image attachment failed");
            wait(2500);
            say("Switch to the rendered document in the same window.", "Shift+V enters Visual mode and keeps the selection.");
            key(Qt::Key_V, Qt::ShiftModifier);
            until([this] { auto *pane = window->findChild<QObject *>("previewWindow"); return pane && pane->property("selectionReady").toBool(); }, "Visual mode did not load");
            if (fast) { QTest::qWait(900); window->grabWindow().save("/tmp/mustermark-demo-visual.png"); }
            say("Images stay compact until you open them.", "Click an attachment icon to reveal its image.");
            js("document.querySelector('.mm-attachment-icon')?.click(); true"); wait(4500);
            js("document.querySelector('.mm-attachment-icon')?.click(); true");
            say("Edit Markdown directly in Visual mode.", "The rendered view and source share the same document.");
            js("(()=>{const node=currentState.nodes.find(n=>n.kind==='item'&&n.text.includes('Ready for'));const el=document.querySelector('[data-mm-ref=\"'+CSS.escape(node.ref)+'\"]');el.scrollIntoView({block:'center'});beginInlineEdit(el,node);return true})()");
            wait(2000);
            js("(()=>{const input=document.querySelector('.mm-inline-editor');input.value='- [ ] Ready for the next adventure, together';input.dispatchEvent(new Event('input',{bubbles:true}));input.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true}));return true})()");
            until([this]{return controller->source().contains("adventure, together");}, "Visual edit did not save");
            say("Adjust a few accent colors with CSS.", "Headings, links, and quote accents change color. The layout and theme stay familiar.", 4500);
            QFile::remove(style); require(QFile::copy(QString(DEMO_ROOT) + "/demo/visual.css", style), "CSS copy failed");
            js("location.reload(); true"); wait(1600);
            until([this] { return js("typeof currentState !== 'undefined' && !!currentState").toBool(); }, "Styled Visual reload failed");
            until([this] { return js("getComputedStyle(document.querySelector('h1')).color === 'rgb(131, 197, 190)'").toBool(); }, "Custom CSS did not apply");
            js("window.scrollTo({top:0,behavior:'smooth'}); true"); wait(6000);
            if (fast) { QTest::qWait(900); window->grabWindow().save("/tmp/mustermark-demo-css.png"); }
            js("document.querySelector('table')?.scrollIntoView({block:'center',behavior:'smooth'}); true");
            say("Tables, code, quotes, and tasks remain ordinary Markdown.", "Custom CSS changes how you read it, not who owns your file.", 5500);
            say("The API reads the same live document.", "A real HTTP request retrieves the current revision and task identities.", 4000);
            auto state = request("api/state"); QJsonObject target;
            for (const auto &entry : state.value("nodes").toArray())
                if (entry.toObject().value("text").toString() == "Updated through the API") target = entry.toObject();
            require(!target.isEmpty(), "API demo task missing");
            overlay->setProperty("code", "GET /api/state\n\n200 OK\nrevision: " + state.value("revision").toString().left(32) + "…\n\nTask: Updated through the API");
            js("(()=>{const n=currentState.nodes.find(n=>n.text==='Updated through the API');selectHostLine(n.startLine);return true})()"); wait(5000);
            QJsonObject instruction{{"action", "task_set"}, {"node", target.value("ref")}, {"checked", true}, {"baseRevision", state.value("revision")}};
            overlay->setProperty("code", "POST /api/actions\n\n{\n  \"action\": \"task_set\",\n  \"node\": \"" + target.value("ref").toString() + "\",\n  \"checked\": true,\n  \"baseRevision\": \"…\"\n}");
            say("An API update appears in the editor immediately.", "Revision checks protect concurrent edits.", 3000);
            request("api/actions", instruction);
            until([this] { return js("currentState.nodes.find(n=>n.text==='Updated through the API')?.checked === true").toBool(); }, "API change did not reach Visual mode");
            wait(5000);
            overlay->setProperty("code", "");
            say("Return to the source. The change is already there.", "Escape returns to Normal; the checked task is saved as Markdown.");
            key(Qt::Key_Escape);
            until([this]{return !window->property("visualMode").toBool();}, "Return to Normal failed");
            select("Updated through the API"); wait(3500);
            controller->save();
            say("Your file. Your structure. Your workflow.", "Mustermark · Normal / Insert / Visual · Stop recording now. F10 closes the demo.", 4500);
            if (fast) QCoreApplication::quit();
        } catch (const std::exception &error) {
            overlay->setProperty("caption", "Demo paused"); overlay->setProperty("detail", QString::fromUtf8(error.what()));
            std::cerr << error.what() << std::endl;
            if (fast) QCoreApplication::exit(1);
        }
    }
};

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("Mustermark Demo"); app.setDesktopFileName("mustermark-demo");
    QTemporaryDir folder(QDir::tempPath()+"/mustermark-demo-XXXXXX"); folder.setAutoRemove(false);
    const QString path=folder.filePath("tour.md"), style=folder.filePath("visual.css");
    QFile::copy(QString(DEMO_ROOT)+"/demo/tour.md", path);
    QFile css(style); if (!css.open(QIODevice::WriteOnly)) return 1; css.close();
    QImage landscape(960,420,QImage::Format_RGB32); QPainter paint(&landscape);
    QLinearGradient sky(0,0,0,420); sky.setColorAt(0,QColor("#10394c")); sky.setColorAt(1,QColor("#95d7c1"));paint.fillRect(landscape.rect(),sky);
    paint.setBrush(QColor("#e6bd64"));paint.setPen(Qt::NoPen);paint.drawEllipse(720,45,85,85);
    paint.setBrush(QColor("#244d56"));paint.drawPolygon(QPolygon{{0,420},{280,105},{530,420}});
    paint.setBrush(QColor("#367b77"));paint.drawPolygon(QPolygon{{280,420},{600,90},{960,420}});paint.end();landscape.save(folder.filePath("landscape.png"));
    DocumentController controller; controller.loadFile(QUrl::fromLocalFile(path));
    MarkdownHighlighter highlighter; Mustermark::DocumentHttpServer server(&controller,style);
    if(!server.listen()) return 1;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("documentController",&controller);
    engine.rootContext()->setContextProperty("markdownHighlighter",&highlighter);
    engine.rootContext()->setContextProperty("previewUrl",QUrl(QString("http://127.0.0.1:%1/#preview").arg(server.port())));
    engine.load(QUrl("qrc:/qml/Main.qml"));if(engine.rootObjects().isEmpty())return 1;
    auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    QQmlComponent captions(&engine,QUrl::fromLocalFile(QString(DEMO_ROOT)+"/demo/CaptionOverlay.qml"));
    auto *overlay=qobject_cast<QQuickItem *>(captions.create());if(!overlay){qWarning()<<captions.errors();return 1;}
    overlay->setParent(window);overlay->setParentItem(window->contentItem());
    Tour tour;tour.window=window;tour.overlay=overlay;tour.controller=&controller;tour.server=&server;tour.directory=folder.path();tour.style=style;
    tour.fast=app.arguments().contains("--fast");tour.pace=tour.fast ? .015 : qBound(.5, qEnvironmentVariable("MUSTERMARK_DEMO_PACE", "2").toDouble(), 4.0);
    app.installEventFilter(&tour);
    if(!app.arguments().contains("--windowed"))window->showFullScreen();
    window->requestActivate();
    QTimer::singleShot(300, &tour, [&tour]{tour.focusWindow();});
    std::cout<<"DEMO_READY "<<folder.path().toStdString()<<std::endl;
    if(app.arguments().contains("--autoplay"))QTimer::singleShot(500,&tour,[&tour]{tour.running=true;tour.run();});
    return app.exec();
}
