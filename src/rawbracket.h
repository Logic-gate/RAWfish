// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <QStringList>
namespace RawBracket {
// Exactly two metadata paths, long followed by short. Produces JPEG + JSON.
bool render(const QString &target, const QStringList &metadataPaths, int quality, QString *error);
}
