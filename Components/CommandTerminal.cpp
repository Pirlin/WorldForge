#include "Components/CommandTerminal.h"
#include "State/ProjectStore.h"

#include <QDir>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>
#include <algorithm>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

CommandTerminal::CommandTerminal(ProjectStore &store,QWidget *parent) : QWidget(parent), store_(store)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6,5,6,5);
    transcript_ = new QPlainTextEdit(this);
    transcript_->setReadOnly(true);
    transcript_->setMaximumBlockCount(3000);
    transcript_->setAccessibleName(tr("Terminal output"));
    transcript_->setPlaceholderText(tr("Run commands in the local project directory. Up and Down recall command history."));
    transcript_->setFont(QFont("Consolas",10));
    layout->addWidget(transcript_,1);
    auto *input = new QHBoxLayout;
    command_ = new QLineEdit(this);
    command_->setAccessibleName(tr("Terminal command"));
    command_->setPlaceholderText(tr("Enter a command"));
    command_->installEventFilter(this);
    input->addWidget(command_,1);
    auto *run = new QPushButton(tr("Run"),this);
    auto *clear = new QPushButton(tr("Clear"),this);
    auto *stop = new QPushButton(tr("Stop Shell"),this);
    input->addWidget(run);
    input->addWidget(clear);
    input->addWidget(stop);
    layout->addLayout(input);
    connect(command_,&QLineEdit::returnPressed,this,&CommandTerminal::submit);
    connect(run,&QPushButton::clicked,this,&CommandTerminal::submit);
    connect(clear,&QPushButton::clicked,transcript_,&QPlainTextEdit::clear);
    connect(stop,&QPushButton::clicked,&shell_,&QProcess::kill);
    shell_.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    shell_.setProgram(QDir(qEnvironmentVariable("SystemRoot","C:/Windows")).filePath("System32/cmd.exe"));
    shell_.setArguments({"/d","/q","/k","chcp 65001 > nul"});
    shell_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#else
    shell_.setProgram("/bin/sh");
#endif
    connect(&shell_,&QProcess::readyReadStandardOutput,this,&CommandTerminal::readOutput);
    connect(&shell_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error) {
        if (error != QProcess::Crashed)
            append(tr("\nShell error: %1\n").arg(shell_.errorString()));
    });
    connect(&shell_,&QProcess::finished,this,[this](int code,QProcess::ExitStatus status) {
        append(status == QProcess::CrashExit ? tr("\nShell stopped.\n") : tr("\nShell exited (%1).\n").arg(code));
    });
    connect(&store_,&ProjectStore::projectsChanged,this,[this] {
        if (shell_.state() != QProcess::NotRunning)
            shell_.kill();
    });
}

void CommandTerminal::append(const QString &output)
{
    auto cursor = transcript_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(output);
    transcript_->setTextCursor(cursor);
    transcript_->ensureCursorVisible();
}

void CommandTerminal::readOutput()
{
    QString output = decoder_(shell_.readAllStandardOutput());
    append(output.remove('\r'));
}

void CommandTerminal::submit()
{
    const auto value = command_->text().trimmed();
    if (value.isEmpty())
        return;
    if (history_.size() >= 100)
        history_.removeFirst();
    history_.append(value);
    historyIndex_ = history_.size();
    command_->clear();
    if (shell_.state() == QProcess::NotRunning) {
        const auto *project = store_.project();
        auto folder = project ? project->configuration.value("location").toString() : QString();
        if (folder.isEmpty() || !QDir(folder).exists())
            folder = QDir::currentPath();
        shell_.setWorkingDirectory(folder);
        decoder_.resetState();
        append(tr("Shell started in %1\n").arg(QDir::toNativeSeparators(folder)));
        shell_.start();
    }
    append(QString("> %1\n").arg(value));
#ifdef Q_OS_WIN
    shell_.write(value.toUtf8()+"\r\n");
#else
    shell_.write(value.toUtf8()+"\n");
#endif
}

bool CommandTerminal::eventFilter(QObject *object,QEvent *event)
{
    if (object == command_ && event->type() == QEvent::KeyPress) {
        const auto key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down) {
            historyIndex_ = std::clamp(historyIndex_+(key == Qt::Key_Up ? -1 : 1),qsizetype(0),history_.size());
            command_->setText(history_.value(historyIndex_));
            return true;
        }
    }
    return QWidget::eventFilter(object,event);
}
