#include "buildrun.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QVBoxLayout>

#include <csignal>
#include <unistd.h>

namespace {

// Compilers may spawn an assembler or linker. Isolate the whole build so
// cancelling it also stops those children.
class CompilerProcess final : public QProcess {
public:
    explicit CompilerProcess(QObject *parent) : QProcess(parent) {}
    void cancel()
    {
        if (state() == NotRunning) return;
        const auto pid = processId();
        if (pid > 0) ::kill(-static_cast<pid_t>(pid), SIGKILL);
        kill();
    }

protected:
    void setupChildProcess() override
    {
        if (::setsid() == -1) ::_exit(125);
    }
};

struct Artifact final : QObject {
    explicit Artifact(QObject *parent)
        : QObject(parent), dir(QDir::tempPath() + "/interact-build-XXXXXX") {}
    QTemporaryDir dir;
};

class BuildDialog final : public QDialog {
public:
    BuildDialog(QWidget *parent, QString workspace, QString path,
                std::function<void(QString, QStringList)> launch)
        : QDialog(parent), process(new CompilerProcess(this)), artifact(new Artifact(this)),
          status(new QLabel(this)), progress(new QProgressBar(this)),
          output(new QPlainTextEdit(this)), button(new QPushButton(tr("Cancel"), this))
    {
        setObjectName("buildRunDialog");
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(tr("Build and run"));
        resize(580, 290);
        auto *layout = new QVBoxLayout(this);
        status->setTextFormat(Qt::PlainText);
        status->setObjectName("buildStatus");
        progress->setRange(0, 0);
        output->setObjectName("buildOutput");
        output->setReadOnly(true);
        output->setMaximumBlockCount(2000);
        layout->addWidget(status);
        layout->addWidget(progress);
        layout->addWidget(output);
        layout->addWidget(button, 0, Qt::AlignRight);
        connect(button, &QPushButton::clicked, this, &BuildDialog::reject);

        QFileInfo source(path);
        const QString suffix = source.suffix().toLower();
        const QStringList cppSuffixes{"cpp", "cc", "cxx", "c++"};
        QString compiler;
        QStringList args;
        if (suffix == "rs") {
            compiler = QStandardPaths::findExecutable("rustc");
            args = QStringList{"--edition=2021", "--crate-name", "interact_run"};
        } else if (cppSuffixes.contains(suffix) || source.suffix() == "C") {
            for (const auto &name : {"c++", "g++", "clang++"}) {
                compiler = QStandardPaths::findExecutable(name);
                if (!compiler.isEmpty()) break;
            }
            args = QStringList{"-std=c++17", "-g"};
        } else {
            finish(tr("Choose a C++ or Rust source file to build."));
            return;
        }
        if (compiler.isEmpty()) {
            finish(suffix == "rs" ? tr("Install rustc and add it to PATH to build Rust files.")
                                  : tr("Install a C++ compiler and add it to PATH to build C++ files."));
            return;
        }
        if (!source.isFile() || !QFileInfo(workspace).isDir()) {
            finish(tr("The source file or workspace directory is unavailable."));
            return;
        }
        if (!artifact->dir.isValid()) {
            finish(tr("Cannot create a temporary build directory."));
            return;
        }

        const QString binary = artifact->dir.filePath("program");
        args << source.absoluteFilePath() << "-o" << binary;
        status->setText(tr("Compiling %1 with %2").arg(source.fileName(), QFileInfo(compiler).fileName()));
        connect(process, &QProcess::readyRead, this, &BuildDialog::readOutput);
        connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                finish(tr("Compiler could not start: %1").arg(process->errorString()));
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this, binary, launch](int code, QProcess::ExitStatus exitStatus) {
            if (cancelled) return;
            readOutput();
            if (exitStatus == QProcess::CrashExit || code != 0 || !QFileInfo(binary).isExecutable()) {
                finish(tr("Build failed (compiler exit %1)").arg(code));
                return;
            }
            // Terminal sessions and their child processes can outlive this dialog.
            artifact->setParent(parentWidget() ? static_cast<QObject *>(parentWidget()->window()) : qApp);
            accept();
            launch(binary, {});
        });
        process->setProcessChannelMode(QProcess::MergedChannels);
        process->setWorkingDirectory(workspace);
        process->start(compiler, args, QIODevice::ReadOnly);
    }

    ~BuildDialog() override
    {
        cancelled = true;
        process->cancel();
    }

    void reject() override
    {
        cancelled = true;
        process->cancel();
        QDialog::reject();
    }

private:
    void finish(const QString &message)
    {
        status->setText(message);
        progress->hide();
        button->setText(tr("Close"));
    }

    void readOutput()
    {
        const auto bytes = process->readAll();
        if (bytes.isEmpty()) return;
        constexpr int limit = 256 * 1024;
        auto cursor = output->textCursor();
        if (output->document()->characterCount() > limit) {
            cursor.setPosition(0);
            cursor.setPosition(limit / 2, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
        }
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(QString::fromLocal8Bit(bytes.right(limit)));
        output->setTextCursor(cursor);
    }

    CompilerProcess *process;
    Artifact *artifact;
    QLabel *status;
    QProgressBar *progress;
    QPlainTextEdit *output;
    QPushButton *button;
    bool cancelled = false;
};
}

void buildAndRun(QWidget *parent, QString workspace, QString path,
                 std::function<void(QString, QStringList)> launch)
{
    auto *dialog = new BuildDialog(parent, std::move(workspace), std::move(path), std::move(launch));
    dialog->show();
}
