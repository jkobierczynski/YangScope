// SPDX-License-Identifier: GPL-3.0-or-later
// Small helpers for the offscreen GUI tests.
#pragma once

#include <QAbstractButton>
#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QTreeWidget>

#include <cstdio>
#include <functional>

inline int g_failed = 0;
inline int g_passed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                          \
        } else {                                                                 \
            ++g_passed;                                                          \
        }                                                                        \
    } while (0)

inline bool waitFor(const std::function<bool()> &cond, int ms = 15000)
{
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > ms)
            return false;
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return true;
}

/// Answers the next modal dialog by clicking the button labelled @p button.
struct DialogAnswer {
    QString button;
    QString screenshot;
    bool answered = false;
    QString seenText;
};

inline void armAnswer(DialogAnswer *a)
{
    auto *timer = new QTimer(qApp);
    timer->setInterval(100);
    QObject::connect(timer, &QTimer::timeout, [a, timer] {
        QWidget *m = QApplication::activeModalWidget();
        if (!m)
            return;
        for (QLabel *l : m->findChildren<QLabel *>())
            a->seenText += l->text() + QLatin1Char('\n');
        for (QPlainTextEdit *e : m->findChildren<QPlainTextEdit *>())
            a->seenText += e->toPlainText() + QLatin1Char('\n');
        if (!a->screenshot.isEmpty())
            m->grab().save(a->screenshot);
        for (QAbstractButton *b : m->findChildren<QAbstractButton *>()) {
            if (b->text().remove(QLatin1Char('&')) == a->button) {
                a->answered = true;
                timer->stop();
                timer->deleteLater();
                b->click();
                return;
            }
        }
    });
    timer->start();
}

inline QTreeWidgetItem *findItem(QTreeWidget *tree, const QRegularExpression &re, int column = 0)
{
    std::function<QTreeWidgetItem *(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *it) -> QTreeWidgetItem * {
        for (int i = 0; i < it->childCount(); ++i) {
            QTreeWidgetItem *c = it->child(i);
            if (re.match(c->text(column)).hasMatch())
                return c;
            if (QTreeWidgetItem *f = walk(c))
                return f;
        }
        return nullptr;
    };
    return walk(tree->invisibleRootItem());
}

inline int countItems(QTreeWidget *tree, const QRegularExpression &re)
{
    int n = 0;
    std::function<void(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *it) {
        for (int i = 0; i < it->childCount(); ++i) {
            n += re.match(it->child(i)->text(0)).hasMatch();
            walk(it->child(i));
        }
    };
    walk(tree->invisibleRootItem());
    return n;
}

/// Start a Python test server; returns the port it reports ("listening on host:port").
inline int startServer(QProcess &proc, const QString &python, const QStringList &args)
{
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(python, args);
    if (!proc.waitForStarted(5000))
        return 0;
    QByteArray all;
    while (proc.waitForReadyRead(30000)) {
        all += proc.readAll();
        const qsizetype i = all.indexOf("listening on ");
        if (i >= 0) {
            QByteArray line = all.mid(i).split('\n').first().trimmed();
            line = line.split(' ').value(2);
            return line.mid(line.lastIndexOf(':') + 1).toInt();
        }
    }
    std::fprintf(stderr, "server output: %s\n", all.constData());
    return 0;
}
