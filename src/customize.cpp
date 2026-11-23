#include "customize.h"
#include "catalog.h"
#include "fonts.h"
#include <algorithm>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QUrl>
#include <QPixmap>
#include <QIcon>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLockFile>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSaveFile>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {
const QStringList profiles = {"Studio", "Emacs"};
const QColor defaultAccent("#e0ae69");
constexpr int minDelay = 200;
constexpr int maxDelay = 30000;
constexpr int defaultDelay = 1500;
constexpr int maxSettingsBytes = 256 * 1024;
constexpr int maxStyleBytes = 512 * 1024;

struct SettingsRead {
    QByteArray data;
    QJsonObject values;
    QString error;
    bool exists = false;
};

bool integerField(const QJsonValue &field, int low, int high) {
    return field.isUndefined() || (field.isDouble() && field.toInt(low - 1) >= low && field.toInt() <= high);
}

bool stringField(const QJsonValue &field, int maxLength) {
    if (field.isUndefined()) return true;
    if (!field.isString()) return false;
    const QString value = field.toString();
    return value.size() <= maxLength && !value.contains(QChar::Null);
}

bool themeId(const QString &id) {
    static const QRegularExpression pattern("^[A-Za-z0-9_-][A-Za-z0-9._-]{0,79}$");
    return pattern.match(id).hasMatch();
}

QString validate(const QJsonObject &settings, bool project) {
    const auto version = settings.value("version");
    if (!version.isUndefined() && (!version.isDouble() || version.toInt() != 1))
        return "Unsupported settings version. Use version 1.";

    const auto editor = settings.value("editor");
    const auto appearance = settings.value("appearance");
    if ((!editor.isUndefined() && !editor.isObject()) || (!appearance.isUndefined() && !appearance.isObject()))
        return "The editor and appearance settings must be objects.";
    const auto editing = editor.toObject();
    const auto autosave = editing.value("autosave");
    if ((!autosave.isUndefined() && !autosave.isBool()) ||
        !integerField(editing.value("autosaveDelayMs"), minDelay, maxDelay))
        return "Set editor.autosave to true or false and autosaveDelayMs to an integer from 200 to 30000.";
    if (!stringField(editing.value("fontFamily"), 256) || !integerField(editing.value("fontSize"), 7, 48))
        return "Set editor.fontFamily to a string and fontSize to an integer from 7 to 48.";
    const auto visual = appearance.toObject();
    const auto accent = visual.value("accent");
    if (!accent.isUndefined() && (!stringField(accent, 40) || !QColor(accent.toString()).isValid()))
        return "Set appearance.accent to a color such as #e0ae69.";
    if (project) return {};

    const auto overrides = settings.value("workspaceOverrides");
    if (!overrides.isUndefined() && !overrides.isBool())
        return "Set workspaceOverrides to true or false.";
    const auto theme = visual.value("theme");
    if (!theme.isUndefined() && (!theme.isString() ||
        (!theme.toString().isEmpty() && !themeId(theme.toString()))))
        return "Set appearance.theme to an installed theme ID or an empty string.";

    const auto keyboard = settings.value("keyboard");
    if (!keyboard.isUndefined() && !keyboard.isObject()) return "Keyboard settings must be an object.";
    const auto keymap = keyboard.toObject();
    const auto profile = keymap.value("profile");
    if (!profile.isUndefined() && !profiles.contains(profile.toString())) return "Select Studio or Emacs as the profile.";
    const auto shortcuts = keymap.value("shortcuts");
    if (shortcuts.isUndefined()) return {};
    if (!shortcuts.isObject()) return "Keyboard shortcuts must be grouped by profile.";
    const auto shortcutProfiles = shortcuts.toObject();
    for (auto maps = shortcutProfiles.constBegin(); maps != shortcutProfiles.constEnd(); ++maps) {
        if (!maps.value().isObject()) return "Each shortcut profile must contain a command-to-shortcut object.";
        const auto bindings = maps.value().toObject();
        for (auto binding = bindings.constBegin(); binding != bindings.constEnd(); ++binding) {
            const auto value = binding.value();
            if (!value.isString() || !stringField(value, 160) ||
                (!value.toString().isEmpty() &&
                 QKeySequence::fromString(value.toString(), QKeySequence::PortableText)
                     .toString(QKeySequence::PortableText).isEmpty()))
                return QString("Invalid shortcut for %1. Use a key such as Ctrl+S, or an empty string.").arg(binding.key());
        }
    }
    return {};
}

SettingsRead readSettings(const QString &path, bool project = false) {
    SettingsRead result;
    QFile file(path);
    result.exists = file.exists() || QFileInfo(path).isSymLink();
    if (!result.exists) return result;
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = file.errorString();
        return result;
    }
    result.data = file.read(maxSettingsBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        result.error = file.errorString();
    } else if (result.data.size() > maxSettingsBytes) {
        result.error = "Settings must be at most 256 KiB.";
    } else {
        QJsonParseError parse;
        const auto json = QJsonDocument::fromJson(result.data, &parse);
        if (parse.error != QJsonParseError::NoError)
            result.error = QString("%1 (byte %2)").arg(parse.errorString()).arg(parse.offset);
        else if (!json.isObject()) result.error = "Settings must contain one JSON object.";
        else {
            result.values = json.object();
            result.error = validate(result.values, project);
        }
    }
    return result;
}

QString writeSettings(const QString &path, const QJsonObject &settings) {
    if (const auto error = validate(settings, false); !error.isEmpty()) return error;
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return "Could not create the preferences directory.";
    const auto bytes = QJsonDocument(settings).toJson(QJsonDocument::Indented);
    if (bytes.size() > maxSettingsBytes) return "Settings must be at most 256 KiB.";
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return file.errorString();
    return {};
}

QJsonObject readLegacySettings(const QString &path, QString &error) {
    QSettings legacy(path, QSettings::IniFormat);
    const auto oldFont = legacy.value("editor/font", defaultEditorFont()).value<QFont>();
    const auto oldAccent = QColor(legacy.value("appearance/accent", defaultAccent.name()).toString());
    const auto oldProfile = legacy.value("keys/profile", profiles.first()).toString();
    QJsonObject shortcuts;
    for (const auto &profile : profiles) {
        QJsonObject bindings;
        legacy.beginGroup("shortcuts/" + profile);
        for (const auto &id : legacy.allKeys()) {
            const auto value = legacy.value(id).toString();
            if (value.isEmpty() || !QKeySequence::fromString(value, QKeySequence::PortableText)
                                          .toString(QKeySequence::PortableText).isEmpty())
                bindings.insert(id, value);
        }
        legacy.endGroup();
        if (!bindings.isEmpty()) shortcuts.insert(profile, bindings);
    }
    if (legacy.status() != QSettings::NoError) {
        error = "Could not import settings.ini. The original file was kept.";
        return {};
    }
    const int size = oldFont.pointSize() > 0 ? oldFont.pointSize() : defaultEditorFont().pointSize();
    return {
        {"version", 1},
        {"editor", QJsonObject{{"fontFamily", oldFont.family()},
                               {"fontSize", qBound(7, size, 48)},
                               {"autosave", true}, {"autosaveDelayMs", defaultDelay}}},
        {"appearance", QJsonObject{{"accent", oldAccent.isValid() ? oldAccent.name() : defaultAccent.name()}}},
        {"keyboard", QJsonObject{{"profile", profiles.contains(oldProfile) ? oldProfile : profiles.first()},
                                 {"shortcuts", shortcuts}}},
        {"workspaceOverrides", false}
    };
}

QFont editorFont(const QJsonObject &editor) {
    auto font = defaultEditorFont();
    const QString family = editor.value("fontFamily").toString();
    if (!family.isEmpty() && family != font.family()) {
        // Preserve the monospace hint when no typeface was selected explicitly.
        font = QFont(family);
    }
    font.setPointSize(editor.value("fontSize").toInt(defaultEditorFont().pointSize()));
    return font;
}

void overrideFields(QJsonObject &target, const QJsonObject &overrides, const QStringList &fields) {
    for (const auto &field : fields)
        if (overrides.contains(field)) target.insert(field, overrides.value(field));
}

QString safeWorkspaceSettingsPath(const QString &workspace) {
    if (workspace.isEmpty()) return {};
    const QDir root(workspace);
    const auto path = root.filePath(".interact-studio/settings.json");
    const QFileInfo file(path);
    if (!file.exists()) return {};
    const auto canonical = file.canonicalFilePath();
    if (file.isSymLink() || QFileInfo(root.filePath(".interact-studio")).isSymLink() ||
        canonical.isEmpty() || !file.isFile() ||
        root.relativeFilePath(canonical) != ".interact-studio/settings.json") return {};
    return path;
}

QString readStyle(const QString &path, QString &error) {
    error.clear();
    QFile file(path);
    if (!file.exists()) return {};
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return {};
    }
    const auto bytes = file.read(maxStyleBytes + 1);
    const QString style = QString::fromUtf8(bytes);
    if (file.error() != QFileDevice::NoError) error = file.errorString();
    else if (bytes.size() > maxStyleBytes) error = "Style sheets must be at most 512 KiB.";
    else if (style.toUtf8() != bytes) error = "The style sheet must be a UTF-8 text file.";
    return error.isEmpty() ? style : QString();
}

QString displayName(QAction *action) {
    return QString(action->text()).remove('&');
}

bool overlaps(const QKeySequence &left, const QKeySequence &right) {
    return !left.isEmpty() && !right.isEmpty() &&
           (left.matches(right) != QKeySequence::NoMatch ||
            right.matches(left) != QKeySequence::NoMatch);
}

QKeySequence profileDefault(const QString &id, const QString &profile, QKeySequence fallback) {
    static const QMap<QString, QString> emacs = {
        {"file.open", "Ctrl+X, Ctrl+F"}, {"file.save", "Ctrl+X, Ctrl+S"},
        {"file.close", "Ctrl+X, K"}, {"app.quit", "Ctrl+X, Ctrl+C"},
        {"edit.find", "Ctrl+S"}, {"edit.goto", "Alt+G, G"}, {"app.palette", "Alt+X"},
        {"edit.cut", "Ctrl+W"}, {"edit.copy", "Alt+W"}, {"edit.paste", "Ctrl+Y"}
    };
    if (profile == "Emacs" && emacs.contains(id))
        return QKeySequence::fromString(emacs.value(id), QKeySequence::PortableText);
    return fallback;
}
}

Customize::Customize(QWidget *window)
    : QObject(window), window_(window),
      configDir_(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/interact-studio"),
      profile_(profiles.first()), font_(defaultEditorFont()), accent_(defaultAccent) {
    qApp->installEventFilter(this);
}

void Customize::bind(QString id, QAction *action, QString defaultShortcut) {
    commands_.insert(id, {action, QKeySequence::fromString(defaultShortcut, QKeySequence::PortableText)});
    window_->addAction(action);
    action->setShortcut(shortcut(id, profile_, settings_));
}

QKeySequence Customize::shortcut(const QString &id, const QString &profile, const QJsonObject &settings) const {
    const auto profileBindings = settings.value("keyboard").toObject().value("shortcuts").toObject()
        .value(profile).toObject();
    if (profileBindings.contains(id))
        return QKeySequence::fromString(profileBindings.value(id).toString(), QKeySequence::PortableText);
    return profileDefault(id, profile, commands_.value(id).defaultShortcut);
}

QString Customize::shortcutConflict(const QJsonObject &settings) const {
    for (const auto &profile : profiles) {
        // Only the first stroke can have a prefix conflict. Group by it
        // instead of comparing the entire catalogue during startup.
        QMap<int, QList<QPair<QString, QKeySequence>>> starts;
        for (auto cmd = commands_.cbegin(); cmd != commands_.cend(); ++cmd) {
            const auto sequence = shortcut(cmd.key(), profile, settings);
            if (sequence.isEmpty()) continue;
            auto &group = starts[sequence[0]];
            const auto label = displayName(cmd->action);
            for (const auto &previous : group) {
                if (overlaps(sequence, previous.second))
                    return QString("%1 and %2 use overlapping shortcuts in %3.")
                        .arg(previous.first, label, profile);
            }
            group.append(qMakePair(label, sequence));
        }
    }
    return {};
}

bool Customize::eventFilter(QObject *object, QEvent *event) {
    // QTextControl normally consumes Ctrl+X before Qt's chord matcher sees it.
    if (event->type() == QEvent::ShortcutOverride) {
        const auto *editor = qobject_cast<QPlainTextEdit *>(object);
        if (editor && editor->window() == window_ && !QApplication::activeModalWidget()) {
            auto *key = static_cast<QKeyEvent *>(event);
            const int stroke = key->key() | int(key->modifiers());
            for (const auto &command : commands_) {
                const auto sequence = command.action->shortcut();
                if (!sequence.isEmpty() && sequence[0] == stroke && command.action->isEnabled()) {
                    key->ignore();
                    return true;
                }
            }
        }
    }
    return QObject::eventFilter(object, event);
}

void Customize::loadExtensions() {
    for (auto it = extensions_.cbegin(); it != extensions_.cend(); ++it) {
        commands_.remove(it.key());
        delete it.value();
    }
    extensions_.clear();
    extensionErrors_.clear();
    const QDir dir(configDir_ + "/extensions");
    static const QRegularExpression validId("^[A-Za-z0-9_-][A-Za-z0-9._-]{0,79}$");
    const auto files = dir.entryInfoList({"*.json"}, QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const auto &info : files) {
        auto reject = [&](QString reason) { extensionErrors_ << info.fileName() + " — " + reason; };
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) {
            reject(file.errorString());
            continue;
        }
        const auto bytes = file.read(65537);
        if (bytes.size() > 65536) {
            reject("Manifest exceeds 64 KiB");
            continue;
        }
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            reject(error.error == QJsonParseError::NoError ? "Expected a JSON object" : error.errorString());
            continue;
        }
        const auto manifest = doc.object();
        const auto id = manifest.value("id").toString();
        const auto name = manifest.value("name").toString().trimmed();
        const auto program = manifest.value("program").toString().trimmed();
        const auto args = manifest.value("arguments");
        if (!validId.match(id).hasMatch() || name.isEmpty() || program.isEmpty() ||
            (!args.isUndefined() && !args.isArray())) {
            reject("Expected a valid id, name, program and an optional arguments array");
            continue;
        }
        const QString commandId = "extension." + id;
        if (commands_.contains(commandId)) {
            reject("Duplicate command ID");
            continue;
        }
        QStringList argv;
        const auto array = args.toArray();
        for (const auto &arg : array)
            if (arg.isString()) argv << arg.toString();
        if (argv.size() != array.size() || program.contains(QChar::Null) ||
            std::any_of(argv.cbegin(), argv.cend(), [](const QString &s){ return s.contains(QChar::Null); })) {
            reject("Command and arguments must be strings without null bytes");
            continue;
        }
        auto *action = new QAction(name, this);
        connect(action, &QAction::triggered, this, [this, program, argv]() {
            if (onRun) onRun(program, argv);
        });
        extensions_.insert(commandId, action);
        bind(commandId, action, {});
    }
}

QList<QAction *> Customize::extensionActions() const {
    return extensions_.values();
}

void Customize::setWorkspace(const QString &path) {
    const auto next = QFileInfo(path).canonicalFilePath();
    if (next == workspace_) return;
    workspace_ = next;
    projectSettings_ = {};
}

QString Customize::userStyleSheet() const {
    if (themeStyle_.isEmpty()) return styleSheet_;
    return themeStyle_ + '\n' + styleSheet_;
}

void Customize::reload() {
    reloadCatalogSnippets();
    loadExtensions();
    const auto path = configDir_ + "/settings.json";
    const auto legacyPath = configDir_ + "/settings.ini";
    auto disk = readSettings(path);
    settingsError_ = disk.error;
    if (!disk.exists && QFileInfo::exists(legacyPath)) {
        auto legacy = readLegacySettings(legacyPath, settingsError_);
        if (settingsError_.isEmpty()) settingsError_ = shortcutConflict(legacy);
        if (settingsError_.isEmpty()) {
            settings_ = legacy;
            QLockFile lock(path + ".lock");
            if (lock.tryLock(0)) {
                disk = readSettings(path);
                if (!disk.exists) settingsError_ = writeSettings(path, legacy);
                else if (disk.error.isEmpty()) settings_ = disk.values;
                else settingsError_ = disk.error;
            }
        }
    } else if (settingsError_.isEmpty()) {
        settingsError_ = shortcutConflict(disk.values);
        if (settingsError_.isEmpty()) settings_ = disk.values;
    }
    if (!settingsError_.isEmpty()) qWarning("Preferences: %s", qPrintable(settingsError_));

    auto editing = settings_.value("editor").toObject();
    auto appearance = settings_.value("appearance").toObject();
    projectError_.clear();
    if (settings_.value("workspaceOverrides").toBool()) {
        const auto projectPath = safeWorkspaceSettingsPath(workspace_);
        if (!projectPath.isEmpty()) {
            const auto project = readSettings(projectPath, true);
            projectError_ = project.error;
            if (projectError_.isEmpty()) projectSettings_ = project.values;
            overrideFields(editing, projectSettings_.value("editor").toObject(),
                           {"fontFamily", "fontSize", "autosave", "autosaveDelayMs"});
            overrideFields(appearance, projectSettings_.value("appearance").toObject(), {"accent"});
        } else projectSettings_ = {};
    } else projectSettings_ = {};
    profile_ = settings_.value("keyboard").toObject().value("profile").toString(profiles.first());
    font_ = editorFont(editing);
    accent_ = QColor(appearance.value("accent").toString(defaultAccent.name()));
    styleSheet_ = readStyle(configDir_ + "/studio.qss", styleError_);
    const QString theme = appearance.value("theme").toString();
    themeStyle_.clear();
    themeError_.clear();
    if (!theme.isEmpty()) {
        const auto themePath = configDir_ + "/themes/" + theme + ".qss";
        const QFileInfo installed(themePath);
        if (installed.isFile() && !installed.isSymLink())
            themeStyle_ = readStyle(themePath, themeError_);
        else themeError_ = QString("Theme %1 is not installed. Studio Dark is shown instead.").arg(theme);
    }
    for (auto it = commands_.cbegin(); it != commands_.cend(); ++it)
        it->action->setShortcut(shortcut(it.key(), profile_, settings_));
    if (onAppearance) onAppearance(font_, accent_);
    if (onAutosave) onAutosave(editing.value("autosave").toBool(true),
                               editing.value("autosaveDelayMs").toInt(defaultDelay));
    if (onExtensionsChanged) onExtensionsChanged();
}

QString Customize::extensionReport() const {
    QStringList lines;
    for (auto *action : extensions_) lines << displayName(action);
    if (lines.isEmpty()) lines << "No commands installed.";
    if (!extensionErrors_.isEmpty()) lines << "" << extensionErrors_;
    return lines.join('\n');
}

void Customize::configure() {
    const QString settingsPath = configDir_ + "/settings.json";
    auto original = readSettings(settingsPath);
    if (original.error.isEmpty()) original.error = shortcutConflict(original.values);
    const auto settings = original.exists && original.error.isEmpty() ? original.values : settings_;
    const auto editing = settings.value("editor").toObject();
    const auto visual = settings.value("appearance").toObject();
    const auto keyboardSettings = settings.value("keyboard").toObject();
    QDialog dialog(window_);
    dialog.setWindowTitle("Preferences · Interact Studio");
    dialog.resize(590, 490);
    auto *layout = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget(&dialog);
    layout->addWidget(tabs);
    auto addNotice = [](QLayout *parent, const QString &message) {
        if (message.isEmpty()) return;
        auto *notice = new QLabel(message);
        notice->setTextFormat(Qt::PlainText);
        notice->setWordWrap(true);
        parent->addWidget(notice);
    };

    auto *appearance = new QWidget;
    auto *form = new QFormLayout(appearance);
    form->setContentsMargins(18, 20, 18, 20);
    const QFont configuredFont = editorFont(editing);
    auto *family = new QFontComboBox;
    family->setCurrentFont(configuredFont);
    form->addRow("Editor typeface", family);
    auto *size = new QSpinBox;
    size->setObjectName("editorFontSize");
    size->setRange(7, 48);
    size->setSuffix(" pt");
    size->setValue(configuredFont.pointSize());
    form->addRow("Text size", size);
    QColor color(visual.value("accent").toString(defaultAccent.name()));
    auto *chooseColor = new QPushButton;
    const auto swatch = [&]() {
        QPixmap chip(18, 18);
        chip.fill(color);
        chooseColor->setIcon(QIcon(chip));
        chooseColor->setText(color.name());
    };
    swatch();
    form->addRow("Accent color", chooseColor);
    connect(chooseColor, &QPushButton::clicked, &dialog, [&]() {
        const auto selected = QColorDialog::getColor(color, &dialog, "Accent color");
        if (selected.isValid()) {
            color = selected;
            swatch();
        }
    });
    auto *themeBox = new QComboBox;
    themeBox->setObjectName("editorTheme");
    const auto refreshThemes = [&](const QString &selected) {
        themeBox->clear();
        themeBox->addItem("Studio Dark", QString());
        const QDir themes(configDir_ + "/themes");
        const auto packages = extensionCatalog();
        for (const auto &info : themes.entryInfoList({"*.qss"}, QDir::Files | QDir::NoSymLinks, QDir::Name)) {
            const auto id = info.completeBaseName();
            if (!themeId(id)) continue;
            const auto package = std::find_if(packages.cbegin(), packages.cend(), [&id](const auto &item) {
                return item.category == CatalogCategory::Theme && item.id == id;
            });
            themeBox->addItem(package == packages.cend() ? id : package->name, id);
        }
        const int selection = themeBox->findData(selected);
        if (selection >= 0) themeBox->setCurrentIndex(selection);
    };
    refreshThemes(visual.value("theme").toString());
    form->addRow("Theme", themeBox);
    addNotice(form, themeError_);
    tabs->addTab(appearance, "Appearance");

    auto *saving = new QWidget;
    auto *savingForm = new QFormLayout(saving);
    savingForm->setContentsMargins(18, 20, 18, 20);
    auto *autosave = new QCheckBox("Save named files after a pause in typing");
    autosave->setObjectName("editorAutosave");
    autosave->setChecked(editing.value("autosave").toBool(true));
    savingForm->addRow(autosave);
    auto *delay = new QSpinBox;
    delay->setObjectName("editorAutosaveDelay");
    delay->setRange(minDelay, maxDelay);
    delay->setSingleStep(100);
    delay->setSuffix(" ms");
    delay->setValue(editing.value("autosaveDelayMs").toInt(defaultDelay));
    delay->setEnabled(autosave->isChecked());
    connect(autosave, &QCheckBox::toggled, delay, &QSpinBox::setEnabled);
    savingForm->addRow("Delay", delay);
    addNotice(savingForm, "Recovery drafts are kept locally even when automatic file saving is off.");
    tabs->addTab(saving, "Saving");

    const QString stylePath = configDir_ + "/studio.qss";
    auto *styleTab = new QWidget;
    auto *styleLayout = new QVBoxLayout(styleTab);
    auto *stylePathLabel = new QLabel(QDir::toNativeSeparators(stylePath));
    stylePathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    styleLayout->addWidget(stylePathLabel);
    auto *styleEdit = new QPlainTextEdit(styleSheet_);
    styleEdit->setObjectName("styleEditor");
    styleEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    styleEdit->setFont(defaultEditorFont());
    styleEdit->setPlaceholderText("QLabel#brand { color: #74d4bd; }");
    if (!styleError_.isEmpty()) {
        styleEdit->setReadOnly(true);
        styleLayout->addWidget(new QLabel(styleError_));
    }
    styleLayout->addWidget(styleEdit);
    tabs->addTab(styleTab, "Style sheet");

    auto *keyboard = new QWidget;
    auto *keysLayout = new QVBoxLayout(keyboard);
    auto *keyHeader = new QHBoxLayout;
    auto *profileBox = new QComboBox;
    profileBox->addItems(profiles);
    const QString selectedProfile = keyboardSettings.value("profile").toString(profiles.first());
    profileBox->setCurrentText(selectedProfile);
    auto *reset = new QPushButton("Reset profile");
    keyHeader->addWidget(new QLabel("Keymap"));
    keyHeader->addWidget(profileBox);
    keyHeader->addStretch();
    keyHeader->addWidget(reset);
    keysLayout->addLayout(keyHeader);
    auto *table = new QTableWidget;
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({"Command", "Shortcut"});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->setColumnWidth(1, 180);
    keysLayout->addWidget(table);
    tabs->addTab(keyboard, "Keyboard");

    QString activeProfile = selectedProfile;
    QMap<QString, QMap<QString, QKeySequence>> changes;
    const auto capture = [&]() {
        for (int row = 0; row < table->rowCount(); ++row) {
            const auto id = table->item(row, 0)->data(Qt::UserRole).toString();
            auto *edit = static_cast<QKeySequenceEdit *>(table->cellWidget(row, 1));
            changes[activeProfile][id] = edit->keySequence();
        }
    };
    const auto populate = [&]() {
        table->setRowCount(0);
        for (auto it = commands_.cbegin(); it != commands_.cend(); ++it) {
            const int row = table->rowCount();
            table->insertRow(row);
            auto *item = new QTableWidgetItem(displayName(it->action));
            item->setData(Qt::UserRole, it.key());
            item->setToolTip(it.key());
            table->setItem(row, 0, item);
            auto sequence = changes.value(activeProfile).value(it.key(), shortcut(it.key(), activeProfile, settings));
            auto *edit = new QKeySequenceEdit(sequence);
            edit->setContextMenuPolicy(Qt::ActionsContextMenu);
            auto *clearKey = new QAction("Clear shortcut", edit);
            edit->addAction(clearKey);
            connect(clearKey, &QAction::triggered, edit, &QKeySequenceEdit::clear);
            table->setCellWidget(row, 1, edit);
        }
    };
    populate();
    connect(profileBox, &QComboBox::currentTextChanged, &dialog, [&](const QString &selected) {
        capture();
        activeProfile = selected;
        populate();
    });
    connect(reset, &QPushButton::clicked, &dialog, [&]() {
        for (auto it = commands_.cbegin(); it != commands_.cend(); ++it)
            changes[activeProfile][it.key()] = profileDefault(it.key(), activeProfile, it->defaultShortcut);
        populate();
    });

    auto *ext = new QWidget;
    auto *extLayout = new QVBoxLayout(ext);
    const QString extPath = QDir::toNativeSeparators(configDir_ + "/extensions");
    auto *pathLabel = new QLabel(extPath);
    pathLabel->setWordWrap(true);
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    extLayout->addWidget(new QLabel("User command directory"));
    extLayout->addWidget(pathLabel);
    auto *report = new QPlainTextEdit(extensionReport());
    report->setReadOnly(true);
    extLayout->addWidget(report);
    auto *buttons = new QHBoxLayout;
    auto *folder = new QPushButton("Open folder");
    auto *rescan = new QPushButton("Reload commands");
    auto *catalog = new QPushButton("Browse catalog…");
    buttons->addWidget(folder);
    buttons->addWidget(rescan);
    buttons->addWidget(catalog);
    buttons->addStretch();
    extLayout->addLayout(buttons);
    connect(folder, &QPushButton::clicked, &dialog, [&]() {
        if (!QDir().mkpath(extPath) || !QDesktopServices::openUrl(QUrl::fromLocalFile(extPath)))
            QMessageBox::warning(&dialog, "Extensions", "Could not open " + extPath);
    });
    const auto refreshExtensions = [&]() {
        capture();
        reloadCatalogSnippets();
        loadExtensions();
        if (onExtensionsChanged) onExtensionsChanged();
        populate();
        report->setPlainText(extensionReport());
        const auto selected = themeBox->currentData().toString();
        refreshThemes(selected);
    };
    connect(rescan, &QPushButton::clicked, &dialog, refreshExtensions);
    connect(catalog, &QPushButton::clicked, &dialog, [&]() {
        showExtensionCatalog(&dialog, refreshExtensions);
    });
    tabs->addTab(ext, "Extensions");

    auto *files = new QWidget;
    auto *filesLayout = new QVBoxLayout(files);
    const auto nativePath = QDir::toNativeSeparators(settingsPath);
    auto *path = new QLabel(nativePath);
    path->setTextFormat(Qt::PlainText);
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path->setWordWrap(true);
    filesLayout->addWidget(path);
    addNotice(filesLayout, original.error.isEmpty() ? settingsError_ : original.error);
    auto *open = new QPushButton("Open settings.json");
    auto *reloadFromDisk = new QPushButton("Discard panel edits and reload from disk");
    filesLayout->addWidget(open, 0, Qt::AlignLeft);
    filesLayout->addWidget(reloadFromDisk, 0, Qt::AlignLeft);
    auto *project = new QCheckBox("Allow workspace appearance and saving overrides");
    project->setObjectName("allowWorkspaceOverrides");
    project->setChecked(settings.value("workspaceOverrides").toBool());
    filesLayout->addWidget(project);
    addNotice(filesLayout, "Settings at .interact-studio/settings.json in the workspace can change the font, "
                           "accent and autosave. The values in this panel edit your user defaults.");
    addNotice(filesLayout, projectError_);
    filesLayout->addStretch();
    connect(reloadFromDisk, &QPushButton::clicked, &dialog, [&]() {
        dialog.reject();
        this->reload();
    });
    connect(open, &QPushButton::clicked, &dialog, [&]() {
        if (!readSettings(settingsPath).exists) {
            if (!QDir().mkpath(configDir_)) {
                QMessageBox::warning(&dialog, "Preferences", "Could not create the preferences directory.");
                return;
            }
            QLockFile lock(settingsPath + ".lock");
            if (!lock.tryLock(0)) {
                QMessageBox::warning(&dialog, "Preferences", "Another window is saving preferences. Try again.");
                return;
            }
            if (!readSettings(settingsPath).exists) {
                QJsonObject initial = settings;
                initial.insert("version", 1);
                const auto error = writeSettings(settingsPath, initial);
                if (!error.isEmpty()) {
                    QMessageBox::warning(&dialog, "Preferences", error);
                    return;
                }
                original = readSettings(settingsPath);
            }
        }
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(settingsPath)))
            QMessageBox::warning(&dialog, "Preferences", "Could not open " + nativePath);
    });
    tabs->addTab(files, "Settings file");

    auto *actions = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(actions);
    actions->button(QDialogButtonBox::Save)->setEnabled(original.error.isEmpty());
    connect(actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(actions, &QDialogButtonBox::accepted, &dialog, [&]() {
        capture();
        QJsonObject next = settings;
        next.insert("version", 1);
        next.insert("workspaceOverrides", project->isChecked());
        auto updatedEditor = editing;
        updatedEditor.insert("fontFamily", family->currentFont().family());
        updatedEditor.insert("fontSize", size->value());
        updatedEditor.insert("autosave", autosave->isChecked());
        updatedEditor.insert("autosaveDelayMs", delay->value());
        next.insert("editor", updatedEditor);
        auto updatedVisual = visual;
        updatedVisual.insert("accent", color.name());
        updatedVisual.insert("theme", themeBox->currentData().toString());
        next.insert("appearance", updatedVisual);
        auto updatedKeys = keyboardSettings;
        updatedKeys.insert("profile", activeProfile);
        auto shortcuts = updatedKeys.value("shortcuts").toObject();
        for (auto p = changes.cbegin(); p != changes.cend(); ++p) {
            auto bindings = shortcuts.value(p.key()).toObject();
            for (auto it = p->cbegin(); it != p->cend(); ++it) {
                if (!commands_.contains(it.key())) continue;
                if (profileDefault(it.key(), p.key(), commands_.value(it.key()).defaultShortcut) == it.value())
                    bindings.remove(it.key());
                else
                    bindings.insert(it.key(), it.value().toString(QKeySequence::PortableText));
            }
            if (bindings.isEmpty()) shortcuts.remove(p.key());
            else shortcuts.insert(p.key(), bindings);
        }
        updatedKeys.insert("shortcuts", shortcuts);
        next.insert("keyboard", updatedKeys);
        if (const auto conflict = shortcutConflict(next); !conflict.isEmpty()) {
            QMessageBox::warning(&dialog, "Shortcut conflict", conflict);
            tabs->setCurrentWidget(keyboard);
            return;
        }

        if (!QDir().mkpath(configDir_)) {
            QMessageBox::warning(&dialog, "Preferences", "Could not create the preferences directory.");
            return;
        }
        QLockFile lock(settingsPath + ".lock");
        if (!lock.tryLock(0)) {
            QMessageBox::warning(&dialog, "Preferences", "Another window is saving preferences. Try again.");
            return;
        }
        const auto latest = readSettings(settingsPath);
        if (!latest.error.isEmpty() || latest.exists != original.exists || latest.data != original.data) {
            QMessageBox::warning(&dialog, "Preferences",
                                 "settings.json changed on disk. Reopen Preferences after reloading.");
            return;
        }
        QSaveFile styleOutput(stylePath);
        if (styleEdit->document()->isModified()) {
            QString diskError;
            const QString diskStyle = readStyle(stylePath, diskError);
            if (!diskError.isEmpty() || diskStyle != styleSheet_) {
                QMessageBox::warning(&dialog, "Style sheet", diskError.isEmpty()
                    ? "The style sheet changed on disk. Reopen Preferences after reloading." : diskError);
                return;
            }
            const QByteArray edited = styleEdit->toPlainText().toUtf8();
            if (edited.size() > maxStyleBytes) {
                QMessageBox::warning(&dialog, "Style sheet", "Style sheets must be at most 512 KiB.");
                return;
            }
            if (!styleOutput.open(QIODevice::WriteOnly) || styleOutput.write(edited) != edited.size()) {
                QMessageBox::warning(&dialog, "Style sheet", "Could not save " + stylePath);
                return;
            }
        }
        const auto error = writeSettings(settingsPath, next);
        if (!error.isEmpty()) {
            QMessageBox::warning(&dialog, "Preferences", error);
            return;
        }
        original = readSettings(settingsPath);
        if (styleOutput.isOpen() && !styleOutput.commit()) {
            QMessageBox::warning(&dialog, "Style sheet", "Could not save " + stylePath);
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() == QDialog::Accepted) reload();
}
