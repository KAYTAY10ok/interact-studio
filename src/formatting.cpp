#include "formatting.h"
#include "languages.h"

#include <QDir>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTimer>
#include <memory>

namespace {

struct Formatter {
    QString tool;
    QStringList args;
};

class FormatProcess final : public QProcess {
public:
    using QProcess::QProcess;
    ~FormatProcess() override
    {
        disconnect();
        if (state() != NotRunning) {
            kill();
            waitForFinished(1000);
        }
    }
};

Formatter formatterFor(const QString &path)
{
    const auto language = editorLanguage(path);
    if (language == "C" || language == "C++" || language == "Objective-C")
        return {"clang-format", {"--assume-filename=" + path}};
    if (language == "Rust") return {"rustfmt", {"--emit", "stdout", "--edition", "2021"}};
    if (language == "Go") return {"gofmt", {}};
    if (language == "Python") return {"black", {"--quiet", "--stdin-filename", path, "-"}};
    if (language == "Shell") return {"shfmt", {"-filename", path}};
    const auto ext = QFileInfo(path).suffix().toLower();
    if (QStringList{"js", "cjs", "mjs", "jsx", "ts", "cts", "mts", "tsx", "html", "htm",
                    "css", "scss", "less", "json", "jsonc", "yaml", "yml", "md", "vue", "svelte"}.contains(ext))
        return {"prettier", {"--stdin-filepath", path}};
    return {};
}

struct Position {
    int line;
    int column;
};

int offset(QTextDocument *document, Position point)
{
    const auto block = document->findBlockByNumber(point.line);
    return block.isValid() ? block.position() + qBound(0, point.column, block.length()-1) :
                             document->characterCount()-1;
}

}

void formatDocument(QWidget *owner, QPlainTextEdit *view, const QString &path,
                    std::function<void(QString)> report)
{
    auto say = [&](QString text){ if (report) report(text); };
    if (!view || path.isEmpty()) {
        say(QObject::tr("Save the file with a supported extension before formatting."));
        return;
    }
    const auto formatter = formatterFor(path);
    if (formatter.tool.isEmpty()) {
        say(QObject::tr("No formatter is configured for this file type."));
        return;
    }
    const auto executable = QStandardPaths::findExecutable(formatter.tool);
    if (executable.isEmpty()) {
        say(QObject::tr("Install %1 and add it to PATH to format this file.").arg(formatter.tool));
        return;
    }
    constexpr int outputLimit = 24*1024*1024;
    auto *process = new FormatProcess(owner);
    auto *deadline = new QTimer(process);
    deadline->setSingleShot(true);
    QPointer<QPlainTextEdit> editor(view);
    QPointer<QTextDocument> document(view->document());
    const auto revision = document->revision();
    const auto input = std::make_shared<QByteArray>(view->toPlainText().toUtf8());
    auto output = std::make_shared<QByteArray>();
    auto diagnostics = std::make_shared<QByteArray>();
    auto failure = std::make_shared<QString>();
    auto collect = [process,output,diagnostics,failure] {
        const auto chunk = process->readAllStandardOutput();
        if (chunk.size() > outputLimit-output->size()) {
            *failure = QObject::tr("Formatter output exceeded 24 MiB.");
            process->kill();
        } else output->append(chunk);
        const auto errors = process->readAllStandardError();
        diagnostics->append(errors.left(qMax(0,4096-diagnostics->size())));
    };
    QObject::connect(process,&QProcess::readyReadStandardOutput,process,collect);
    QObject::connect(process,&QProcess::readyReadStandardError,process,collect);
    QObject::connect(deadline,&QTimer::timeout,process,[process,failure] {
        *failure = QObject::tr("Formatting timed out.");
        process->kill();
    });
    QObject::connect(process,&QProcess::started,process,[process,input] {
        process->write(*input);
        process->closeWriteChannel();
    });
    QObject::connect(process,&QProcess::errorOccurred,process,[process,report](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        if (report) report(QObject::tr("Could not start formatter: %1").arg(process->errorString()));
        process->deleteLater();
    });
    QObject::connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),process,
        [=](int code, QProcess::ExitStatus status) {
            deadline->stop();
            collect();
            process->deleteLater();
            auto finish = [&](QString message){ if (report) report(message); };
            if (!failure->isEmpty()) { finish(*failure); return; }
            if (status != QProcess::NormalExit || code != 0) {
                auto detail = QString::fromUtf8(*diagnostics).simplified().left(450);
                if (detail.isEmpty()) detail = QObject::tr("exit status %1").arg(code);
                finish(QObject::tr("Formatting failed: %1").arg(detail));
                return;
            }
            if (!editor || !document || editor->document()!=document || document->revision()!=revision) {
                finish(QObject::tr("Formatting skipped because the document changed."));
                return;
            }
            if (*output == *input) { finish(QObject::tr("Already formatted.")); return; }
            const auto text = QString::fromUtf8(*output);
            if (text.toUtf8() != *output || text.contains(QChar::Null) ||
                    (text.isEmpty() && !input->trimmed().isEmpty())) {
                finish(QObject::tr("Formatter returned invalid or empty text."));
                return;
            }

            QTextCursor cursor = editor->textCursor();
            const Position caret{cursor.blockNumber(),cursor.positionInBlock()};
            QTextCursor anchorCursor(document);
            anchorCursor.setPosition(cursor.anchor());
            const Position anchor{anchorCursor.blockNumber(),anchorCursor.positionInBlock()};
            cursor.beginEditBlock();
            cursor.select(QTextCursor::Document);
            cursor.insertText(text);
            cursor.endEditBlock();
            cursor.setPosition(offset(document,anchor));
            cursor.setPosition(offset(document,caret),QTextCursor::KeepAnchor);
            editor->setTextCursor(cursor);
            finish(QObject::tr("Formatted with %1.").arg(formatter.tool));
        });
    process->setWorkingDirectory(QFileInfo(path).absolutePath());
    process->start(executable,formatter.args);
    deadline->start(10000);
    say(QObject::tr("Formatting with %1…").arg(formatter.tool));
}
