#include <QtTest>
#include <QApplication>
#include <QAbstractTextDocumentLayout>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QClipboard>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextTable>
#include <limits>
#include <QQuickWindow>
#include <QSettings>
#include <QUuid>
#include <memory>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#include "backend.h"
#include "controltransport.h"
#include "editorsession.h"
#include "markdownhighlighter.h"
#include "runtimeprofile.h"

namespace {
QByteArray jsonBytes(const QJsonObject &object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
QString errorCode(const QJsonObject &reply) {
    return reply.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString();
}
bool writeFile(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(contents) == contents.size();
}
QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QJsonObject request(const QString &command, const QJsonObject &args = {}) {
    return {{QStringLiteral("protocol"), 1}, {QStringLiteral("command"), command},
            {QStringLiteral("args"), args}};
}
struct UiFixture {
    Backend backend;
    EditorSession session{&backend};
    QQmlEngine engine;
    std::unique_ptr<QQuickWindow> window;
    QString error;

    UiFixture() {
        QObject::connect(&backend, &Backend::textScaleChanged, &backend, [this] {
            RuntimeProfile::applyTextScale(*qobject_cast<QApplication *>(qApp), backend.textScale());
        });
    }
    bool load(QStringList &warnings) {
        QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                         [&warnings](const QList<QQmlError> &errors) {
            for (const QQmlError &error : errors) warnings.append(error.toString());
        });
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        engine.rootContext()->setContextProperty(QStringLiteral("session"), &session);
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/Main.qml")));
        if (!component.isReady()) { error = component.errorString(); return false; }
        QObject *root = component.create();
        QQuickWindow *quickWindow = qobject_cast<QQuickWindow *>(root);
        if (!quickWindow) {
            error = component.errorString(); delete root; return false;
        }
        window.reset(quickWindow);
        backend.setParentWindow(quickWindow);
        session.attachWindow(quickWindow);
        quickWindow->requestActivate();
        return true;
    }
    QObject *find(const char *name) const {
        return window->findChild<QObject *>(QString::fromLatin1(name));
    }
    QJsonObject state() { return session.state(); }
    QJsonObject command(const QString &name, QJsonObject args = {}) {
        if (!args.contains(QStringLiteral("expectedVersion")))
            args.insert(QStringLiteral("expectedVersion"), state().value(QStringLiteral("version")));
        return session.execute(request(name, args));
    }
    QJsonObject replace(int start, int end, const QString &text) {
        return command(QStringLiteral("replace_range"), {{QStringLiteral("start"), start},
            {QStringLiteral("end"), end}, {QStringLiteral("text"), text}});
    }
};
struct AppProcess {
    QProcess process;
    QByteArray diagnostics;
    QString logPath;
    AppProcess() {
        logPath = QDir(RuntimeProfile::path()).filePath(QStringLiteral("subprocess-%1.log")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QObject::connect(&process, &QProcess::readyReadStandardError, &process, [this] {
            diagnostics += process.readAllStandardError();
        });
    }
    ~AppProcess() {
        if (process.state() != QProcess::NotRunning) {
            process.kill(); process.waitForFinished(3000);
        }
        diagnostics += process.readAllStandardError();
        writeFile(logPath, diagnostics);
        if (!diagnostics.trimmed().isEmpty()) {
            const QByteArray failure = QByteArray("Unexpected application diagnostics:\n") + diagnostics;
            QTest::qFail(failure.constData(), __FILE__, __LINE__);
        }
    }
};
struct ClientResult {
    int exitCode = -1;
    QJsonObject reply;
    QByteArray diagnostics;
};
ClientResult control(const QString &binary, const QString &profile,
                     const QStringList &arguments, const QByteArray &input = {}) {
    QProcess process;
    QStringList args{QStringLiteral("--profile"), profile, QStringLiteral("--control")};
    args.append(arguments);
    process.start(binary, args);
    if (!process.waitForStarted(3000)) return {-1, {}, process.errorString().toUtf8()};
    if (!input.isEmpty()) process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(8000)) {
        process.kill(); process.waitForFinished();
        return {-1, {}, QByteArray("Control client did not finish")};
    }
    return {process.exitCode(), QJsonDocument::fromJson(process.readAllStandardOutput()).object(),
            process.readAllStandardError()};
}
QJsonArray sessions(const QString &binary, const QString &profile) {
    return control(binary, profile, {QStringLiteral("list")}).reply
        .value(QStringLiteral("result")).toObject().value(QStringLiteral("sessions")).toArray();
}
ClientResult call(const QString &binary, const QString &profile, const QString &id,
                  const QString &command, const QJsonObject &args = {}) {
    return control(binary, profile, {QStringLiteral("--session"), id},
                   jsonBytes(request(command, args)) + '\n');
}
QString binaryPath() {
    const QString configured = qEnvironmentVariable("OMASCRIBE_BINARY");
    if (!configured.isEmpty()) return configured;
    return QFileInfo(QCoreApplication::applicationDirPath()
                     + QStringLiteral("/../build/omascribe")).absoluteFilePath();
}
}

class OmascribeTest : public QObject {
    Q_OBJECT
private slots:
    void init() {
        const QString artifacts = qEnvironmentVariable("OMASCRIBE_TEST_ARTIFACT_DIR");
        if (artifacts.isEmpty()) {
            m_profile = std::make_unique<QTemporaryDir>();
        } else {
            const QString profiles = QDir(artifacts).absoluteFilePath("../profile/tests");
            QVERIFY(QDir().mkpath(profiles));
            m_profile = std::make_unique<QTemporaryDir>(QDir(profiles).filePath(
                QString::fromLatin1(QTest::currentTestFunction()) + QStringLiteral("-XXXXXX")));
            m_profile->setAutoRemove(false);
        }
        QVERIFY(m_profile->isValid());
        QString error;
        const QByteArray home = qgetenv("HOME");
        QVERIFY2(RuntimeProfile::configure(m_profile->path(), &error), qPrintable(error));
        QCOMPARE(qgetenv("HOME"), home);
        m_qmlWarnings.clear();
        RuntimeProfile::applyTextScale(*qobject_cast<QApplication *>(qApp), 1.0);
    }
    void cleanup() {
        QVERIFY2(m_qmlWarnings.isEmpty(), qPrintable(m_qmlWarnings.join(QLatin1Char('\n'))));
        m_profile.reset();
    }
    void countsWords() {
        QCOMPARE(Backend::countWords(QStringLiteral("one two-three don't 42")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("你好 世界")), 2);
        QCOMPARE(Backend::countWords(QString()), 0);
    }
    void normalizesLinks() {
        QCOMPARE(Backend::normalizedLinkUrl(QStringLiteral("www.example.com/path")),
                 QStringLiteral("https://www.example.com/path"));
        QCOMPARE(Backend::normalizedLinkUrl(QStringLiteral("mailto:writer@example.com")),
                 QStringLiteral("mailto:writer@example.com"));
        QVERIFY(Backend::normalizedLinkUrl(QStringLiteral("example.com")).isEmpty());
        QVERIFY(Backend::normalizedLinkUrl(QStringLiteral("file:///tmp/private")).isEmpty());
    }
    void suggestsSafeNames() {
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("My first draft\nBody")),
                 QStringLiteral("My first draft.md"));
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("A/B")), QStringLiteral("A-B.md"));
        QCOMPARE(Backend::suggestedFileName(QString()), QStringLiteral("Untitled.md"));
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("Already.md")), QStringLiteral("Already.md"));
    }
    void keepsNotoFromCrowdingTheFontList() {
        const QStringList fonts = Backend::selectableFontFamilies({
            QStringLiteral("Noto Sans Devanagari"), QStringLiteral("Liberation Serif"),
            QStringLiteral("Noto Serif"), QStringLiteral("Noto Sans Tamil UI"),
            QStringLiteral("adwaita Sans"), QStringLiteral("IBM Plex Mono"),
            QStringLiteral("Noto Sans"), QStringLiteral("Noto Sans Mono"),
            QStringLiteral("Liberation Serif"), QStringLiteral("Monospace"),
            QStringLiteral("Standard Symbols PS"), QStringLiteral("D050000L"),
            QStringLiteral("Noto Color Emoji"), QStringLiteral("Nimbus Sans [UKWN]"),
            QStringLiteral("Nimbus Sans [URW ]")});
        QCOMPARE(fonts, QStringList({QStringLiteral("IBM Plex Mono"), QStringLiteral("adwaita Sans"),
            QStringLiteral("Liberation Serif"), QStringLiteral("Nimbus Sans"), QStringLiteral("Noto Sans"),
            QStringLiteral("Noto Sans Mono"), QStringLiteral("Noto Serif")}));
    }
    void remembersEditorFont() {
        const QString installed = Backend().availableFonts().value(1);
        QVERIFY(!installed.isEmpty());
        {
            Backend backend;
            QCOMPARE(backend.editorFont(), Backend::defaultEditorFont());
            QSignalSpy spy(&backend, &Backend::editorFontChanged);
            backend.setEditorFont(installed);
            QCOMPARE(spy.count(), 1);
        }
        QCOMPARE(Backend().editorFont(), installed);
        QSettings().setValue(QStringLiteral("editor/font"), QStringLiteral("No Such Font"));
        QCOMPARE(Backend().editorFont(), Backend::defaultEditorFont());
    }
    void remembersViewPreferencesInIsolatedProfiles() {
        {
            Backend backend;
            QCOMPARE(backend.viewMode(), QStringLiteral("source"));
            QCOMPARE(backend.splitRatio(), 0.5);
            backend.setViewMode("split");
            backend.setSplitRatio(0.65);
            backend.setViewMode("invalid");
            backend.setSplitRatio(std::numeric_limits<qreal>::quiet_NaN());
            QCOMPARE(backend.viewMode(), QStringLiteral("split"));
            QCOMPARE(backend.splitRatio(), 0.65);
        }
        QCOMPARE(Backend().viewMode(), QStringLiteral("split"));
        QCOMPARE(Backend().splitRatio(), 0.65);
        QTemporaryDir otherProfile;
        QString error;
        QVERIFY(RuntimeProfile::configure(otherProfile.path(), &error));
        QCOMPARE(Backend().viewMode(), QStringLiteral("source"));
        QCOMPARE(Backend().splitRatio(), 0.5);
        QVERIFY(RuntimeProfile::configure(m_profile->path(), &error));
        QSettings().setValue("editor/viewMode", "unknown");
        QSettings().setValue("editor/splitRatio", "not a number");
        QCOMPARE(Backend().viewMode(), QStringLiteral("source"));
        QCOMPARE(Backend().splitRatio(), 0.5);
    }
    void restoresViewsAndRecoveredPreview() {
        for (const QString &mode : {QStringLiteral("split"), QStringLiteral("preview")}) {
            QSettings().setValue("editor/viewMode", mode);
            QSettings().setValue("editor/splitRatio", 0.6);
            UiFixture fixture;
            QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
            QVERIFY(QTest::qWaitForWindowExposed(fixture.window.get()));
            QCOMPARE(fixture.backend.viewMode(), mode);
            QTRY_VERIFY(fixture.find(mode == "preview" ? "renderedPreview" : "sourceEditor")
                            ->property("activeFocus").toBool());
            if (mode == "split") {
                auto *source = fixture.find("sourcePane");
                auto *split = fixture.find("documentSplit");
                QTRY_VERIFY(qAbs(source->property("width").toReal()
                    / split->property("usableWidth").toReal() - 0.6) < 0.01);
            }
        }
        const QString text = "# Recovered preview\n\nA **draft**.\n";
        QVERIFY(writeFile(QDir(RuntimeProfile::dataDirectory()).filePath("recovery-0.json"),
                          QJsonDocument(QJsonObject{{"text", text}}).toJson()));
        UiFixture recovered;
        QVERIFY2(recovered.load(m_qmlWarnings), qPrintable(recovered.error));
        auto *quickDocument = recovered.find("renderedPreview")->property("textDocument")
                                  .value<QQuickTextDocument *>();
        QVERIFY(quickDocument);
        QTRY_VERIFY(quickDocument->textDocument()->toPlainText().startsWith("Recovered preview"));
        QVERIFY(recovered.backend.modified());
        QCOMPARE(recovered.state().value("text").toString(), text);
    }
    void switchesViewsWithoutChangingDocument() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window.get()));
        QVERIFY(fixture.replace(0, 0, "# Heading\n\n**bold** and *italic*\n").value("ok").toBool());
        QVERIFY(fixture.command("select", {{"start", 2}, {"end", 9}}).value("ok").toBool());
        const auto before = fixture.state();
        auto *editor = fixture.find("sourceEditor");
        auto *preview = fixture.find("renderedPreview");
        auto clickMode = [&](const char *name) {
            auto *button = qobject_cast<QQuickItem *>(fixture.find(name));
            if (!button) return false;
            QTest::mouseClick(fixture.window.get(), Qt::LeftButton, Qt::NoModifier,
                             button->mapToScene(QPointF(button->width()/2, button->height()/2)).toPoint());
            return true;
        };
        QVERIFY(clickMode("splitModeButton"));
        QTRY_COMPARE(fixture.backend.viewMode(), QStringLiteral("split"));
        QVERIFY(clickMode("splitModeButton"));
        QVERIFY(fixture.find("splitModeButton")->property("checked").toBool());
        QTRY_VERIFY(editor->property("activeFocus").toBool());
        QVERIFY(fixture.find("sourcePane")->property("visible").toBool());
        QVERIFY(fixture.find("previewPane")->property("visible").toBool());
        QCOMPARE(fixture.state().value("version"), before.value("version"));
        QCOMPARE(fixture.state().value("selectionStart"), before.value("selectionStart"));
        QCOMPARE(fixture.state().value("selectionEnd"), before.value("selectionEnd"));
        QVERIFY(clickMode("previewModeButton"));
        QTRY_COMPARE(fixture.backend.viewMode(), QStringLiteral("preview"));
        QTRY_VERIFY(preview->property("activeFocus").toBool());
        QVERIFY(!editor->property("visible").toBool());
        QVERIFY(preview->property("readOnly").toBool());
        QTest::keyClick(fixture.window.get(), Qt::Key_B, Qt::ControlModifier);
        QTest::keyClick(fixture.window.get(), Qt::Key_Z, Qt::ControlModifier);
        QTest::keyClick(fixture.window.get(), Qt::Key_X);
        QCOMPARE(fixture.state().value("text"), before.value("text"));
        QCOMPARE(fixture.state().value("version"), before.value("version"));
        QVERIFY(clickMode("sourceModeButton"));
        QTRY_COMPARE(fixture.backend.viewMode(), QStringLiteral("source"));
        QTRY_VERIFY(editor->property("activeFocus").toBool());
        QCOMPARE(fixture.state().value("selectionStart"), before.value("selectionStart"));
        QCOMPARE(fixture.state().value("selectionEnd"), before.value("selectionEnd"));
        QCOMPARE(fixture.state().value("canUndo"), before.value("canUndo"));
        QVERIFY(fixture.command("undo").value("ok").toBool());
        QCOMPARE(fixture.state().value("text").toString(), QString());
        QVERIFY(fixture.command("redo").value("ok").toBool());
        fixture.backend.setViewMode("preview");
        QTRY_VERIFY(preview->property("activeFocus").toBool());
        QTest::keyClick(fixture.window.get(), Qt::Key_F, Qt::ControlModifier);
        QTRY_COMPARE(fixture.backend.viewMode(), QStringLiteral("split"));
        QTRY_VERIFY(fixture.find("searchField")->property("activeFocus").toBool());
        QVERIFY(QMetaObject::invokeMethod(fixture.window.get(), "closeSearch"));
        fixture.backend.setViewMode("preview");
        QTRY_VERIFY(preview->property("activeFocus").toBool());
        QTest::keyClick(fixture.window.get(), Qt::Key_H, Qt::ControlModifier);
        QTRY_COMPARE(fixture.backend.viewMode(), QStringLiteral("split"));
        QVERIFY(fixture.window->property("replaceOpen").toBool());
    }
    void rendersLiveMarkdownAndRelativeImages() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window.get()));
        const QString directory = m_profile->filePath("images");
        QVERIFY(QDir().mkpath(directory));
        QImage image(1600, 24, QImage::Format_ARGB32); image.fill(Qt::red);
        QVERIFY(image.save(directory + "/example.png"));
        const QString sample = "# Heading\n\n**bold** and *italic*\n\n- item\n- [x] done\n\n"
                               "```cpp\nint value = 1;\n```\n\n| A | B |\n| --- | --- |\n| 1 | 2 |\n\n![Image](example.png)\n";
        QVERIFY(writeFile(directory + "/document.md", sample.toUtf8()));
        QVERIFY(fixture.command("open", {{"path", directory + "/document.md"}}).value("ok").toBool());
        fixture.backend.setViewMode("split");
        auto *preview = fixture.find("renderedPreview"); QVERIFY(preview);
        auto *quickDocument = preview->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(quickDocument);
        auto *document = quickDocument->textDocument();
        QTRY_VERIFY(document->toPlainText().contains("Heading"));
        QVERIFY(!document->toPlainText().contains("**bold**"));
        QCOMPARE(document->begin().blockFormat().headingLevel(), 1);
        QTextCursor bold = document->find("bold");
        QVERIFY(bold.charFormat().fontWeight() >= QFont::Bold);
        QVERIFY(document->find("italic").charFormat().fontItalic());
        QVERIFY(document->find("item").block().textList());
        QVERIFY(document->toPlainText().contains("int value = 1;"));
        bool hasTable = false;
        for (auto *frame : document->rootFrame()->childFrames())
            hasTable |= qobject_cast<QTextTable *>(frame) != nullptr;
        QVERIFY(hasTable);
        QCOMPARE(preview->property("baseUrl").toUrl(), QUrl::fromLocalFile(directory + "/"));
        const auto resource = document->resource(QTextDocument::ImageResource, QUrl("example.png"));
        QVERIFY(!resource.value<QImage>().isNull() || !resource.value<QPixmap>().isNull());
        auto *previewViewport = fixture.find("previewViewport");
        QTRY_VERIFY(previewViewport->property("contentWidth").toReal()
                    > previewViewport->property("width").toReal());
        QVERIFY(QMetaObject::invokeMethod(preview, "select", Q_ARG(int, 0), Q_ARG(int, 7)));
        QVERIFY(QMetaObject::invokeMethod(preview, "copy"));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("Heading"));
        const auto before = fixture.state();
        QVERIFY(fixture.replace(2, 9, "Updated").value("ok").toBool());
        QTRY_VERIFY(document->toPlainText().startsWith("Updated"));
        QVERIFY(fixture.command("undo").value("ok").toBool());
        QTRY_VERIFY(document->toPlainText().startsWith("Heading"));
        QVERIFY(fixture.command("redo").value("ok").toBool());
        QTRY_VERIFY(document->toPlainText().startsWith("Updated"));
        const QString moved = m_profile->filePath("moved");
        QVERIFY(QDir().mkpath(moved));
        image.fill(Qt::green);
        QVERIFY(image.save(moved + "/example.png"));
        QVERIFY(fixture.command("save_as", {{"path", moved + "/document.md"}}).value("ok").toBool());
        QTRY_COMPARE(preview->property("baseUrl").toUrl(), QUrl::fromLocalFile(moved + "/"));
        const auto movedResource = document->resource(QTextDocument::ImageResource, QUrl("example.png"));
        const QImage movedImage = movedResource.canConvert<QImage>()
            ? movedResource.value<QImage>() : movedResource.value<QPixmap>().toImage();
        QVERIFY(!movedImage.isNull());
        QCOMPARE(movedImage.pixelColor(0, 0), QColor(Qt::green));
        QVERIFY(writeFile(moved + "/document.md", "# Reloaded\n"));
        QVERIFY(fixture.command("reload").value("ok").toBool());
        QTRY_COMPARE(document->toPlainText().trimmed(), QStringLiteral("Reloaded"));
        QVERIFY(!before.value("modified").toBool());
    }
    void spacesPreviewBlocksWithoutExpandingLines() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window.get()));
        const QString sample = "# Title\n\nFirst paragraph line one  \nline two.\n\n"
            "Second paragraph.\n\n## Section\n\n- First item\n- Second item\n\n"
            "```cpp\nfirst_code_line();\nsecond_code_line();\n```\n\nAfter code.\n\n"
            "> First quote paragraph.\n>\n> Second quote paragraph.\n\n"
            "| Header | Other |\n| --- | --- |\n| Cell | Value |\n\nAfter table.\n";
        QVERIFY(fixture.replace(0, 0, sample).value("ok").toBool());
        const auto sourceState = fixture.state();
        fixture.backend.setViewMode("preview");
        auto *preview = fixture.find("renderedPreview");
        auto *quickDocument = preview->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(quickDocument);
        QTextDocument *document = quickDocument->textDocument();
        auto findBlock = [&](const QString &text) { return document->find(text).block(); };
        QTRY_VERIFY(findBlock("Second paragraph.").isValid());
        QTextBlock first = findBlock("First paragraph");
        QTextBlock second = findBlock("Second paragraph");
        QTextBlock heading = findBlock("Section");
        QTextBlock item1 = findBlock("First item");
        QTextBlock item2 = findBlock("Second item");
        QTextBlock code1 = findBlock("first_code_line");
        QTextBlock code2 = findBlock("second_code_line");
        const qreal gap = second.blockFormat().bottomMargin();
        QTRY_VERIFY(second.blockFormat().bottomMargin() > 0);
        const QTextBlock hardBreakLine = findBlock("line two.");
        if (hardBreakLine != first) {
            QCOMPARE(first.blockFormat().bottomMargin(), 0.0);
            QCOMPARE(hardBreakLine.blockFormat().topMargin(), 0.0);
        }
        QVERIFY(document->begin().blockFormat().topMargin() == 0);
        QVERIFY(heading.blockFormat().topMargin() > gap);
        QVERIFY(heading.blockFormat().bottomMargin() > 0);
        QCOMPARE(first.blockFormat().lineHeightType(), int(QTextBlockFormat::SingleHeight));
        QVERIFY(item2.blockFormat().topMargin() < gap);
        QCOMPARE(item1.blockFormat().bottomMargin(), 0.0);
        QVERIFY(item2.blockFormat().bottomMargin() > 0);
        QCOMPARE(code1.blockFormat().bottomMargin(), 0.0);
        QCOMPARE(code2.blockFormat().topMargin(), 0.0);
        QVERIFY(code2.blockFormat().bottomMargin() > 0);
        auto *layout = document->documentLayout();
        QTRY_VERIFY(layout->blockBoundingRect(second).width() > 100);
        const auto firstRect = layout->blockBoundingRect(hardBreakLine);
        const auto secondRect = layout->blockBoundingRect(second);
        QVERIFY(secondRect.top() - firstRect.bottom() >= gap - 1);
        const auto code1Rect = layout->blockBoundingRect(code1);
        const auto code2Rect = layout->blockBoundingRect(code2);
        QVERIFY(code2Rect.top() - code1Rect.bottom() < gap / 2);
        const QTextBlock cell = findBlock("Cell");
        QCOMPARE(cell.blockFormat().topMargin(), 0.0);
        QCOMPARE(cell.blockFormat().bottomMargin(), 0.0);
        QVERIFY(QTextCursor(cell).currentTable()->format().bottomMargin() > 0);
        const auto headerRect = layout->blockBoundingRect(findBlock("Header"));
        const auto quoteRect = layout->blockBoundingRect(findBlock("Second quote"));
        QVERIFY(headerRect.top() - quoteRect.bottom() >= gap - 1);
        const auto afterTableRect = layout->blockBoundingRect(findBlock("After table"));
        const auto cellRect = layout->blockBoundingRect(cell);
        QVERIFY(afterTableRect.top() - cellRect.bottom() >= gap - 1);
        QVERIFY(afterTableRect.top() - cellRect.bottom() < gap * 2);
        QVERIFY(findBlock("First quote").blockFormat().bottomMargin() > 0);
        QCOMPARE(fixture.state().value("text"), sourceState.value("text"));
        QCOMPARE(fixture.state().value("version"), sourceState.value("version"));
        QCOMPARE(fixture.state().value("canUndo"), sourceState.value("canUndo"));
        fixture.backend.setTextScale(4.0 / 3.0);
        QTRY_VERIFY(findBlock("Second paragraph").blockFormat().bottomMargin() > gap);
        QCOMPARE(fixture.state().value("version"), sourceState.value("version"));
        QVERIFY(fixture.replace(2, 7, "Updated title").value("ok").toBool());
        QTRY_VERIFY(document->toPlainText().startsWith("Updated title"));
        QTRY_VERIFY(findBlock("Second paragraph").blockFormat().bottomMargin() > 0);
        fixture.backend.setTextScale(1);
        capture(fixture, "spacing-preview");
    }
    void synchronizesScrollingAndResizesSplit() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window.get()));
        QString sample;
        for (int i = 0; i < 100; ++i)
            sample += QString("## Section %1\n\nA paragraph with **bold** text and several words.\n\n").arg(i);
        QVERIFY(fixture.replace(0, 0, sample).value("ok").toBool());
        fixture.backend.setViewMode("split");
        auto *sourcePane = qobject_cast<QQuickItem *>(fixture.find("sourcePane"));
        auto *previewPane = qobject_cast<QQuickItem *>(fixture.find("previewPane"));
        QVERIFY(sourcePane); QVERIFY(previewPane);
        QTRY_VERIFY(qAbs(sourcePane->width() - previewPane->width()) < 2);
        auto *source = fixture.find("sourceViewport");
        auto *preview = fixture.find("previewViewport");
        auto extent = [](QObject *view) {
            return view->property("contentHeight").toReal() - view->property("height").toReal();
        };
        QTRY_VERIFY(extent(source) > 0 && extent(preview) > 0);
        QTRY_VERIFY(!fixture.window->property("synchronizingScroll").toBool());
        QCoreApplication::processEvents();
        source->setProperty("contentY", extent(source) * 0.35);
        QTRY_VERIFY(qAbs(preview->property("contentY").toReal()/extent(preview) - 0.35) < 0.01);
        preview->setProperty("contentY", extent(preview) * 0.7);
        QTRY_VERIFY(qAbs(source->property("contentY").toReal()/extent(source) - 0.7) < 0.01);
        const QPoint handle = sourcePane->mapToScene(QPointF(sourcePane->width() + 3, 80)).toPoint();
        QTest::mousePress(fixture.window.get(), Qt::LeftButton, Qt::NoModifier, handle);
        QTest::mouseMove(fixture.window.get(), handle + QPoint(100, 0));
        QTest::mouseRelease(fixture.window.get(), Qt::LeftButton, Qt::NoModifier, handle + QPoint(100, 0));
        QTRY_VERIFY(fixture.backend.splitRatio() > 0.55);
        QCOMPARE(fixture.state().value("splitRatio").toDouble(), fixture.backend.splitRatio());
        fixture.window->resize(720, 520);
        fixture.backend.setTextScale(4.0/3.0);
        QTRY_VERIFY(sourcePane->width() > 0 && previewPane->width() > 0);
        auto *editor = qobject_cast<QQuickItem *>(fixture.find("sourceEditor"));
        auto *rendered = qobject_cast<QQuickItem *>(fixture.find("renderedPreview"));
        QTRY_VERIFY(editor->width() <= sourcePane->width());
        QTRY_VERIFY(rendered->width() <= previewPane->width());
        QTRY_VERIFY(qAbs(source->property("contentY").toReal()/extent(source)
                           - preview->property("contentY").toReal()/extent(preview)) < 0.01);
        QTRY_VERIFY(qAbs(preview->property("contentY").toReal()/extent(preview) - 0.7) < 0.01);
        QVERIFY(QMetaObject::invokeMethod(editor, "forceActiveFocus"));
        QTest::keyClick(fixture.window.get(), Qt::Key_X);
        QTRY_COMPARE(fixture.state().value("text").toString().size(), sample.size() + 1);
        auto caretVisible = [&] {
            const QRectF caret = editor->property("cursorRectangle").toRectF();
            const qreal bottom = editor->y() + caret.bottom();
            const qreal top = source->property("contentY").toReal();
            return bottom <= top + source->property("height").toReal()
                && editor->y() + caret.top() >= top;
        };
        QTRY_VERIFY(caretVisible());
        QVERIFY(fixture.replace(0, sample.size() + 1, "short").value("ok").toBool());
        QTRY_COMPARE(source->property("contentY").toReal(), 0.0);
        QTRY_COMPARE(preview->property("contentY").toReal(), 0.0);
    }
    void findsInlineMarkdownRanges() {
        const auto markup = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("**bold** and *italic* and [site](https://example.com)"));
        QCOMPARE(markup.size(), 3);
        QCOMPARE(markup.at(0).content.start, 2);
        QCOMPARE(markup.at(0).content.length, 4);
        QCOMPARE(markup.at(2).content.length, 4);
        QCOMPARE(markup.at(2).markers[0].length, 1);
    }
    void loadsIsolatedThemeWithoutChangingHome() {
        const QByteArray home = qgetenv("HOME");
        QVERIFY(writeFile(RuntimeProfile::themeDirectory() + QStringLiteral("/colors.toml"),
            "mode = \"light\"\naccent = \"#112233\"\nselection = \"#445566\"\n"
            "background = \"#fefefe\"\nforeground = \"#101010\"\n"));
        Backend backend;
        QCOMPARE(backend.themeBackground(), QStringLiteral("#fefefe"));
        QCOMPARE(backend.themeForeground(), QStringLiteral("#101010"));
        QCOMPARE(backend.themeAccent(), QStringLiteral("#112233"));
        QCOMPARE(backend.themeSelection(), QStringLiteral("#445566"));
        QVERIFY(!backend.darkMode());
        QCOMPARE(qgetenv("HOME"), home);
    }
    void ignoresFileWatcherEventsForSavedContents() {
        const QString path = m_profile->filePath(QStringLiteral("first-save.md"));
        Backend backend;
        QSignalSpy spy(&backend, &Backend::externalChangeDetected);
        backend.saveAs(QUrl::fromLocalFile(path));
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(writeFile(path, {}));
        // A second save compares the disk directly, as well as processing watcher events.
        backend.save();
        QCOMPARE(spy.count(), 0);
        QVERIFY(writeFile(path, "changed elsewhere"));
        QTRY_COMPARE(spy.count(), 1);
    }
    void keepsCursorAndSelectionStableAcrossInsertions() {
        QQmlEngine engine;
        connect(&engine, &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) {
            for (const QQmlError &error : errors) m_qmlWarnings.append(error.toString());
        });
        QQmlComponent component(&engine);
        const QByteArray harness = R"QML(
            import QtQuick
            import "EditorMutations.js" as EditorMutations
            TextEdit {
                property string insertionText
                property int insertionCursor
                property string wrappedText
                property int wrappedSelectionStart
                property int wrappedSelectionEnd
                Component.onCompleted: {
                    text = "alpha omega";
                    cursorPosition = 5;
                    EditorMutations.replaceRange(this, 5, 5, "one\r\ntwo");
                    insertionText = text; insertionCursor = cursorPosition;
                    text = "alpha beta omega";
                    select(6, 10);
                    EditorMutations.replaceRange(this, selectionStart, selectionEnd, "**beta**", 2, 6);
                    wrappedText = text;
                    wrappedSelectionStart = selectionStart; wrappedSelectionEnd = selectionEnd;
                }
            }
        )QML";
        component.setData(harness, QUrl(QStringLiteral("qrc:/MutationHarness.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> editor(component.create());
        QVERIFY2(editor, qPrintable(component.errorString()));
        QCOMPARE(editor->property("insertionText").toString(), QStringLiteral("alphaone\ntwo omega"));
        QCOMPARE(editor->property("insertionCursor").toInt(), 12);
        QCOMPARE(editor->property("wrappedText").toString(), QStringLiteral("alpha **beta** omega"));
        QCOMPARE(editor->property("wrappedSelectionStart").toInt(), 8);
        QCOMPARE(editor->property("wrappedSelectionEnd").toInt(), 12);
    }
    void savesAndOpensFromFooterButtons() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        QVERIFY(fixture.find("sourceEditor"));
        QVERIFY(fixture.find("renderedPreview"));
        QVERIFY(fixture.find("modeToggle"));
        QObject *save = fixture.find("saveButton");
        QObject *open = fixture.find("openButton");
        QVERIFY(save); QVERIFY(open);
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window.get()));
        QSignalSpy saveSpy(&fixture.backend, &Backend::saveDialogRequested);
        QVERIFY(QMetaObject::invokeMethod(save, "clicked"));
        QCOMPARE(saveSpy.count(), 1);
        // Finish the first dialog before asking the application to open another.
        QObject *saveDialog = fixture.find("saveFileDialog"); QVERIFY(saveDialog);
        QTRY_VERIFY(saveDialog->property("visible").toBool());
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(saveDialog, "close"));
        QTRY_VERIFY(!saveDialog->property("visible").toBool());
        fixture.backend.fileDialogCanceled();
        QSignalSpy openSpy(&fixture.backend, &Backend::openDialogRequested);
        QVERIFY(QMetaObject::invokeMethod(open, "clicked"));
        QCOMPARE(openSpy.count(), 1);
        QObject *openDialog = fixture.find("openFileDialog"); QVERIFY(openDialog);
        QTRY_VERIFY(openDialog->property("visible").toBool());
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(openDialog, "close"));
        QTRY_VERIFY(!openDialog->property("visible").toBool());
    }
    void scalesTextWithDesktopTextSize() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        QObject *editor = fixture.find("sourceEditor"); QVERIFY(editor);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 20);
        fixture.backend.setTextScale(16.0 / 12.0);
        QCOMPARE(fixture.window->property("editorFontPixelSize").toInt(), 27);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 27);
        fixture.backend.setTextScale(9.0 / 12.0);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 15);
    }
    void remembersLastSaveDirectory() {
        const QString savedPath = m_profile->filePath(QStringLiteral("first.md"));
        Backend savedDocument; savedDocument.saveAs(QUrl::fromLocalFile(savedPath));
        Backend nextDocument;
        QSignalSpy spy(&nextDocument, &Backend::saveDialogRequested);
        nextDocument.saveAsDialog(); QCOMPARE(spy.count(), 1);
        const QUrl suggested = spy.takeFirst().constFirst().toUrl();
        QCOMPARE(QFileInfo(suggested.toLocalFile()).absolutePath(), m_profile->path());
        QCOMPARE(QFileInfo(suggested.toLocalFile()).fileName(), QStringLiteral("Untitled.md"));
        QSettings().setValue(QStringLiteral("file/lastSaveDirectory"), m_profile->filePath("missing"));
        Backend fallback;
        QSignalSpy fallbackSpy(&fallback, &Backend::saveDialogRequested);
        fallback.saveAsDialog();
        QCOMPARE(QFileInfo(fallbackSpy.takeFirst().constFirst().toUrl().toLocalFile()).absolutePath(),
                 m_profile->filePath(QStringLiteral("documents")));
    }

    void controlsLiveDocumentAndUndoHistory() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        const QString initialVersion = fixture.state().value("version").toString();
        const auto first = fixture.replace(0, 0, QStringLiteral("alpha beta"));
        QVERIFY2(first.value("ok").toBool(), jsonBytes(first).constData());
        QCOMPARE(fixture.find("sourceEditor")->property("text").toString(), QStringLiteral("alpha beta"));
        QVERIFY(fixture.state().value("modified").toBool());
        QVERIFY(fixture.state().value("canUndo").toBool());
        QVERIFY(fixture.state().value("version").toString() != initialVersion);
        auto selected = fixture.command("select", {{"start", 6}, {"end", 10}});
        QVERIFY2(selected.value("ok").toBool(), jsonBytes(selected).constData());
        QCOMPARE(fixture.state().value("selectionStart").toInt(), 6);
        QCOMPARE(fixture.state().value("selectionEnd").toInt(), 10);
        const auto edited = fixture.replace(6, 10, QStringLiteral("one\r\ntwo\rthree"));
        QVERIFY2(edited.value("ok").toBool(), jsonBytes(edited).constData());
        QCOMPARE(fixture.state().value("text").toString(), QStringLiteral("alpha one\ntwo\nthree"));
        QCOMPARE(fixture.state().value("cursor").toInt(), 19);
        QVERIFY(fixture.command("undo").value("ok").toBool());
        QCOMPARE(fixture.state().value("text").toString(), QStringLiteral("alpha beta"));
        QVERIFY(fixture.command("undo").value("ok").toBool());
        QCOMPARE(fixture.state().value("text").toString(), QString());
        QVERIFY(fixture.command("redo").value("ok").toBool());
        QCOMPARE(fixture.state().value("text").toString(), QStringLiteral("alpha beta"));
    }
    void preservesUserUndoAndRejectsAbaVersion() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window.get()));
        QVERIFY(QMetaObject::invokeMethod(fixture.find("sourceEditor"), "forceActiveFocus"));
        QTest::keyClick(fixture.window.get(), Qt::Key_A);
        QTRY_COMPARE(fixture.state().value("text").toString(), QStringLiteral("a"));
        const QString userVersion = fixture.state().value("version").toString();
        QVERIFY(fixture.replace(1, 1, QStringLiteral(" from agent")).value("ok").toBool());
        QVERIFY(fixture.command("undo").value("ok").toBool());
        QCOMPARE(fixture.state().value("text").toString(), QStringLiteral("a"));
        // The text equals the earlier read, but its version must remain obsolete.
        QCOMPARE(errorCode(fixture.command("replace_range", {{"expectedVersion", userVersion},
            {"start", 0}, {"end", 1}, {"text", "lost"}})), QStringLiteral("version_conflict"));
        QVERIFY(fixture.command("undo").value("ok").toBool());
        QCOMPARE(fixture.state().value("text").toString(), QString());
        QVERIFY(fixture.command("redo").value("ok").toBool());
        QCOMPARE(fixture.state().value("text").toString(), QStringLiteral("a"));
    }
    void rejectsStaleVersionsAndInvalidUtf16Ranges() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        const QString version = fixture.state().value("version").toString();
        QVERIFY(fixture.replace(0, 0, QString::fromUtf8("a😀b")).value("ok").toBool());
        auto stale = fixture.command("replace_range", {{"expectedVersion", version},
            {"start", 0}, {"end", 1}, {"text", "x"}});
        QCOMPARE(errorCode(stale), QStringLiteral("version_conflict"));
        const QString text = fixture.state().value("text").toString();
        const QString currentVersion = fixture.state().value("version").toString();
        for (const auto range : {qMakePair(-1, 0), qMakePair(2, 2), qMakePair(3, 2), qMakePair(0, 5)}) {
            const auto invalid = fixture.replace(range.first, range.second, QStringLiteral("wrong"));
            QCOMPARE(errorCode(invalid), QStringLiteral("invalid_range"));
            QCOMPARE(fixture.state().value("text").toString(), text);
            QCOMPARE(fixture.state().value("version").toString(), currentVersion);
        }
        QVERIFY(fixture.replace(1, 3, QStringLiteral("emoji")).value("ok").toBool());
        QCOMPARE(fixture.state().value("text").toString(), QStringLiteral("aemojib"));
        // Real typing also invalidates an agent's previously read version.
        const QString beforeTyping = fixture.state().value("version").toString();
        QVERIFY(QMetaObject::invokeMethod(fixture.find("sourceEditor"), "forceActiveFocus"));
        QTest::keyClick(fixture.window.get(), Qt::Key_X);
        QTRY_VERIFY(fixture.state().value("version").toString() != beforeTyping);
        QCOMPARE(errorCode(fixture.command("undo", {{"expectedVersion", beforeTyping}})),
                 QStringLiteral("version_conflict"));
    }
    void changesIdentityEvenForIdenticalFiles() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        const QString first = m_profile->filePath("first.md");
        const QString second = m_profile->filePath("second.md");
        QVERIFY(writeFile(first, "same")); QVERIFY(writeFile(second, "same"));
        QVERIFY(fixture.command("open", {{"path", first}}).value("ok").toBool());
        const QString old = fixture.state().value("version").toString();
        QVERIFY(fixture.command("open", {{"path", second}}).value("ok").toBool());
        QVERIFY(fixture.state().value("version").toString() != old);
        QCOMPARE(errorCode(fixture.command("replace_range", {{"expectedVersion", old},
            {"start", 0}, {"end", 1}, {"text", "x"}})), QStringLiteral("version_conflict"));
        QCOMPARE(fixture.state().value("file").toString(), second);
    }
    void protectsDirtyTransitionsAndPreflightsDestination() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        QVERIFY(fixture.replace(0, 0, QStringLiteral("precious draft")).value("ok").toBool());
        const QString path = m_profile->filePath("target.md");
        QVERIFY(writeFile(path, "target"));
        QCOMPARE(errorCode(fixture.command("open", {{"path", path}})), QStringLiteral("unsaved_changes"));
        QCOMPARE(errorCode(fixture.command("close")), QStringLiteral("unsaved_changes"));
        QCOMPARE(fixture.state().value("text").toString(), QStringLiteral("precious draft"));
        QCOMPARE(errorCode(fixture.command("open", {{"path", m_profile->filePath("absent.md")},
            {"onUnsaved", "discard"}})), QStringLiteral("io_error"));
        QCOMPARE(fixture.state().value("text").toString(), QStringLiteral("precious draft"));
        QCOMPARE(errorCode(fixture.command("open", {{"path", path}, {"onUnsaved", "save"}})),
                 QStringLiteral("save_path_required"));
        const QString savePath = m_profile->filePath("draft.md");
        QVERIFY(fixture.command("open", {{"path", path}, {"onUnsaved", "save"},
            {"savePath", savePath}}).value("ok").toBool());
        QCOMPARE(readFile(savePath), QByteArray("precious draft"));
        QCOMPARE(fixture.state().value("text").toString(), QStringLiteral("target"));
        QVERIFY(!fixture.state().value("modified").toBool());
    }
    void failedSaveDoesNotReactivateTransition() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        QVERIFY(fixture.replace(0, 0, QStringLiteral("draft")).value("ok").toBool());
        const QString target = m_profile->filePath("target.md");
        QVERIFY(writeFile(target, "target"));
        QCOMPARE(errorCode(fixture.command("open", {{"path", target}, {"onUnsaved", "save"},
            {"savePath", m_profile->filePath("missing/draft.md")}})), QStringLiteral("io_error"));
        QCOMPARE(fixture.state().value("text").toString(), QStringLiteral("draft"));
        QVERIFY(fixture.state().value("pendingAction").toString().isEmpty());
        const QString saved = m_profile->filePath("saved.md");
        QVERIFY(fixture.command("save_as", {{"path", saved}}).value("ok").toBool());
        QCOMPARE(fixture.state().value("text").toString(), QStringLiteral("draft"));
        QCOMPARE(fixture.state().value("file").toString(), saved);
    }
    void checksDiskBeforeWatcherAndProtectsAliases() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        const QString path = m_profile->filePath("original.md");
        const QString alias = m_profile->filePath("alias.md");
        QVERIFY(writeFile(path, "original"));
        QVERIFY(QFile::link(path, alias));
        QVERIFY(fixture.command("open", {{"path", path}}).value("ok").toBool());
        QVERIFY(fixture.replace(0, 8, QStringLiteral("mine")).value("ok").toBool());
        QVERIFY(writeFile(path, "theirs"));
        // No event-loop turn has delivered QFileSystemWatcher::fileChanged yet.
        const auto save = fixture.command("save_as", {{"path", alias}, {"overwrite", true}});
        QCOMPARE(errorCode(save), QStringLiteral("disk_conflict"));
        QCOMPARE(readFile(path), QByteArray("theirs"));
        QVERIFY(fixture.state().value("externalChanged").toBool());
        QVERIFY(fixture.command("resolve_external", {{"choice", "keep"}}).value("ok").toBool());
        QVERIFY(fixture.command("save").value("ok").toBool());
        QCOMPARE(readFile(path), QByteArray("mine"));
        QVERIFY(!fixture.state().value("externalChanged").toBool());
        const QString other = m_profile->filePath("existing.md");
        QVERIFY(writeFile(other, "keep"));
        QCOMPARE(errorCode(fixture.command("save_as", {{"path", other}})), QStringLiteral("target_exists"));
        QCOMPARE(readFile(other), QByteArray("keep"));
        QVERIFY(fixture.command("save_as", {{"path", other}, {"overwrite", true}}).value("ok").toBool());
        QCOMPARE(readFile(other), QByteArray("mine"));
    }
    void protectsDeletedDocumentsThroughAliases_data() {
        QTest::addColumn<int>("aliasKind");
        QTest::newRow("dangling-file-link") << 0;
        QTest::newRow("linked-parent-directory") << 1;
        QTest::newRow("linked-parent-before-dot-dot") << 2;
#ifdef Q_OS_UNIX
        QTest::newRow("relative-link-target-before-dot-dot") << 3;
#endif
    }
    void protectsDeletedDocumentsThroughAliases() {
        QFETCH(int, aliasKind);
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        const QString directory = m_profile->filePath("originals");
        QVERIFY(QDir().mkpath(directory));
        const QString path = QDir(directory).filePath("deleted.md");
        QString alias;
        QVERIFY(writeFile(path, "original"));
        if (aliasKind == 2 || aliasKind == 3) {
            const QString nestedDirectory = QDir(directory).filePath("nested");
            QVERIFY(QDir().mkpath(nestedDirectory));
            const QString aliasDirectory = m_profile->filePath("nested-link");
            QVERIFY(QFile::link(nestedDirectory, aliasDirectory));
            alias = aliasDirectory + QStringLiteral("/../deleted.md");
#ifdef Q_OS_UNIX
            if (aliasKind == 3) {
                alias = m_profile->filePath("indirect-link.md");
                const QByteArray encoded = QFile::encodeName(alias);
                QVERIFY(::symlink("nested-link/../deleted.md", encoded.constData()) == 0);
            }
#endif
        } else if (aliasKind == 1) {
            const QString aliasDirectory = m_profile->filePath("linked-directory");
            QVERIFY(QFile::link(directory, aliasDirectory));
            alias = QDir(aliasDirectory).filePath("deleted.md");
        } else {
            alias = m_profile->filePath("dangling.md");
            QVERIFY(QFile::link(path, alias));
        }
        QVERIFY(fixture.command("open", {{"path", path}}).value("ok").toBool());
        QVERIFY(fixture.replace(0, 8, QStringLiteral("mine")).value("ok").toBool());
        QVERIFY(QFile::remove(path));
        // Do not let the watcher run: aliases must be recognized directly.
        const auto blocked = fixture.command("save_as", {{"path", alias}, {"overwrite", true}});
        QCOMPARE(errorCode(blocked), QStringLiteral("disk_conflict"));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(alias));
        QVERIFY(fixture.state().value("externalDeleted").toBool());
        QVERIFY(fixture.command("resolve_external", {{"choice", "keep"}}).value("ok").toBool());
        const auto saved = fixture.command("save_as", {{"path", alias}, {"overwrite", true}});
        QVERIFY2(saved.value("ok").toBool(), jsonBytes(saved).constData());
        QCOMPARE(readFile(path), QByteArray("mine"));
        QCOMPARE(readFile(alias), QByteArray("mine"));
        QVERIFY(!fixture.state().value("externalChanged").toBool());
    }
    void handlesDeletedFilesAndExplicitReload() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        const QString path = m_profile->filePath("removed.md");
        QVERIFY(writeFile(path, "original"));
        QVERIFY(fixture.command("open", {{"path", path}}).value("ok").toBool());
        QVERIFY(QFile::remove(path));
        QCOMPARE(errorCode(fixture.command("save")), QStringLiteral("disk_conflict"));
        QVERIFY(fixture.state().value("externalDeleted").toBool());
        QVERIFY(fixture.command("resolve_external", {{"choice", "keep"}}).value("ok").toBool());
        QCOMPARE(fixture.state().value("file").toString(), path);
        QVERIFY(fixture.command("save").value("ok").toBool());
        QCOMPARE(readFile(path), QByteArray("original"));
        QVERIFY(fixture.replace(0, 8, QStringLiteral("local")).value("ok").toBool());
        QCOMPARE(errorCode(fixture.command("reload")), QStringLiteral("unsaved_changes"));
        QVERIFY(fixture.command("reload", {{"onUnsaved", "discard"}}).value("ok").toBool());
        QCOMPARE(fixture.state().value("text").toString(), QStringLiteral("original"));
    }

    void savesDirtyReloadWithoutRestoringOldDiskText() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        const QString path = m_profile->filePath("reload.md");
        QVERIFY(writeFile(path, "before"));
        QVERIFY(fixture.command("open", {{"path", path}}).value("ok").toBool());
        QVERIFY(fixture.replace(0, 6, QStringLiteral("saved draft")).value("ok").toBool());
        const auto reloaded = fixture.command("reload", {{"onUnsaved", "save"}});
        QVERIFY2(reloaded.value("ok").toBool(), jsonBytes(reloaded).constData());
        QCOMPARE(readFile(path), QByteArray("saved draft"));
        QCOMPARE(fixture.state().value("text").toString(), QStringLiteral("saved draft"));
        QVERIFY(!fixture.state().value("modified").toBool());
    }
    void handlesUiSaveAndDiscardThroughSharedController() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window.get()));
        const QString original = m_profile->filePath("original.md");
        const QString target = m_profile->filePath("target.md");
        QVERIFY(writeFile(original, "original"));
        QVERIFY(writeFile(target, "target"));
        QVERIFY(fixture.command("open", {{"path", original}}).value("ok").toBool());
        QVERIFY(fixture.replace(0, 8, QStringLiteral("saved via UI")).value("ok").toBool());
        fixture.session.requestOpen(QUrl::fromLocalFile(target));
        QObject *dialog = fixture.find("unsavedChangesDialog"); QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        click(fixture, "unsavedSave");
        QTRY_VERIFY(!dialog->property("opened").toBool());
        QTRY_VERIFY(fixture.state().value("activeDialog").toString().isEmpty());
        QTRY_COMPARE(fixture.state().value("text").toString(), QStringLiteral("target"));
        QCOMPARE(readFile(original), QByteArray("saved via UI"));
        QVERIFY(fixture.state().value("pendingAction").toString().isEmpty());
        QVERIFY(fixture.replace(0, 6, QStringLiteral("draft to discard")).value("ok").toBool());
        fixture.session.requestOpen(QUrl::fromLocalFile(original));
        QTRY_VERIFY(dialog->property("opened").toBool());
        click(fixture, "unsavedDiscard");
        QTRY_COMPARE(fixture.state().value("text").toString(), QStringLiteral("saved via UI"));
        QTRY_VERIFY(fixture.state().value("activeDialog").toString().isEmpty());
        QCOMPARE(readFile(target), QByteArray("target"));
        // A failed destination read after Discard must preserve the current draft.
        QVERIFY(fixture.replace(0, 12, QStringLiteral("keep this draft")).value("ok").toBool());
        fixture.session.requestOpen(QUrl::fromLocalFile(m_profile->filePath("absent.md")));
        QTRY_VERIFY(dialog->property("opened").toBool());
        click(fixture, "unsavedDiscard");
        QTRY_VERIFY(!dialog->property("opened").toBool());
        QCOMPARE(fixture.state().value("text").toString(), QStringLiteral("keep this draft"));
        QVERIFY(fixture.state().value("modified").toBool());
        QVERIFY(fixture.state().value("pendingAction").toString().isEmpty());
    }
    void keepsOverlappingDecisionsBusyUntilBothResolve() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window.get()));
        const QString path = m_profile->filePath("overlap.md");
        QVERIFY(writeFile(path, "original"));
        QVERIFY(fixture.command("open", {{"path", path}}).value("ok").toBool());
        QVERIFY(fixture.replace(0, 8, QStringLiteral("mine")).value("ok").toBool());
        fixture.window->close();
        QCOMPARE(errorCode(fixture.replace(0, 1, QStringLiteral("x"))), QStringLiteral("busy"));
        QObject *unsaved = fixture.find("unsavedChangesDialog"); QVERIFY(unsaved);
        QTRY_VERIFY(unsaved->property("opened").toBool());
        QVERIFY(writeFile(path, "theirs"));
        QObject *external = fixture.find("externalChangeDialog"); QVERIFY(external);
        QTRY_VERIFY(external->property("opened").toBool());
        QVERIFY(fixture.command("resolve_external", {{"choice", "keep"}}).value("ok").toBool());
        QCOMPARE(errorCode(fixture.replace(0, 1, QStringLiteral("x"))), QStringLiteral("busy"));
        QTRY_VERIFY(!external->property("visible").toBool());
        click(fixture, "unsavedCancel");
        QTRY_VERIFY(fixture.state().value("activeDialog").toString().isEmpty());
        QVERIFY(fixture.state().value("pendingAction").toString().isEmpty());
        QVERIFY(fixture.replace(0, 1, QStringLiteral("M")).value("ok").toBool());
        QCOMPARE(fixture.state().value("text").toString(), QStringLiteral("Mine"));
        QCOMPARE(readFile(path), QByteArray("theirs"));
    }
    void ignoresMalformedRecoveryText() {
        QVERIFY(QDir().mkpath(RuntimeProfile::dataDirectory()));
        QVERIFY(writeFile(QDir(RuntimeProfile::dataDirectory()).filePath("recovery-0.json"), "{\"text\":17}"));
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        QCOMPARE(fixture.state().value("text").toString(), QString());
        QVERIFY(!fixture.state().value("modified").toBool());
    }
    void findsAndReplacesThroughTheLiveInterface() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window.get()));
        QVERIFY(fixture.replace(0, 0, QStringLiteral("alpha beta alpha beta alpha")).value("ok").toBool());
        const QString version = fixture.state().value("version").toString();
        QTest::keyClick(fixture.window.get(), Qt::Key_H, Qt::ControlModifier);
        QTRY_VERIFY(fixture.window->property("replaceOpen").toBool());
        QObject *search = fixture.find("searchField"); QVERIFY(search);
        QObject *replacement = fixture.find("replaceField"); QVERIFY(replacement);
        QVERIFY(QMetaObject::invokeMethod(search, "forceActiveFocus"));
        for (const QChar key : QStringLiteral("alpha")) QTest::keyClick(fixture.window.get(), key.toLatin1());
        QTRY_COMPARE(fixture.state().value("selectionStart").toInt(), 0);
        QTRY_COMPARE(fixture.state().value("selectionEnd").toInt(), 5);
        QVERIFY(QMetaObject::invokeMethod(replacement, "forceActiveFocus"));
        for (const QChar key : QStringLiteral("gamma")) QTest::keyClick(fixture.window.get(), key.toLatin1());
        QTRY_COMPARE(replacement->property("text").toString(), QStringLiteral("gamma"));
        QSignalSpy rendered(fixture.window.get(), &QQuickWindow::frameSwapped);
        fixture.window->update();
        QTRY_VERIFY(!rendered.isEmpty());
        click(fixture, "replaceCurrentButton");
        QTRY_COMPARE(fixture.state().value("text").toString(), QStringLiteral("gamma beta alpha beta alpha"));
        QVERIFY(fixture.state().value("canUndo").toBool());
        QVERIFY(fixture.state().value("version").toString() != version);
        QCOMPARE(errorCode(fixture.command("undo", {{"expectedVersion", version}})), QStringLiteral("version_conflict"));
        click(fixture, "replaceAllButton");
        QTRY_COMPARE(fixture.state().value("text").toString(), QStringLiteral("gamma beta gamma beta gamma"));
        QCOMPARE(search->property("text").toString(), QStringLiteral("alpha"));
        QCOMPARE(fixture.state().value("selectionStart"), fixture.state().value("selectionEnd"));
        QVERIFY(QMetaObject::invokeMethod(search, "forceActiveFocus"));
        QTest::keyClick(fixture.window.get(), Qt::Key_Escape);
        QTRY_VERIFY(!fixture.window->property("searchOpen").toBool());
    }
    void capturesInterfaceAndDecisionDialogs() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window.get()));
        const QString sample = QStringLiteral("# A quiet workspace\n\nDraft **ideas**, inspect changes and keep writing.\n\n"
            "- One reliable document\n- A reversible edit\n\nSearch this draft. Another draft follows.\n");
        QVERIFY(fixture.replace(0, 0, sample).value("ok").toBool());
        fixture.window->resize(1280, 820);
        auto captureViews = [&](const QString &name) {
            fixture.backend.setViewMode("split");
            auto *source = fixture.find("sourceViewport");
            auto *preview = fixture.find("previewViewport");
            auto progress = [](QObject *view) {
                const qreal extent = view->property("contentHeight").toReal() - view->property("height").toReal();
                return extent > 0 ? view->property("contentY").toReal() / extent : 0;
            };
            QTRY_VERIFY(qAbs(progress(source) - progress(preview)) < 0.01);
            capture(fixture, name + "-split");
            fixture.backend.setViewMode("preview");
            capture(fixture, name + "-preview");
            fixture.backend.setViewMode("source");
        };
        capture(fixture, QStringLiteral("dark"));
        captureViews("dark");
        QVERIFY(writeFile(RuntimeProfile::themeDirectory() + QStringLiteral("/colors.toml"),
            "mode = \"light\"\nbackground = \"#ffffff\"\nforeground = \"#222324\"\n"
            "accent = \"#2077b2\"\nselection = \"#2077b2\"\n"));
        QTRY_VERIFY(!fixture.backend.darkMode());
        capture(fixture, QStringLiteral("light"));
        captureViews("light");
        QVERIFY(writeFile(RuntimeProfile::themeDirectory() + QStringLiteral("/colors.toml"),
            "mode = \"dark\"\nbackground = \"#101010\"\nforeground = \"#eeeeee\"\n"
            "accent = \"#5584aa\"\nselection = \"#186a9a\"\n"));
        QTRY_VERIFY(fixture.backend.darkMode());
        fixture.backend.setTextScale(4.0 / 3.0);
        fixture.window->resize(720, 520);
        QCOMPARE(fixture.find("sourceEditor")->property("font").value<QFont>().pixelSize(), 27);
        auto *editor = qobject_cast<QQuickItem *>(fixture.find("sourceEditor")); QVERIFY(editor);
        QTRY_VERIFY(editor->width() <= fixture.window->width());
        capture(fixture, QStringLiteral("minimum-scale"));
        captureViews("minimum-scale");
        fixture.backend.setTextScale(1);
        fixture.window->resize(1280, 820);
        QTest::keyClick(fixture.window.get(), Qt::Key_F, Qt::ControlModifier);
        QTRY_VERIFY(fixture.window->property("searchOpen").toBool());
        QObject *search = fixture.find("searchField"); QVERIFY(search);
        QVERIFY(QMetaObject::invokeMethod(search, "forceActiveFocus"));
        for (const QChar key : QStringLiteral("draft"))
            QTest::keyClick(fixture.window.get(), key.toLatin1());
        QTRY_COMPARE(search->property("text").toString(), QStringLiteral("draft"));
        QTRY_VERIFY(fixture.state().value("selectionEnd").toInt()
                    > fixture.state().value("selectionStart").toInt());
        capture(fixture, QStringLiteral("search"));
        QTest::keyClick(fixture.window.get(), Qt::Key_Escape);
        QTRY_VERIFY(!fixture.window->property("searchOpen").toBool());
        fixture.window->close();
        QCOMPARE(errorCode(fixture.replace(0, 1, QStringLiteral("x"))), QStringLiteral("busy"));
        QObject *unsaved = fixture.find("unsavedChangesDialog"); QVERIFY(unsaved);
        QTRY_VERIFY(unsaved->property("opened").toBool());
        QCOMPARE(errorCode(fixture.replace(0, 1, QStringLiteral("x"))), QStringLiteral("busy"));
        capture(fixture, QStringLiteral("unsaved"));
        click(fixture, "unsavedCancel");
        QTRY_VERIFY(!unsaved->property("opened").toBool());
        QTRY_VERIFY(fixture.state().value("activeDialog").toString().isEmpty());
        QVERIFY(fixture.window->isVisible());
        QVERIFY(fixture.state().value("pendingAction").toString().isEmpty());
        const QString path = m_profile->filePath("external.md");
        QVERIFY(fixture.command("save_as", {{"path", path}}).value("ok").toBool());
        QVERIFY(writeFile(path, "Changed in another editor"));
        QObject *external = fixture.find("externalChangeDialog"); QVERIFY(external);
        QTRY_VERIFY(external->property("opened").toBool());
        capture(fixture, QStringLiteral("external"));
        click(fixture, "externalReload");
        QTRY_VERIFY(!external->property("opened").toBool());
        QCOMPARE(fixture.state().value("text").toString(), QStringLiteral("Changed in another editor"));
    }
    void rejectsScreenshotOverwrite() {
        UiFixture fixture;
        QVERIFY2(fixture.load(m_qmlWarnings), qPrintable(fixture.error));
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window.get()));
        const QString path = m_profile->filePath("existing.png");
        QVERIFY(writeFile(path, "valuable"));
        QCOMPARE(errorCode(fixture.command("screenshot", {{"path", path}})), QStringLiteral("target_exists"));
        QCOMPARE(readFile(path), QByteArray("valuable"));
        QVERIFY(fixture.command("screenshot", {{"path", path}, {"overwrite", true}}).value("ok").toBool());
        QVERIFY(!QImage(path).isNull());
    }

    void preservesSymlinkSemanticsForStartupAndFollowUpSave() {
        const QString binary = binaryPath(); QVERIFY(QFileInfo(binary).isExecutable());
        const QString actualDirectory = m_profile->filePath("actual");
        const QString nestedDirectory = QDir(actualDirectory).filePath("nested");
        QVERIFY(QDir().mkpath(nestedDirectory));
        const QString actualFile = QDir(actualDirectory).filePath("file.md");
        const QString decoyFile = m_profile->filePath("file.md");
        QVERIFY(writeFile(actualFile, "actual target"));
        QVERIFY(writeFile(decoyFile, "decoy target"));
        const QString link = m_profile->filePath("linked-directory");
        QVERIFY(QFile::link(nestedDirectory, link));
        const QString alias = link + QStringLiteral("/../file.md");
        AppProcess app;
        app.process.start(binary, {"--profile", m_profile->path(), alias});
        QVERIFY(app.process.waitForStarted());
        QTRY_COMPARE_WITH_TIMEOUT(sessions(binary, m_profile->path()).size(), 1, 8000);
        const QString id = sessions(binary, m_profile->path()).first().toObject().value("id").toString();
        auto state = call(binary, m_profile->path(), id, "state").reply.value("result").toObject();
        QCOMPARE(state.value("text").toString(), QStringLiteral("actual target"));
        QCOMPARE(state.value("file").toString(), alias);
        QCOMPARE(call(binary, m_profile->path(), id, "replace_range", {{"expectedVersion", state.value("version")},
            {"start", 0}, {"end", 13}, {"text", "saved actual"}}).exitCode, 0);
        state = call(binary, m_profile->path(), id, "state").reply.value("result").toObject();
        QCOMPARE(call(binary, m_profile->path(), id, "save", {{"expectedVersion", state.value("version")}}).exitCode, 0);
        QCOMPARE(readFile(actualFile), QByteArray("saved actual"));
        QCOMPARE(readFile(decoyFile), QByteArray("decoy target"));
        state = call(binary, m_profile->path(), id, "state").reply.value("result").toObject();
        QCOMPARE(call(binary, m_profile->path(), id, "close", {{"expectedVersion", state.value("version")}}).exitCode, 0);
        QTRY_VERIFY_WITH_TIMEOUT(app.process.state() == QProcess::NotRunning, 5000);
    }
    void controlsRealProcessesAndIsolatesProfiles() {
        const QString binary = binaryPath();
        QVERIFY2(QFileInfo(binary).isExecutable(), qPrintable(QStringLiteral("Build the application first: ") + binary));
        QTemporaryDir secondProfile(m_profile->filePath("second-XXXXXX"));
        secondProfile.setAutoRemove(qEnvironmentVariable("OMASCRIBE_TEST_ARTIFACT_DIR").isEmpty());
        QVERIFY(secondProfile.isValid());
        AppProcess first, second;
        first.process.start(binary, {"--profile", m_profile->path()});
        second.process.start(binary, {"--profile", secondProfile.path()});
        QVERIFY(first.process.waitForStarted()); QVERIFY(second.process.waitForStarted());
        QTRY_COMPARE_WITH_TIMEOUT(sessions(binary, m_profile->path()).size(), 1, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(sessions(binary, secondProfile.path()).size(), 1, 8000);
        const QString id = sessions(binary, m_profile->path()).first().toObject().value("id").toString();
        const QString otherId = sessions(binary, secondProfile.path()).first().toObject().value("id").toString();
        QVERIFY(id != otherId);
        auto initial = call(binary, m_profile->path(), id, "state");
        QCOMPARE(initial.exitCode, 0);
        const QString initialVersion = initial.reply.value("result").toObject().value("version").toString();
        auto edited = call(binary, m_profile->path(), id, "replace_range", {{"expectedVersion", initialVersion},
            {"start", 0}, {"end", 0}, {"text", "Draft from an agent"}});
        QCOMPARE(edited.exitCode, 0);
        QCOMPARE(call(binary, secondProfile.path(), otherId, "state").reply.value("result").toObject()
                 .value("text").toString(), QString());
        const auto stale = call(binary, m_profile->path(), id, "undo", {{"expectedVersion", initialVersion}});
        QCOMPARE(stale.exitCode, 1);
        QCOMPARE(errorCode(stale.reply), QStringLiteral("version_conflict"));
        QString version = call(binary, m_profile->path(), id, "state").reply.value("result").toObject().value("version").toString();
        QCOMPARE(call(binary, m_profile->path(), id, "undo", {{"expectedVersion", version}}).exitCode, 0);
        auto state = call(binary, m_profile->path(), id, "state").reply.value("result").toObject();
        QCOMPARE(state.value("text").toString(), QString());
        QCOMPARE(call(binary, m_profile->path(), id, "redo", {{"expectedVersion", state.value("version")}}).exitCode, 0);
        state = call(binary, m_profile->path(), id, "state").reply.value("result").toObject();
        const QString saved = m_profile->filePath("controlled.md");
        QCOMPARE(call(binary, m_profile->path(), id, "save_as", {{"expectedVersion", state.value("version")}, {"path", saved}}).exitCode, 0);
        QCOMPARE(readFile(saved), QByteArray("Draft from an agent"));
        const auto invalid = control(binary, m_profile->path(), {"--session", id}, "not JSON\n");
        QCOMPARE(invalid.exitCode, 1); QVERIFY(!invalid.reply.value("ok").toBool());
        QCOMPARE(call(binary, m_profile->path(), id, "unknown").exitCode, 1);
        const auto oversized = control(binary, m_profile->path(), {"--session", id}, QByteArray(16 * 1024 * 1024 + 1, 'x') + '\n');
        QCOMPARE(oversized.exitCode, 1); QVERIFY(!oversized.reply.value("ok").toBool());
        const QString socketPath = QDir(RuntimeProfile::socketDirectory()).filePath(id + QStringLiteral(".sock"));
        // Exercise server validation directly, so client validation cannot mask it.
        QCOMPARE(errorCode(rawRequest(socketPath, QByteArray("not JSON\n"))), QStringLiteral("invalid_json"));
        QCOMPARE(errorCode(rawRequest(socketPath, QByteArray(ControlProtocol::maxMessageSize + 1, 'x'))),
                 QStringLiteral("request_too_large"));
        QElapsedTimer timeout;
        timeout.start();
        QCOMPARE(errorCode(rawRequest(socketPath, QByteArray("{\"protocol\":1"))), QStringLiteral("timeout"));
        QVERIFY(timeout.elapsed() >= 4500);
        // Successful close response must arrive before the process exits.
        state = call(binary, m_profile->path(), id, "state").reply.value("result").toObject();
        const auto closed = call(binary, m_profile->path(), id, "close", {{"expectedVersion", state.value("version")}});
        QCOMPARE(closed.exitCode, 0); QVERIFY(closed.reply.value("ok").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(first.process.state() == QProcess::NotRunning, 5000);
        QCOMPARE(first.process.exitCode(), 0);
        QCOMPARE(sessions(binary, m_profile->path()).size(), 0);
        QCOMPARE(sessions(binary, secondProfile.path()).size(), 1);
    }
    void boundsOversizedStateResponses() {
        const QJsonObject metadata{{"id", "session"}, {"version", "session:7"}, {"file", "/draft.md"}};
        QJsonObject state = metadata;
        // JSON escaping exceeds the protocol limit without a large rendered
        // document obscuring the actual wire-boundary behavior.
        state.insert("text", QString(3 * 1024 * 1024, QChar(1)));
        const auto encoded = ControlProtocol::encodeResponse({{"protocol", 1}, {"ok", true}, {"result", state}});
        QVERIFY(encoded.endsWith('\n'));
        QVERIFY(encoded.size() <= ControlProtocol::maxMessageSize + 1);
        const auto reply = QJsonDocument::fromJson(encoded).object();
        QCOMPARE(errorCode(reply), QStringLiteral("response_too_large"));
        QCOMPARE(reply.value("state").toObject(), metadata);
        const auto small = QJsonDocument::fromJson(ControlProtocol::encodeResponse(
            {{"protocol", 1}, {"ok", true}, {"result", metadata}})).object();
        QVERIFY(small.value("ok").toBool());
        QCOMPARE(small.value("result").toObject(), metadata);
    }
    void isolatesWindowsAndRecoversAnOrphanedSecondWindow() {
        const QString binary = binaryPath(); QVERIFY(QFileInfo(binary).isExecutable());
        AppProcess first, second;
        first.process.start(binary, {"--profile", m_profile->path()});
        QVERIFY(first.process.waitForStarted());
        QTRY_COMPARE_WITH_TIMEOUT(sessions(binary, m_profile->path()).size(), 1, 8000);
        const QString firstId = sessions(binary, m_profile->path()).first().toObject().value("id").toString();
        second.process.start(binary, {"--profile", m_profile->path()});
        QVERIFY(second.process.waitForStarted());
        QTRY_COMPARE_WITH_TIMEOUT(sessions(binary, m_profile->path()).size(), 2, 8000);
        QString secondId;
        for (const auto &entry : sessions(binary, m_profile->path())) {
            const QString candidate = entry.toObject().value("id").toString();
            if (candidate != firstId) secondId = candidate;
        }
        QVERIFY(!secondId.isEmpty());
        auto state = call(binary, m_profile->path(), secondId, "state").reply.value("result").toObject();
        QCOMPARE(call(binary, m_profile->path(), secondId, "replace_range", {{"expectedVersion", state.value("version")},
            {"start", 0}, {"end", 0}, {"text", "Second window draft"}}).exitCode, 0);
        QCOMPARE(call(binary, m_profile->path(), firstId, "state").reply.value("result").toObject()
                 .value("text").toString(), QString());
        const QString data = m_profile->filePath("data/omascribe");
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(QDir(data).filePath("recovery-1.json")), 4000);
        second.process.kill(); QVERIFY(second.process.waitForFinished());
        state = call(binary, m_profile->path(), firstId, "state").reply.value("result").toObject();
        QCOMPARE(call(binary, m_profile->path(), firstId, "close", {{"expectedVersion", state.value("version")}}).exitCode, 0);
        QTRY_VERIFY_WITH_TIMEOUT(first.process.state() == QProcess::NotRunning, 5000);
        first.process.start(binary, {"--profile", m_profile->path()});
        QVERIFY(first.process.waitForStarted());
        QTRY_COMPARE_WITH_TIMEOUT(sessions(binary, m_profile->path()).size(), 1, 8000);
        const QString recoveredId = sessions(binary, m_profile->path()).first().toObject().value("id").toString();
        state = call(binary, m_profile->path(), recoveredId, "state").reply.value("result").toObject();
        QCOMPARE(state.value("text").toString(), QStringLiteral("Second window draft"));
        QCOMPARE(call(binary, m_profile->path(), recoveredId, "close", {{"expectedVersion", state.value("version")},
            {"onUnsaved", "discard"}}).exitCode, 0);
        QTRY_VERIFY_WITH_TIMEOUT(first.process.state() == QProcess::NotRunning, 5000);
        QVERIFY(QDir(data).entryList({"recovery-*.json"}, QDir::Files).isEmpty());
    }
    void preservesRecoveryDiskBaselineAndIgnoresStartupFile() {
        const QString binary = binaryPath(); QVERIFY(QFileInfo(binary).isExecutable());
        const QString path = m_profile->filePath("named.md");
        const QString alternate = m_profile->filePath("alternate.md");
        QVERIFY(writeFile(path, "original")); QVERIFY(writeFile(alternate, "wrong startup text"));
        AppProcess app;
        app.process.start(binary, {"--profile", m_profile->path(), path});
        QVERIFY(app.process.waitForStarted());
        QTRY_COMPARE_WITH_TIMEOUT(sessions(binary, m_profile->path()).size(), 1, 8000);
        const QString id = sessions(binary, m_profile->path()).first().toObject().value("id").toString();
        auto state = call(binary, m_profile->path(), id, "state").reply.value("result").toObject();
        QCOMPARE(state.value("text").toString(), QStringLiteral("original"));
        QCOMPARE(call(binary, m_profile->path(), id, "replace_range", {{"expectedVersion", state.value("version")},
            {"start", 0}, {"end", 8}, {"text", "recovered local"}}).exitCode, 0);
        const QString data = m_profile->filePath("data/omascribe");
        QTRY_VERIFY_WITH_TIMEOUT(!QDir(data).entryList({"recovery-*.json"}, QDir::Files).isEmpty(), 4000);
        app.process.kill(); QVERIFY(app.process.waitForFinished());
        QVERIFY(writeFile(path, "changed while closed"));
        app.process.start(binary, {"--profile", m_profile->path(), alternate});
        QVERIFY(app.process.waitForStarted());
        QTRY_COMPARE_WITH_TIMEOUT(sessions(binary, m_profile->path()).size(), 1, 8000);
        const QString recoveredId = sessions(binary, m_profile->path()).first().toObject().value("id").toString();
        state = call(binary, m_profile->path(), recoveredId, "state").reply.value("result").toObject();
        QCOMPARE(state.value("text").toString(), QStringLiteral("recovered local"));
        QCOMPARE(state.value("file").toString(), path);
        QVERIFY(state.value("externalChanged").toBool());
        const auto blocked = call(binary, m_profile->path(), recoveredId, "save", {{"expectedVersion", state.value("version")}});
        QCOMPARE(blocked.exitCode, 1);
        QCOMPARE(errorCode(blocked.reply), QStringLiteral("busy"));
        QCOMPARE(readFile(path), QByteArray("changed while closed"));
        QCOMPARE(call(binary, m_profile->path(), recoveredId, "resolve_external", {{"expectedVersion", state.value("version")},
            {"choice", "keep"}}).exitCode, 0);
        QTRY_VERIFY(call(binary, m_profile->path(), recoveredId, "state").reply.value("result").toObject()
                    .value("activeDialog").toString().isEmpty());
        state = call(binary, m_profile->path(), recoveredId, "state").reply.value("result").toObject();
        QCOMPARE(call(binary, m_profile->path(), recoveredId, "save", {{"expectedVersion", state.value("version")}}).exitCode, 0);
        QCOMPARE(readFile(path), QByteArray("recovered local"));
        QVERIFY(QDir(data).entryList({"recovery-*.json"}, QDir::Files).isEmpty());
        state = call(binary, m_profile->path(), recoveredId, "state").reply.value("result").toObject();
        QCOMPARE(call(binary, m_profile->path(), recoveredId, "close", {{"expectedVersion", state.value("version")}}).exitCode, 0);
        QTRY_VERIFY_WITH_TIMEOUT(app.process.state() == QProcess::NotRunning, 5000);
    }
    void recoversAfterProcessCrashAndInvalidatesOldVersion() {
        const QString binary = binaryPath(); QVERIFY(QFileInfo(binary).isExecutable());
        AppProcess app;
        app.process.start(binary, {"--profile", m_profile->path()});
        QVERIFY(app.process.waitForStarted());
        QTRY_COMPARE_WITH_TIMEOUT(sessions(binary, m_profile->path()).size(), 1, 8000);
        const QString id = sessions(binary, m_profile->path()).first().toObject().value("id").toString();
        auto state = call(binary, m_profile->path(), id, "state").reply.value("result").toObject();
        const auto edit = call(binary, m_profile->path(), id, "replace_range", {{"expectedVersion", state.value("version")},
            {"start", 0}, {"end", 0}, {"text", "Recovered draft"}});
        QCOMPARE(edit.exitCode, 0);
        const QString oldVersion = call(binary, m_profile->path(), id, "state").reply.value("result").toObject().value("version").toString();
        const QString dataDirectory = m_profile->filePath("data/omascribe");
        QTRY_VERIFY_WITH_TIMEOUT(!QDir(dataDirectory).entryList({"recovery-*.json"}, QDir::Files).isEmpty(), 4000);
        app.process.kill(); QVERIFY(app.process.waitForFinished());
        app.process.start(binary, {"--profile", m_profile->path()});
        QVERIFY(app.process.waitForStarted());
        QTRY_COMPARE_WITH_TIMEOUT(sessions(binary, m_profile->path()).size(), 1, 8000);
        const QString recoveredId = sessions(binary, m_profile->path()).first().toObject().value("id").toString();
        QVERIFY(recoveredId != id);
        state = call(binary, m_profile->path(), recoveredId, "state").reply.value("result").toObject();
        QCOMPARE(state.value("text").toString(), QStringLiteral("Recovered draft"));
        QVERIFY(state.value("modified").toBool());
        QVERIFY(state.value("version").toString() != oldVersion);
        QCOMPARE(errorCode(call(binary, m_profile->path(), recoveredId, "undo", {{"expectedVersion", oldVersion}}).reply),
                 QStringLiteral("version_conflict"));
        QCOMPARE(call(binary, m_profile->path(), recoveredId, "close", {{"expectedVersion", state.value("version")},
            {"onUnsaved", "discard"}}).exitCode, 0);
        QTRY_VERIFY_WITH_TIMEOUT(app.process.state() == QProcess::NotRunning, 5000);
        QVERIFY(QDir(dataDirectory).entryList({"recovery-*.json"}, QDir::Files).isEmpty());
    }
private:
    QJsonObject rawRequest(const QString &path, const QByteArray &bytes) {
        QLocalSocket socket;
        socket.connectToServer(path);
        if (!socket.waitForConnected(2000)) return {};
        socket.write(bytes);
        socket.flush();
        QByteArray reply;
        QElapsedTimer timer;
        timer.start();
        while (!reply.contains('\n') && timer.elapsed() < 8000) {
            if (!socket.bytesAvailable()) socket.waitForReadyRead(8000 - int(timer.elapsed()));
            reply += socket.readAll();
            if (socket.state() == QLocalSocket::UnconnectedState) break;
        }
        return QJsonDocument::fromJson(reply).object();
    }
    void click(UiFixture &fixture, const char *name) {
        auto *item = qobject_cast<QQuickItem *>(fixture.find(name));
        QVERIFY2(item, name);
        QVERIFY(item->isVisible());
        QTest::mouseClick(fixture.window.get(), Qt::LeftButton, Qt::NoModifier,
                         item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
    }
    void capture(UiFixture &fixture, const QString &name) {
        const QString configured = qEnvironmentVariable("OMASCRIBE_TEST_ARTIFACT_DIR");
        const QString directory = configured.isEmpty() ? m_profile->filePath("artifacts") : configured;
        QVERIFY(QDir().mkpath(directory));
        const QString path = QDir(directory).filePath(name + QStringLiteral(".png"));
        const auto reply = fixture.command("screenshot", {{"path", path}, {"overwrite", true}});
        QVERIFY2(reply.value("ok").toBool(), jsonBytes(reply).constData());
        const QImage image(path);
        QVERIFY(!image.isNull());
        QVERIFY(image.width() >= fixture.window->width());
        QVERIFY(image.height() >= fixture.window->height());
        QJsonObject state = fixture.state();
        state.insert("capture", reply.value("result"));
        QVERIFY(writeFile(QDir(directory).filePath(name + QStringLiteral(".json")),
                          QJsonDocument(state).toJson(QJsonDocument::Indented)));
    }
    std::unique_ptr<QTemporaryDir> m_profile;
    QStringList m_qmlWarnings;
};

int main(int argc, char **argv) {
    QTemporaryDir profile;
    QString error;
    if (!profile.isValid() || !RuntimeProfile::configure(profile.path(), &error)) {
        qCritical().noquote() << error; return 1;
    }
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    RuntimeProfile::initializeApplication(app);
    OmascribeTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "tst_omascribe.moc"
