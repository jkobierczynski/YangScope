// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/NetconfSession.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace ys {

/// Pick or edit a device profile and connect. Profiles (host, port, user,
/// auth method, key file) are stored in the settings; passwords never are.
class ConnectDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConnectDialog(QWidget *parent = nullptr);

    netconf::ConnectParams params() const;
    QString profileName() const;

    static QStringList profileNames();
    /// Profile by name (no secret filled in).
    static bool loadProfile(const QString &name, netconf::ConnectParams *out);

private:
    void selectProfile(const QString &name);
    void saveProfile();
    void deleteProfile();
    void updateAuthUi();
    void browseKey();
    void validate();

    QComboBox *m_profile = nullptr;
    QLineEdit *m_host = nullptr;
    QSpinBox *m_port = nullptr;
    QLineEdit *m_user = nullptr;
    QComboBox *m_auth = nullptr;
    QLineEdit *m_secret = nullptr;
    QLabel *m_secretLabel = nullptr;
    QLineEdit *m_keyFile = nullptr;
    QPushButton *m_browse = nullptr;
    QLabel *m_keyLabel = nullptr;
    QSpinBox *m_timeout = nullptr;
    QPushButton *m_connect = nullptr;
};

} // namespace ys
