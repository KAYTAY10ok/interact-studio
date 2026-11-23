#pragma once

#include <QString>
#include <functional>

class QPlainTextEdit;
class QWidget;

void formatDocument(QWidget *owner, QPlainTextEdit *view, const QString &path,
                    std::function<void(QString)> report);
