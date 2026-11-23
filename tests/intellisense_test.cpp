#include "intellisense.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace {

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QJsonObject readPacket()
{
    std::string line;
    int length = -1;
    while (std::getline(std::cin, line) && line != "\r") {
        if (line.rfind("Content-Length: ", 0) == 0)
            length = QByteArray::fromStdString(line.substr(16)).trimmed().toInt();
    }
    if (length <= 0 || length > 1000000) return {};
    QByteArray body(length, Qt::Uninitialized);
    if (!std::cin.read(body.data(), length)) return {};
    return QJsonDocument::fromJson(body).object();
}

void writePacket(QJsonObject packet)
{
    packet.insert("jsonrpc", "2.0");
    const QByteArray body = QJsonDocument(packet).toJson(QJsonDocument::Compact);
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n";
    std::cout.write(body.constData(), body.size());
    std::cout << std::flush;
}

QJsonObject params(const QJsonObject &packet)
{
    return packet.value("params").toObject();
}

QJsonObject span(int start, int end)
{
    return {{"start", QJsonObject{{"line", 0}, {"character", start}}},
            {"end", QJsonObject{{"line", 0}, {"character", end}}}};
}

void sendDiagnostics(const QJsonValue &uri, int version, const QString &label)
{
    writePacket({{"method", "textDocument/publishDiagnostics"},
                 {"params", QJsonObject{{"uri", uri}, {"version", version},
                     {"diagnostics", QJsonArray{QJsonObject{
                         {"message", label}, {"severity", 2}, {"range", span(2, 3)}}}}}}});
}

int serveFixture(bool rust)
{
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (!writeFile(".semantic-process", "started")) return 9;
    const auto initialize = readPacket();
    const auto initialization = params(initialize);
    const auto capabilities = initialization.value("capabilities").toObject();
    if (initialize.value("method") != QJsonValue("initialize") ||
        capabilities.value("general").toObject().value("positionEncodings") != QJsonValue(QJsonArray{"utf-16"})) return 10;
    if (rust) {
        const auto config = initialization.value("initializationOptions").toObject();
        if (qgetenv("CARGO_NET_OFFLINE") != "true" ||
            config.value("cargo").toObject().value("buildScripts").toObject().value("enable") != QJsonValue(false) ||
            config.value("procMacro").toObject().value("enable") != QJsonValue(false)) return 11;
    }
    writePacket({{"id", initialize.value("id")}, {"result", QJsonObject{{"capabilities",QJsonObject{
        {"completionProvider", QJsonObject{}}, {"signatureHelpProvider", QJsonObject{}},
        {"positionEncoding", "utf-16"}, {"textDocumentSync", 2}}}}}});
    if (readPacket().value("method") != QJsonValue("initialized")) return 12;
    const auto open = readPacket();
    const auto doc = params(open).value("textDocument").toObject();
    if (open.value("method") != QJsonValue("textDocument/didOpen") ||
        doc.value("text") != QJsonValue(QString::fromUtf8("😀me")) ||
        doc.value("languageId") != QJsonValue(rust ? "rust" : "cpp")) return 13;
    const auto uri = doc.value("uri");
    const int firstVersion = doc.value("version").toInt(-1);
    const auto first = readPacket();
    if (first.value("method") != QJsonValue("textDocument/completion") ||
        params(first).value("position").toObject().value("character") != QJsonValue(4)) return 14;
    sendDiagnostics(uri, firstVersion, "first warning");
    if (!writeFile(".semantic-opened", "ready")) return 15;

    const auto cancel = readPacket();
    if (cancel.value("method") != QJsonValue("$/cancelRequest") ||
        params(cancel).value("id") != first.value("id")) return 16;
    const auto change = readPacket();
    const auto delta = params(change).value("contentChanges").toArray();
    if (change.value("method") != QJsonValue("textDocument/didChange") ||
        delta.size() != 1 || delta.at(0).toObject().value("range") != QJsonValue(span(0, 4)) ||
        delta.at(0).toObject().value("text") != QJsonValue(QString::fromUtf8("😀mea"))) return 17;
    const int secondVersion = params(change).value("textDocument").toObject().value("version").toInt(-1);
    const auto second = readPacket();
    if (second.value("method") != QJsonValue("textDocument/completion") ||
        params(second).value("position").toObject().value("character") != QJsonValue(5)) return 18;

    // A server may still reply to a canceled request. The client must ignore it.
    sendDiagnostics(uri, firstVersion, "obsolete warning");
    writePacket({{"id", first.value("id")}, {"result", QJsonArray{
        QJsonObject{{"label", "obsolete"}}}}});
    sendDiagnostics(uri, secondVersion, "current warning");
    writePacket({{"id", second.value("id")}, {"result", QJsonObject{{"items", QJsonArray{
        QJsonObject{{"label", "measure(a: i32)"}, {"detail", "member"},
            {"textEdit", QJsonObject{{"range", span(2, 5)}, {"newText", "measure"}}}},
        QJsonObject{{"label", "unwanted snippet"}, {"insertTextFormat", 2}, {"insertText", "$1"}}
    }}}}});
    const auto signature = readPacket();
    if (signature.value("method") != QJsonValue("textDocument/signatureHelp")) return 19;
    writePacket({{"id",signature.value("id")},{"result",QJsonObject{
        {"signatures",QJsonArray{QJsonObject{{"label","measure(a: i32) -> i32"}}}}}}});
    if (readPacket().value("method") != QJsonValue("textDocument/didClose")) return 20;
    if (!writeFile(".semantic-closed", "closed")) return 21;
    readPacket(); // stay alive until the test disposes of the language service
    return 0;
}

bool until(const std::function<bool()> &ready, int limit = 4500)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < limit) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    return ready();
}

int failure(const QString &reason)
{
    qCritical().noquote() << reason;
    return 1;
}

int realClangd()
{
    if (QStandardPaths::findExecutable("clangd").isEmpty())
        return failure("clangd unavailable");
    QTemporaryDir workspace;
    const QString file = workspace.filePath("main.cpp");
    const QString source = "struct Widget { int measure(int m); };\n"
                           "int main() { Widget widget; widget.meas }\n";
    const int column = source.section('\n', 1, 1).indexOf("meas") + 4;
    if (!workspace.isValid() || !writeFile(file, source.toUtf8())) return failure("Could not seed C++");
    LanguageService service(workspace.path());
    service.onStatus = [](const QString &status) { qInfo().noquote() << status; };
    QList<SemanticCompletion> list;
    bool complete = false;
    bool sawDiagnostics = false;
    service.onDiagnostics = [&](QString path, int revision, const QList<SemanticDiagnostic> &results) {
        sawDiagnostics |= path == file && revision == 17 && !results.isEmpty();
    };
    service.complete({file, source, 17, 1, column}, [&](QList<SemanticCompletion> entries) {
        list = std::move(entries);
        complete = true;
    }, true);
    if (!until([&] { return complete && sawDiagnostics; }, 14000))
        return failure("clangd did not send both completion and diagnostics");
    for (const auto &entry : list) {
        if (entry.label.contains("measure") && entry.insertText == "measure") {
            qInfo() << "PASS: real clangd member completion and diagnostics";
            return 0;
        }
    }
    return failure("clangd member completion missing");
}

int realRustAnalyzer()
{
    if (QStandardPaths::findExecutable("rust-analyzer").isEmpty())
        return failure("rust-analyzer unavailable");
    QTemporaryDir workspace;
    const QString file = workspace.filePath("code.rs");
    const QString source = "struct Widget { measure: i32 }\n"
                           "fn main() { let widget = Widget { measure: 42 }; widget.mea; }\n";
    const int column = source.section('\n', 1, 1).lastIndexOf(".mea") + 4;
    const QByteArray project = R"({"crates":[{"root_module":"code.rs","edition":"2021","deps":[]}]})";
    if (!workspace.isValid() || !writeFile(file, source.toUtf8()) ||
        !writeFile(workspace.filePath("rust-project.json"), project))
        return failure("Could not seed Rust project");
    LanguageService service(workspace.path());
    service.onStatus = [](const QString &status) { qInfo().noquote() << status; };
    bool found = false;
    bool answered = false;
    for (int attempt = 0; attempt < 8 && !found; ++attempt) {
        answered = false;
        service.complete({file, source, 17, 1, column}, [&](const QList<SemanticCompletion> &entries) {
            for (const auto &entry : entries) found |= entry.insertText == "measure";
            answered = true;
        }, true);
        if (!until([&] { return answered; }, 1600)) continue;
        if (!found) QThread::msleep(350); // allow the initial workspace load to complete
    }
    if (!found) return failure("Rust member completion missing");
    qInfo() << "PASS: real rust-analyzer member completion";
    return 0;
}

bool testFixture(const QString &root, bool rust)
{
    QDir directory(root);
    const auto marker = directory.filePath(".semantic-opened");
    const auto closed = directory.filePath(".semantic-closed");
    const auto started = directory.filePath(".semantic-process");
    const auto path = directory.filePath(rust ? "source.rs" : "source.cpp");
    if (!writeFile(path, "// test\n")) return false;
    LanguageService service(root);
    service.onStatus = [](const QString &status) { qInfo().noquote() << status; };
    QStringList warnings;
    QList<int> revisions;
    service.onDiagnostics = [&](QString located, int revision, QList<SemanticDiagnostic> diagnostics) {
        revisions << revision;
        if (located == path && !diagnostics.isEmpty() && diagnostics.first().severity == 2)
            warnings << diagnostics.first().message;
    };

    const LanguageSnapshot first{path, QString::fromUtf8("😀me"), 17, 0, 4};
    service.update(first);
    if (until([&] { return QFileInfo::exists(started); }, 120)) return false;

    bool oldCompletion = false;
    service.complete(first, [&](const QList<SemanticCompletion> &) { oldCompletion = true; });
    if (!until([&] { return warnings.size() == 1 && QFileInfo::exists(marker); })) return false;
    const LanguageSnapshot second{path, QString::fromUtf8("😀mea"), 31, 0, 5};
    QList<SemanticCompletion> result;
    service.update(second);
    service.complete(second, [&](QList<SemanticCompletion> entries) { result = std::move(entries); });
    if (!until([&] { return !result.isEmpty(); })) return false;
    if (oldCompletion || result.size() != 1 || result.first().insertText != "measure" ||
        result.first().startColumn != 2 || result.first().endColumn != 5 ||
        warnings != QStringList{"first warning", "current warning"} || revisions != QList<int>{17, 31}) return false;

    QString help;
    service.signature(second, [&](QString text) { help = text; });
    if (!until([&] { return !help.isEmpty(); }) || help != "measure(a: i32) -> i32") return false;
    service.closeDocument(path);
    return until([&] { return QFileInfo::exists(closed); });
}

} // namespace

int main(int argc, char **argv)
{
    const QString executable = QFileInfo(QString::fromLocal8Bit(argv[0])).baseName();
    if (executable == "clangd" || executable == "rust-analyzer") return serveFixture(executable == "rust-analyzer");
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsSet("INTERACT_REAL_CLANGD")) return realClangd();
    if (qEnvironmentVariableIsSet("INTERACT_REAL_RUST_ANALYZER")) return realRustAnalyzer();
    QTemporaryDir temp;
    if (!temp.isValid()) return failure("Could not create fixture directory");
    for (const auto &tool : {QString("clangd"), QString("rust-analyzer")}) {
        QString name = tool;
#ifdef Q_OS_WIN
        name += ".exe";
#endif
        if (!QFile::copy(app.applicationFilePath(), temp.filePath(name)))
            return failure("Could not prepare fixture binary for " + tool);
    }
    qputenv("PATH", QFile::encodeName(QDir::toNativeSeparators(temp.path())) +
                    QDir::listSeparator().toLatin1() + qgetenv("PATH"));
    for (const QString &language : {QString("C++"), QString("Rust")}) {
        const bool rust = language == "Rust";
        const QString root = temp.filePath(language + " space #1");
        if (!QDir().mkpath(root) || !testFixture(QFileInfo(root).canonicalFilePath(), rust))
            return failure("Language fixture failed for " + language);
    }
    qInfo() << "PASS: lazy C++/Rust completion, UTF-16 changes, diagnostics versions and signature help";
    return 0;
}
