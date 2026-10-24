// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/ConnectDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

namespace ys {

using netconf::AuthMethod;
using netconf::ConnectParams;

namespace {
const QString kGroup = QStringLiteral("devices");

QString authKey(AuthMethod m)
{
    switch (m) {
    case AuthMethod::KeyFile: return QStringLiteral("key");
    case AuthMethod::Auto:    return QStringLiteral("auto");
    default:                  return QStringLiteral("password");
    }
}

AuthMethod authFromKey(const QString &k)
{
    if (k == QLatin1String("key"))
        return AuthMethod::KeyFile;
    if (k == QLatin1String("auto"))
        return AuthMethod::Auto;
    return AuthMethod::Password;
}
} // namespace

QStringList ConnectDialog::profileNames()
{
    QSettings s;
    s.beginGroup(kGroup);
    QStringList names = s.childGroups();
    names.sort(Qt::CaseInsensitive);
    return names;
}

bool ConnectDialog::loadProfile(const QString &name, ConnectParams *out)
{
    QSettings s;
    s.beginGroup(kGroup);
    if (!s.childGroups().contains(name))
        return false;
    s.beginGroup(name);
    out->host = s.value(QStringLiteral("host")).toString();
    out->port = s.value(QStringLiteral("port"), 830).toInt();
    out->user = s.value(QStringLiteral("user")).toString();
    out->auth = authFromKey(s.value(QStringLiteral("auth")).toString());
    out->keyFile = s.value(QStringLiteral("keyFile")).toString();
    out->timeoutSec = s.value(QStringLiteral("timeout"), 15).toInt();
    return true;
}

ConnectDialog::ConnectDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Connect to NETCONF Device"));

    m_profile = new QComboBox(this);
    m_profile->setEditable(true);
    m_profile->setInsertPolicy(QComboBox::NoInsert);
    m_profile->lineEdit()->setPlaceholderText(tr("profile name (optional)"));
    m_profile->addItems(profileNames());
    auto *saveBtn = new QPushButton(tr("Save"), this);
    auto *delBtn = new QPushButton(tr("Delete"), this);
    saveBtn->setToolTip(tr("Save host, port, user and authentication method (not the password)"));
    auto *profRow = new QHBoxLayout;
    profRow->addWidget(m_profile, 1);
    profRow->addWidget(saveBtn);
    profRow->addWidget(delBtn);

    m_host = new QLineEdit(this);
    m_host->setPlaceholderText(tr("hostname or IP address"));
    m_port = new QSpinBox(this);
    m_port->setRange(1, 65535);
    m_port->setValue(830);
    auto *hostRow = new QHBoxLayout;
    hostRow->addWidget(m_host, 1);
    hostRow->addWidget(new QLabel(tr("Port"), this));
    hostRow->addWidget(m_port);

    m_user = new QLineEdit(this);
    m_auth = new QComboBox(this);
    m_auth->addItem(tr("Password"), authKey(AuthMethod::Password));
    m_auth->addItem(tr("Private key file"), authKey(AuthMethod::KeyFile));
    m_auth->addItem(tr("SSH agent / default keys"), authKey(AuthMethod::Auto));
    m_secret = new QLineEdit(this);
    m_secret->setEchoMode(QLineEdit::Password);
    m_secretLabel = new QLabel(tr("Password"), this);
    m_keyFile = new QLineEdit(this);
    m_browse = new QPushButton(tr("Browse..."), this);
    m_keyLabel = new QLabel(tr("Key file"), this);
    auto *keyRow = new QHBoxLayout;
    keyRow->addWidget(m_keyFile, 1);
    keyRow->addWidget(m_browse);
    m_timeout = new QSpinBox(this);
    m_timeout->setRange(2, 600);
    m_timeout->setValue(15);
    m_timeout->setSuffix(tr(" s"));

    auto *form = new QFormLayout;
    form->addRow(tr("Profile"), profRow);
    form->addRow(tr("Host"), hostRow);
    form->addRow(tr("Username"), m_user);
    form->addRow(tr("Authentication"), m_auth);
    form->addRow(m_keyLabel, keyRow);
    form->addRow(m_secretLabel, m_secret);
    form->addRow(tr("Timeout"), m_timeout);

    auto *note = new QLabel(tr("Host keys are checked against ~/.ssh/known_hosts. "
                               "Settings from ~/.ssh/config apply. Passwords are not stored."),
                            this);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_connect = buttons->addButton(tr("Connect"), QDialogButtonBox::AcceptRole);
    m_connect->setDefault(true);

    auto *lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addWidget(note);
    lay->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, this, &ConnectDialog::saveProfile);
    connect(delBtn, &QPushButton::clicked, this, &ConnectDialog::deleteProfile);
    connect(m_profile, &QComboBox::textActivated, this, &ConnectDialog::selectProfile);
    connect(m_auth, &QComboBox::currentIndexChanged, this, &ConnectDialog::updateAuthUi);
    connect(m_browse, &QPushButton::clicked, this, &ConnectDialog::browseKey);
    connect(m_host, &QLineEdit::textChanged, this, &ConnectDialog::validate);
    connect(m_user, &QLineEdit::textChanged, this, &ConnectDialog::validate);

    const QString last = QSettings().value(QStringLiteral("devices.last")).toString();
    if (!last.isEmpty() && profileNames().contains(last)) {
        m_profile->setCurrentText(last);
        selectProfile(last);
    } else {
        m_profile->setCurrentText(QString());
    }
    updateAuthUi();
    validate();
    if (!m_host->text().isEmpty())
        m_secret->setFocus();
}

void ConnectDialog::selectProfile(const QString &name)
{
    ConnectParams p;
    if (!loadProfile(name, &p))
        return;
    m_host->setText(p.host);
    m_port->setValue(p.port);
    m_user->setText(p.user);
    m_auth->setCurrentIndex(m_auth->findData(authKey(p.auth)));
    m_keyFile->setText(p.keyFile);
    m_timeout->setValue(p.timeoutSec);
    m_secret->clear();
    m_secret->setFocus();
}

void ConnectDialog::saveProfile()
{
    QString name = m_profile->currentText().trimmed();
    if (name.isEmpty())
        name = m_user->text().isEmpty() ? m_host->text() : m_user->text() + QLatin1Char('@') + m_host->text();
    name.replace(QLatin1Char('/'), QLatin1Char('_')).replace(QLatin1Char('\\'), QLatin1Char('_'));
    if (name.isEmpty())
        return;
    QSettings s;
    s.beginGroup(kGroup);
    s.beginGroup(name);
    const ConnectParams p = params();
    s.setValue(QStringLiteral("host"), p.host);
    s.setValue(QStringLiteral("port"), p.port);
    s.setValue(QStringLiteral("user"), p.user);
    s.setValue(QStringLiteral("auth"), authKey(p.auth));
    s.setValue(QStringLiteral("keyFile"), p.keyFile);
    s.setValue(QStringLiteral("timeout"), p.timeoutSec);
    s.endGroup();
    s.endGroup();
    m_profile->clear();
    m_profile->addItems(profileNames());
    m_profile->setCurrentText(name);
}

void ConnectDialog::deleteProfile()
{
    const QString name = m_profile->currentText();
    if (!profileNames().contains(name))
        return;
    if (QMessageBox::question(this, tr("Delete Profile"), tr("Delete profile \"%1\"?").arg(name))
        != QMessageBox::Yes)
        return;
    QSettings s;
    s.beginGroup(kGroup);
    s.remove(name);
    s.endGroup();
    m_profile->clear();
    m_profile->addItems(profileNames());
    m_profile->setCurrentText(QString());
}

void ConnectDialog::updateAuthUi()
{
    const AuthMethod m = authFromKey(m_auth->currentData().toString());
    const bool key = m == AuthMethod::KeyFile;
    m_keyFile->setVisible(key);
    m_browse->setVisible(key);
    m_keyLabel->setVisible(key);
    m_secretLabel->setText(m == AuthMethod::Password ? tr("Password") : tr("Passphrase"));
    m_secret->setPlaceholderText(m == AuthMethod::Password ? QString() : tr("only if the key is encrypted"));
}

void ConnectDialog::browseKey()
{
    const QString start = m_keyFile->text().isEmpty() ? QDir::homePath() + QStringLiteral("/.ssh")
                                                      : m_keyFile->text();
    const QString f = QFileDialog::getOpenFileName(this, tr("Private Key"), start);
    if (!f.isEmpty())
        m_keyFile->setText(f);
}

void ConnectDialog::validate()
{
    m_connect->setEnabled(!m_host->text().trimmed().isEmpty());
}

ConnectParams ConnectDialog::params() const
{
    ConnectParams p;
    p.host = m_host->text().trimmed();
    p.port = m_port->value();
    p.user = m_user->text().trimmed();
    p.auth = authFromKey(m_auth->currentData().toString());
    p.password = m_secret->text();
    p.keyFile = m_keyFile->text().trimmed();
    p.timeoutSec = m_timeout->value();
    return p;
}

QString ConnectDialog::profileName() const
{
    const QString n = m_profile->currentText().trimmed();
    return profileNames().contains(n) ? n : QString();
}

} // namespace ys
