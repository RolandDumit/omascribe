#pragma once

#include <QObject>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QUrl>

class Backend;
class QQuickWindow;

class EditorSession : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString pendingAction READ pendingAction NOTIFY pendingChanged)
public:
    explicit EditorSession(Backend *backend, QObject *parent = nullptr);
    void attachWindow(QQuickWindow *window);
    QString id() const { return m_id; }
    QString pendingAction() const { return m_pendingAction; }
    QJsonObject state(bool includeText = true);
    QJsonObject execute(const QJsonObject &request);
    void completeClose();

    Q_INVOKABLE void requestOpen(const QUrl &url);
    Q_INVOKABLE bool requestClose();
    Q_INVOKABLE void requestSave();
    Q_INVOKABLE void requestSaveAs();
    Q_INVOKABLE void acceptSave(const QUrl &url);
    Q_INVOKABLE void cancelPending();
    Q_INVOKABLE void resolvePending(const QString &choice);
    Q_INVOKABLE void resolveExternal(const QString &choice);
    Q_INVOKABLE void setDialog(const QString &name, bool visible);
    Q_INVOKABLE void printDocument();

signals:
    void pendingChanged();
    void unsavedDecisionRequested();
    void closeRequested();
    void externalResolved();

private:
    void refresh();
    void advance();
    QJsonObject success();
    QJsonObject failure(const QString &code, const QString &message);
    QJsonObject saveFile(const QString &path, bool overwrite);
    QJsonObject transition(const QString &action, const QString &path, const QJsonObject &args);
    bool diskMatches() const;
    QString currentFile() const;
    QString activeDialog() const;
    QString version() const;
    bool rangeValid(int start, int end) const;
    void finishUi(const QJsonObject &result);

    Backend *m_backend;
    QPointer<QQuickWindow> m_window;
    QPointer<QObject> m_editor;
    QString m_id;
    quint64 m_generation = 0;
    QString m_seenText;
    QString m_seenFile;
    QString m_pendingAction;
    QString m_pendingPath;
    QSet<QString> m_dialogs;
    bool m_externalChanged = false;
    bool m_externalDeleted = false;
    bool m_closeReady = false;
    bool m_uiSavingPending = false;
    bool m_capturing = false;
};
