#include "customize.h"
#include "fonts.h"
#include <algorithm>

#include <QAction>
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QUrl>
#include <QPixmap>
#include <QIcon>
#include <iterator>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
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

constexpr int maxStyleBytes = 512 * 1024;

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

QString keyPath(const QString &profile, const QString &id) {
    return "shortcuts/" + profile + '/' + id;
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
    QSettings settings(configDir_ + "/settings.ini", QSettings::IniFormat);
    action->setShortcut(shortcut(id, profile_, settings));
}

QKeySequence Customize::shortcut(const QString &id, const QString &profile, const QSettings &settings) const {
    const QString path = keyPath(profile, id);
    if (settings.contains(path))
        return QKeySequence::fromString(settings.value(path).toString(), QKeySequence::PortableText);
    return profileDefault(id, profile, commands_.value(id).defaultShortcut);
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

void Customize::reload() {
    QSettings settings(configDir_ + "/settings.ini", QSettings::IniFormat);
    profile_ = settings.value("keys/profile", profiles.first()).toString();
    if (!profiles.contains(profile_)) profile_ = profiles.first();
    font_ = settings.value("editor/font", defaultEditorFont()).value<QFont>();
    accent_ = QColor(settings.value("appearance/accent", defaultAccent.name()).toString());
    if (!accent_.isValid()) accent_ = defaultAccent;
    styleSheet_ = readStyle(configDir_ + "/studio.qss", styleError_);
    loadExtensions();
    for (auto it = commands_.cbegin(); it != commands_.cend(); ++it)
        it->action->setShortcut(shortcut(it.key(), profile_, settings));
    if (onAppearance) onAppearance(font_, accent_);
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
    QSettings settings(configDir_ + "/settings.ini", QSettings::IniFormat);
    QDialog dialog(window_);
    dialog.setWindowTitle("Preferences · Interact Studio");
    dialog.resize(590, 490);
    auto *layout = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget(&dialog);
    layout->addWidget(tabs);

    auto *appearance = new QWidget;
    auto *form = new QFormLayout(appearance);
    form->setContentsMargins(18, 20, 18, 20);
    auto *family = new QFontComboBox;
    family->setCurrentFont(font_);
    form->addRow("Editor typeface", family);
    auto *size = new QSpinBox;
    size->setRange(7, 48);
    size->setSuffix(" pt");
    size->setValue(font_.pointSize());
    form->addRow("Text size", size);
    QColor color = accent_;
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
    tabs->addTab(appearance, "Appearance");

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
    profileBox->setCurrentText(profile_);
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

    QString activeProfile = profile_;
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
    buttons->addWidget(folder);
    buttons->addWidget(rescan);
    buttons->addStretch();
    extLayout->addLayout(buttons);
    connect(folder, &QPushButton::clicked, &dialog, [&]() {
        if (!QDir().mkpath(extPath) || !QDesktopServices::openUrl(QUrl::fromLocalFile(extPath)))
            QMessageBox::warning(&dialog, "Extensions", "Could not open " + extPath);
    });
    connect(rescan, &QPushButton::clicked, &dialog, [&]() {
        capture();
        loadExtensions();
        if (onExtensionsChanged) onExtensionsChanged();
        populate();
        report->setPlainText(extensionReport());
    });
    tabs->addTab(ext, "Extensions");

    auto *actions = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(actions);
    connect(actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(actions, &QDialogButtonBox::accepted, &dialog, [&]() {
        capture();
        for (auto p = changes.cbegin(); p != changes.cend(); ++p) {
            for (auto it = p->cbegin(); it != p->cend(); ++it) {
                if (!commands_.contains(it.key())) continue;
                for (auto other = std::next(it); other != p->cend(); ++other) {
                    if (commands_.contains(other.key()) && overlaps(it.value(), other.value())) {
                        QMessageBox::warning(&dialog, "Shortcut conflict",
                            QString("%1 and %2 use overlapping shortcuts in %3.")
                                .arg(displayName(commands_.value(it.key()).action),
                                     displayName(commands_.value(other.key()).action), p.key()));
                        tabs->setCurrentWidget(keyboard);
                        profileBox->setCurrentText(p.key());
                        return;
                    }
                }
            }
        }
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
            QSaveFile file(stylePath);
            if (!QDir().mkpath(configDir_) || !file.open(QIODevice::WriteOnly) ||
                file.write(edited) != edited.size() || !file.commit()) {
                QMessageBox::warning(&dialog, "Style sheet", "Could not save " + stylePath);
                return;
            }
            styleSheet_ = styleEdit->toPlainText();
            styleEdit->document()->setModified(false);
        }
        QFont selectedFont = family->currentFont();
        selectedFont.setPointSize(size->value());
        settings.setValue("editor/font", selectedFont);
        settings.setValue("appearance/accent", color.name());
        settings.setValue("keys/profile", activeProfile);
        for (auto p = changes.cbegin(); p != changes.cend(); ++p) {
            for (auto it = p->cbegin(); it != p->cend(); ++it) {
                if (!commands_.contains(it.key())) continue;
                const auto key = keyPath(p.key(), it.key());
                if (profileDefault(it.key(), p.key(), commands_.value(it.key()).defaultShortcut) == it.value())
                    settings.remove(key);
                else
                    settings.setValue(key, it.value().toString(QKeySequence::PortableText));
            }
        }
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            QMessageBox::warning(&dialog, "Preferences", "Could not save " + settings.fileName());
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() == QDialog::Accepted) reload();
}
