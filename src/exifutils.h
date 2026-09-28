// SPDX-FileCopyrightText: 2026 RAWfish Contributors
//
// SPDX-License-Identifier: BSD-3-Clause

#ifndef EXIFUTILS_H
#define EXIFUTILS_H

#include <QString>

namespace ExifUtils {

bool writeJpegExifFromJsonFile(const QString &jpegPath,
                               const QString &metadataPath,
                               QString *error);

}

#endif
