#pragma once

#include <QList>
#include <QString>
#include <functional>

class QWidget;

struct CatalogSnippet {
    QString trigger;
    QString description;
    QString body;
};

enum class CatalogCategory { Command, Snippets, Theme };
enum class CatalogState { Available, Installed, Modified };

struct CatalogPackage {
    QString id;
    QString name;
    CatalogCategory category;
    QString description;
};

QList<CatalogPackage> extensionCatalog();
CatalogState catalogPackageState(const QString &id, const QString &configRoot = {});
// Return an empty string on success, or a message explaining the failure.
QString installCatalogPackage(const QString &id, const QString &configRoot = {});
QString removeCatalogPackage(const QString &id, const QString &configRoot = {});

QList<CatalogSnippet> installedCatalogSnippets(const QString &language);
QList<CatalogSnippet> installedCatalogSnippets(const QString &language, const QString &configRoot);
void reloadCatalogSnippets();
void showExtensionCatalog(QWidget *parent, std::function<void()> onChanged = {});
