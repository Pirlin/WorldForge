#pragma once

#include <QProcess>
#include <QStringDecoder>
#include <QWidget>

class QLineEdit;
class QPlainTextEdit;
class ProjectStore;

class CommandTerminal final : public QWidget {
public:
    explicit CommandTerminal(ProjectStore &store,QWidget *parent = nullptr);

protected:
    bool eventFilter(QObject *object,QEvent *event) override;

private:
    void submit();
    void append(const QString &output);
    void readOutput();

    ProjectStore &store_;
    QProcess shell_;
    QStringDecoder decoder_{QStringDecoder::Utf8};
    QPlainTextEdit *transcript_ = nullptr;
    QLineEdit *command_ = nullptr;
    QStringList history_;
    qsizetype historyIndex_ = 0;
};
