#include "quickopen.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QProcess>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int MaxFiles = 50000;
constexpr int MaxResults = 80;

// Reward consecutive letters and the start of a path component.
int matchScore(const QString &path, const QString &pattern) {
    if (pattern.isEmpty()) return 0;
    int cursor = 0, previous = -2, score = 1;
    const int basename = path.lastIndexOf('/') + 1;
    for (QChar letter : pattern) {
        const int pos = path.indexOf(letter, cursor, Qt::CaseInsensitive);
        if (pos == -1) return -1;
        if (pos == previous + 1) score += 9;
        if (pos == 0 || path[pos-1] == '/' || path[pos-1] == '_' || path[pos-1] == '-') score += 12;
        if (pos >= basename) score += 4;
        previous = pos;
        cursor = pos + 1;
    }
    if (path.midRef(basename, pattern.size()).compare(pattern, Qt::CaseInsensitive) == 0) score += 30;
    return score;
}

class FilePicker final : public QDialog {
public:
    FilePicker(QWidget *parent, QString root, std::function<void(QString)> open)
        : QDialog(parent), root_(QDir(root).absolutePath()), open_(std::move(open)) {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(tr("Open file in project"));
        resize(590, 410);
        auto *layout = new QVBoxLayout(this);
        input_ = new QLineEdit;
        input_->setAccessibleName(tr("File name"));
        input_->setPlaceholderText(tr("Search by file name or path…"));
        results_ = new QListWidget;
        results_->setUniformItemSizes(true);
        results_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        summary_ = new QLabel;
        summary_->setTextFormat(Qt::PlainText);
        layout->addWidget(input_);
        layout->addWidget(results_, 1);
        layout->addWidget(summary_);
        input_->installEventFilter(this);
        setFocusProxy(input_);
        input_->setFocus();
        connect(input_, &QLineEdit::textChanged, this, [this] {
            scanned_ = 0;
            count_ = 0;
            best_.clear();
            results_->clear();
            work_.start(0);
        });
        connect(input_, &QLineEdit::returnPressed, this, [this] { choose(); });
        connect(results_, &QListWidget::itemActivated, this, [this] { choose(); });
        work_.setSingleShot(true);
        refresh_.setSingleShot(true);
        connect(&work_, &QTimer::timeout, this, [this] { advance(); });
        connect(&refresh_, &QTimer::timeout, this, [this] { render(); });

        process_ = new QProcess(this);
        process_->setWorkingDirectory(root_);
        connect(process_, &QProcess::readyReadStandardOutput, this, [this] { work_.start(0); });
        connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int status, QProcess::ExitStatus exit) {
            if (!limited_ && (exit != QProcess::NormalExit || status > 1)) {
                error_ = QString::fromLocal8Bit(process_->readAllStandardError()).trimmed().left(220);
                if (error_.isEmpty()) error_ = tr("Could not list project files");
            }
            work_.start(0);
        });
        connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                error_ = tr("Install ripgrep (rg) to use quick open.");
                work_.start(0);
            }
        });
        process_->start("rg", {"--no-config", "--files", "--hidden", "--glob", "!.git",
                               "--null", "--color", "never"});
        summary_->setText(tr("Reading project files…"));
    }

    ~FilePicker() override {
        if (process_->state() != QProcess::NotRunning) {
            // Reap asynchronously so dismissing the picker never waits on rg.
            process_->disconnect(this);
            process_->setParent(qApp);
            connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                    process_, &QObject::deleteLater);
            process_->kill();
        }
    }

protected:
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (obj == input_ && event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            int delta = 0;
            if (key == Qt::Key_Down) delta = 1;
            else if (key == Qt::Key_Up) delta = -1;
            else if (key == Qt::Key_PageDown) delta = 10;
            else if (key == Qt::Key_PageUp) delta = -10;
            if (delta && results_->count()) {
                results_->setCurrentRow(qBound(0, results_->currentRow() + delta, results_->count() - 1));
                return true;
            }
        }
        return QDialog::eventFilter(obj, event);
    }

private:
    void advance() {
        QElapsedTimer timer;
        timer.start();
        // Keep enumeration and ranking incremental even if rg produces its entire listing at once.
        if (process_->bytesAvailable() && !limited_) {
            partial_ += process_->read(32768);
            int offset = 0;
            while (true) {
                const int end = partial_.indexOf('\0', offset);
                if (end == -1) break;
                files_.append(QFile::decodeName(partial_.mid(offset, end-offset)));
                offset = end + 1;
                if (files_.size() == MaxFiles) {
                    limited_ = true;
                    process_->kill();
                    break;
                }
            }
            partial_.remove(0, offset);
        }
        const QString pattern = input_->text();
        while (scanned_ < files_.size() && timer.elapsed() < 4) {
            const QString &path = files_.at(scanned_++);
            const int score = matchScore(path, pattern);
            if (score < 0) continue;
            ++count_;
            const auto key = qMakePair(-score, path);
            if (best_.size() == MaxResults && !(key < best_.lastKey())) continue;
            best_.insert(key, true);
            if (best_.size() > MaxResults) best_.erase(std::prev(best_.end()));
        }
        if (scanned_ < files_.size() || (!limited_ && process_->bytesAvailable())) work_.start(0);
        if (!refresh_.isActive()) refresh_.start(35);
    }

    void render() {
        const QString selected = results_->currentItem() ? results_->currentItem()->text() : QString();
        results_->clear();
        for (auto it = best_.cbegin(); it != best_.cend(); ++it) {
            auto *item = new QListWidgetItem(it.key().second, results_);
            item->setToolTip(it.key().second);
            if (it.key().second == selected) results_->setCurrentItem(item);
        }
        if (results_->currentRow() < 0 && results_->count()) results_->setCurrentRow(0);
        QString status;
        if (!error_.isEmpty()) status = error_;
        else {
            status = tr("%1 matches · %2 shown").arg(count_).arg(best_.size());
            if (limited_) status += tr(" · first %1 files indexed").arg(MaxFiles);
            else if (process_->state() != QProcess::NotRunning || work_.isActive()) status += tr(" · scanning…");
        }
        summary_->setText(status);
    }

    void choose() {
        if (!results_->currentItem()) return;
        const QString path = QDir(root_).absoluteFilePath(results_->currentItem()->text());
        auto open = open_;
        accept();
        open(path);
    }

    QString root_, error_;
    std::function<void(QString)> open_;
    QLineEdit *input_;
    QListWidget *results_;
    QLabel *summary_;
    QProcess *process_;
    QTimer work_, refresh_;
    QByteArray partial_;
    QStringList files_;
    QMap<QPair<int,QString>, bool> best_;
    int scanned_ = 0, count_ = 0;
    bool limited_ = false;
};
}

void quickOpenFile(QWidget *parent, QString root, std::function<void(QString)> open) {
    auto *picker = new FilePicker(parent, std::move(root), std::move(open));
    picker->open();
}
