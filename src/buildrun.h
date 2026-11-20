#pragma once

#include <QStringList>
#include <functional>

class QWidget;

// Build the saved standalone source, then hand the binary to the terminal.
void buildAndRun(QWidget *parent, QString workspace, QString path,
                 std::function<void(QString, QStringList)> launch);
