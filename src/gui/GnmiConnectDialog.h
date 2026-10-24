// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/GnmiTypes.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace ys {

/// gNMI target profiles: address, TLS settings and username are stored,
/// passwords never.
class GnmiConnectDialog : public QDialog {
    Q_OBJECT
public:
    explicit GnmiConnectDialog(QWidget *parent = nullptr);
    gnmi::ConnectParams params() const;
    QString profileName() const;

    static QStringList profileNames();
    static bool loadProfile(const QString &name, gnmi::ConnectParams *out);

private:
    void selectProfile(const QString &name);
    void saveProfile();
    void deleteProfile();
    void updateTlsUi();
    void browse(QLineEdit *edit, const QString &title);

    QComboBox *m_profile = nullptr;
    QLineEdit *m_host = nullptr;
    QSpinBox *m_port = nullptr;
    QComboBox *m_tls = nullptr;
    QLineEdit *m_ca = nullptr;
    QLineEdit *m_cert = nullptr;
    QLineEdit *m_key = nullptr;
    QLineEdit *m_serverName = nullptr;
    QWidget *m_tlsBox = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_password = nullptr;
    QSpinBox *m_timeout = nullptr;
    QLabel *m_insecureWarning = nullptr;
    QPushButton *m_connect = nullptr;
};

} // namespace ys
