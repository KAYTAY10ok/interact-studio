#pragma once

#include <QString>
#include <functional>

class QWidget;

// Opens a language server for one lookup. Positions in both directions are
// zero-based, with columns measured in UTF-16 code units.
void goToDefinition(QWidget *window, QString root, QString path, QString source,
                    int line, int column, std::function<void(QString, int, int)> onLocation);
