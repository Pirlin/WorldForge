#include "Components/LocalMultiplayer.h"

#include <QCoreApplication>
#include <QDir>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <array>

namespace {
class LoopbackCheck : public QObject {
public:
    LoopbackCheck(QPushButton *button,QLabel *result,QObject *parent)
        : QObject(parent), button_(button), result_(result)
    {
        timer_.setSingleShot(true);
        connect(&timer_,&QTimer::timeout,this,[this] { finish(false); });
        connect(&server_,&QTcpServer::newConnection,this,[this] {
            while (server_.hasPendingConnections()) {
                auto *socket = server_.nextPendingConnection();
                if (connected_ == peers_.size()) {
                    socket->deleteLater();
                    continue;
                }
                const auto index = connected_++;
                peers_[index] = socket;
                socket->setReadBufferSize(128);
                socket->write(index == 0 ? "player 1\n" : "player 2\n");
                connect(socket,&QTcpSocket::readyRead,this,[this,socket,index] {
                    if (!socket->canReadLine())
                        return;
                    if (socket->readLine() != "interact\n" || attempted_[index]) {
                        finish(false);
                        return;
                    }
                    attempted_[index] = true;
                    const bool authorized = index == 0;
                    beaconActive_ |= authorized;
                    socket->write(authorized ? "accepted\n" : "denied\n");
                    if (attempted_[0] && attempted_[1])
                        for (auto *peer : peers_)
                            peer->write(beaconActive_ ? "beacon 1\n" : "beacon 0\n");
                });
            }
        });
    }

    void start()
    {
        button_->setEnabled(false);
        if (!server_.listen(QHostAddress::LocalHost,0)) {
            finish(false);
            return;
        }
        result_->setText(tr("Connecting two local clients…"));
        timer_.start(10000);
        for (auto &client : clients_) {
            client = new QProcess(this);
            connect(client,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart)
                    finish(false);
            });
            connect(client,&QProcess::finished,this,[this](int code,QProcess::ExitStatus status) {
                if (status != QProcess::NormalExit || code != 0)
                    finish(false);
                else if (++verified_ == clients_.size())
                    finish(true);
            });
            client->start(QDir(QCoreApplication::applicationDirPath()).filePath("WorldForgeEditor.exe"),
                {"--loopback-client",QString::number(server_.serverPort())});
        }
    }

private:
    void finish(bool success)
    {
        if (finished_)
            return;
        finished_ = true;
        timer_.stop();
        server_.close();
        button_->setEnabled(true);
        for (auto *client : clients_)
            if (client && client->state() != QProcess::NotRunning)
                client->kill();
        result_->setText(success ? tr("Passed. Two local clients connected. Player 1 activated the beacon, Player 2 was denied, and both received the same state.")
            : tr("Failed. The two local clients could not verify connection, interaction ownership, and shared beacon state."));
        deleteLater();
    }

    QPushButton *button_;
    QLabel *result_;
    QTcpServer server_;
    QTimer timer_;
    std::array<QProcess *,2> clients_{};
    std::array<QTcpSocket *,2> peers_{};
    std::array<bool,2> attempted_{};
    std::size_t connected_ = 0, verified_ = 0;
    bool beaconActive_ = false, finished_ = false;
};
}

QWidget *createLocalMultiplayerPanel(QWidget *parent)
{
    auto *panel = new QWidget(parent);
    auto *layout = new QVBoxLayout(panel);
    auto *run = new QPushButton(QObject::tr("Run Two-Client Loopback Check"),panel);
    run->setObjectName("runLocalMultiplayerCheck");
    auto *result = new QLabel(QObject::tr("Verify two local processes and server ownership of a beacon interaction."),panel);
    result->setObjectName("localMultiplayerResult");
    result->setWordWrap(true);
    layout->addWidget(run,0,Qt::AlignLeft);
    layout->addWidget(result);
    layout->addStretch();
    QObject::connect(run,&QPushButton::clicked,panel,[panel,run,result] {
        (new LoopbackCheck(run,result,panel))->start();
    });
    return panel;
}

int runLocalMultiplayerClient(int argc,char **argv)
{
    QCoreApplication app(argc,argv);
    bool valid = false;
    const auto port = app.arguments().value(2).toUShort(&valid);
    if (!valid || !port)
        return 2;
    QTcpSocket socket;
    socket.setReadBufferSize(128);
    QByteArray expected;
    bool received = false;
    QObject::connect(&socket,&QTcpSocket::readyRead,&app,[&] {
        while (socket.canReadLine()) {
            const auto line = socket.readLine();
            if (expected.isEmpty() && (line == "player 1\n" || line == "player 2\n")) {
                expected = line == "player 1\n" ? "accepted\n" : "denied\n";
                socket.write("interact\n");
            } else if (!received && line == expected) {
                received = true;
            } else if (received && line == "beacon 1\n") {
                app.exit(0);
                return;
            } else {
                app.exit(3);
                return;
            }
        }
    });
    QObject::connect(&socket,&QTcpSocket::errorOccurred,&app,[&] { app.exit(4); });
    QTimer::singleShot(7000,&app,[&] { app.exit(5); });
    QTimer::singleShot(0,&app,[&] { socket.connectToHost(QHostAddress::LocalHost,port); });
    return app.exec();
}
