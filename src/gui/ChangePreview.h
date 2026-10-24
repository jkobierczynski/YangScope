// SPDX-License-Identifier: GPL-3.0-or-later
// Validation report and change preview shared by the NETCONF and gNMI panels.
#pragma once

#include "core/EditConfig.h"

#include <QString>

class QWidget;

namespace ys {

QString changeLines(const QVector<EditChange> &changes);
/// HTML report of a validation: verdict, errors, warnings and changes.
QString validationHtml(const EditValidation &v, const QString &operationsLabel = {});
/// Modal dialog listing the changes; returns true when the user confirms.
bool confirmChangesDialog(QWidget *parent, const QString &title, const QString &intro,
                          const QVector<EditChange> &changes, const QStringList &warnings,
                          const QString &okText, bool warnOnly);

} // namespace ys
