#include "customize.h"

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QTest>

namespace {
int fail(const char *message) {
    qCritical("%s", message);
    return 1;
}
}

int main(int argc, char **argv) {
    QTemporaryDir home;
    if (!home.isValid()) return fail("Could not create temporary configuration");
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    QApplication app(argc, argv);
    QWidget window;
    QLabel brand("Interact Studio", &window);
    brand.setObjectName("brand");
    QPlainTextEdit editor(&window);
    QAction save("Save", &window);
    bool saved = false;
    QObject::connect(&save, &QAction::triggered, [&]{ saved = true; });
    Customize custom(&window);
    custom.bind("file.save", &save, "Ctrl+S");
    QFont font;
    custom.onAppearance = [&](QFont selected, QColor) {
        font = selected;
        window.setStyleSheet(custom.userStyleSheet());
    };
    custom.reload();
    window.show();
    app.processEvents();
    const QString css = "QLabel#brand { color: #74d4bd; }";
    QTimer::singleShot(0, [&]{
        auto *dialog = qobject_cast<QDialog *>(app.activeModalWidget());
        if (!dialog) qFatal("Preferences dialog missing");
        for (auto *combo : dialog->findChildren<QComboBox *>())
            if (combo->findText("Emacs") >= 0) combo->setCurrentText("Emacs");
        dialog->findChild<QSpinBox *>()->setValue(14);
        dialog->findChild<QPlainTextEdit *>("styleEditor")->insertPlainText(css);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
    });
    custom.configure();
    if (font.pointSize() != 14 || save.shortcut() != QKeySequence("Ctrl+X, Ctrl+S"))
        return fail("Saved keymap or font did not reload");
    if (custom.userStyleSheet() != css ||
        brand.palette().color(QPalette::WindowText) != QColor("#74d4bd"))
        return fail("Personal stylesheet did not change the requested widget");
    window.activateWindow();
    editor.setFocus();
    app.processEvents();
    editor.setPlainText("keep this selection");
    editor.selectAll();
    QTest::keyClick(&editor, Qt::Key_X, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_S, Qt::ControlModifier);
    if (!saved || editor.toPlainText() != "keep this selection")
        return fail("Emacs save chord cut the selection or failed to save");
    qInfo("PASS: preferences persist, user stylesheet applies, Emacs chord preserves selection");
}
