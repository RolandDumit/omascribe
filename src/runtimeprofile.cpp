#include "runtimeprofile.h"
#include "appversion.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QQuickStyle>
#include <QSettings>
#include <QStandardPaths>
#include <unistd.h>

namespace {
QString profilePath;
QString runtimePath;

bool privateDirectory(const QString &path, QString *error) {
    const QFileInfo before(path);
    if (before.isSymLink() || (before.exists() && before.ownerId() != uint(getuid()))) {
        if (error) *error = QStringLiteral("Profile directory must belong to the current user: %1").arg(path);
        return false;
    }
    if (!QDir().mkpath(path) || !QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        if (error) *error = QStringLiteral("Could not create private directory: %1").arg(path);
        return false;
    }
    return true;
}
}

bool RuntimeProfile::configure(const QString &directory, QString *error, bool prepare) {
    profilePath = QDir::cleanPath(QFileInfo(directory).absoluteFilePath());
    if (prepare && !privateDirectory(profilePath, error)) return false;
    const QString canonical = QFileInfo(profilePath).canonicalFilePath();
    if (!canonical.isEmpty()) profilePath = canonical;
    if (prepare) for (const QString &part : {QStringLiteral("config"), QStringLiteral("data"), QStringLiteral("cache"),
                                QStringLiteral("documents"), QStringLiteral("theme")}) {
        if (!privateDirectory(profilePath + QLatin1Char('/') + part, error)) return false;
    }
    const QString privateRoot = QStringLiteral("/tmp/omascribe-%1").arg(getuid());
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(profilePath.toUtf8(), QCryptographicHash::Sha256).toHex().left(20));
    runtimePath = privateRoot + QLatin1Char('/') + hash;
    if (prepare && (!privateDirectory(privateRoot, error) || !privateDirectory(runtimePath, error))) return false;
    qputenv("XDG_CONFIG_HOME", (profilePath + QStringLiteral("/config")).toUtf8());
    qputenv("XDG_DATA_HOME", (profilePath + QStringLiteral("/data")).toUtf8());
    qputenv("XDG_CACHE_HOME", (profilePath + QStringLiteral("/cache")).toUtf8());
    qputenv("XDG_STATE_HOME", (profilePath + QStringLiteral("/data")).toUtf8());
    qputenv("XDG_RUNTIME_DIR", runtimePath.toUtf8());
    qputenv("QT_QPA_PLATFORMTHEME", QByteArray());
    qputenv("GSETTINGS_BACKEND", QByteArray("memory"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profilePath + QStringLiteral("/config"));
    const QString themeFile = themeDirectory() + QStringLiteral("/colors.toml");
    const QByteArray requestedTheme = qgetenv("OMASCRIBE_PROFILE_THEME");
    if (prepare && (!QFileInfo::exists(themeFile) || !requestedTheme.isEmpty())) {
        const bool light = requestedTheme == "light";
        QFile file(themeFile);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error) *error = QStringLiteral("Could not write profile theme.");
            return false;
        }
        const QByteArray palette = light
            ? QByteArray("mode = \"light\"\nbackground = \"#ffffff\"\nforeground = \"#222324\"\naccent = \"#2077b2\"\nselection = \"#2077b2\"\n")
            : QByteArray("mode = \"dark\"\nbackground = \"#101010\"\nforeground = \"#eeeeee\"\naccent = \"#5584aa\"\nselection = \"#186a9a\"\n");
        if (file.write(palette) != palette.size()) return false;
    }
    return true;
}

QString RuntimeProfile::path() { return profilePath; }
QString RuntimeProfile::dataDirectory() {
    return profilePath.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                 : profilePath + QStringLiteral("/data/omascribe");
}
QString RuntimeProfile::themeDirectory() {
    return profilePath.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/state/omarchy/current/theme")
                                 : profilePath + QStringLiteral("/theme");
}
QString RuntimeProfile::socketDirectory() {
    if (!profilePath.isEmpty()) return runtimePath + QStringLiteral("/control");
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return runtime.isEmpty() ? QStringLiteral("/tmp/omascribe-%1/control").arg(getuid())
                             : runtime + QStringLiteral("/omascribe-control");
}
qreal RuntimeProfile::textScale() {
    bool ok = false;
    const qreal scale = qEnvironmentVariable("OMASCRIBE_PROFILE_SCALE").toDouble(&ok);
    return ok && scale > 0 ? qBound(0.5, scale, 3.0) : 1.0;
}
void RuntimeProfile::initializeApplication(QApplication &app) {
    app.setApplicationName(QStringLiteral("omascribe"));
    app.setApplicationDisplayName(QStringLiteral("Omascribe"));
    app.setApplicationVersion(QString::fromLatin1(Omascribe::version));
    app.setOrganizationName(QStringLiteral("Omascribe"));
    app.setDesktopFileName(QStringLiteral("omascribe"));
    for (const QString &font : {QStringLiteral("Regular"), QStringLiteral("Italic"),
                                QStringLiteral("Bold"), QStringLiteral("BoldItalic")})
        QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/IBMPlexMono-%1.ttf").arg(font));
    QQuickStyle::setStyle(QStringLiteral("Material"));
    applyTextScale(app, profilePath.isEmpty() ? 1.0 : textScale());
}
void RuntimeProfile::applyTextScale(QApplication &app, qreal scale) {
    QFont font(QStringLiteral("IBM Plex Mono"));
    static const qreal basePointSize = font.pointSizeF() > 0 ? font.pointSizeF() : app.font().pointSizeF();
    font.setPointSizeF(basePointSize * scale);
    app.setFont(font);
}
