#include "controltransport.h"

#include "editorsession.h"
#include "runtimeprofile.h"

#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

#include <cerrno>
#include <cstdio>
#include <functional>

#ifdef Q_OS_UNIX
#include <poll.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {
QJsonObject failure(const QString &code, const QString &message) {
    return {{QStringLiteral("protocol"), 1}, {QStringLiteral("ok"), false},
            {QStringLiteral("error"), QJsonObject{
                {QStringLiteral("code"), code}, {QStringLiteral("message"), message}}}};
}

QByteArray encoded(const QJsonObject &object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

bool validSessionId(const QString &id) {
    static const QRegularExpression pattern(QStringLiteral(
        "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    return pattern.match(id).hasMatch() && !QUuid(id).isNull();
}

bool secureDirectory(const QString &directory, bool create, QString *error) {
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    QFileInfo before(directory);
    if (before.isSymLink())
        return fail(QStringLiteral("The control directory must not be a symbolic link."));
    if (!before.exists() && (!create || !QDir().mkpath(directory)))
        return fail(QStringLiteral("The control directory is unavailable."));
#ifdef Q_OS_UNIX
    struct stat status {};
    const QByteArray path = QFile::encodeName(directory);
    if (::lstat(path.constData(), &status) != 0 || !S_ISDIR(status.st_mode)
        || status.st_uid != ::geteuid())
        return fail(QStringLiteral("The control directory must belong to the current user."));
    if (create && ::chmod(path.constData(), 0700) != 0)
        return fail(QStringLiteral("Could not make the control directory private."));
    if (::lstat(path.constData(), &status) != 0 || !S_ISDIR(status.st_mode)
        || status.st_uid != ::geteuid() || (status.st_mode & 0777) != 0700)
        return fail(QStringLiteral("The control directory must have permissions 0700."));
#else
    if (!QFileInfo(directory).isDir())
        return fail(QStringLiteral("The control directory is not a directory."));
    if (create && !QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner
                                                   | QFile::ExeOwner))
        return fail(QStringLiteral("Could not make the control directory private."));
#endif
    return true;
}

bool secureSocket(const QString &path) {
#ifdef Q_OS_UNIX
    struct stat status {};
    const QByteArray name = QFile::encodeName(path);
    return ::lstat(name.constData(), &status) == 0 && S_ISSOCK(status.st_mode)
        && status.st_uid == ::geteuid() && (status.st_mode & 0077) == 0;
#else
    return !QFileInfo(path).isSymLink();
#endif
}

bool parseRequest(const QByteArray &bytes, QJsonObject *request, QJsonObject *error) {
    if (bytes.size() > ControlProtocol::maxMessageSize) {
        *error = failure(QStringLiteral("request_too_large"),
                         QStringLiteral("The request exceeds 16 MiB."));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = failure(QStringLiteral("invalid_json"),
                         QStringLiteral("Expected one JSON object."));
        return false;
    }
    const QJsonObject object = document.object();
    if (!object.value(QStringLiteral("protocol")).isDouble()
        || object.value(QStringLiteral("protocol")).toDouble() != 1) {
        *error = failure(QStringLiteral("unsupported_protocol"),
                         QStringLiteral("The protocol field must be 1."));
        return false;
    }
    if (!object.value(QStringLiteral("command")).isString()
        || object.value(QStringLiteral("command")).toString().isEmpty()
        || !object.value(QStringLiteral("args")).isObject()) {
        *error = failure(QStringLiteral("invalid_request"),
                         QStringLiteral("A command string and an args object are required."));
        return false;
    }
    *request = object;
    return true;
}

QByteArray boundedResponse(const QJsonObject &response) {
    QByteArray bytes = encoded(response);
    if (bytes.size() <= ControlProtocol::maxMessageSize + 1)
        return bytes;
    QJsonObject error = failure(QStringLiteral("response_too_large"),
                               QStringLiteral("The response exceeds 16 MiB; text was omitted."));
    QJsonObject metadata = response.value(QStringLiteral("result")).toObject();
    if (metadata.isEmpty())
        metadata = response.value(QStringLiteral("state")).toObject();
    metadata.remove(QStringLiteral("text"));
    metadata.remove(QStringLiteral("markdown"));
    error.insert(QStringLiteral("state"), metadata);
    bytes = encoded(error);
    if (bytes.size() > ControlProtocol::maxMessageSize + 1) {
        error.insert(QStringLiteral("state"), QJsonObject{
            {QStringLiteral("id"), metadata.value(QStringLiteral("id"))},
            {QStringLiteral("version"), metadata.value(QStringLiteral("version"))}});
        bytes = encoded(error);
    }
    return bytes;
}

class RequestConnection : public QObject {
public:
    using Dispatch = std::function<void(const QJsonObject &, RequestConnection *)>;
    RequestConnection(QLocalSocket *socket, EditorSession *session,
                      const Dispatch &dispatch, QObject *parent)
        : QObject(parent), m_socket(socket), m_session(session), m_dispatch(dispatch) {
        socket->setParent(this);
        socket->setReadBufferSize(ControlProtocol::maxMessageSize + 1);
        m_timer.setSingleShot(true);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this] {
            m_expired = true;
            if (!m_replyStarted && !m_executing)
                reply(failure(QStringLiteral("timeout"),
                              QStringLiteral("The request was not executed within 5 seconds.")));
            else
                m_socket->abort();
            if (m_closeAfterReply)
                scheduleClose();
        });
        QObject::connect(socket, &QLocalSocket::readyRead, this, [this] { read(); });
        QObject::connect(socket, &QLocalSocket::bytesWritten, this,
                         [this](qint64) { finishReply(); });
        QObject::connect(socket, &QLocalSocket::disconnected, this, [this] {
            m_timer.stop();
            if (m_closeAfterReply)
                scheduleClose();
            deleteLater();
        });
        m_timer.start(ControlProtocol::timeoutMs);
        if (socket->bytesAvailable())
            read();
    }

    bool beginExecute() {
        if (m_expired || m_replyStarted || m_socket->state() != QLocalSocket::ConnectedState)
            return false;
        m_executing = true;
        return true;
    }

    void respond(const QJsonObject &result, bool close) {
        m_closeAfterReply = close;
        if (m_expired || m_replyStarted || m_socket->state() != QLocalSocket::ConnectedState) {
            if (close)
                scheduleClose();
            return;
        }
        reply(result);
    }

private:
    void read() {
        if (m_answered)
            return;
        const qsizetype remaining = ControlProtocol::maxMessageSize + 1 - m_input.size();
        m_input += m_socket->read(remaining);
        const qsizetype newline = m_input.indexOf('\n');
        if (newline < 0) {
            if (m_input.size() > ControlProtocol::maxMessageSize)
                reply(failure(QStringLiteral("request_too_large"),
                              QStringLiteral("The request exceeds 16 MiB.")));
            return;
        }
        if (!m_input.mid(newline + 1).trimmed().isEmpty()) {
            reply(failure(QStringLiteral("invalid_request"),
                          QStringLiteral("Only one request is accepted per connection.")));
            return;
        }
        QJsonObject request;
        QJsonObject error;
        if (!parseRequest(m_input.left(newline), &request, &error)) {
            reply(error);
            return;
        }
        // A request can wait in the server queue while another command waits
        // for rendering. Further readyRead events must never dispatch it twice.
        m_answered = true;
        m_dispatch(request, this);
    }

    void reply(const QJsonObject &response) {
        m_answered = true;
        m_replyStarted = true;
        m_replyPending = true;
        m_socket->write(ControlProtocol::encodeResponse(response));
        m_socket->flush();
        finishReply();
    }

    void finishReply() {
        if (!m_replyPending || m_socket->bytesToWrite() != 0)
            return;
        m_replyPending = false;
        m_timer.stop();
        m_socket->disconnectFromServer();
        if (m_closeAfterReply)
            scheduleClose();
    }

    void scheduleClose() {
        if (m_closeScheduled || !m_session)
            return;
        m_closeScheduled = true;
        // Drain the acknowledgement first whenever the connection is alive.
        // If the peer left after executing close, finish the committed close;
        // the client correctly observes an unknown outcome rather than a retry.
        QPointer<EditorSession> session = m_session;
        QTimer::singleShot(0, m_session, [session] {
            if (session)
                session->completeClose();
        });
    }

    QLocalSocket *m_socket;
    QPointer<EditorSession> m_session;
    Dispatch m_dispatch;
    QTimer m_timer;
    QByteArray m_input;
    bool m_answered = false;
    bool m_replyStarted = false;
    bool m_replyPending = false;
    bool m_closeAfterReply = false;
    bool m_closeScheduled = false;
    bool m_executing = false;
    bool m_expired = false;
};

struct Exchange {
    QJsonObject response;
    int exitCode = 3;
};

int remainingMs(const QDeadlineTimer &deadline) {
    return int(qMax<qint64>(0, deadline.remainingTime()));
}

Exchange exchange(const QString &path, const QJsonObject &request) {
    if (!secureSocket(path))
        return {failure(QStringLiteral("unavailable"),
                        QStringLiteral("The session socket is unavailable or is not private.")), 3};
    QLocalSocket socket;
    socket.setReadBufferSize(ControlProtocol::maxMessageSize + 1);
    QDeadlineTimer deadline(ControlProtocol::timeoutMs);
    socket.connectToServer(path);
    if (!socket.waitForConnected(remainingMs(deadline)))
        return {failure(QStringLiteral("unavailable"), socket.errorString()), 3};
    const QByteArray message = encoded(request);
    const qint64 accepted = socket.write(message);
    if (accepted < 0)
        return {failure(QStringLiteral("transport_error"), socket.errorString()), 3};
    const auto uncertain = [](const QString &message) {
        return Exchange{failure(QStringLiteral("unknown_outcome"), message), 2};
    };
    while (socket.bytesToWrite() > 0) {
        if (deadline.hasExpired() || !socket.waitForBytesWritten(remainingMs(deadline)))
            return uncertain(QStringLiteral("The connection ended or timed out after sending; do not retry a change automatically."));
    }
    QByteArray response;
    while (true) {
        response += socket.read(ControlProtocol::maxMessageSize + 1 - response.size());
        const qsizetype newline = response.indexOf('\n');
        if (newline >= 0) {
            if (!response.mid(newline + 1).trimmed().isEmpty())
                return uncertain(QStringLiteral("The server returned more than one response."));
            QJsonParseError error;
            const QJsonDocument json = QJsonDocument::fromJson(response.left(newline), &error);
            if (error.error != QJsonParseError::NoError || !json.isObject())
                return uncertain(QStringLiteral("The server returned invalid JSON."));
            const QJsonObject object = json.object();
            if (object.value(QStringLiteral("protocol")).toDouble(-1) != 1
                || !object.value(QStringLiteral("ok")).isBool())
                return uncertain(QStringLiteral("The server returned an invalid response envelope."));
            return {object, object.value(QStringLiteral("ok")).toBool() ? 0 : 1};
        }
        if (response.size() > ControlProtocol::maxMessageSize)
            return uncertain(QStringLiteral("The server response exceeds 16 MiB."));
        if (socket.state() != QLocalSocket::ConnectedState || deadline.hasExpired()
            || !socket.waitForReadyRead(remainingMs(deadline))) {
            // Closing after the reply is normal. Consume the last buffered bytes
            // even when waitForReadyRead reported the disconnection.
            if (socket.bytesAvailable() > 0)
                continue;
            return uncertain(QStringLiteral("The connection ended or timed out before a complete response; do not retry a change automatically."));
        }
    }
}

bool readInput(QByteArray *input, QJsonObject *error) {
#ifdef Q_OS_UNIX
    QDeadlineTimer deadline(ControlProtocol::timeoutMs);
    while (true) {
        struct pollfd descriptor {STDIN_FILENO, POLLIN, 0};
        const int result = ::poll(&descriptor, 1, remainingMs(deadline));
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0) {
            *error = failure(QStringLiteral("input_timeout"),
                             QStringLiteral("No complete JSON request arrived on stdin within 5 seconds."));
            return false;
        }
        char buffer[4096];
        const ssize_t count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            *error = failure(QStringLiteral("input_error"), QStringLiteral("Could not read stdin."));
            return false;
        }
        if (count == 0)
            return true;
        input->append(buffer, int(count));
        if (input->size() > ControlProtocol::maxMessageSize + 1) {
            *error = failure(QStringLiteral("request_too_large"), QStringLiteral("The request exceeds 16 MiB."));
            return false;
        }
        const qsizetype newline = input->indexOf('\n');
        if (newline >= 0) {
            if (!input->mid(newline + 1).trimmed().isEmpty()) {
                *error = failure(QStringLiteral("invalid_request"),
                                 QStringLiteral("Provide only one JSON object on stdin."));
                return false;
            }
            input->truncate(newline);
            return true;
        }
    }
#else
    QFile standardInput;
    if (!standardInput.open(stdin, QIODevice::ReadOnly)) {
        *error = failure(QStringLiteral("input_error"), QStringLiteral("Could not read stdin."));
        return false;
    }
    *input = standardInput.read(ControlProtocol::maxMessageSize + 1);
    return true;
#endif
}

int printResult(const Exchange &result) {
    const QByteArray bytes = ControlProtocol::encodeResponse(result.response);
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fflush(stdout);
    if (result.exitCode != 0) {
        const QByteArray diagnostic = result.response.value(QStringLiteral("error"))
                                          .toObject().value(QStringLiteral("message"))
                                          .toString().toUtf8();
        std::fprintf(stderr, "%s\n", diagnostic.constData());
    }
    return result.exitCode;
}
} // namespace

QByteArray ControlProtocol::encodeResponse(const QJsonObject &response) {
    return boundedResponse(response);
}

ControlServer::ControlServer(EditorSession *session, QObject *parent)
    : QObject(parent), m_session(session), m_server(new QLocalServer(this)) {
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    QObject::connect(m_server, &QLocalServer::newConnection, this, [this] {
        while (m_server->hasPendingConnections()) {
            new RequestConnection(m_server->nextPendingConnection(), m_session,
                [this](const QJsonObject &request, RequestConnection *connection) {
                    m_commands.enqueue([this, request, target = QPointer<RequestConnection>(connection)] {
                        if (!target || !target->beginExecute())
                            return;
                        const QJsonObject result = m_session->execute(request);
                        const bool close = request.value(QStringLiteral("command")).toString()
                                               == QStringLiteral("close")
                                           && result.value(QStringLiteral("ok")).toBool();
                        if (target)
                            target->respond(result, close);
                        else if (close) {
                            QPointer<EditorSession> session = m_session;
                            QTimer::singleShot(0, m_session, [session] {
                                if (session)
                                    session->completeClose();
                            });
                        }
                    });
                    if (m_dispatching)
                        return;
                    m_dispatching = true;
                    while (!m_commands.isEmpty())
                        m_commands.dequeue()();
                    m_dispatching = false;
                }, this);
        }
    });
}

ControlServer::~ControlServer() {
    m_server->close();
    if (!m_socketPath.isEmpty())
        QLocalServer::removeServer(m_socketPath);
}

bool ControlServer::listen(QString *error) {
    if (m_server->isListening())
        return true;
    const QString directory = RuntimeProfile::socketDirectory();
    if (!secureDirectory(directory, true, error))
        return false;
    if (!validSessionId(m_session->id())) {
        if (error)
            *error = QStringLiteral("The session identifier is not a UUID.");
        return false;
    }
    const QString path = QDir(directory).filePath(m_session->id() + QStringLiteral(".sock"));
#ifdef Q_OS_UNIX
    if (QFile::encodeName(path).size() >= qsizetype(sizeof(sockaddr_un::sun_path))) {
        if (error)
            *error = QStringLiteral("The control socket path is too long.");
        return false;
    }
#endif
    // Never remove an existing endpoint: another live window may own it.
    if (!m_server->listen(path)) {
        if (error)
            *error = m_server->errorString();
        return false;
    }
    m_socketPath = path;
#ifdef Q_OS_UNIX
    const QByteArray filename = QFile::encodeName(path);
    if (::chmod(filename.constData(), 0600) != 0 || !secureSocket(path)) {
        m_server->close();
        QLocalServer::removeServer(path);
        m_socketPath.clear();
        if (error)
            *error = QStringLiteral("Could not make the control socket private.");
        return false;
    }
#endif
    return true;
}

int runControlClient(const QStringList &arguments) {
    const int marker = arguments.indexOf(QStringLiteral("--control"));
    bool list = false;
    QString session;
    QJsonObject inputError;
    const auto usage = [] {
        return printResult({failure(QStringLiteral("invalid_arguments"),
            QStringLiteral("Use --control list or --control --session <UUID>.")), 1});
    };
    if (marker < 0)
        return usage();
    for (int index = marker + 1; index < arguments.size(); ++index) {
        const QString argument = arguments.at(index);
        if (argument == QStringLiteral("list") && !list && session.isEmpty())
            list = true;
        else if (argument == QStringLiteral("--session") && !list && session.isEmpty()
                 && index + 1 < arguments.size())
            session = arguments.at(++index);
        else
            return usage();
    }
    if ((!list && !validSessionId(session)) || (list && !session.isEmpty()))
        return usage();
    const QString directory = RuntimeProfile::socketDirectory();
    QString directoryError;
    if (!secureDirectory(directory, false, &directoryError)) {
        // Discovery of an unused profile is a successful empty listing.
        if (list && !QFileInfo::exists(directory) && !QFileInfo(directory).isSymLink())
            return printResult({{{QStringLiteral("protocol"), 1}, {QStringLiteral("ok"), true},
                {QStringLiteral("result"), QJsonObject{{QStringLiteral("sessions"), QJsonArray()}}}}, 0});
        return printResult({failure(QStringLiteral("unavailable"), directoryError), 3});
    }
    if (list) {
        QJsonArray sessions;
        const QDir sockets(directory);
        const QStringList names = sockets.entryList({QStringLiteral("*.sock")}, QDir::System,
                                                    QDir::Name);
        const QJsonObject request {{QStringLiteral("protocol"), 1},
            {QStringLiteral("command"), QStringLiteral("state")},
            {QStringLiteral("args"), QJsonObject{{QStringLiteral("includeText"), false}}}};
        for (const QString &name : names) {
            const QString id = name.chopped(5);
            if (!validSessionId(id))
                continue;
            const Exchange probe = exchange(sockets.filePath(name), request);
            if (probe.exitCode != 0)
                continue;
            QJsonObject state = probe.response.value(QStringLiteral("result")).toObject();
            if (state.value(QStringLiteral("id")).toString() != id)
                continue;
            sessions.append(state);
        }
        return printResult({{{QStringLiteral("protocol"), 1}, {QStringLiteral("ok"), true},
            {QStringLiteral("result"), QJsonObject{{QStringLiteral("sessions"), sessions}}}}, 0});
    }
    QByteArray bytes;
    if (!readInput(&bytes, &inputError))
        return printResult({inputError, 1});
    QJsonObject request;
    if (!parseRequest(bytes, &request, &inputError))
        return printResult({inputError, 1});
    return printResult(exchange(QDir(directory).filePath(session + QStringLiteral(".sock")), request));
}
