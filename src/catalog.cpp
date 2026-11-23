#include "catalog.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVBoxLayout>

namespace {

constexpr qint64 manifestLimit = 65536;

struct Entry {
    CatalogPackage package;
    QByteArray payload;
    QString instructions;
};

class CatalogStrings {
    Q_DECLARE_TR_FUNCTIONS(CatalogStrings)
};

QString configDirectory(const QString &overridePath = {})
{
    if (!overridePath.isEmpty()) return overridePath;
    const auto location = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return location.isEmpty() ? QString() : QDir(location).filePath("interact-studio");
}

QByteArray json(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

QString themeSheet(const QString &editor, const QString &panel, const QString &border,
                   const QString &text, const QString &selection, const QString &accent)
{
    return QString::fromLatin1(R"QSS(
QMainWindow, QWidget#topbar, QToolBar#activityRail, QTabBar::base { background: %2; }
QDialog, QMenu, QToolBar, QDockWidget::title, QTabBar::tab, QStatusBar { background: %2; color: %4; }
QMenuBar, QLabel, QLabel#location, QStatusBar QLabel, QToolButton { color: %4; }
QMenuBar::item:selected, QMenu::item:selected, QToolButton:hover { background: %5; color: %4; }
QPlainTextEdit, QTextEdit, QTreeView, QListWidget, QTableWidget, QLineEdit, QComboBox, QTabBar::tab:selected {
    background: %1; color: %4; selection-background-color: %5; selection-color: %4;
}
QDockWidget::title, QWidget#topbar, QToolBar#projectStrip, QStatusBar { border-bottom: 1px solid %3; }
QLineEdit, QComboBox, QToolButton#paletteButton { border: 1px solid %3; }
QLineEdit:focus, QComboBox:focus, QToolButton#railItem[selected="true"] { border-color: %6; }
QTabBar::tab:selected { border-top-color: %6; }
QMainWindow::separator, QSplitter::handle, QScrollBar::handle { background: %3; }
QLabel#statusBrand, QToolButton#liveStatus { color: %6; }
)QSS").arg(editor, panel, border, text, selection, accent);
}

// The package index is built only when the catalog is opened or a package
// action is requested. Startup only loads the user's installed manifests.
const QList<Entry> &entries()
{
    static const QList<Entry> data = [] {
        QList<Entry> out;
        auto command = [&](QString id, QString name, QString description, QString program, QStringList arguments) {
            const QJsonObject manifest{
                {"id", "studio." + id}, {"name", name}, {"program", program},
                {"arguments", QJsonArray::fromStringList(arguments)},
                {"source", "Interact Studio"}, {"version", "1.0.0"}, {"license", "GPL-3.0-or-later"}
            };
            const auto instructions = CatalogStrings::tr(
                "Requires %1 on PATH. Run this command from the Extensions menu or command palette.")
                .arg(program);
            out.push_back({{id, name, CatalogCategory::Command, description}, json(manifest), instructions});
        };
        command("git-history", "Git · Branch graph", "Show recent commits and branches as a text graph.",
                "git", {"log", "--all", "--graph", "--decorate", "--oneline", "-35"});
        command("git-remotes", "Git · Remote URLs", "List fetch and push remotes configured for this repository.",
                "git", {"remote", "-v"});
        command("git-whitespace", "Git · Whitespace check", "Check unstaged changes for whitespace errors.",
                "git", {"diff", "--no-ext-diff", "--check"});
        command("git-staged-summary", "Git · Staged file summary", "Show which files and how many lines the index changes.",
                "git", {"diff", "--cached", "--no-ext-diff", "--stat"});
        command("git-stash-list", "Git · Stash list", "List saved stashes without applying or deleting them.",
                "git", {"stash", "list"});
        command("git-tags", "Git · Recent tags", "List up to 25 local tags, most recent first.",
                "git", {"for-each-ref", "--count=25", "--sort=-creatordate",
                        "--format=%(refname:short) %(creatordate:short)", "refs/tags/"});

        command("cargo-check", "Cargo · Type check", "Check a Rust package without linking the final executable.",
                "cargo", {"check"});
        command("cargo-test", "Cargo · Run tests", "Build and run the Rust project's test suite.",
                "cargo", {"test"});
        command("cargo-format-check", "Cargo · Format check", "Check Rust formatting without rewriting files. Requires rustfmt.",
                "cargo", {"fmt", "--all", "--", "--check"});
        command("cargo-clippy", "Cargo · Clippy", "Check Rust code with Clippy. Requires the Clippy component.",
                "cargo", {"clippy", "--all-targets"});
        command("cargo-tree", "Cargo · Dependency tree", "Inspect the dependencies of the Rust project.",
                "cargo", {"tree"});

        command("python-pytest", "Python · Run pytest", "Run pytest using the Python interpreter on PATH.",
                "python", {"-m", "pytest"});
        command("python-ruff", "Python · Ruff check", "Check Python code using Ruff without applying fixes.",
                "ruff", {"check", "."});
        command("cmake-build", "CMake · Build", "Build an existing CMake configuration in the build directory.",
                "cmake", {"--build", "build"});
        command("cmake-tests", "CMake · Run CTest", "Run the configured CTest suite and print failed test output.",
                "ctest", {"--test-dir", "build", "--output-on-failure"});
        command("go-tests", "Go · Run package tests", "Build and run the tests of all packages in the Go module.",
                "go", {"test", "./..."});
        command("docker-compose-check", "Docker · Validate Compose", "Validate the local Compose configuration.",
                "docker", {"compose", "config", "--quiet"});
        command("docker-compose-services", "Docker · Compose service status", "List the running services of the Compose project.",
                "docker", {"compose", "ps"});

        auto snippets = [&](QString id, QString name, QString description, QString language,
                            std::initializer_list<CatalogSnippet> items) {
            QJsonArray content;
            for (const auto &item : items)
                content.append(QJsonObject{{"trigger", item.trigger}, {"description", item.description}, {"body", item.body}});
            const QJsonObject pack{
                {"schema", 1}, {"language", language}, {"snippets", content},
                {"source", "Interact Studio"}, {"version", "1.0.0"}, {"license", "GPL-3.0-or-later"}
            };
            QStringList triggers;
            for (const auto &item : items) triggers.append(item.trigger);
            out.push_back({{id, name, CatalogCategory::Snippets, description}, json(pack), CatalogStrings::tr(
                "In a %1 file, type a trigger and press Tab to insert the selected snippet. Triggers: %2")
                .arg(language, triggers.join(", "))});
        };

        snippets("python-workflows", "Python · Data and tests", "Data classes, parametrized tests and UTF-8 file reads.",
                 "Python", {
            {"dataclass", "Typed data class", "from dataclasses import dataclass\n\n@dataclass\nclass Record:\n    name: str\n    \v"},
            {"pytestparam", "Parametrized pytest test", "import pytest\n\n@pytest.mark.parametrize(\"value,expected\", [(1, 1)])\ndef test_values(value, expected):\n    assert \vvalue == expected"},
            {"pathread", "Read a UTF-8 text file", "from pathlib import Path\n\ntext = Path(\"\vfile.txt\").read_text(encoding=\"utf-8\")"}
        });
        snippets("cpp-utilities", "C++ · Results and tests", "Scoped locking, move-only values and optional results.",
                 "C++", {
            {"lockscope", "Scoped mutex lock", "{\n    const std::lock_guard<std::mutex> lock(mutex);\n    \v\n}"},
            {"makeunique", "Own an object with make_unique", "auto object = std::make_unique<\vType>();"},
            {"optionalresult", "Function with optional result", "std::optional<Result> getResult() {\n    \vreturn std::nullopt;\n}"}
        });
        snippets("rust-workflows", "Rust · Results and tests", "Fallible functions, unit tests and iterator transforms.",
                 "Rust", {
            {"resultfn", "Fallible function", "fn process() -> Result<(), Box<dyn std::error::Error>> {\n    \vOk(())\n}"},
            {"rusttest", "Rust unit tests", "#[cfg(test)]\nmod tests {\n    use super::*;\n\n    #[test]\n    fn it_works() {\n        \vassert_eq!(1 + 1, 2);\n    }\n}"},
            {"itermap", "Collect transformed values", "let items: Vec<_> = input.iter()\n    .map(|item| \vitem)\n    .collect();"}
        });
        snippets("javascript-browser", "JavaScript · Browser APIs", "Async declarations, event handlers and fetch with error checking.",
                 "JavaScript", {
            {"asyncfn", "Async function", "async function \vloadData() {\n  return {};\n}"},
            {"eventlistener", "Subscribe to an event", "element.addEventListener(\"\vclick\", (event) => {\n  console.log(event.type);\n});"},
            {"fetchjson", "Fetch JSON and check for HTTP errors", "const response = await fetch(\vurl);\nif (!response.ok) throw new Error(response.statusText);\nconst data = await response.json();"}
        });
        snippets("typescript-types", "TypeScript · Type patterns", "Interfaces, type guards and discriminated union types.",
                 "TypeScript", {
            {"iface", "Export an interface", "export interface \vRecord {\n  readonly id: string;\n}"},
            {"tsguard", "Type guard for strings", "function \visText(value: unknown): value is string {\n  return typeof value === \"string\";\n}"},
            {"tsunion", "Discriminated union", "type \vResult<T> =\n  | { ok: true; value: T }\n  | { ok: false; error: Error };"}
        });
        snippets("html-accessible", "HTML · Accessible patterns", "Dialog markup, a responsive picture, and a keyboard skip link.",
                 "HTML", {
            {"htmldialog", "Accessible dialog", "<dialog aria-labelledby=\"dialog-title\">\n  <h2 id=\"dialog-title\">\vTitle</h2>\n  <form method=\"dialog\"><button>Close</button></form>\n</dialog>"},
            {"htmlpicture", "Responsive image", "<picture>\n  <source srcset=\"\vimage.webp\" type=\"image/webp\">\n  <img src=\"image.jpg\" alt=\"Description\" loading=\"lazy\">\n</picture>"},
            {"htmlskip", "Link directly to the main content", "<a href=\"#main\">Skip to content</a>\n<main id=\"main\">\n  \v\n</main>"}
        });
        snippets("css-layout", "CSS · Layout and accessibility", "A responsive grid, a visible focus ring and a reduced-motion rule.",
                 "CSS", {
            {"gridcards", "Responsive card grid", ".\vcards {\n  display: grid;\n  gap: 1rem;\n  grid-template-columns: repeat(auto-fit, minmax(min(100%, 16rem), 1fr));\n}"},
            {"focusring", "Visible focus ring", "\v.button:focus-visible {\n  outline: 2px solid currentColor;\n  outline-offset: 3px;\n}"},
            {"reducedmotion", "Respect reduced-motion preference", "@media (prefers-reduced-motion: reduce) {\n  .\vanimated {\n    animation: none;\n    transition: none;\n  }\n}"}
        });
        snippets("sql-queries", "SQL · Query patterns", "Common table expressions, EXISTS filters and window numbering.",
                 "SQL", {
            {"sqlcte", "Common table expression", "WITH \vfiltered AS (\n    SELECT id FROM records\n)\nSELECT id FROM filtered;"},
            {"sqlexists", "Filter with EXISTS", "SELECT id FROM \vrecords r\nWHERE EXISTS (\n    SELECT 1 FROM related s WHERE s.record_id = r.id\n);"},
            {"sqlrownum", "Number rows in each group", "SELECT \vid, ROW_NUMBER() OVER (PARTITION BY group_id ORDER BY created_at DESC) AS position\nFROM records;"}
        });

        auto theme = [&](QString id, QString name, QString description, const QByteArray &qss) {
            out.push_back({{id, name, CatalogCategory::Theme, description}, qss,
                          CatalogStrings::tr("After installing, choose this theme under Preferences → Appearance.")});
        };
        theme("graphite", "Graphite", "Neutral dark gray with a muted teal accent.",
              themeSheet("#191c1e", "#222629", "#3b464b", "#e0e7e6", "#2c4f4e", "#90d9cc").toUtf8());
        theme("deep-blue", "Deep blue", "Dark blue panels with a cool periwinkle accent.",
              themeSheet("#161b2b", "#20283b", "#3b4963", "#e2e9fa", "#374a73", "#a7bcff").toUtf8());
        theme("warm-slate", "Warm slate", "Slate panels and parchment text with a warm amber accent.",
              themeSheet("#25211e", "#302a25", "#53453c", "#eee1d4", "#654a35", "#f6be7a").toUtf8());
        theme("high-contrast", "High contrast", "Near-black panels, bright text and a clear yellow focus indicator.",
              themeSheet("#0a0a0a", "#171717", "#767676", "#ffffff", "#394d68", "#ffe76b").toUtf8());
        return out;
    }();
    return data;
}

const Entry *findEntry(const QString &id)
{
    for (const auto &entry : entries())
        if (entry.package.id == id) return &entry;
    return nullptr;
}

QString packagePath(const Entry &entry, const QString &root)
{
    if (root.isEmpty()) return {};
    const auto fileName = "catalog-" + entry.package.id;
    const QDir dir(root);
    switch (entry.package.category) {
    case CatalogCategory::Command: return dir.filePath("extensions/" + fileName + ".json");
    case CatalogCategory::Snippets: return dir.filePath("snippets/" + fileName + ".json");
    case CatalogCategory::Theme: return dir.filePath("themes/" + fileName + ".qss");
    }
    return {};
}

CatalogState stateFor(const Entry &entry, const QString &path)
{
    if (path.isEmpty()) return CatalogState::Modified;
    const QFileInfo info(path);
    if (info.isSymLink()) return CatalogState::Modified;
    if (!info.exists()) return CatalogState::Available;
    QFile file(path);
    if (info.isFile() && info.size() == entry.payload.size() && file.open(QIODevice::ReadOnly) &&
            file.read(entry.payload.size() + 1) == entry.payload) return CatalogState::Installed;
    return CatalogState::Modified;
}

struct SnippetCache {
    QHash<QString, QList<CatalogSnippet>> byLanguage;
    bool loaded = false;
};

SnippetCache &snippetCache()
{
    static SnippetCache cache;
    return cache;
}

void readSnippetPacks(SnippetCache &cache, const QString &root)
{
    cache = {{}, true};
    if (root.isEmpty()) return;
    const QDir dir(QDir(root).filePath("snippets"));
    if (!dir.exists()) return;
    const auto packs = dir.entryInfoList({"catalog-*.json"}, QDir::Files | QDir::NoSymLinks, QDir::Name);
    const QRegularExpression triggerPattern("^[A-Za-z_][A-Za-z_0-9]{0,39}$");
    QHash<QString, QSet<QString>> seen;
    for (const auto &pack : packs.mid(0, 100)) {
        QFile file(pack.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        const auto payload = file.read(manifestLimit + 1);
        if (payload.size() > manifestLimit) continue;
        const auto object = QJsonDocument::fromJson(payload).object();
        const auto language = object.value("language").toString().trimmed().toCaseFolded();
        if (language.isEmpty() || language.size() > 40 || object.value("schema").toInt() != 1) continue;
        auto &suggestions = cache.byLanguage[language];
        auto &triggers = seen[language];
        for (const auto &value : object.value("snippets").toArray()) {
            const auto snippet = value.toObject();
            const auto trigger = snippet.value("trigger").toString();
            const auto description = snippet.value("description").toString().simplified();
            const auto body = snippet.value("body").toString();
            if (!triggerPattern.match(trigger).hasMatch() || triggers.contains(trigger) || description.isEmpty() ||
                    description.size() > 150 || body.isEmpty() || body.size() > 4096 || body.contains(QChar::Null)) continue;
            if (suggestions.size() >= 100) break;
            suggestions.push_back({trigger, description, body});
            triggers.insert(trigger);
        }
    }
}

QString categoryLabel(CatalogCategory category)
{
    switch (category) {
    case CatalogCategory::Command: return CatalogStrings::tr("Command");
    case CatalogCategory::Snippets: return CatalogStrings::tr("Snippet pack");
    case CatalogCategory::Theme: return CatalogStrings::tr("Theme");
    }
    return {};
}

} // namespace

QList<CatalogPackage> extensionCatalog()
{
    QList<CatalogPackage> packages;
    for (const auto &entry : entries()) packages.push_back(entry.package);
    return packages;
}

CatalogState catalogPackageState(const QString &id, const QString &configRoot)
{
    const auto *entry = findEntry(id);
    return entry ? stateFor(*entry, packagePath(*entry, configDirectory(configRoot))) : CatalogState::Modified;
}

QString installCatalogPackage(const QString &id, const QString &configRoot)
{
    const auto *entry = findEntry(id);
    const auto directory = configDirectory(configRoot);
    if (!entry || directory.isEmpty()) return CatalogStrings::tr("Package or configuration directory unavailable.");
    const auto path = packagePath(*entry, directory);
    const auto current = stateFor(*entry, path);
    if (current == CatalogState::Installed) return {};
    if (current != CatalogState::Available) return CatalogStrings::tr("A local file already occupies this location.");

    const auto parent = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(parent)) return CatalogStrings::tr("Could not create %1.").arg(parent);
    // Stage and validate the write before publishing. QFile::rename refuses
    // overwrites, also when another Studio window installs at the same time.
    QTemporaryDir staging(QDir(parent).filePath(".catalog-XXXXXX"));
    if (!staging.isValid()) return CatalogStrings::tr("Could not stage the package.");
    const auto staged = staging.filePath("payload");
    QSaveFile file(staged);
    if (!file.open(QIODevice::WriteOnly) || file.write(entry->payload) != entry->payload.size() ||
            !file.commit()) return file.errorString();
    QFile complete(staged);
    if (!complete.rename(path)) return complete.errorString();
    if (entry->package.category == CatalogCategory::Snippets) reloadCatalogSnippets();
    return {};
}

QString removeCatalogPackage(const QString &id, const QString &configRoot)
{
    const auto *entry = findEntry(id);
    const auto directory = configDirectory(configRoot);
    if (!entry || directory.isEmpty()) return CatalogStrings::tr("Package or configuration directory unavailable.");
    const auto path = packagePath(*entry, directory);
    const auto current = stateFor(*entry, path);
    if (current == CatalogState::Available) return {};
    if (current != CatalogState::Installed) return CatalogStrings::tr("The installed file was changed locally.");
    QFile file(path);
    if (!file.remove()) return file.errorString();
    if (entry->package.category == CatalogCategory::Snippets) reloadCatalogSnippets();
    return {};
}

QList<CatalogSnippet> installedCatalogSnippets(const QString &language)
{
    return installedCatalogSnippets(language, {});
}

QList<CatalogSnippet> installedCatalogSnippets(const QString &language, const QString &configRoot)
{
    auto key = language.toCaseFolded();
    if (key == "javascript react" || key == "jsx") key = "javascript";
    if (key == "typescript react" || key == "tsx") key = "typescript";
    if (key == "scss") key = "css";
    if (!configRoot.isEmpty()) {
        SnippetCache packs;
        readSnippetPacks(packs, configRoot);
        return packs.byLanguage.value(key);
    }
    auto &cache = snippetCache();
    if (!cache.loaded) readSnippetPacks(cache, configDirectory());
    return cache.byLanguage.value(key);
}

void reloadCatalogSnippets()
{
    snippetCache() = {};
}

void showExtensionCatalog(QWidget *parent, std::function<void()> onChanged)
{
    QDialog dialog(parent);
    dialog.setObjectName("extensionCatalog");
    dialog.setWindowTitle(CatalogStrings::tr("Extension catalog"));
    dialog.resize(720, 430);
    auto *layout = new QVBoxLayout(&dialog);
    auto *filters = new QHBoxLayout;
    auto *search = new QLineEdit;
    search->setObjectName("catalogSearch");
    search->setPlaceholderText(CatalogStrings::tr("Search extensions"));
    search->setClearButtonEnabled(true);
    filters->addWidget(search, 1);
    auto *filter = new QComboBox;
    filter->setAccessibleName(CatalogStrings::tr("Extension category"));
    filter->addItems({CatalogStrings::tr("All categories"), CatalogStrings::tr("Commands"),
                      CatalogStrings::tr("Snippet packs"), CatalogStrings::tr("Themes")});
    filters->addWidget(filter);
    layout->addLayout(filters);

    auto *content = new QHBoxLayout;
    auto *list = new QListWidget;
    list->setObjectName("catalogList");
    list->setMinimumWidth(215);
    content->addWidget(list, 1);
    auto *details = new QVBoxLayout;
    auto *title = new QLabel;
    auto font = title->font();
    font.setBold(true);
    title->setFont(font);
    details->addWidget(title);
    auto *description = new QLabel;
    details->addWidget(description);
    auto *guide = new QLabel;
    details->addWidget(guide);
    auto *about = new QLabel;
    details->addWidget(about);
    auto *fileLabel = new QLabel;
    fileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->addWidget(fileLabel);
    for (auto *label : {title, description, guide, about, fileLabel}) {
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
    }
    details->addStretch();
    content->addLayout(details, 1);
    layout->addLayout(content, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *install = buttons->addButton(CatalogStrings::tr("Install"), QDialogButtonBox::ActionRole);
    install->setObjectName("catalogInstall");
    auto *remove = buttons->addButton(CatalogStrings::tr("Uninstall"), QDialogButtonBox::ActionRole);
    remove->setObjectName("catalogRemove");
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    const auto root = configDirectory();
    auto selectedId = [list] {
        const auto *item = list->currentItem();
        return item ? item->data(Qt::UserRole).toString() : QString();
    };
    auto showDetails = [&] {
        const auto *entry = findEntry(selectedId());
        if (!entry) {
            title->setText(CatalogStrings::tr("No matching extensions"));
            for (auto *label : {description, guide, about, fileLabel}) label->clear();
            install->setEnabled(false);
            remove->setEnabled(false);
            return;
        }
        const auto path = packagePath(*entry, root);
        const auto state = stateFor(*entry, path);
        title->setText(entry->package.name);
        description->setText(entry->package.description);
        guide->setText(entry->instructions);
        about->setText(CatalogStrings::tr("%1 · 1.0.0\nInteract Studio contributors · GPL-3.0-or-later")
                       .arg(categoryLabel(entry->package.category)));
        if (state == CatalogState::Modified)
            fileLabel->setText(CatalogStrings::tr("Local file retained: %1").arg(QDir::toNativeSeparators(path)));
        else
            fileLabel->clear();
        install->setEnabled(state == CatalogState::Available);
        remove->setEnabled(state == CatalogState::Installed);
    };
    auto refresh = [&] {
        const auto id = selectedId();
        const auto query = search->text().simplified().split(' ', Qt::SkipEmptyParts);
        list->clear();
        for (const auto &entry : entries()) {
            const auto &p = entry.package;
            const int category = filter->currentIndex();
            if (category && category != static_cast<int>(p.category) + 1) continue;
            const auto haystack = p.name + ' ' + p.description + ' ' + p.id;
            bool matches = true;
            for (const auto &term : query) {
                if (!haystack.contains(term, Qt::CaseInsensitive)) { matches = false; break; }
            }
            if (!matches) continue;
            const auto state = stateFor(entry, packagePath(entry, root));
            auto titleText = p.name;
            if (state == CatalogState::Installed) titleText += CatalogStrings::tr(" · Installed");
            if (state == CatalogState::Modified) titleText += CatalogStrings::tr(" · Local file");
            auto *item = new QListWidgetItem(titleText, list);
            item->setData(Qt::UserRole, p.id);
            if (p.id == id) list->setCurrentItem(item);
        }
        if (!list->currentItem() && list->count()) list->setCurrentRow(0);
        showDetails();
    };
    auto change = [&](bool installing) {
        const auto id = selectedId();
        if (id.isEmpty()) return;
        const auto problem = installing ? installCatalogPackage(id, root) : removeCatalogPackage(id, root);
        if (!problem.isEmpty()) {
            auto warning = QMessageBox(QMessageBox::Warning, CatalogStrings::tr("Extension catalog"),
                                       problem, QMessageBox::Ok, &dialog);
            warning.setTextFormat(Qt::PlainText);
            warning.exec();
        } else if (onChanged) {
            onChanged();
        }
        refresh();
    };

    QObject::connect(list, &QListWidget::currentRowChanged, &dialog, showDetails);
    QObject::connect(search, &QLineEdit::textChanged, &dialog, refresh);
    QObject::connect(filter, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, refresh);
    QObject::connect(install, &QPushButton::clicked, &dialog, [&] { change(true); });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [&] { change(false); });
    refresh();
    search->setFocus();
    dialog.exec();
}
