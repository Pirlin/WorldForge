#include "App/EditorLog.h"

#include <QMutex>
#include <QtLogging>
#include <cstdio>

namespace {
QMutex lock;
QList<EditorLogEntry> pending;
QtMessageHandler upstream = nullptr;

void capture(QtMsgType level,const QMessageLogContext &context,const QString &message)
{
    if (level != QtDebugMsg) {
        QMutexLocker guard(&lock);
        if (pending.size() >= 500)
            pending.removeFirst();
        pending.append({level,QString("[%1] %2").arg(QString::fromUtf8(context.category),message)});
    }
    if (upstream)
        upstream(level,context,message);
    else
        std::fprintf(stderr,"%s\n",qPrintable(qFormatLogMessage(level,context,message)));
}
}

void installEditorLog() { upstream = qInstallMessageHandler(capture); }

QList<EditorLogEntry> takeEditorLog()
{
    QMutexLocker guard(&lock);
    QList<EditorLogEntry> result;
    result.swap(pending);
    return result;
}
