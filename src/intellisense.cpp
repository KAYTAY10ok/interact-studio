#include "intellisense.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <array>
#include <utility>

namespace {

constexpr int packetLimit = 4 * 1024 * 1024;
constexpr int headerLimit = 4096;
constexpr int retainedDocs = 8;
constexpr int idleMs = 60000;
constexpr int startupMs = 18000;
constexpr int replyMs = 7000;
constexpr int restartCooldownMs = 3000;

QString translateStatus(const char *text)
{
    return QCoreApplication::translate("LanguageService", text);
}

QString localPath(const QString &path)
{
    if (path.isEmpty()) return {};
    const QFileInfo info(path);
    const QString resolved = info.canonicalFilePath();
    return QDir::cleanPath(resolved.isEmpty() ? info.absoluteFilePath() : resolved);
}

QString fileUri(const QString &path)
{
    return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
}

QString languageId(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix();
    const QString lower = suffix.toLower();
    if (lower == "rs") return "rust";
    if (suffix == "c") return "c";
    static const QStringList cpp{"h", "hh", "hpp", "hxx", "cc", "cpp", "cxx", "c++", "ipp", "inl"};
    if (suffix == "C" || cpp.contains(lower)) return "cpp";
    return {};
}

QJsonObject position(int line, int column)
{
    return {{"line", line}, {"character", column}};
}

bool readRange(const QJsonObject &range, int &firstLine, int &firstColumn,
               int &lastLine, int &lastColumn)
{
    const auto start = range.value("start").toObject();
    const auto end = range.value("end").toObject();
    firstLine = start.value("line").toInt(-1);
    firstColumn = start.value("character").toInt(-1);
    lastLine = end.value("line").toInt(-1);
    lastColumn = end.value("character").toInt(-1);
    return firstLine >= 0 && firstColumn >= 0 && lastLine >= firstLine &&
           lastColumn >= 0 && (lastLine != firstLine || lastColumn >= firstColumn);
}

QJsonObject rustConfig()
{
    return {
        {"cargo", QJsonObject{{"buildScripts", QJsonObject{{"enable", false}}}}},
        {"procMacro", QJsonObject{{"enable", false}}},
        {"checkOnSave", false},
        {"check", QJsonObject{{"enable", false}}}
    };
}

QList<SemanticCompletion> completionsFrom(const QJsonValue &result)
{
    QList<SemanticCompletion> items;
    const QJsonObject list = result.toObject();
    const auto values = result.isArray() ? result.toArray() : list.value("items").toArray();
    const auto defaultRange = list.value("itemDefaults").toObject().value("editRange").toObject();

    for (const auto &value : values) {
        const auto item = value.toObject();
        // No snippet capability is advertised. Never insert literal snippet
        // placeholders or a completion that requires additional edits.
        if (item.value("insertTextFormat").toInt(1) != 1 ||
            !item.value("additionalTextEdits").toArray().isEmpty()) continue;
        SemanticCompletion entry;
        entry.label = item.value("label").toString().trimmed();
        if (entry.label.isEmpty() || entry.label.size() > 250 || entry.label.contains('\n')) continue;
        entry.detail = item.value("detail").toString().simplified().left(240);
        const auto edit = item.value("textEdit").toObject();
        entry.insertText = edit.value("newText").toString(
            item.value("textEditText").toString(item.value("insertText").toString(entry.label)));
        if (entry.insertText.isEmpty() || entry.insertText.size() > 4096 || entry.insertText.contains(QChar::Null))
            continue;
        auto range = edit.value("range").toObject();
        if (range.isEmpty()) range = edit.value("replace").toObject();
        if (range.isEmpty()) {
            range = defaultRange.value("replace").toObject();
            if (range.isEmpty()) range = defaultRange;
        }
        if (!range.isEmpty() &&
            !readRange(range, entry.startLine, entry.startColumn, entry.endLine, entry.endColumn)) continue;
        items.append(std::move(entry));
        if (items.size() == 64) break;
    }
    return items;
}

QString signatureFrom(const QJsonValue &result)
{
    const auto details = result.toObject();
    const auto signatures = details.value("signatures").toArray();
    if (signatures.isEmpty()) return {};
    const int active = qBound(0, details.value("activeSignature").toInt(0), signatures.size() - 1);
    return signatures.at(active).toObject().value("label").toString().simplified().left(600);
}

class Session final : public QObject {
public:
    using Reply = std::function<void(QJsonValue)>;

    Session(LanguageService *client, const QString &root, QString tool, QString executable)
        : QObject(client), client(client), tool(std::move(tool)), executable(std::move(executable)),
          folder{{"uri", fileUri(root)}, {"name", QDir(root).dirName()}}
    {
        process.setWorkingDirectory(root);
        auto environment = QProcessEnvironment::systemEnvironment();
        // This IDE session only needs local analysis. Registry downloads
        // remain an explicit build/dependency-management operation.
        if (isRust()) environment.insert("CARGO_NET_OFFLINE", "true");
        process.setProcessEnvironment(environment);
        connect(&process, &QProcess::started, this, [this] { initialize(); });
        connect(&process, &QProcess::readyReadStandardOutput, this, [this] { consume(); });
        connect(&process, &QProcess::readyReadStandardError, this, [this] {
            // Drain logs without keeping them in memory.
            process.readAllStandardError();
        });
        connect(&process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this] {
            if (phase != Phase::Closing) report(translateStatus("%1 exited. Local suggestions remain available.").arg(this->tool));
            deleteLater();
        });
        connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                fail(translateStatus("Could not start %1.").arg(this->tool));
                deleteLater();
            }
        });
        idle.setSingleShot(true);
        transition.setSingleShot(true);
        connect(&idle, &QTimer::timeout, this, [this] { stop(); });
        connect(&transition, &QTimer::timeout, this, [this] {
            if (phase == Phase::Closing) process.kill();
            else fail(translateStatus("%1 did not initialize in time.").arg(this->tool));
        });
        for (int index = 0; index < 2; ++index) {
            timers.at(index).setSingleShot(true);
            connect(&timers.at(index), &QTimer::timeout, this, [this, index] {
                const bool explicitRequest = requests.at(index).explicitRequest;
                cancel(index);
                if (explicitRequest) report(translateStatus("%1 did not answer in time.").arg(this->tool));
            });
        }
    }

    ~Session() override
    {
        process.disconnect(this);
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(800);
        }
    }

    bool active() const { return phase != Phase::Closing; }

    void start()
    {
        const QStringList flags = isRust()
            ? QStringList{} : QStringList{"-j=2", "--background-index=false", "--log=error"};
        transition.start(startupMs);
        process.start(executable, flags);
    }

    bool sync(const LanguageSnapshot &snapshot, const QString &language, bool openDocument, bool activity)
    {
        auto existing = documents.find(snapshot.path);
        if (existing == documents.end()) {
            if (!openDocument) return false;
            if (documents.size() >= retainedDocs) {
                auto oldest = documents.begin();
                for (auto next = documents.begin(); next != documents.end(); ++next)
                    if (next->touch < oldest->touch) oldest = next;
                close(oldest.key());
            }
            existing = documents.insert(snapshot.path, Document{});
        }

        Document &document = existing.value();
        document.touch = ++nextTouch;
        if (activity) idle.start(idleMs);
        if (document.version > 0 && document.revision == snapshot.revision && document.text == snapshot.text)
            return true;
        if (!activity) idle.start(idleMs);

        for (int index = 0; index < 2; ++index)
            if (requests.at(index).path == snapshot.path) cancel(index);
        document.text = snapshot.text;
        document.revision = snapshot.revision;
        document.language = language;
        document.version = ++nextVersion;
        if (phase == Phase::Ready) transmit(snapshot.path, document);
        return true;
    }

    void close(const QString &path)
    {
        auto document = documents.find(path);
        if (document == documents.end()) return;
        for (int index = 0; index < 2; ++index)
            if (requests.at(index).path == path) cancel(index);
        if (phase == Phase::Ready && document->sentVersion != 0)
            send({{"method", "textDocument/didClose"}, {"params", QJsonObject{
                {"textDocument", QJsonObject{{"uri", fileUri(path)}}}}}});
        documents.erase(document);
    }

    void request(int index, const LanguageSnapshot &snapshot, bool explicitRequest, Reply reply)
    {
        const auto document = documents.constFind(snapshot.path);
        if (document == documents.constEnd()) return;
        auto &current = requests.at(index);
        if (current.path == snapshot.path && current.version == document->version &&
            current.line == snapshot.line && current.column == snapshot.column) {
            current.reply = std::move(reply);
            current.explicitRequest = current.explicitRequest || explicitRequest;
            return;
        }
        cancel(index);
        current = {snapshot.path, document->version, snapshot.line, snapshot.column,
                   0, explicitRequest, std::move(reply)};
        if (phase == Phase::Ready) dispatch(index);
    }

private:
    enum class Phase { Initializing, Ready, Closing };

    struct Document {
        QString text, language;
        int revision = 0;
        int version = 0;
        int sentVersion = 0;
        int sentLines = 0;
        int sentFinalColumn = 0;
        quint64 touch = 0;
    };

    struct Request {
        QString path;
        int version = 0, line = 0, column = 0, id = 0;
        bool explicitRequest = false;
        Reply reply;
    };

    LanguageService *client;
    QString tool, executable;
    QJsonObject folder;
    QProcess process;
    QTimer idle, transition;
    std::array<QTimer, 2> timers;
    std::array<Request, 2> requests;
    QHash<QString, Document> documents;
    QByteArray buffer;
    Phase phase = Phase::Initializing;
    quint64 nextTouch = 0;
    int nextVersion = 0;
    int nextId = 2;
    int syncKind = 1;
    int expected = 0;
    bool signatures = false;
    bool completion = false;

    bool isRust() const { return tool == "rust-analyzer"; }

    void report(const QString &status)
    {
        if (client->onStatus) client->onStatus(status);
    }

    void send(QJsonObject object)
    {
        if (process.state() != QProcess::Running) return;
        object.insert("jsonrpc", "2.0");
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        if (process.bytesToWrite() > packetLimit) {
            fail(translateStatus("%1 stopped reading requests.").arg(tool));
            return;
        }
        process.write("Content-Length: " + QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes);
    }

    void initialize()
    {
        const QJsonArray encodings{"utf-16"};
        QJsonObject params{
            {"processId", static_cast<double>(QCoreApplication::applicationPid())},
            {"rootUri", folder.value("uri")},
            {"workspaceFolders", QJsonArray{folder}},
            {"capabilities", QJsonObject{
                {"general", QJsonObject{{"positionEncodings", encodings}}},
                {"offsetEncoding", encodings},
                {"workspace", QJsonObject{{"workspaceFolders", true}, {"configuration", true}}},
                {"textDocument", QJsonObject{
                    {"completion", QJsonObject{{"completionItem", QJsonObject{{"snippetSupport", false}}}}},
                    {"publishDiagnostics", QJsonObject{{"versionSupport", true}}}
                }}
            }}
        };
        if (isRust()) params.insert("initializationOptions", rustConfig());
        send({{"id", 1}, {"method", "initialize"}, {"params", params}});
    }

    void transmit(const QString &path, Document &doc)
    {
        const auto uri = fileUri(path);
        if (!doc.sentVersion) {
            send({{"method", "textDocument/didOpen"}, {"params", QJsonObject{
                {"textDocument", QJsonObject{{"uri", uri}, {"languageId", doc.language},
                                             {"version", doc.version}, {"text", doc.text}}}}}});
        } else if (doc.sentVersion != doc.version && syncKind != 0) {
            QJsonObject change{{"text", doc.text}};
            if (syncKind == 2) {
                // A single edit covering the previous snapshot is a valid
                // incremental change. Its range uses the old UTF-16 length.
                change.insert("range", QJsonObject{{"start", position(0, 0)},
                    {"end", position(doc.sentLines, doc.sentFinalColumn)}});
            }
            send({{"method", "textDocument/didChange"}, {"params", QJsonObject{
                {"textDocument", QJsonObject{{"uri", uri}, {"version", doc.version}}},
                {"contentChanges", QJsonArray{change}}}}});
        }
        if (syncKind == 0 && doc.sentVersion) return;
        doc.sentVersion = doc.version;
        doc.sentLines = doc.text.count('\n');
        doc.sentFinalColumn = doc.text.size() - doc.text.lastIndexOf('\n') - 1;
    }

    void dispatch(int index)
    {
        auto &pending = requests.at(index);
        if (!pending.reply || phase != Phase::Ready) return;
        if ((index == 0 && !completion) || (index == 1 && !signatures)) {
            const bool explicitRequest = pending.explicitRequest;
            const auto reply = std::move(pending.reply);
            cancel(index);
            if (explicitRequest) report(translateStatus("%1 does not provide this capability.").arg(tool));
            reply(QJsonValue::Null);
            return;
        }
        if (documents.value(pending.path).sentVersion != pending.version) {
            cancel(index);
            return;
        }
        pending.id = nextId++;
        timers.at(index).start(replyMs);
        send({{"id", pending.id},
              {"method", index == 0 ? "textDocument/completion" : "textDocument/signatureHelp"},
              {"params", QJsonObject{
                  {"textDocument", QJsonObject{{"uri", fileUri(pending.path)}}},
                  {"position", position(pending.line, pending.column)}}}});
    }

    void cancel(int index)
    {
        timers.at(index).stop();
        const int id = requests.at(index).id;
        requests.at(index) = {};
        if (id) send({{"method", "$/cancelRequest"}, {"params", QJsonObject{{"id", id}}}});
    }

    void consume()
    {
        buffer += process.readAllStandardOutput();
        while (!buffer.isEmpty()) {
            if (!expected) {
                int separator = buffer.indexOf("\r\n\r\n");
                if (separator < 0 && buffer.size() <= headerLimit) return;
                if (separator < 0 || separator > headerLimit) {
                    fail(translateStatus("%1 sent an invalid message header.").arg(tool));
                    return;
                }
                for (const auto &line : buffer.left(separator).split('\n')) {
                    const int colon = line.indexOf(':');
                    if (colon < 0 || line.left(colon).trimmed().toLower() != "content-length") continue;
                    if (expected) {
                        fail(translateStatus("%1 sent a duplicate message length.").arg(tool));
                        return;
                    }
                    expected = line.mid(colon + 1).trimmed().toInt();
                }
                if (expected <= 0 || expected > packetLimit) {
                    fail(translateStatus("%1 sent an invalid or oversized message.").arg(tool));
                    return;
                }
                buffer.remove(0, separator + 4);
            }
            if (buffer.size() < expected) return;
            const auto message = QJsonDocument::fromJson(buffer.left(expected));
            buffer.remove(0, expected);
            expected = 0;
            if (!message.isObject()) {
                fail(translateStatus("%1 sent invalid JSON.").arg(tool));
                return;
            }
            handle(message.object());
        }
    }

    void handle(const QJsonObject &message)
    {
        const auto id = message.value("id");
        const auto method = message.value("method").toString();
        if (!method.isEmpty()) {
            const auto params = message.value("params").toObject();
            if (!id.isUndefined() && !id.isNull()) replyToServer(id, method, params);
            else if (phase == Phase::Ready && method == "textDocument/publishDiagnostics")
                diagnostics(params);
            return;
        }
        const int responseId = id.toInt(-1);
        if (responseId == 0 && phase == Phase::Closing) {
            send({{"method", "exit"}});
            process.closeWriteChannel();
            return;
        }
        if (responseId == 1 && phase == Phase::Initializing) {
            initialized(message);
            return;
        }
        if (phase != Phase::Ready || responseId < 2) return;
        for (int index = 0; index < 2; ++index) {
            if (requests.at(index).id != responseId) continue;
            timers.at(index).stop();
            auto request = std::exchange(requests.at(index), {});
            const auto document = documents.constFind(request.path);
            if (document == documents.constEnd() || document->version != request.version) return;
            const auto error = message.value("error").toObject();
            if (!error.isEmpty() && request.explicitRequest) {
                const int code = error.value("code").toInt();
                if (code != -32800 && code != -32801 && code != -32802)
                    report(translateStatus("%1: %2").arg(tool, error.value("message").toString().left(240)));
            }
            if (request.reply) request.reply(message.value("result"));
            return;
        }
    }

    void replyToServer(const QJsonValue &id, const QString &method, const QJsonObject &params)
    {
        if (method == "workspace/workspaceFolders") {
            send({{"id", id}, {"result", QJsonArray{folder}}});
        } else if (method == "workspace/configuration") {
            QJsonArray response;
            for (const auto &item : params.value("items").toArray()) {
                const auto section = item.toObject().value("section").toString();
                QJsonValue setting = QJsonValue::Null;
                if (isRust() && (section.isEmpty() || section == "rust-analyzer" ||
                                 section.startsWith("rust-analyzer."))) {
                    setting = rustConfig();
                    for (const auto &key : section.section('.', 1).split('.', Qt::SkipEmptyParts))
                        setting = setting.toObject().value(key);
                }
                response.append(setting.isUndefined() ? QJsonValue(QJsonValue::Null) : setting);
            }
            send({{"id", id}, {"result", response}});
        } else if (method == "window/workDoneProgress/create" || method == "client/registerCapability" ||
                   method == "client/unregisterCapability") {
            send({{"id", id}, {"result", QJsonValue::Null}});
        } else {
            send({{"id", id}, {"error", QJsonObject{
                {"code", -32601}, {"message", "Method not supported"}}}});
        }
    }

    void initialized(const QJsonObject &message)
    {
        const auto result = message.value("result").toObject();
        if (message.contains("error") || !message.value("result").isObject()) {
            fail(translateStatus("%1 rejected initialization.").arg(tool));
            return;
        }
        const auto capabilities = result.value("capabilities").toObject();
        const auto encoding = capabilities.value("positionEncoding").toString(
            result.value("offsetEncoding").toString("utf-16"));
        if (encoding != "utf-16") {
            fail(translateStatus("%1 does not support UTF-16 positions.").arg(tool));
            return;
        }
        const auto sync = capabilities.value("textDocumentSync");
        syncKind = sync.isObject() ? sync.toObject().value("change").toInt(1) : sync.toInt(1);
        signatures = capabilities.value("signatureHelpProvider").isObject();
        completion = capabilities.value("completionProvider").isObject();
        phase = Phase::Ready;
        transition.stop();
        send({{"method", "initialized"}, {"params", QJsonObject{}}});
        for (auto doc = documents.begin(); doc != documents.end(); ++doc) transmit(doc.key(), doc.value());
        for (int index = 0; index < 2; ++index) dispatch(index);
    }

    void diagnostics(const QJsonObject &params)
    {
        if (!client->onDiagnostics) return;
        const QUrl uri(params.value("uri").toString(), QUrl::StrictMode);
        if (!uri.isValid() || !uri.isLocalFile() || !uri.host().isEmpty() || uri.hasQuery() || uri.hasFragment()) return;
        const QString path = localPath(uri.toLocalFile());
        const auto doc = documents.constFind(path);
        if (doc == documents.constEnd() || doc->version != doc->sentVersion) return;
        const auto version = params.value("version");
        if (version.toInt(-1) != doc->version) return;

        QList<SemanticDiagnostic> marks;
        for (const auto &item : params.value("diagnostics").toArray()) {
            const auto data = item.toObject();
            SemanticDiagnostic mark;
            if (!readRange(data.value("range").toObject(), mark.startLine, mark.startColumn,
                           mark.endLine, mark.endColumn)) continue;
            mark.message = data.value("message").toString().left(1200);
            if (mark.message.isEmpty()) continue;
            mark.severity = data.value("severity").toInt(0);
            marks.append(std::move(mark));
            if (marks.size() == 80) break;
        }
        client->onDiagnostics(path, doc->revision, std::move(marks));
    }

    void fail(const QString &status)
    {
        if (phase == Phase::Closing) return;
        phase = Phase::Closing;
        idle.stop();
        transition.stop();
        for (auto &timer : timers) timer.stop();
        report(status);
        buffer.clear();
        expected = 0;
        process.kill();
    }

    void stop()
    {
        if (phase == Phase::Closing) return;
        if (phase != Phase::Ready) {
            phase = Phase::Closing;
            process.kill();
            return;
        }
        const auto paths = documents.keys();
        for (const auto &path : paths) close(path);
        phase = Phase::Closing;
        transition.start(900);
        send({{"id", 0}, {"method", "shutdown"}});
    }
};

} // namespace

class LanguageService::State {
public:
    explicit State(LanguageService *owner, QString root)
        : owner(owner), root(localPath(root)) {}

    ~State()
    {
        delete clang.data();
        delete rust.data();
    }

    QString filePath(const QString &path) const
    {
        const auto file = localPath(path);
        if (file.isEmpty() || root.isEmpty()) return {};
        const auto relative = QDir(root).relativeFilePath(file);
        if (QDir::isAbsolutePath(relative) || relative == ".." || relative.startsWith("../")) return {};
        return file;
    }

    Session *select(const QString &language, bool activate, bool explicitRequest)
    {
        if (language.isEmpty()) return nullptr;
        auto &slot = language == "rust" ? rust : clang;
        if (slot && slot->active()) return slot;
        if (!activate) return nullptr;
        auto &attempt = language == "rust" ? rustAttempt : clangAttempt;
        if (!explicitRequest && attempt.isValid() && !attempt.hasExpired(restartCooldownMs)) return nullptr;
        attempt.start();
        const QString tool = language == "rust" ? "rust-analyzer" : "clangd";
        const QString executable = QStandardPaths::findExecutable(tool);
        if (executable.isEmpty()) {
            if (explicitRequest && owner->onStatus)
                owner->onStatus(translateStatus("Install %1 and add it to PATH to enable language assistance.").arg(tool));
            return nullptr;
        }
        slot = new Session(owner, root, tool, executable);
        slot->start();
        return slot;
    }

    Session *synchronize(const LanguageSnapshot &input, LanguageSnapshot &snapshot, bool activate, bool explicitRequest)
    {
        if (input.path.isEmpty() || input.text.size() > semanticDocumentLimit) return nullptr;
        snapshot = input;
        snapshot.path = filePath(input.path);
        const auto language = languageId(snapshot.path);
        if (auto *server = select(language, activate, explicitRequest)) {
            if (server->sync(snapshot, language, activate, activate)) return server;
        }
        return nullptr;
    }

    LanguageService *owner;
    QString root;
    QPointer<Session> clang, rust;
    QElapsedTimer clangAttempt, rustAttempt;
};

LanguageService::LanguageService(QString workspace, QObject *parent)
    : QObject(parent), state(std::make_unique<State>(this, std::move(workspace))) {}

LanguageService::~LanguageService() = default;

void LanguageService::complete(const LanguageSnapshot &input,
                               std::function<void(QList<SemanticCompletion>)> onItems,
                               bool explicitRequest)
{
    if (!onItems) return;
    LanguageSnapshot snapshot;
    if (auto *session = state->synchronize(input, snapshot, true, explicitRequest))
        session->request(0, snapshot, explicitRequest,
            [onItems = std::move(onItems)](const QJsonValue &reply) { onItems(completionsFrom(reply)); });
}

void LanguageService::signature(const LanguageSnapshot &input, std::function<void(QString)> onSignature)
{
    if (!onSignature) return;
    LanguageSnapshot snapshot;
    if (auto *session = state->synchronize(input, snapshot, true, true))
        session->request(1, snapshot, true,
            [onSignature = std::move(onSignature)](const QJsonValue &reply) { onSignature(signatureFrom(reply)); });
}

void LanguageService::analyze(const LanguageSnapshot &input)
{
    LanguageSnapshot snapshot;
    state->synchronize(input, snapshot, true, true);
}

void LanguageService::update(const LanguageSnapshot &input)
{
    if (!state->clang && !state->rust) return;
    LanguageSnapshot snapshot;
    state->synchronize(input, snapshot, false, false);
}

void LanguageService::closeDocument(QString path)
{
    path = state->filePath(path);
    if (auto *session = state->select(languageId(path), false, false)) session->close(path);
}
