// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

#include <QDialog>

#include "core/file_sys/music_extract.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

// Asks where the music of a NUP archive or loose dump goes and how it is written:
// output folder, WAV or FLAC, subfolders by kind or a flat directory.
class MusicExtractDialog : public QDialog {
    Q_OBJECT
public:
    MusicExtractDialog(const FileSys::Music::SourceInfo& info, QWidget* parent = nullptr);

    std::filesystem::path OutDir() const;
    FileSys::Music::Layout Layout() const;
    FileSys::Music::Encoding Encoding() const;

private:
    void Browse();
    void UpdateOkEnabled();

    QLineEdit* out_edit;
    QComboBox* encoding_combo;
    QRadioButton* layout_tree;
    QRadioButton* layout_flat;
    QPushButton* ok_button;
};
