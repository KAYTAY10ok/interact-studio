#include "definition.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace {

constexpr int messageLimit = 4 * 1024 * 1024;
constexpr int headerLimit = 4096;
constexpr int requestTimeout = 20000;
constexpr int shutdownTimeout = 1000;
constexpr int shutdownId = 10;

QString tr(const char *text)
{
    return QCoreApplication::translate("Definition", text);
}

void showMessage(QWidget *parent, const QString &text)
{
    auto *box = new QMessageBox(QMessageBox::Information, tr("Go to definition"),
                                text, QMessageBox::Ok, parent);
    box->setTextFormat(Qt::PlainText);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
}

QString fileUri(const QString &path)
{
    return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
}

QString languageId(const QString &path)
{
    const auto suffix = QFileInfo(path).suffix();
    if (suffix == "C") return "cpp";
    const auto lower = suffix.toLower();
    if (lower == "rs") return "rust";
    if (lower == "c") return "c";
    if (QStringList{"h", "hh", "hpp", "hxx", "cc", "cpp", "cxx", "c++", "ipp", "inl"}.contains(lower))
        return "cpp";
    return {};
}

struct Location {
    QString path;
    int line = -1;
    int column = -1;
};

Location parseLocation(const QJsonObject &object)
{
    const bool isLink = object.contains("targetUri");
    const auto url = QUrl(object.value(isLink ? "targetUri" : "uri").toString(), QUrl::StrictMode);
    if (!url.isValid() || !url.isLocalFile() || !url.host().isEmpty() ||
        url.hasFragment() || url.hasQuery())
        return {};
    const auto range = object.value(isLink ? "targetSelectionRange" : "range").toObject();
    const auto start = range.value("start").toObject();
    Location location{url.toLocalFile(), start.value("line").toInt(-1),
                      start.value("character").toInt(-1)};
    if (location.line < 0 || location.column < 0 || !QFileInfo(location.path).isFile())
        return {};
    return location;
}

class DefinitionJob : public QObject {
public:
    DefinitionJob(QWidget *parent, QString root, QString path, QString source,
                  QString language, int line, int column,
                  std::function<void(QString, int, int)> onLocation)
        : QObject(parent), window(parent),
          folder{{"uri", fileUri(root)}, {"name", QDir(root).dirName()}},
          uri(fileUri(path)), source(std::move(source)), language(std::move(language)),
          position{{"line", line}, {"character", column}}, onLocation(std::move(onLocation))
    {
        setObjectName("interact.definition.job");
        process.setWorkingDirectory(root);
        progress = new QProgressDialog(tr("Finding definition…"), tr("Cancel"), 0, 0, window);
        progress->setWindowTitle(tr("Go to definition"));
        progress->setWindowModality(Qt::WindowModal);
        progress->setMinimumDuration(0);
        progress->setAutoReset(false);
        connect(progress, &QProgressDialog::canceled, this, [this] { complete({}); });
        connect(&process, &QProcess::started, this, [this] { initialize(); });
        connect(&process, &QProcess::readyReadStandardOutput, this, [this] { consume(); });
        connect(&process, &QProcess::readyReadStandardError, this, [this] {
            diagnostics = (diagnostics + process.readAllStandardError()).right(2048);
        });
        connect(&process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this] {
            consume();
            if (!settled) complete(tr("The language server exited before replying."));
            deleteLater();
        });
        connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (!settled && error != QProcess::Crashed)
                complete(tr("Language server error: %1").arg(process.errorString()));
        });
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, this, [this] {
            if (!settled) complete(tr("The language server took too long to respond."));
            else if (process.state() != QProcess::NotRunning) process.kill();
            else deleteLater();
        });
    }

    ~DefinitionJob() override
    {
        process.disconnect(this);
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(1000);
        }
        delete progress.data();
    }

    void start(const QString &executable)
    {
        const QStringList flags = language == "rust"
            ? QStringList{} : QStringList{"-j=2", "--background-index=false", "--log=error"};
        timer.start(requestTimeout);
        process.start(executable, flags);
        progress->show();
    }

private:
    void send(QJsonObject object)
    {
        object.insert("jsonrpc", "2.0");
        const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        process.write("Content-Length: " + QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes);
    }

    void initialize()
    {
        if (settled) return;
        const QJsonArray encodings{"utf-16"};
        const QJsonObject capabilities{
            {"general", QJsonObject{{"positionEncodings", encodings}}},
            {"offsetEncoding", encodings},
            {"workspace", QJsonObject{{"workspaceFolders", true}}},
            {"textDocument", QJsonObject{{"definition", QJsonObject{{"linkSupport", true}}}}}
        };
        QJsonObject params{
            {"processId", static_cast<double>(QCoreApplication::applicationPid())},
            {"rootUri", folder.value("uri")},
            {"workspaceFolders", QJsonArray{folder}},
            {"capabilities", capabilities}
        };
        if (language == "rust") {
            // One symbol lookup does not need to build the workspace.
            params.insert("initializationOptions", QJsonObject{
                {"cargo", QJsonObject{{"buildScripts", QJsonObject{{"enable", false}}}}},
                {"procMacro", QJsonObject{{"enable", false}}},
                {"checkOnSave", false}
            });
        }
        send({{"id", 1}, {"method", "initialize"}, {"params", params}});
    }

    void consume()
    {
        buffer += process.readAllStandardOutput();
        while (!buffer.isEmpty()) {
            const int separator = buffer.indexOf("\r\n\r\n");
            if (separator == -1 && buffer.size() <= headerLimit) return;
            int length = -1;
            if (separator >= 0 && separator <= headerLimit) {
                for (const auto &line : buffer.left(separator).split('\n')) {
                    const int colon = line.indexOf(':');
                    if (colon < 0 || line.left(colon).trimmed().toLower() != "content-length") continue;
                    bool valid = false;
                    length = line.mid(colon + 1).trimmed().toInt(&valid);
                    if (!valid) length = -1;
                    break;
                }
            }
            if (length <= 0 || length > messageLimit) {
                buffer.clear();
                complete(tr("The language server sent an invalid or oversized response."));
                process.kill();
                return;
            }
            const int contentStart = separator + 4;
            if (buffer.size() < contentStart + length) return;
            const auto document = QJsonDocument::fromJson(buffer.mid(contentStart, length));
            buffer.remove(0, contentStart + length);
            if (!document.isObject()) {
                complete(tr("The language server sent invalid JSON."));
                process.kill();
                return;
            }
            handle(document.object());
        }
    }

    void handle(const QJsonObject &message)
    {
        const auto id = message.value("id");
        if (id.isUndefined() || id.isNull()) return;
        // Both directions share the stream, and server requests can reuse IDs.
        if (message.contains("method")) {
            const auto method = message.value("method").toString();
            if (method == "workspace/workspaceFolders") {
                send({{"id", id}, {"result", QJsonArray{folder}}});
            } else if (method == "workspace/configuration") {
                QJsonArray values;
                const auto items = message.value("params").toObject().value("items").toArray();
                for (int i = 0; i < items.size(); ++i) values.append(QJsonValue::Null);
                send({{"id", id}, {"result", values}});
            } else if (method == "window/workDoneProgress/create") {
                send({{"id", id}, {"result", QJsonValue::Null}});
            } else {
                send({{"id", id}, {"error", QJsonObject{
                    {"code", -32601}, {"message", "Method not supported"}}}});
            }
            return;
        }

        if (id == QJsonValue(shutdownId) && settled) {
            send({{"method", "exit"}});
            process.closeWriteChannel();
            return;
        }
        if (settled) return;
        const auto error = message.value("error").toObject();
        const bool lookupReply = definitionPending && id == QJsonValue(lookupId);
        if (lookupReply) definitionPending = false;
        if (lookupReply && (error.value("code").toInt() == -32802 ||
                            error.value("code").toInt() == -32801) && retryRustLookup())
            return;
        if ((id == QJsonValue(1) || lookupReply) && !error.isEmpty()) {
            complete(tr("The language server reported: %1").arg(error.value("message").toString().left(512)));
            return;
        }
        if (id == QJsonValue(1) && !initialized) {
            const auto result = message.value("result").toObject();
            const auto capabilities = result.value("capabilities").toObject();
            if (!result.contains("capabilities")) {
                complete(tr("The language server returned no capabilities."));
                return;
            }
            initialized = true;
            send({{"method", "initialized"}, {"params", QJsonObject{}}});
            const auto encoding = capabilities.value("positionEncoding").toString(
                result.value("offsetEncoding").toString("utf-16"));
            if (encoding != "utf-16") {
                complete(tr("The language server did not accept UTF-16 positions."));
                return;
            }
            const auto support = capabilities.value("definitionProvider");
            if (!support.toBool() && !support.isObject()) {
                complete(tr("The language server does not support definition lookup."));
                return;
            }
            send({{"method", "textDocument/didOpen"}, {"params", QJsonObject{
                {"textDocument", QJsonObject{{"uri", uri}, {"languageId", language},
                                              {"version", 1}, {"text", source}}}}}});
            findDefinition();
        } else if (lookupReply) {
            const auto result = message.value("result");
            const auto candidates = result.isArray() ? result.toArray() : QJsonArray{result};
            for (const auto &candidate : candidates) {
                auto location = parseLocation(candidate.toObject());
                if (location.path.isEmpty()) continue;
                complete({});
                QTimer::singleShot(0, window, [callback = std::move(onLocation), location] {
                    callback(location.path, location.line, location.column);
                });
                return;
            }
            if (!retryRustLookup()) complete(tr("No local definition found at the cursor."));
        }
    }

    void findDefinition()
    {
        if (settled) return;
        definitionPending = true;
        send({{"id", ++lookupId}, {"method", "textDocument/definition"}, {"params", QJsonObject{
            {"textDocument", QJsonObject{{"uri", uri}}}, {"position", position}}}});
    }

    bool retryRustLookup()
    {
        // An on-demand Rust server may still be loading the project after initialize.
        if (language != "rust" || lookupId >= 5) return false;
        QTimer::singleShot(250 << (lookupId - 2), this, [this] { findDefinition(); });
        return true;
    }

    void complete(QString error)
    {
        if (settled) return;
        settled = true;
        progress->hide();
        timer.start(shutdownTimeout);
        if (process.state() == QProcess::NotRunning) deleteLater();
        else if (initialized) send({{"id", shutdownId}, {"method", "shutdown"}});
        else process.kill();
        if (!error.isEmpty()) {
            if (error.startsWith(tr("The language server exited")) && !diagnostics.trimmed().isEmpty())
                error += "\n\n" + QString::fromUtf8(diagnostics.trimmed());
            showMessage(window, error);
        }
    }

    QWidget *window;
    QPointer<QProgressDialog> progress;
    QJsonObject folder;
    QString uri, source, language;
    QJsonObject position;
    std::function<void(QString, int, int)> onLocation;
    QProcess process;
    QTimer timer;
    QByteArray buffer, diagnostics;
    int lookupId = 1;
    bool initialized = false;
    bool definitionPending = false;
    bool settled = false;
};
}

void goToDefinition(QWidget *window, QString root, QString path, QString source,
                    int line, int column, std::function<void(QString, int, int)> onLocation)
{
    if (!window || !onLocation || line < 0 || column < 0) return;
    if (window->findChild<QObject *>("interact.definition.job", Qt::FindDirectChildrenOnly)) return;
    const QFileInfo document(path);
    const auto language = languageId(path);
    if (!document.isFile() || language.isEmpty()) {
        showMessage(window, tr("Open a saved C, C++ or Rust source file to find a definition."));
        return;
    }
    if (source.toUtf8().size() > messageLimit) {
        showMessage(window, tr("Definition lookup supports documents up to 4 MiB."));
        return;
    }
    const QString server = language == "rust" ? "rust-analyzer" : "clangd";
    const QString executable = QStandardPaths::findExecutable(server);
    if (executable.isEmpty()) {
        showMessage(window, tr("Install %1 and make it available on PATH.").arg(server));
        return;
    }
    if (!QFileInfo(root).isDir()) root = document.absolutePath();
    auto *job = new DefinitionJob(window, QFileInfo(root).absoluteFilePath(), document.absoluteFilePath(),
                                  std::move(source), language, line, column, std::move(onLocation));
    job->start(executable);
}
