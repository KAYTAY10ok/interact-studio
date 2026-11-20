#pragma once
#include <QColor>
#include <QIcon>

enum class Glyph { Mark, Files, Search, Branch, Console, Plug, Settings, Command, Folder, Split, Play, Layout };
QIcon studioIcon(Glyph glyph, QColor color = QColor("#afb5b9"));
