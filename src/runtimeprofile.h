#pragma once

#include <QString>

class QApplication;

namespace RuntimeProfile {
bool configure(const QString &directory, QString *error = nullptr, bool prepare = true);
QString path();
QString dataDirectory();
QString themeDirectory();
QString socketDirectory();
qreal textScale();
void initializeApplication(QApplication &app);
void applyTextScale(QApplication &app, qreal scale);
}
