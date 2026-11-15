// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QFileInfo>
#include <QImageReader>
#include <QMovie>
#include <QPainter>
#include <QStringList>

#if YUZU_USE_QT_MULTIMEDIA
#include <QMediaPlayer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>
#endif

#include "yuzu/game/home_background.h"

namespace {

// Larger stills are downscaled on load; the background never needs more than a 4K screen.
constexpr int MaxStillDimension = 3840;

const QStringList& VideoSuffixes() {
    static const QStringList suffixes{QStringLiteral("mp4"), QStringLiteral("m4v"),
                                      QStringLiteral("webm"), QStringLiteral("mkv"),
                                      QStringLiteral("mov"), QStringLiteral("avi")};
    return suffixes;
}

bool IsAnimatedImage(const QString& path) {
    QImageReader reader(path);
    return reader.supportsAnimation() && reader.imageCount() != 1;
}

QImage ReadStill(const QString& path, const QSize& bound) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid() && (size.width() > bound.width() || size.height() > bound.height())) {
        reader.setScaledSize(size.scaled(bound, Qt::KeepAspectRatio));
    }
    return reader.read();
}

} // namespace

HomeBackground::HomeBackground(QObject* parent) : QObject{parent} {}

HomeBackground::~HomeBackground() = default;

bool HomeBackground::IsVideo(const QString& path) {
    return VideoSuffixes().contains(QFileInfo(path).suffix().toLower());
}

bool HomeBackground::CanLoad(const QString& path) {
    if (!QFileInfo(path).isFile()) {
        return false;
    }
    if (IsVideo(path)) {
#if YUZU_USE_QT_MULTIMEDIA
        return true;
#else
        return false;
#endif
    }
    return QImageReader(path).canRead();
}

QString HomeBackground::FileDialogFilter() {
    QStringList patterns;
    for (const QByteArray& format : QImageReader::supportedImageFormats()) {
        patterns << QStringLiteral("*.%1").arg(QString::fromLatin1(format));
    }
#if YUZU_USE_QT_MULTIMEDIA
    for (const QString& suffix : VideoSuffixes()) {
        patterns << QStringLiteral("*.%1").arg(suffix);
    }
#endif
    return tr("Images and videos (%1)").arg(patterns.join(QLatin1Char{' '}));
}

QImage HomeBackground::LoadThumbnail(const QString& path, const QSize& size) {
    if (IsVideo(path)) {
        return {};
    }
    // Read at twice the cell size so the cover crop stays sharp on HiDPI screens.
    return ReadStill(path, size * 2);
}

void HomeBackground::PaintCover(QPainter& painter, const QRect& target, const QImage& image) {
    if (image.isNull() || target.isEmpty()) {
        return;
    }
    const QSize scaled = image.size().scaled(target.size(), Qt::KeepAspectRatioByExpanding);
    const double scale = static_cast<double>(scaled.width()) / image.width();
    const QSizeF visible(target.width() / scale, target.height() / scale);
    const QRectF source((image.width() - visible.width()) / 2.0,
                        (image.height() - visible.height()) / 2.0, visible.width(),
                        visible.height());
    painter.drawImage(QRectF(target), image, source);
}

bool HomeBackground::SetSource(const QString& path) {
    if (path == source && (path.isEmpty() || !frame.isNull())) {
        return true;
    }
    Clear();
    if (path.isEmpty()) {
        emit FrameChanged();
        return true;
    }
    if (!CanLoad(path)) {
        emit FrameChanged();
        return false;
    }

    source = path;
    if (IsVideo(path)) {
#if YUZU_USE_QT_MULTIMEDIA
        player = new QMediaPlayer(this);
        sink = new QVideoSink(this);
        player->setVideoSink(sink);
        player->setLoops(QMediaPlayer::Infinite);
        // No QAudioOutput is attached, so the video plays muted.
        connect(sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& video) {
            QImage image = video.toImage();
            if (!image.isNull()) {
                frame = std::move(image);
                emit FrameChanged();
            }
        });
        player->setSource(QUrl::fromLocalFile(path));
#endif
    } else if (IsAnimatedImage(path)) {
        movie = new QMovie(path, {}, this);
        movie->setCacheMode(QMovie::CacheNone);
        connect(movie, &QMovie::frameChanged, this, [this] {
            frame = movie->currentImage();
            emit FrameChanged();
        });
        movie->jumpToFrame(0);
        frame = movie->currentImage();
    } else {
        frame = ReadStill(path, QSize(MaxStillDimension, MaxStillDimension));
        if (frame.isNull()) {
            source.clear();
            emit FrameChanged();
            return false;
        }
    }

    UpdatePlayback();
    emit FrameChanged();
    return true;
}

void HomeBackground::SetActive(bool active_) {
    if (active == active_) {
        return;
    }
    active = active_;
    UpdatePlayback();
}

void HomeBackground::UpdatePlayback() {
    if (movie) {
        if (!active) {
            movie->setPaused(true);
        } else if (movie->state() == QMovie::Paused) {
            movie->setPaused(false);
        } else if (movie->state() == QMovie::NotRunning) {
            movie->start();
        }
    }
#if YUZU_USE_QT_MULTIMEDIA
    if (player) {
        if (active) {
            player->play();
        } else {
            player->pause();
        }
    }
#endif
}

void HomeBackground::Clear() {
    if (movie) {
        movie->disconnect(this);
        movie->stop();
        movie->deleteLater();
        movie = nullptr;
    }
#if YUZU_USE_QT_MULTIMEDIA
    if (player) {
        player->stop();
        player->deleteLater();
        player = nullptr;
    }
    if (sink) {
        sink->disconnect(this);
        sink->deleteLater();
        sink = nullptr;
    }
#endif
    source.clear();
    frame = {};
}
