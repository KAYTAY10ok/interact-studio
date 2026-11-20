#include "editor.h"

#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cstdlib>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTimer::singleShot(30000, [] {
        qCritical() << "Benchmark timed out";
        std::_Exit(2);
    });
    QTemporaryDir dir;
    if (!dir.isValid()) return 1;
    QFile file(dir.filePath("sample.cpp"));
    if (!file.open(QIODevice::WriteOnly)) return 1;
    for (int i = 0; i < 5000; ++i) {
        const QByteArray line = "constexpr int result_" + QByteArray::number(i) + " = compute(3); // worker\n";
        if (file.write(line) != line.size()) return 1;
    }
    if (!file.flush()) return 1;
    file.close();

    Editor editor;
    editor.resize(1200, 780);
    editor.show();
    if (!editor.openFile(file.fileName())) return 1;
    auto *view = editor.currentText();
    view->moveCursor(QTextCursor::End);
    app.processEvents();
    const int initialLength = view->document()->characterCount();
    constexpr int samples = 500;
    QVector<qint64> elapsed;
    elapsed.reserve(samples);
    QElapsedTimer timer;
    for (int i = 0; i < samples; ++i) {
        QKeyEvent key(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, "x");
        timer.start();
        QApplication::sendEvent(view, &key);
        app.processEvents();
        elapsed.push_back(timer.nsecsElapsed());
    }
    if (view->document()->characterCount() != initialLength + samples) return 1;
    std::sort(elapsed.begin(), elapsed.end());
    const double medianMs = (elapsed[249] + elapsed[250]) / 2e6;
    const double p95Ms = elapsed[474] / 1e6;
    qInfo().nospace() << "Qt " << qVersion() << " (" << QApplication::platformName()
        << "), editor 1200x780, 5000 C++ lines, 500 synthetic keypresses; "
        << "dispatch and pending event processing: median " << medianMs << " ms, p95 " << p95Ms << " ms.";
    return 0;
}
