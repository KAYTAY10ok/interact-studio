#include "catalog.h"

#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>

namespace {

int fail(const char *detail)
{
    qCritical("%s", detail);
    return 1;
}

QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir area;
    if (!area.isValid()) return fail("Could not create temporary extension configuration");
    const QString root = area.path();

    const auto all = extensionCatalog();
    QSet<QString> unique;
    int commands = 0;
    int snippetPacks = 0;
    int themes = 0;
    for (const auto &package : all) {
        unique.insert(package.id);
        if (catalogPackageState(package.id, root) != CatalogState::Available ||
                !installCatalogPackage(package.id, root).isEmpty() ||
                catalogPackageState(package.id, root) != CatalogState::Installed)
            return fail("Catalog package could not be installed");
        const QString fileName = "catalog-" + package.id;
        switch (package.category) {
        case CatalogCategory::Command: {
            ++commands;
            const auto manifest = QJsonDocument::fromJson(
                read(area.filePath("extensions/" + fileName + ".json"))).object();
            if (manifest.value("id").toString() != "studio." + package.id ||
                    manifest.value("name").toString() != package.name ||
                    manifest.value("program").toString().isEmpty() ||
                    manifest.value("arguments").toArray().isEmpty())
                return fail("Installed command is not a usable extension manifest");
            break;
        }
        case CatalogCategory::Snippets:
            ++snippetPacks;
            break;
        case CatalogCategory::Theme:
            ++themes;
            if (!read(area.filePath("themes/" + fileName + ".qss")).contains("QPlainTextEdit"))
                return fail("Installed theme does not style the editor");
            break;
        }
    }
    if (unique.size() != all.size() || commands != 18 || snippetPacks != 8 || themes != 4)
        return fail("Missing or duplicate packages in the initial catalog");

    const auto historyPath = area.filePath("extensions/catalog-git-history.json");
    auto history = QJsonDocument::fromJson(read(historyPath)).object();
    if (history.value("program").toString() != "git" ||
            history.value("arguments").toArray().first().toString() != "log")
        return fail("Git history extension is not an executable command");
    QProcess git;
    git.setWorkingDirectory(root);
    auto runGit = [&](const QStringList &arguments) {
        git.start("git", arguments);
        return git.waitForFinished(5000) && git.exitStatus() == QProcess::NormalExit && git.exitCode() == 0;
    };
    if (!runGit({"init", "-q"}) || !runGit({"-c", "user.name=Catalog Test",
             "-c", "user.email=test@example.invalid", "-c", "commit.gpgsign=false",
             "commit", "--allow-empty", "-qm", "catalog fixture"}))
        return fail("Could not create a repository for the installed Git command");
    QStringList historyArguments;
    for (const auto &arg : history.value("arguments").toArray()) historyArguments << arg.toString();
    if (!runGit(historyArguments) || !git.readAllStandardOutput().contains("catalog fixture"))
        return fail("The installed Git history command did not return the fixture history");

    QPlainTextEdit editor;
    editor.setStyleSheet(QString::fromUtf8(read(area.filePath("themes/catalog-high-contrast.qss"))));
    editor.ensurePolished();
    if (editor.palette().color(QPalette::Base) != QColor("#0a0a0a") ||
            editor.palette().color(QPalette::Text) != QColor("#ffffff"))
        return fail("Installed editor theme did not apply its background and text colors");

    const auto html = installedCatalogSnippets("html", root);
    if (html.size() != 3 || html.first().trigger != "htmldialog" ||
            !html.first().body.contains("<dialog") || !html.first().body.contains(QChar('')))
        return fail("Installed HTML snippet pack is not available for editor completion");
    if (installedCatalogSnippets("typescript react", root).size() != 3)
        return fail("Language-family snippet alias is unavailable");
    if (!removeCatalogPackage("html-accessible", root).isEmpty() ||
            !installedCatalogSnippets("HTML", root).isEmpty())
        return fail("Uninstalled snippets remained in the completion source");

    const QByteArray personal = R"({"id":"my.history","name":"My history","program":"git","arguments":["log"]})";
    QFile edited(historyPath);
    if (!edited.open(QIODevice::WriteOnly | QIODevice::Truncate) || edited.write(personal) != personal.size())
        return fail("Could not edit a package file for overwrite tests");
    edited.close();
    if (catalogPackageState("git-history", root) != CatalogState::Modified ||
            installCatalogPackage("git-history", root).isEmpty() ||
            removeCatalogPackage("git-history", root).isEmpty() || read(historyPath) != personal)
        return fail("Install or uninstall overwrote a custom local file");

    if (installCatalogPackage("../other", root).isEmpty())
        return fail("The catalog accepted an unknown package id");
    qInfo("PASS: 30 catalog packages, command manifests, themed editor, installed snippets, local edit protection");
}
