#pragma once

#include <QObject>
#include <QByteArray>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <functional>

class EditorSession;
class QJsonObject;
class QLocalServer;

namespace ControlProtocol {
constexpr qsizetype maxMessageSize = 16 * 1024 * 1024;
constexpr int timeoutMs = 5000;
QByteArray encodeResponse(const QJsonObject &response);
}

// The transport runs in the editor's thread. Each connection carries exactly
// one bounded JSONL request and response; command semantics belong to the session.
class ControlServer : public QObject {
public:
    explicit ControlServer(EditorSession *session, QObject *parent = nullptr);
    ~ControlServer() override;
    bool listen(QString *error = nullptr);

private:
    EditorSession *m_session;
    QLocalServer *m_server;
    QString m_socketPath;
    QQueue<std::function<void()>> m_commands;
    bool m_dispatching = false;
};

// 0: success; 1: definite command/input error; 2: unknown outcome after sending;
// 3: no request sent (endpoint unavailable or another transport failure).
int runControlClient(const QStringList &arguments);
