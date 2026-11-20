#pragma once

#include <QFontDatabase>
#include <QFontInfo>

inline QFont defaultEditorFont() {
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setStyleHint(QFont::Monospace);
    if (!QFontInfo(font).fixedPitch()) font.setFamily(QStringLiteral("monospace"));
    font.setPointSize(11);
    return font;
}
