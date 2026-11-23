#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>

// Lines and columns are zero-based. Columns count UTF-16 code units.
struct LanguageSnapshot {
    QString path;
    QString text;
    int revision = 0;
    int line = 0;
    int column = 0;
};

struct SemanticCompletion {
    QString label;
    QString detail;
    QString insertText;
    // -1 indicates that the editor should replace the current token.
    int startLine = -1;
    int startColumn = -1;
    int endLine = -1;
    int endColumn = -1;
};

struct SemanticDiagnostic {
    QString message;
    int startLine = 0;
    int startColumn = 0;
    int endLine = 0;
    int endColumn = 0;
    int severity = 0;
};

constexpr int semanticDocumentLimit = 256 * 1024;

class LanguageService final : public QObject {
public:
    explicit LanguageService(QString workspace, QObject *parent = nullptr);
    ~LanguageService() override;

    // When canceled or superseded, a request's reply is not delivered.
    void complete(const LanguageSnapshot &snapshot,
                  std::function<void(QList<SemanticCompletion>)> onItems,
                  bool explicitRequest = false);
    void signature(const LanguageSnapshot &snapshot, std::function<void(QString)> onSignature);
    void analyze(const LanguageSnapshot &snapshot);
    // An edit can update an existing session, but never starts a process.
    void update(const LanguageSnapshot &snapshot);
    void closeDocument(QString path);

    std::function<void(QString, int, QList<SemanticDiagnostic>)> onDiagnostics;
    std::function<void(QString)> onStatus;

private:
    class State;
    std::unique_ptr<State> state;
};
