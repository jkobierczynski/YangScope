// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/GnmiConnectDialog.h"

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

using gnmi::ConnectParams;
using gnmi::TlsMode;

namespace {
const QString kGroup = QStringLiteral("gnmiTargets");
}

QStringList GnmiConnectDialog::profileNames()
{
    QSettings s;
    s.beginGroup(kGroup);
    QStringList n = s.childGroups();
    n.sort(Qt::CaseInsensitive);
    return n;
}

bool GnmiConnectDialog::loadProfile(const QString &name, ConnectParams *out)
{
    QSettings s;
    s.beginGroup(kGroup);
    if (!s.childGroups().contains(name))
        return false;
    s.beginGroup(name);
    out->host = s.value(QStringLiteral("host")).toString();
    out->port = s.value(QStringLiteral("port"), 57400).toInt();
    out->tls = s.value(QStringLiteral("tls"), true).toBool() ? TlsMode::Tls : TlsMode::Insecure;
    out->caFile = s.value(QStringLiteral("caFile")).toString();
    out->certFile = s.value(QStringLiteral("certFile")).toString();
    out->keyFile = s.value(QStringLiteral("keyFile")).toString();
    out->serverName = s.value(QStringLiteral("serverName")).toString();
    out->username = s.value(QStringLiteral("user")).toString();
    out->timeoutSec = s.value(QStringLiteral("timeout"), 15).toInt();
    return true;
}

GnmiConnectDialog::GnmiConnectDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Connect to gNMI Target"));

    m_profile = new QComboBox(this);
    m_profile->setEditable(true);
    m_profile->setInsertPolicy(QComboBox::NoInsert);
    m_profile->lineEdit()->setPlaceholderText(tr("profile name (optional)"));
    m_profile->addItems(profileNames());
    auto *saveBtn = new QPushButton(tr("Save"), this);
    saveBtn->setToolTip(tr("Save address, TLS files and username (not the password)"));
    auto *delBtn = new QPushButton(tr("Delete"), this);
    auto *profRow = new QHBoxLayout;
    profRow->addWidget(m_profile, 1);
    profRow->addWidget(saveBtn);
    profRow->addWidget(delBtn);

    m_host = new QLineEdit(this);
    m_host->setPlaceholderText(tr("hostname or IP address"));
    m_port = new QSpinBox(this);
    m_port->setRange(1, 65535);
    m_port->setValue(57400);
    m_port->setToolTip(tr("Common ports: 57400 (Nokia, Cisco), 6030 (Arista), 9339 (IANA gNMI), 50051"));
    auto *hostRow = new QHBoxLayout;
    hostRow->addWidget(m_host, 1);
    hostRow->addWidget(new QLabel(tr("Port"), this));
    hostRow->addWidget(m_port);

    m_tls = new QComboBox(this);
    m_tls->addItem(tr("TLS"), true);
    m_tls->addItem(tr("Insecure (no TLS)"), false);

    m_tlsBox = new QWidget(this);
    m_ca = new QLineEdit(this);
    m_ca->setPlaceholderText(tr("empty: system trust store"));
    m_cert = new QLineEdit(this);
    m_cert->setPlaceholderText(tr("only for mutual TLS"));
    m_key = new QLineEdit(this);
    m_key->setPlaceholderText(tr("only for mutual TLS"));
    m_serverName = new QLineEdit(this);
    m_serverName->setPlaceholderText(tr("name in the device certificate, if not the host"));
    auto fileRow = [this](QLineEdit *e, const QString &title) {
        auto *w = new QWidget(this);
        auto *l = new QHBoxLayout(w);
        l->setContentsMargins(0, 0, 0, 0);
        auto *b = new QPushButton(tr("Browse..."), w);
        l->addWidget(e, 1);
        l->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, e, title] { browse(e, title); });
        return w;
    };
    auto *tlsForm = new QFormLayout(m_tlsBox);
    tlsForm->setContentsMargins(0, 0, 0, 0);
    tlsForm->addRow(tr("CA certificate"), fileRow(m_ca, tr("CA Certificate")));
    tlsForm->addRow(tr("Client certificate"), fileRow(m_cert, tr("Client Certificate")));
    tlsForm->addRow(tr("Client key"), fileRow(m_key, tr("Client Key")));
    tlsForm->addRow(tr("TLS server name"), m_serverName);

    m_insecureWarning = new QLabel(tr("<span style='color:#b45309'>Without TLS, the username, password "
                                      "and all data cross the network in clear text. Use only in a lab.</span>"),
                                   this);
    m_insecureWarning->setWordWrap(true);

    m_user = new QLineEdit(this);
    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);
    m_timeout = new QSpinBox(this);
    m_timeout->setRange(2, 600);
    m_timeout->setValue(15);
    m_timeout->setSuffix(tr(" s"));

    auto *form = new QFormLayout;
    form->addRow(tr("Profile"), profRow);
    form->addRow(tr("Target"), hostRow);
    form->addRow(tr("Transport"), m_tls);
    form->addRow(m_tlsBox);
    form->addRow(m_insecureWarning);
    form->addRow(tr("Username"), m_user);
    form->addRow(tr("Password"), m_password);
    form->addRow(tr("Timeout"), m_timeout);

    auto *note = new QLabel(tr("Username and password are sent as gRPC call metadata, as most devices "
                               "expect. Passwords are not stored."), this);
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
    connect(saveBtn, &QPushButton::clicked, this, &GnmiConnectDialog::saveProfile);
    connect(delBtn, &QPushButton::clicked, this, &GnmiConnectDialog::deleteProfile);
    connect(m_profile, &QComboBox::textActivated, this, &GnmiConnectDialog::selectProfile);
    connect(m_tls, &QComboBox::currentIndexChanged, this, &GnmiConnectDialog::updateTlsUi);
    connect(m_host, &QLineEdit::textChanged, this,
            [this] { m_connect->setEnabled(!m_host->text().trimmed().isEmpty()); });

    const QString last = QSettings().value(QStringLiteral("gnmiTargets.last")).toString();
    if (!last.isEmpty() && profileNames().contains(last)) {
        m_profile->setCurrentText(last);
        selectProfile(last);
    } else {
        m_profile->setCurrentText(QString());
    }
    m_connect->setEnabled(!m_host->text().trimmed().isEmpty());
    updateTlsUi();
}

void GnmiConnectDialog::browse(QLineEdit *edit, const QString &title)
{
    const QString start = edit->text().isEmpty() ? QDir::homePath() : edit->text();
    const QString f = QFileDialog::getOpenFileName(this, title, start,
                                                   tr("PEM files (*.pem *.crt *.cer *.key);;All files (*)"));
    if (!f.isEmpty())
        edit->setText(f);
}

void GnmiConnectDialog::updateTlsUi()
{
    const bool tls = m_tls->currentData().toBool();
    m_tlsBox->setVisible(tls);
    m_insecureWarning->setVisible(!tls);
    adjustSize();
}

void GnmiConnectDialog::selectProfile(const QString &name)
{
    ConnectParams p;
    if (!loadProfile(name, &p))
        return;
    m_host->setText(p.host);
    m_port->setValue(p.port);
    m_tls->setCurrentIndex(p.tls == TlsMode::Tls ? 0 : 1);
    m_ca->setText(p.caFile);
    m_cert->setText(p.certFile);
    m_key->setText(p.keyFile);
    m_serverName->setText(p.serverName);
    m_user->setText(p.username);
    m_timeout->setValue(p.timeoutSec);
    m_password->clear();
    m_password->setFocus();
}

void GnmiConnectDialog::saveProfile()
{
    QString name = m_profile->currentText().trimmed();
    if (name.isEmpty())
        name = QStringLiteral("%1:%2").arg(m_host->text().trimmed()).arg(m_port->value());
    name.replace(QLatin1Char('/'), QLatin1Char('_')).replace(QLatin1Char('\\'), QLatin1Char('_'));
    if (name.isEmpty() || name.startsWith(QLatin1Char(':')))
        return;
    const ConnectParams p = params();
    QSettings s;
    s.beginGroup(kGroup);
    s.beginGroup(name);
    s.setValue(QStringLiteral("host"), p.host);
    s.setValue(QStringLiteral("port"), p.port);
    s.setValue(QStringLiteral("tls"), p.tls == TlsMode::Tls);
    s.setValue(QStringLiteral("caFile"), p.caFile);
    s.setValue(QStringLiteral("certFile"), p.certFile);
    s.setValue(QStringLiteral("keyFile"), p.keyFile);
    s.setValue(QStringLiteral("serverName"), p.serverName);
    s.setValue(QStringLiteral("user"), p.username);
    s.setValue(QStringLiteral("timeout"), p.timeoutSec);
    s.endGroup();
    s.endGroup();
    m_profile->clear();
    m_profile->addItems(profileNames());
    m_profile->setCurrentText(name);
}

void GnmiConnectDialog::deleteProfile()
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

ConnectParams GnmiConnectDialog::params() const
{
    ConnectParams p;
    p.host = m_host->text().trimmed();
    p.port = m_port->value();
    p.tls = m_tls->currentData().toBool() ? TlsMode::Tls : TlsMode::Insecure;
    p.caFile = m_ca->text().trimmed();
    p.certFile = m_cert->text().trimmed();
    p.keyFile = m_key->text().trimmed();
    p.serverName = m_serverName->text().trimmed();
    p.username = m_user->text().trimmed();
    p.password = m_password->text();
    p.timeoutSec = m_timeout->value();
    return p;
}

QString GnmiConnectDialog::profileName() const
{
    const QString n = m_profile->currentText().trimmed();
    return profileNames().contains(n) ? n : QString();
}

} // namespace ys
