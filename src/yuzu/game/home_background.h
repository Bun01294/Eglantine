// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QImage>
#include <QObject>
#include <QString>

class QMovie;
class QPainter;
class QRect;
class QSize;

#if YUZU_USE_QT_MULTIMEDIA
class QMediaPlayer;
class QVideoSink;
#endif

/// The home screen background: a still image, an animated image (GIF, WebP) or, when built with
/// QtMultimedia, a muted looping video. Consumers paint Frame() and repaint on FrameChanged.
class HomeBackground : public QObject {
    Q_OBJECT

public:
    explicit HomeBackground(QObject* parent = nullptr);
    ~HomeBackground() override;

    /// Loads `path`; an empty path clears the background. Returns false if it could not be loaded.
    bool SetSource(const QString& path);
    const QString& Source() const {
        return source;
    }
    bool IsEmpty() const {
        return frame.isNull();
    }
    const QImage& Frame() const {
        return frame;
    }

    /// Pauses animation and video playback while nothing shows the background.
    void SetActive(bool active);

    static bool IsVideo(const QString& path);
    static bool CanLoad(const QString& path);
    static QString FileDialogFilter();
    /// A small still of `path` for previews; null if it cannot be decoded without playing it.
    static QImage LoadThumbnail(const QString& path, const QSize& size);
    /// Scales `image` to cover `target`, cropping the overflow evenly on both sides.
    static void PaintCover(QPainter& painter, const QRect& target, const QImage& image);

signals:
    void FrameChanged();

private:
    void Clear();
    void UpdatePlayback();

    QString source;
    QImage frame;
    bool active = true;

    QMovie* movie = nullptr;
#if YUZU_USE_QT_MULTIMEDIA
    QMediaPlayer* player = nullptr;
    QVideoSink* sink = nullptr;
#endif
};
