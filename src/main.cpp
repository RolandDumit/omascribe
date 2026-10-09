#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickWindow>
#include <cstdio>
#include <memory>

#include "backend.h"
#include "appversion.h"
#include "controltransport.h"
#include "editorsession.h"
#include "runtimeprofile.h"
#include "systemtheme.h"

int main(int argc, char *argv[]) {
    QStringList arguments;
    for (int i = 1; i < argc; ++i) arguments << QString::fromLocal8Bit(argv[i]);
    QString profile;
    const int profileIndex = arguments.indexOf(QStringLiteral("--profile"));
    if (profileIndex >= 0) {
        if (profileIndex + 1 >= arguments.size()) {
            std::fprintf(stderr, "--profile needs a directory.\n");
            return 1;
        }
        profile = arguments.takeAt(profileIndex + 1);
        arguments.removeAt(profileIndex);
        QString error;
        if (!RuntimeProfile::configure(profile, &error, !arguments.contains(QStringLiteral("--control")))) {
            std::fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
            return 1;
        }
    }
    if (arguments.contains(QStringLiteral("--control"))) {
        QCoreApplication app(argc, argv);
        app.setApplicationName(QStringLiteral("omascribe"));
        app.setApplicationVersion(QString::fromLatin1(Omascribe::version));
        return runControlClient(arguments);
    }
    if (arguments.contains(QStringLiteral("--help")) || arguments.contains(QStringLiteral("-h"))) {
        std::puts("Usage: omascribe [--profile DIRECTORY] [FILE]\n"
                  "       omascribe [--profile DIRECTORY] --control list\n"
                  "       omascribe [--profile DIRECTORY] --control --session UUID < request.json");
        return 0;
    }
    if (arguments == QStringList{QStringLiteral("--version")}) {
        std::printf("Omascribe %s\n", Omascribe::version);
        return 0;
    }
    if (arguments.size() > 1 || (!arguments.isEmpty() && arguments.first().startsWith(QLatin1Char('-')))) {
        std::fprintf(stderr, "Use --help for supported arguments.\n");
        return 1;
    }
    if (!profile.isEmpty()) QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    RuntimeProfile::initializeApplication(app);
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("omascribe")));

    Backend backend(&app);
    QObject::connect(&backend, &Backend::textScaleChanged, &app, [&app, &backend] {
        RuntimeProfile::applyTextScale(app, backend.textScale());
    });
    EditorSession session(&backend, &app);
    std::unique_ptr<SystemTheme> systemTheme;
    if (profile.isEmpty()) {
        systemTheme = std::make_unique<SystemTheme>(&app);
        backend.setDarkMode(systemTheme->darkMode());
        backend.setTextScale(systemTheme->textScale());
        RuntimeProfile::applyTextScale(app, systemTheme->textScale());
        QObject::connect(systemTheme.get(), &SystemTheme::darkModeChanged, &backend, &Backend::setDarkMode);
        QObject::connect(systemTheme.get(), &SystemTheme::textScaleChanged, &backend, [&app, &backend](qreal scale) {
            RuntimeProfile::applyTextScale(app, scale);
            backend.setTextScale(scale);
        });
    } else backend.setTextScale(RuntimeProfile::textScale());

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app, [](const QList<QQmlError> &warnings) {
        for (const QQmlError &warning : warnings) qWarning().noquote() << warning.toString();
    });
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
    engine.rootContext()->setContextProperty(QStringLiteral("session"), &session);
    engine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        qCritical() << "Could not load the Omascribe interface.";
        return 1;
    }
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    backend.setParentWindow(window);
    session.attachWindow(window);
    if (!arguments.isEmpty() && !backend.modified()) {
        const QString file = QDir::isAbsolutePath(arguments.first()) ? arguments.first()
            : QDir::currentPath() + QLatin1Char('/') + arguments.first();
        session.requestOpen(QUrl::fromLocalFile(file));
    }
    ControlServer control(&session, &app);
    QString error;
    if (!control.listen(&error)) qWarning().noquote() << "Local control unavailable:" << error;
    return app.exec();
}
