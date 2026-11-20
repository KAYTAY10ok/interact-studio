#pragma once
#include <QString>
#include <functional>
class QWidget;

void quickOpenFile(QWidget *parent, QString root, std::function<void(QString)> open);
