// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QTextLayout>
#include "game_card.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"

GameCard::GameCard(QObject* parent) : QStyledItemDelegate{parent} {
    setObjectName("GameCard");
}

namespace {

constexpr int MaxTitleLines = 2;

/// The platform border for a game icon, loaded once per process.
QPixmap GetPlatformBorder(bool is_switch2) {
    const QString key = is_switch2 ? QStringLiteral("eglantine-border-switch2")
                                   : QStringLiteral("eglantine-border-switch1");
    QPixmap border;
    if (QPixmapCache::find(key, &border)) {
        return border;
    }

    border.load(is_switch2 ? QStringLiteral(":/game_borders/switch2.png")
                           : QStringLiteral(":/game_borders/switch1.png"));
    if (!border.isNull()) {
        QPixmapCache::insert(key, border);
    }
    return border;
}

/// Wraps `text` into at most `max_lines` centred lines, eliding the last one if it overflows.
void DrawTitle(QPainter* painter, const QRect& rect, const QString& text, const QFont& font,
               int max_lines) {
    const QFontMetrics metrics(font);
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);

    QStringList lines;
    layout.beginLayout();
    while (lines.size() < max_lines) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) {
            break;
        }
        line.setLineWidth(rect.width());
        if (lines.size() == max_lines - 1) {
            lines << metrics.elidedText(text.mid(line.textStart()).trimmed(), Qt::ElideRight,
                                        rect.width());
        } else {
            lines << text.mid(line.textStart(), line.textLength()).trimmed();
        }
    }
    layout.endLayout();

    int y = rect.top();
    for (const QString& line : std::as_const(lines)) {
        if (y + metrics.height() > rect.bottom() + 1) {
            break;
        }
        painter->drawText(QRect(rect.left(), y, rect.width(), metrics.height()),
                          Qt::AlignHCenter | Qt::AlignTop, line);
        y += metrics.lineSpacing();
    }
}

} // namespace

void GameCard::paint(QPainter* painter, const QStyleOptionViewItem& option,
                     const QModelIndex& index) const {
    if (!index.isValid())
        return;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    constexpr int cardCornerRadius = 18;

    const QRect cardRect = getCardRect(option, index);
    const QPalette& palette = option.palette;
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;

    // No card frame: only hover and selection get a soft backdrop.
    if (selected || hovered) {
        QColor backdrop = palette.highlight().color();
        backdrop.setAlpha(selected ? 110 : 45);
        painter->setPen(Qt::NoPen);
        painter->setBrush(backdrop);
        painter->drawRoundedRect(cardRect, cardCornerRadius, cardCornerRadius);
    }

    const u32 icon_size = UISettings::values.game_icon_size.GetValue();
    QPixmap icon_pixmap = index.data(Qt::DecorationRole).value<QPixmap>();

    QRect iconRect;
    if (!icon_pixmap.isNull() && icon_size > 0) {
        QSize scaled = icon_pixmap.size();
        scaled.scale(icon_size, icon_size, Qt::KeepAspectRatio);

        iconRect = {cardRect.left() + (cardRect.width() - scaled.width()) / 2,
                    cardRect.top() + cardMargin - 1, scaled.width(), scaled.height()};

        // The platform borders round their outer corners at ~8% of their width; matching that
        // keeps icon pixels from showing past the border's corners.
        const qreal icon_radius = iconRect.width() * 0.08;

        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

        // Soft drop shadow: stacked translucent rects, darkest under the icon's centre.
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0, 0, 0, 14));
        for (int spread = 1; spread <= 6; ++spread) {
            const QRectF shadow = QRectF(iconRect).adjusted(-spread, -spread + 3, spread,
                                                            spread + 3);
            painter->drawRoundedRect(shadow, icon_radius + spread, icon_radius + spread);
        }

        painter->save();
        QPainterPath clip_path;
        clip_path.addRoundedRect(iconRect, icon_radius, icon_radius);
        painter->setClipPath(clip_path);
        painter->drawPixmap(iconRect, icon_pixmap);
        painter->restore();

        // The platform border sits on top of the icon; its centre is transparent.
        if (m_showPlatformBorders) {
            const QPixmap border =
                GetPlatformBorder(index.data(GameListItemPath::Switch2Role).toBool());
            if (!border.isNull()) {
                painter->drawPixmap(iconRect, border);
            }
        }

        if (selected) {
            painter->setPen(QPen(palette.highlight().color(), 2));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(QRectF(iconRect).adjusted(-1, -1, 1, 1), icon_radius,
                                     icon_radius);
        }
    } else {
        iconRect = {cardRect.left() + cardMargin, cardRect.top() + cardMargin,
                    static_cast<int>(icon_size), static_cast<int>(icon_size)};
    }

    if (UISettings::values.show_game_name.GetValue()) {
        QRect textRect = cardRect;
        textRect.setTop(iconRect.bottom() + cardMargin);
        textRect.adjust(cardMargin / 2, 0, -cardMargin / 2, -cardMargin / 2);

        const QString title =
            index.data(Qt::DisplayRole).toString().split(QLatin1Char('\n')).first();

        QFont font = option.font;
        font.setBold(true);
        font.setPixelSize(std::max(11.0, std::sqrt(static_cast<double>(icon_size))));
        painter->setFont(font);
        painter->setPen(palette.text().color());

        DrawTitle(painter, textRect, title, font, MaxTitleLines);
    }

    painter->restore();
}

QRect GameCard::getCardRect(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const int cell_width = option.rect.width();

    const int card_width = cell_width - m_padding;

    int card_left, card_top, card_height;

    if (m_columns >= 1) {
        // grid mode

        // center everything in-line, such that the leftmost and rightmost cards
        // have ~ equal padding to the edge of the viewport
        // spacing between each card is larger, but equal to each other
        const int column = index.row() % m_columns;

        const int row_width = m_columns * cell_width;
        const int total_gap = row_width - cardMargin * 2 - m_columns * card_width;
        const int gap = (m_columns > 1) ? (total_gap / (m_columns - 1)) : 0;
        card_left =
            option.rect.left() - column * cell_width + cardMargin + column * (card_width + gap) + 4;

        // fill cell vertically
        card_top = option.rect.top() + cardMargin;
        card_height = option.rect.height() - cardMargin - 1;
    } else {
        // carousel mode
        card_left = option.rect.left() + cardMargin + 4;

        // the delegate itself takes up the full height, but the card itself
        // gets centered
        const int content_height = m_contentSize.height() - cardMargin;
        const int cell_height = option.rect.height();

        card_height = std::min(content_height, cell_height - cardMargin * 2) - 1;
        card_top = option.rect.top() + (cell_height - card_height) / 2;
    }

    return QRect(card_left, card_top, card_width - cardMargin, card_height);
}

bool GameCard::hitTest(const QPoint& point, const QModelIndex& index, const QWidget* widget,
                       const QRect& cellRect) const {
    QStyleOptionViewItem option;
    option.initFrom(widget);
    option.rect = cellRect;
    return getCardRect(option, index).contains(point);
}

QSize GameCard::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    return m_size;
}

void GameCard::setSize(const QSize& newSize, const QSize& contentSize, const int padding,
                       const int columns) {
    m_size = newSize;
    m_contentSize = contentSize;
    m_padding = padding;
    m_columns = columns;
}

void GameCard::setShowPlatformBorders(bool show) {
    m_showPlatformBorders = show;
}
