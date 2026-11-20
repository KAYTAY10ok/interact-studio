#include "definition.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>

namespace {
bool putFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QJsonObject readMessage()
{
    std::string line;
    int length = -1;
    while (std::getline(std::cin, line) && line != "\r") {
        if (line.rfind("Content-Length: ", 0) == 0)
            length = std::stoi(line.substr(16));
    }
    if (length <= 0 || length > 65536) return {};
    QByteArray body(length, Qt::Uninitialized);
    if (!std::cin.read(body.data(), length)) return {};
    return QJsonDocument::fromJson(body).object();
}

void writeMessage(QJsonObject message)
{
    message.insert("jsonrpc", "2.0");
    const QByteArray body = QJsonDocument(message).toJson(QJsonDocument::Compact);
    std::cout << "Content-Length: " << body.size() << "\r\n" << std::flush;
    std::cout << "\r\n";
    std::cout.write(body.constData(), body.size());
    std::cout << std::flush;
}

int fixture(bool rust)
{
    const auto initialize = readMessage();
    auto params = initialize.value("params").toObject();
    const auto capabilities = params.value("capabilities").toObject();
    const auto encodings = capabilities.value("general").toObject().value("positionEncodings").toArray();
    if (initialize.value("method") != QJsonValue("initialize") ||
        encodings != QJsonArray{"utf-16"}) return 2;
    writeMessage({{"id", initialize.value("id")}, {"result", QJsonObject{
        {"capabilities", QJsonObject{{"definitionProvider", true}, {"positionEncoding", "utf-16"}}}}}});
    if (readMessage().value("method") != QJsonValue("initialized")) return 3;
    const auto open = readMessage();
    const auto document = open.value("params").toObject().value("textDocument").toObject();
    if (open.value("method") != QJsonValue("textDocument/didOpen") ||
        document.value("languageId") != QJsonValue(rust ? "rust" : "cpp") ||
        !document.value("text").toString().contains(QString::fromUtf8("é😀"))) return 4;
    auto definition = readMessage();
    const auto position = definition.value("params").toObject().value("position").toObject();
    if (definition.value("method") != QJsonValue("textDocument/definition") ||
        position.value("line").toInt(-1) != 1 || position.value("character").toInt(-1) != 7) return 5;

    // Interleave a server request with an overlapping ID and an ignored notification.
    writeMessage({{"id", 2}, {"method", "workspace/workspaceFolders"}});
    const auto folders = readMessage();
    if (folders.value("id") != QJsonValue(2) || folders.value("result").toArray().size() != 1)
        return 6;
    writeMessage({{"method", "textDocument/publishDiagnostics"}, {"params", QJsonObject{}}});
    if (rust) {
        writeMessage({{"id", definition.value("id")}, {"result", QJsonValue::Null}});
        definition = readMessage();
        if (definition.value("method") != QJsonValue("textDocument/definition") ||
            definition.value("id") != QJsonValue(3)) return 10;
    }

    const QJsonObject range{{"start", QJsonObject{{"line", 0}, {"character", 4}}},
                            {"end", QJsonObject{{"line", 0}, {"character", 11}}}};
    const auto uri = document.value("uri");
    const QJsonObject link{{"targetUri", uri}, {"targetSelectionRange", range}};
    const QJsonObject location{{"uri", uri}, {"range", range}};
    const QJsonValue result = rust ? QJsonValue(location) :
        QJsonValue(QJsonArray{QJsonObject{{"targetUri", "https://example.invalid/definition"}}, link});
    writeMessage({{"id", definition.value("id")}, {"result", result}});

    const auto shutdown = readMessage();
    if (shutdown.value("method") != QJsonValue("shutdown")) return 7;
    writeMessage({{"id", shutdown.value("id")}, {"result", QJsonValue::Null}});
    if (readMessage().value("method") != QJsonValue("exit")) return 8;
    return putFile(".definition-exited", "ok\n") ? 0 : 9;
}

bool lookup(QWidget &window, const QString &root, const QString &path,
            const QString &source, int column, int expectedColumn, bool real)
{
    QFile::remove(QDir(root).filePath(".definition-exited"));
    bool found = false;
    bool matched = false;
    goToDefinition(&window, root, path, source, 1, column,
                   [&](const QString &located, int line, int actualColumn) {
        found = true;
        matched = located == path && line == 0 && actualColumn == expectedColumn;
    });
    QEventLoop loop;
    QTimer poll;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (!window.findChild<QObject *>("interact.definition.job")) loop.quit();
    });
    poll.start(10);
    timeout.start(22000);
    loop.exec();
    if (!found) {
        for (auto *box : window.findChildren<QMessageBox *>()) qCritical().noquote() << box->text();
    }
    return found && matched && (real || QFileInfo::exists(QDir(root).filePath(".definition-exited")));
}
}

int main(int argc, char **argv)
{
    const QString invoked = QFileInfo(QString::fromLocal8Bit(argv[0])).fileName();
    if (invoked == "clangd" || invoked == "rust-analyzer") return fixture(invoked == "rust-analyzer");

    QApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 1;
    const QString root = temporary.filePath("test élève space #1");
    if (!QDir().mkpath(root)) return 1;

    const auto testMode = qgetenv("INTERACT_LSP_TEST_REAL");
    const bool real = !testMode.isEmpty();
    if (!real) {
        for (const auto &server : {"clangd", "rust-analyzer"}) {
            if (!QFile::link(app.applicationFilePath(), temporary.filePath(server))) return 1;
        }
        qputenv("PATH", temporary.path().toUtf8() + ":" + qgetenv("PATH"));
    }

    QWidget window;
    if (testMode == "rust") {
        const QString path = root + "/code.rs";
        const QString source = "fn resolve() -> i32 { 42 }\nfn main() { resolve(); }\n";
        const QByteArray project = R"({"crates":[{"root_module":"code.rs","edition":"2021","deps":[]}]})";
        if (!putFile(root + "/rust-project.json", project) || !putFile(path, source.toUtf8()) ||
            !lookup(window, root, path, source, 12, 3, true)) {
            qCritical() << "Real rust-analyzer definition lookup failed.";
            return 1;
        }
        qInfo() << "PASS: real rust-analyzer go-to-definition";
        return 0;
    }

    const QString cpp = root + "/code ñ.cpp";
    const QString source = real ? "int resolve() { return 42; }\nint main() { return resolve(); }\n"
                                : QString::fromUtf8("int resolve() { return 42; }\n/*é😀*/ int main() { return resolve(); }\n");
    if (!putFile(cpp, source.toUtf8()) || !lookup(window, root, cpp, source, real ? 20 : 7, 4, real)) {
        qCritical() << "C++ definition lookup failed.";
        return 1;
    }
    if (!real) {
        const QString rust = root + "/code.rs";
        const auto rustSource = QString::fromUtf8("pub fn resolve() -> i32 { 42 }\n/*é😀*/ fn main() { resolve(); }\n");
        if (!putFile(rust, rustSource.toUtf8()) || !lookup(window, root, rust, rustSource, 7, 4, false)) {
            qCritical() << "Rust definition lookup failed.";
            return 1;
        }
    }
    qInfo() << (real ? "PASS: real clangd go-to-definition" :
                "PASS: C++ and Rust LSP handshake, UTF-16, local target and shutdown");
    return 0;
}
