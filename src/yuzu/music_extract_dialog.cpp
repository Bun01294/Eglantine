// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/music_extract_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "common/fs/fs_util.h"

namespace {

QString Sanitized(std::string_view name) {
    QString out;
    for (const char c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
        out.append(QLatin1Char(plain ? c : '_'));
    }
    while (out.endsWith(u' ')) {
        out.chop(1);
    }
    return out;
}

} // namespace

MusicExtractDialog::MusicExtractDialog(const FileSys::Music::SourceInfo& info,
                                       QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Extract Music"));
    setModal(true);
    setMinimumWidth(520);

    const QString title = QString::fromStdString(info.game_title).trimmed();
    auto* summary = new QLabel(
        tr("%1 music stream(s), about %2 MB, found in %3.")
            .arg(info.stream_count)
            .arg(info.stream_bytes / (1024 * 1024))
            .arg(title.isEmpty() ? tr("this game") : title));
    summary->setWordWrap(true);

    auto* out_label = new QLabel(tr("Output folder:"));
    out_edit = new QLineEdit;
    const QString default_dir =
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + QDir::separator() +
        (title.isEmpty() ? tr("game music") : Sanitized(title.toStdString()));
    out_edit->setText(QDir::toNativeSeparators(default_dir));
    auto* browse = new QPushButton(tr("Browse..."));
    connect(browse, &QPushButton::clicked, this, &MusicExtractDialog::Browse);

    auto* out_row = new QHBoxLayout;
    out_row->setContentsMargins(0, 0, 0, 0);
    out_row->addWidget(out_label);
    out_row->addWidget(out_edit, 1);
    out_row->addWidget(browse);

    encoding_combo = new QComboBox;
    encoding_combo->addItem(tr("WAV (PCM 16-bit)"), int(FileSys::Music::Encoding::Wav));
    if (FileSys::Music::HasFlacSupport()) {
        encoding_combo->addItem(tr("FLAC (lossless)"), int(FileSys::Music::Encoding::Flac));
    }

    layout_tree =
        new QRadioButton(tr("Sort into subfolders (bgm, cutscene, title, segments, stems)"));
    layout_tree->setChecked(true);
    layout_flat = new QRadioButton(tr("Flat - every track directly in the output folder"));

    auto* structure_box = new QVBoxLayout;
    structure_box->setContentsMargins(0, 0, 0, 0);
    structure_box->setSpacing(4);
    structure_box->addWidget(layout_tree);
    structure_box->addWidget(layout_flat);
    auto* structure_holder = new QWidget;
    structure_holder->setLayout(structure_box);

    auto* form = new QFormLayout;
    form->addRow(tr("Encoding:"), encoding_combo);
    form->addRow(tr("Structure:"), structure_holder);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    ok_button = buttons->button(QDialogButtonBox::Ok);
    ok_button->setText(tr("Extract"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(24, 24, 24, 20);
    column->setSpacing(14);
    column->addWidget(summary);
    column->addLayout(out_row);
    column->addLayout(form);
    column->addStretch();
    column->addWidget(buttons);

    connect(out_edit, &QLineEdit::textChanged, this, &MusicExtractDialog::UpdateOkEnabled);
    UpdateOkEnabled();
}

void MusicExtractDialog::Browse() {
    const QString current = out_edit->text();
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Music Output Folder"),
        QDir(current).dirName().isEmpty() ? QString{} : current);
    if (!dir.isEmpty()) {
        out_edit->setText(QDir::toNativeSeparators(dir));
    }
}

void MusicExtractDialog::UpdateOkEnabled() {
    ok_button->setEnabled(!out_edit->text().trimmed().isEmpty());
}

std::filesystem::path MusicExtractDialog::OutDir() const {
    return std::filesystem::path{out_edit->text().trimmed().toStdString()};
}

FileSys::Music::Layout MusicExtractDialog::Layout() const {
    return layout_flat->isChecked() ? FileSys::Music::Layout::Flat
                                   : FileSys::Music::Layout::ByKind;
}

FileSys::Music::Encoding MusicExtractDialog::Encoding() const {
    return static_cast<FileSys::Music::Encoding>(encoding_combo->currentData().toInt());
}
