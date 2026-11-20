#include "icons.h"
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

QIcon studioIcon(Glyph glyph, QColor color) {
    QPixmap canvas(48,48);
    canvas.fill(Qt::transparent);
    QPainter p(&canvas);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(2,2);
    p.setPen(QPen(color, 1.55, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    auto path = [&](std::initializer_list<QPointF> points) {
        QPainterPath line;
        auto it = points.begin();
        line.moveTo(*it++);
        while (it != points.end()) line.lineTo(*it++);
        p.drawPath(line);
    };
    switch (glyph) {
    case Glyph::Mark:
        p.setPen(Qt::NoPen); p.setBrush(color);
        p.drawPolygon(QPolygonF({{2,16},{9,3},{13,3},{6,16}}));
        p.drawPolygon(QPolygonF({{9,21},{17,6},{21,6},{13,21}}));
        p.drawPolygon(QPolygonF({{15,21},{19,14},{23,14},{19,21}})); break;
    case Glyph::Files:
        path({{5,4},{13,4},{16,7},{16,16},{5,16},{5,4}});
        path({{9,20},{20,20},{20,10}}); break;
    case Glyph::Search:
        p.drawEllipse(QRectF(3.5,3.5,12,12)); path({{14,14},{20,20}}); break;
    case Glyph::Branch:
        p.drawEllipse(QPointF(7,5),2,2); p.drawEllipse(QPointF(7,19),2,2);
        p.drawEllipse(QPointF(17,7),2,2); path({{7,7},{7,17}});
        path({{17,9},{17,11},{14,14},{7,14}}); break;
    case Glyph::Console:
        p.drawRoundedRect(QRectF(2.5,4,19,16),2,2);
        path({{6,9},{9,12},{6,15}}); path({{12,15},{16,15}}); break;
    case Glyph::Plug:
        path({{6,4},{6,8},{4,8},{4,11},{7,14},{10,14},{13,11},{13,8},{11,8},{11,4}});
        path({{8.5,14},{8.5,17},{11,20},{14,20},{17,17},{17,13},{20,10}}); break;
    case Glyph::Settings:
        for (int x : {5,12,19}) {
            int y = x==12 ? 15 : 8;
            path({{double(x),4},{double(x),double(y-2)}});
            path({{double(x),double(y+2)},{double(x),20}});
            p.drawEllipse(QPointF(x,y),2,2);
        } break;
    case Glyph::Command:
        path({{10,9},{15,9},{15,15},{9,15},{9,9}});
        for (auto c : {QPointF(6.5,6.5),QPointF(17.5,6.5),QPointF(6.5,17.5),QPointF(17.5,17.5)})
            p.drawEllipse(c,2.5,2.5);
        break;
    case Glyph::Folder:
        path({{3,6},{9,6},{11,8},{21,8},{21,18},{3,18},{3,6}}); break;
    case Glyph::Split:
        p.drawRoundedRect(QRectF(3,4,18,16),1.5,1.5); path({{12,4},{12,20}}); break;
    case Glyph::Play:
        path({{7,4},{19,12},{7,20},{7,4}}); break;
    case Glyph::Layout:
        p.drawRoundedRect(QRectF(2.5,4,19,16),1.5,1.5);
        path({{8,4},{8,20}}); path({{8,15},{21,15}}); break;
    }
    canvas.setDevicePixelRatio(2);
    return QIcon(canvas);
}
