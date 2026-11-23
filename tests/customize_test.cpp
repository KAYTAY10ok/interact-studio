#include "customize.h"

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QTest>

namespace {
int fail(const char *message) {
    qCritical("%s", message);
    return 1;
}

QByteArray read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

bool write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

bool write(const QString &path, const QJsonObject &settings) {
    return write(path, QJsonDocument(settings).toJson());
}
}

int main(int argc, char **argv) {
    QTemporaryDir home;
    if (!home.isValid()) return fail("Could not create temporary configuration");
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    QApplication app(argc, argv);
    const QString config = home.filePath("interact-studio");
    const QString legacyPath = config + "/settings.ini";
    const QString jsonPath = config + "/settings.json";
    if (!QDir().mkpath(config)) return fail("Could not create the isolated Studio config directory");
    {
        QSettings legacy(legacyPath, QSettings::IniFormat);
        QFont oldFont("Monospace", 13);
        legacy.setValue("editor/font", oldFont);
        legacy.setValue("appearance/accent", "#123456");
        legacy.setValue("shortcuts/Studio/file.save", "Ctrl+Shift+S");
        legacy.sync();
        if (legacy.status() != QSettings::NoError) return fail("Could not seed legacy preferences");
    }
    const QByteArray originalIni = read(legacyPath);
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
    QColor accent;
    bool autosave = false;
    int delay = 0;
    custom.onAppearance = [&](QFont selected, QColor themeAccent) {
        font = selected;
        accent = themeAccent;
        window.setStyleSheet(custom.userStyleSheet());
    };
    custom.onAutosave = [&](bool enabled, int milliseconds) {
        autosave = enabled;
        delay = milliseconds;
    };
    custom.reload();
    auto json = QJsonDocument::fromJson(read(jsonPath)).object();
    if (json.value("version").toInt() != 1 || read(legacyPath) != originalIni ||
        font.pointSize() != 13 || accent != QColor("#123456") ||
        save.shortcut() != QKeySequence("Ctrl+Shift+S") || !autosave || delay != 1500)
        return fail("Legacy preferences were not migrated intact or editing defaults were wrong");
    window.show();
    app.processEvents();
    const QString css = "QLabel#brand { color: #74d4bd; }";
    QTimer::singleShot(0, [&]{
        auto *dialog = qobject_cast<QDialog *>(app.activeModalWidget());
        if (!dialog) qFatal("Preferences dialog missing");
        for (auto *combo : dialog->findChildren<QComboBox *>())
            if (combo->findText("Emacs") >= 0) combo->setCurrentText("Emacs");
        dialog->findChild<QSpinBox *>("editorFontSize")->setValue(14);
        dialog->findChild<QCheckBox *>("editorAutosave")->setChecked(false);
        dialog->findChild<QSpinBox *>("editorAutosaveDelay")->setValue(2700);
        dialog->findChild<QPlainTextEdit *>("styleEditor")->insertPlainText(css);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
    });
    custom.configure();
    if (font.pointSize() != 14 || save.shortcut() != QKeySequence("Ctrl+X, Ctrl+S"))
        return fail("Saved keymap or font did not reload");
    json = QJsonDocument::fromJson(read(jsonPath)).object();
    const auto editing = json.value("editor").toObject();
    if (editing.value("autosave").toBool(true) || autosave ||
        editing.value("autosaveDelayMs").toInt() != 2700 || delay != 2700)
        return fail("Saving controls were not persisted to JSON or sent to the editor");
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
    const QString project = home.filePath("project");
    if (!QDir().mkpath(project + "/.interact-studio") || !QDir().mkpath(config + "/themes"))
        return fail("Could not create workspace override or themes directory");
    const QString projectSettings = project + "/.interact-studio/settings.json";
    const QJsonObject projectData{
        {"version", 1}, {"editor", QJsonObject{{"fontSize", 19}, {"autosave", true}, {"autosaveDelayMs", 800}}},
        {"appearance", QJsonObject{{"accent", "#123abc"}, {"theme", "other"}}},
        {"keyboard", QJsonObject{{"profile", "Studio"}}}
    };
    const QByteArray theme = "QLabel#brand { color: #aabbcc; }";
    if (!write(projectSettings, projectData) || !write(config + "/themes/fixture.qss", theme))
        return fail("Could not create the workspace override or theme fixtures");
    auto appearance = json.value("appearance").toObject();
    appearance.insert("theme", "fixture");
    json.insert("appearance", appearance);
    if (!write(jsonPath, json)) return fail("Could not write the selected theme to JSON");
    custom.setWorkspace(project);
    custom.reload();
    if (font.pointSize() != 14 || autosave) return fail("Workspace defaults applied without permission");
    json.insert("workspaceOverrides", true);
    if (!write(jsonPath, json)) return fail("Could not enable workspace overrides");
    custom.reload();
    if (font.pointSize() != 19 || accent != QColor("#123abc") || !autosave || delay != 800 ||
        save.shortcut() != QKeySequence("Ctrl+X, Ctrl+S"))
        return fail("Settings did not apply safe workspace values while preserving the user keymap");
    if (custom.userStyleSheet() != QString::fromUtf8(theme) + '\n' + css ||
        brand.palette().color(QPalette::WindowText) != QColor("#74d4bd"))
        return fail("Installed theme or personal override did not load");

    const QByteArray invalid = R"({"editor":{"autosaveDelayMs":1}})";
    if (!write(jsonPath, invalid)) return fail("Could not write deliberately invalid preferences");
    custom.reload();
    if (font.pointSize() != 19 || !autosave || delay != 800 || read(jsonPath) != invalid)
        return fail("Invalid JSON schema replaced active preferences or the source file");
    qInfo("PASS: JSON and INI migration, autosave settings, opt-in scoped overrides, theme precedence and Emacs chord");
}
