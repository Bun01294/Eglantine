// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include <QFrame>
#include <QHash>
#include <QPixmap>

#include "common/common_types.h"
#include "common/settings_enums.h"

class HomeBackground;
class QButtonGroup;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QSlider;
class QToolButton;

/// 16:9 preview of the current background; click to browse, or drop a file onto it.
class BackgroundDropZone : public QFrame {
    Q_OBJECT

public:
    explicit BackgroundDropZone(HomeBackground* background, QWidget* parent = nullptr);

    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;

signals:
    void BrowseRequested();
    void FileDropped(const QString& path);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    HomeBackground* background;
    bool hovered = false;
    bool drag_active = false;
};

/// Sidebar opened from the home screen's context menu. Every change applies immediately.
class HomeCustomizePanel : public QFrame {
    Q_OBJECT

public:
    static constexpr int PanelWidth = 320;

    explicit HomeCustomizePanel(HomeBackground* background, QWidget* parent = nullptr);

    QSize sizeHint() const override;

    /// Reflects the current settings (mode, icon size, background, recents).
    void SyncFromSettings();

signals:
    /// Empty path means no background.
    void BackgroundChosen(const QString& path);
    void RecentBackgroundRemoved(const QString& path);
    void GameListModeRequested(Settings::GameListMode mode);
    void GameIconSizeRequested(u32 size);
    void CloseRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void Browse();
    void RefreshRecents();
    void RefreshModeIcons();
    void RetranslateUI();
    void UpdateIconSizeLabel();
    QPixmap Thumbnail(const QString& path);
    void OnRecentContextMenu(const QPoint& pos);

    HomeBackground* background;

    QLabel* title_label = nullptr;
    QToolButton* close_button = nullptr;
    QLabel* background_label = nullptr;
    BackgroundDropZone* drop_zone = nullptr;
    QLabel* recent_label = nullptr;
    QListWidget* recent_list = nullptr;
    QLabel* mode_label = nullptr;
    QButtonGroup* mode_group = nullptr;
    std::array<QToolButton*, 3> mode_buttons{};
    QLabel* icon_size_title = nullptr;
    QLabel* icon_size_value = nullptr;
    QSlider* icon_size_slider = nullptr;

    QHash<QString, QPixmap> thumbnail_cache;
    bool syncing = false;
};
