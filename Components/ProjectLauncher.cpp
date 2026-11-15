#include "Components/ProjectLauncher.h"
#include "Components/WindowCaption.h"
#include "DesignSystem/Icons.h"

#include "App/SystemMetrics.h"

#include <QAbstractItemModel>
#include <QButtonGroup>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScreen>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWindow>

#include <array>

namespace {
const QString skin = QStringLiteral(R"(
#launcher, #launcherScroll, #launcherBody, #recentPage { background:#050812; }
QWidget#launcher QLabel { color:#f5f3fa; background:transparent; }
QWidget#launcher QLabel[muted="true"] { color:#a7a7b8; }
QWidget#launcher QLabel[error="true"] { color:#ffadaf; }
QWidget#launcherIntro { background:transparent; }
QWidget#launcher QToolButton[launcherAction="true"] { background:transparent; border:0; border-radius:4px; }
QWidget#launcher QToolButton[launcherAction="true"]:hover, QWidget#launcher QToolButton[launcherAction="true"]:focus {
    background:#382750;
}
QWidget#launcher QFrame[surface="true"] {
    background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #242044,stop:0.18 #0c1325,stop:1 #070e1b);
    border:1px solid #5d4b83; border-radius:9px;
}
QWidget#launcher QPushButton, QWidget#launcher QComboBox {
    color:#f5f3fa; min-height:22px; padding:2px 9px; border:1px solid #535c82; border-radius:6px;
    background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #212b47,stop:0.42 #101829,stop:1 #080e1b);
    font-size:12px;
}
QWidget#launcher QComboBox { padding-right:24px; }
QWidget#launcher QPushButton:hover, QWidget#launcher QComboBox:hover { background:#2c2b39; border-color:#69627e; }
QWidget#launcher QPushButton:focus, QWidget#launcher QComboBox:focus { border-color:#b798ff; }
QWidget#launcher QPushButton[primary="true"], QWidget#launcher QPushButton[mode="true"]:checked {
    color:#ffffff; border-color:#d672ff; font-weight:600;
    background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #a12eff,stop:0.5 #650de0,stop:1 #480a9f);
}
QWidget#launcher QPushButton[primary="true"]:hover { background:#7d26e9; }
QWidget#launcher QPushButton[primary="true"]:pressed { background:#5411aa; }
QWidget#launcher QPushButton[primary="true"]:focus { border-color:#f1e4ff; }
QWidget#launcher QPushButton:disabled { color:#818292; border-color:#30313d; background:#1c1d26; }
QWidget#launcher QLineEdit, QWidget#launcher QListView {
    background:#14151d; color:#f5f3fa; border:1px solid #393a48; border-radius:6px; padding:7px 9px;
    font-size:13px;
}
QWidget#launcher QLineEdit:focus { border-color:#b798ff; }
QWidget#launcher QListView::item { padding:12px; border-bottom:1px solid #2b2c37; }
QWidget#launcher QListView::item:selected { background:#3a2e53; }
)");

struct Starter {
    const char *title, *summary, *art;
    int content = 0;
    double cropHeight = 0.5;
    const char *artDetail = "";
};
constexpr std::array<Starter,8> starters{{
    {"Open World City","A coastal city concept with neon streets and a waterfront skyline.",":/Data/Launcher/open-world-city.jpg",0,0.5,"Waterfront skyline artwork"},
    {"Fantasy RPG","A mountain kingdom concept with a castle, valleys, and winding rivers.",":/Data/Launcher/fantasy-rpg.jpg",1,0.5,"Mountain castle artwork"},
    {"Action Adventure","Ancient ruins and cinematic exploration for an adventure project.",":/Data/Launcher/action-adventure.jpg",0,0.20,"Ancient ruins artwork"},
    {"Racing Experience","A sports-car and circuit concept for a racing project.",":/Data/Launcher/racing-experience.jpg",0,0.5,"Sports car and track artwork"},
    {"Shooter Arena","A fictional urban arena concept for fast-paced gameplay.",":/Data/Launcher/shooter-arena.jpg",0,0.5,"Urban arena artwork"},
    {"Survival Sandbox","A wilderness and campfire concept for a survival project.",":/Data/Launcher/survival-sandbox.jpg",1,0.5,"Wilderness camp artwork"},
    {"Simulation City","A modern city construction concept for a simulation project.",":/Data/Launcher/simulation-city.jpg",0,0.5,"City construction artwork"},
    {"Blank Project","Start with an empty scene and build it from scratch.",""}
}};

QLabel *label(const QString &text,int pixels,bool bold = false,bool muted = false)
{
    auto *result = new QLabel(text);
    result->setProperty("muted",muted);
    result->setTextFormat(Qt::PlainText);
    result->setStyleSheet(QString("font-family:'Segoe UI';font-size:%1px;font-weight:%2;").arg(pixels).arg(bold ? 650 : 400));
    return result;
}

QPushButton *button(const QString &text,const QString &icon = {})
{
    auto *result = new QPushButton(text);
    if (!icon.isEmpty())
        result->setIcon(editorIcon(icon));
    result->setCursor(Qt::PointingHandCursor);
    return result;
}

QVBoxLayout *surface()
{
    auto *frame = new QFrame;
    frame->setProperty("surface",true);
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(16,16,16,16);
    layout->setSpacing(12);
    return layout;
}

void addSectionTitle(QVBoxLayout *panel,int step,const QString &title)
{
    auto *heading = new QHBoxLayout;
    heading->setSpacing(7);
    auto *badge = label(QString::number(step),13,true);
    badge->setAlignment(Qt::AlignCenter);
    badge->setFixedSize(23,23);
    badge->setStyleSheet("color:#fff;background:#750bdc;border:1px solid #c46cff;border-radius:11px;");
    heading->addWidget(badge);
    heading->addWidget(label(title,15,true),1);
    panel->addLayout(heading);
}

QPixmap readArt(const char *path,int maxWidth)
{
    if (!*path)
        return {};
    QImageReader reader(QString::fromUtf8(path));
    const auto size = reader.size();
    if (size.width() > maxWidth)
        reader.setScaledSize(size.scaled(maxWidth,maxWidth,Qt::KeepAspectRatio));
    return QPixmap::fromImage(reader.read());
}

void drawCover(QPainter &p,const QRect &rect,const QPixmap &art,double vertical = 0.5)
{
    if (art.isNull() || rect.isEmpty())
        return;
    auto source = rect.size().scaled(art.size(),Qt::KeepAspectRatio);
    const QRect crop(QPoint((art.width()-source.width())/2,int((art.height()-source.height())*vertical)),source);
    p.drawPixmap(rect,art,crop);
}

class LauncherHeader final : public QWidget {
public:
    LauncherHeader() : art_(readArt(":/Data/Launcher/skyline-banner.png",1920)) {}

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.fillRect(rect(),QColor("#050812"));
        const QRect band(width()*32/100,0,width()*68/100,height());
        drawCover(painter,band,art_,0.64);
        QLinearGradient fade(band.left(),0,band.right(),0);
        fade.setColorAt(0,QColor("#050812"));
        fade.setColorAt(0.38,QColor(5,8,18,15));
        fade.setColorAt(1,QColor(5,8,18,195));
        painter.fillRect(band,fade);
        QLinearGradient base(0,0,0,height());
        base.setColorAt(0,QColor(5,8,18,25));
        base.setColorAt(0.45,QColor(5,8,18,45));
        base.setColorAt(1,QColor("#050812"));
        painter.fillRect(rect(),base);
        painter.fillRect(0,39,width(),1,QColor(139,83,201,125));
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            if (auto *handle = window()->windowHandle())
                handle->startSystemMove();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            window()->isMaximized() ? window()->showNormal() : window()->showMaximized();
    }
private:
    QPixmap art_;
};

class TemplateButton final : public QPushButton {
public:
    explicit TemplateButton(const Starter &starter)
        : art_(readArt(starter.art,480)), blank_(editorIcon("box",QColor("#ead0ff"))),
          selected_(editorIcon("check")), summary_(tr(starter.summary)), crop_(starter.cropHeight)
    {
        setText(tr(starter.title));
        setToolTip(tr(starter.summary));
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setMinimumHeight(124);
        setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Expanding);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        const QRect bounds = rect().adjusted(4,4,-4,-4);
        if (isChecked()) {
            for (int spread = 3; spread > 0; --spread) {
                painter.setPen(QColor(172,53,255,40));
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(bounds.adjusted(-spread,-spread,spread,spread),10,10);
            }
        }
        const QRect thumbnail(bounds.left(),bounds.top(),bounds.width(),bounds.height()-49);
        QPainterPath clip;
        clip.addRoundedRect(bounds,9,9);
        painter.save();
        painter.setClipPath(clip);
        if (art_.isNull()) {
            painter.fillRect(thumbnail,QColor("#0c1222"));
            blank_.paint(&painter,QRect(thumbnail.center()-QPoint(22,22),QSize(44,44)));
        } else {
            drawCover(painter,thumbnail,art_,crop_);
        }
        painter.fillRect(QRect(bounds.left(),thumbnail.bottom()+1,bounds.width(),49),
            isChecked() ? QColor("#161029") : QColor("#0b1322"));
        QFont font("Segoe UI");
        font.setPixelSize(14);
        font.setWeight(QFont::DemiBold);
        font.setStretch(QFont::SemiCondensed);
        painter.setFont(font);
        painter.setPen(QColor("#fffaff"));
        const QRect title(bounds.left()+5,thumbnail.bottom()+2,bounds.width()-10,19);
        painter.drawText(title,Qt::AlignCenter,QFontMetrics(font).elidedText(text(),Qt::ElideRight,title.width()));
        font.setPixelSize(10);
        font.setWeight(QFont::Normal);
        painter.setFont(font);
        painter.setPen(QColor("#e3e1ee"));
        painter.drawText(QRect(title.left(),title.bottom()+1,title.width(),27),
            Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,summary_);
        painter.restore();
        painter.setPen(QPen(isChecked() ? QColor("#e193ff") : hasFocus() ? QColor("#bb92e5") : QColor("#596689"),
            isChecked() ? 2 : 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(bounds,9,9);
        if (isChecked()) {
            const QRect badge(bounds.right()-28,bounds.top()+6,22,22);
            painter.setPen(QColor("#c485ff"));
            painter.setBrush(QColor("#2a124b"));
            painter.drawEllipse(badge);
            selected_.paint(&painter,badge.adjusted(4,4,-4,-4));
        }
    }

private:
    QPixmap art_;
    QIcon blank_, selected_;
    QString summary_;
    double crop_;
};

class ArtworkButton final : public QPushButton {
public:
    ArtworkButton()
        : blank_(editorIcon("box",QColor("#bd9cff"))), expand_(editorIcon("expand"))
    {
        setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
        setFixedHeight(138);
        setCursor(Qt::PointingHandCursor);
    }

    void setStarter(const Starter &starter)
    {
        art_ = readArt(starter.art,1920);
        crop_ = starter.cropHeight;
        const bool available = !art_.isNull();
        setAccessibleName(available ? tr("Enlarge %1 artwork").arg(tr(starter.title)) : tr("Blank project preview"));
        setToolTip(available ? tr("Open full-size artwork") : QString{});
        setEnabled(available);
        update();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QPushButton::resizeEvent(event);
        if (event->size().width() != event->oldSize().width())
            setFixedHeight(qMax(138,event->size().width()*9/16));
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        const QRect bounds = rect().adjusted(1,1,-1,-1);
        QPainterPath clip;
        clip.addRoundedRect(bounds,8,8);
        painter.setClipPath(clip);
        if (art_.isNull()) {
            QRadialGradient shade(bounds.center(),bounds.width()*0.65);
            shade.setColorAt(0,QColor("#322746"));
            shade.setColorAt(1,QColor("#161622"));
            painter.fillRect(bounds,shade);
            blank_.paint(&painter,QRect(bounds.center()-QPoint(32,32),QSize(64,64)));
        } else {
            drawCover(painter,bounds,art_,crop_);
            const QRect zoom(width()-46,12,34,34);
            painter.setPen(Qt::NoPen);
            painter.setBrush(underMouse() ? QColor(24,21,34,240) : QColor(17,16,25,200));
            painter.drawRoundedRect(zoom,6,6);
            expand_.paint(&painter,zoom.adjusted(8,8,-8,-8));
        }
        painter.setClipping(false);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(hasFocus() ? QColor("#bd9cff") : QColor("#596689"),hasFocus() ? 2 : 1));
        painter.drawRoundedRect(bounds,8,8);
    }

private:
    QPixmap art_;
    QIcon blank_, expand_;
    double crop_ = 0.5;
};

QScrollArea *scrollable(QWidget *content)
{
    auto *scroll = new QScrollArea;
    scroll->setObjectName("launcherScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    return scroll;
}
}

ProjectLauncher::ProjectLauncher(QAbstractItemModel *projects,QWidget *parent) : QWidget(parent)
{
    setObjectName("launcher");
    setStyleSheet(skin);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    layout->setSpacing(0);

    auto *header = new LauncherHeader;
    auto *masthead = new QVBoxLayout(header);
    masthead->setContentsMargins(0,0,0,0);
    masthead->setSpacing(0);
    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(14,0,0,0);
    headerRow->setSpacing(7);
    auto *logo = label("W",26,true);
    logo->setStyleSheet("font-family:Georgia;font-size:26px;font-weight:700;color:#f6e3ff;");
    auto *brand = label(tr("WorldForge Engine"),17,true);
    auto *tagline = label(tr("Next-Generation AI-Native Game Creation Platform"),12);
    for (auto *item : {logo,brand,tagline}) {
        item->setAttribute(Qt::WA_TransparentForMouseEvents);
        headerRow->addWidget(item);
    }
    headerRow->addStretch();
    auto *recentButton = button(tr("Recent Projects"),"folder");
    recentButton->setStyleSheet("font-size:11px;min-height:19px;padding:1px 7px;");
    headerRow->addWidget(recentButton);
    connect(recentButton,&QPushButton::clicked,this,&ProjectLauncher::showRecent);
    const auto headerAction = [&](const QString &title,const QString &icon) {
        auto *item = new QToolButton(header);
        item->setProperty("launcherAction",true);
        item->setAutoRaise(true);
        item->setIcon(editorIcon(icon));
        item->setIconSize({17,17});
        item->setFixedSize(27,29);
        item->setAccessibleName(title);
        item->setToolTip(title);
        headerRow->addWidget(item);
        return item;
    };
    connect(headerAction(tr("Account"),"person"),&QToolButton::clicked,this,[this] {
        QMessageBox::information(this,tr("Account"),tr("Local workspace · %1").arg(QDir::home().dirName()));
    });
    connect(headerAction(tr("Notifications"),"bell"),&QToolButton::clicked,this,[this] {
        const auto message = error_->isVisible() ? error_->text() : tr("No project creation notifications.");
        QMessageBox::information(this,tr("Notifications"),message);
    });
    connect(headerAction(tr("Help"),"help"),&QToolButton::clicked,this,[this] {
        QMessageBox::information(this,tr("Create New Project"),
            tr("Choose a template, name the project, and choose its location. Starter Content determines the initial scene. Recent Projects opens saved projects."));
    });
    headerRow->addWidget(new WindowCaption(*window(),40,header));
    masthead->addLayout(headerRow);
    auto *intro = new QWidget(header);
    intro->setObjectName("launcherIntro");
    intro->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *introRow = new QHBoxLayout(intro);
    introRow->setContentsMargins(24,3,12,4);
    introRow->setSpacing(10);
    auto *mark = new QLabel;
    mark->setPixmap(editorIcon("layers",QColor("#d297ff")).pixmap(35,35));
    introRow->addWidget(mark);
    auto *introText = new QVBoxLayout;
    introText->setSpacing(0);
    introText->addWidget(label(tr("Create New Project"),24,true));
    introText->addWidget(label(tr("Start a new game project with templates, AI assistance, or from scratch."),12));
    introRow->addLayout(introText,1);
    masthead->addWidget(intro);
    layout->addWidget(header);

    pages_ = new QStackedWidget;
    layout->addWidget(pages_,1);
    connect(pages_,&QStackedWidget::currentChanged,this,[intro](int index) {
        intro->setVisible(index == 0);
    });

    auto *creator = new QWidget;
    auto *creatorLayout = new QVBoxLayout(creator);
    creatorLayout->setContentsMargins(0,0,0,0);
    creatorLayout->setSpacing(0);
    auto *body = new QWidget;
    body->setObjectName("launcherBody");
    auto *content = new QVBoxLayout(body);
    content->setContentsMargins(13,3,13,9);
    content->setSpacing(10);
    auto *top = new QHBoxLayout;
    top->setSpacing(9);
    content->addLayout(top,1);
    auto *catalog = new QVBoxLayout;
    catalog->setSpacing(7);
    top->addLayout(catalog,21);

    auto *modeRow = new QHBoxLayout;
    modeRow->setSpacing(7);
    auto *modes = new QButtonGroup(this);
    const QStringList types{tr("Game Project"),tr("Simulation"),tr("Cinematic"),
        tr("Virtual Production"),tr("AI Assisted Project")};
    const QStringList modeIcons{"gamepad","box","film","video","spark"};
    for (int i = 0; i < types.size(); ++i) {
        auto *mode = button(types[i],modeIcons[i]);
        mode->setCheckable(true);
        mode->setProperty("mode",true);
        mode->setFixedHeight(32);
        mode->setToolTip(tr("Stores a project type label. Starter Content controls the initial scene."));
        modes->addButton(mode);
        modeRow->addWidget(mode,1);
        connect(mode,&QPushButton::clicked,this,[this,type=types[i]] { category_ = type; });
        mode->setChecked(i == 0);
    }
    category_ = types.front();
    catalog->addLayout(modeRow);
    auto *templateHeading = surface();
    templateHeading->setContentsMargins(9,3,9,3);
    addSectionTitle(templateHeading,1,tr("Choose a Template"));
    catalog->addWidget(templateHeading->parentWidget());

    auto *cards = new QGridLayout;
    cards->setSpacing(2);
    auto *group = new QButtonGroup(this);
    for (int i = 0; i < int(starters.size()); ++i) {
        auto *item = new TemplateButton(starters[i]);
        group->addButton(item,i);
        cards_.append(item);
        cards->addWidget(item,i/4,i%4);
    }
    for (int col = 0; col < 4; ++col)
        cards->setColumnStretch(col,1);
    cards->setRowStretch(0,1);
    cards->setRowStretch(1,1);
    connect(group,&QButtonGroup::idClicked,this,&ProjectLauncher::selectTemplate);
    catalog->addLayout(cards,1);

    auto *showcase = surface();
    showcase->setContentsMargins(9,6,9,10);
    showcase->setSpacing(6);
    auto *previewHeader = new QHBoxLayout;
    auto *eye = new QLabel;
    eye->setPixmap(editorIcon("eye").pixmap(18,18));
    previewHeader->addWidget(eye);
    previewHeader->addWidget(label(tr("Template Preview"),15,true),1);
    showcase->addLayout(previewHeader);
    previewArt_ = new ArtworkButton;
    showcase->addWidget(previewArt_);
    connect(previewArt_,&QPushButton::clicked,this,[this] { preview(); });
    auto *overlay = new QGridLayout(previewArt_);
    overlay->setContentsMargins(0,0,0,0);
    play_ = new QToolButton(previewArt_);
    play_->setAccessibleName(tr("Play artwork preview"));
    play_->setToolTip(tr("Play artwork preview"));
    play_->setIcon(editorIcon("playFill"));
    play_->setIconSize({23,23});
    play_->setFixedSize(45,45);
    play_->setCursor(Qt::PointingHandCursor);
    play_->setStyleSheet("QToolButton{background:rgba(9,13,27,205);border:2px solid #eee5ff;border-radius:22px;}"
                        "QToolButton:hover,QToolButton:focus{background:#5a238b;}");
    overlay->addWidget(play_,0,0,Qt::AlignCenter);
    connect(play_,&QToolButton::clicked,this,[this] { preview(true); });

    auto *previewCaption = new QHBoxLayout;
    auto *previewIcon = new QLabel;
    previewIcon->setPixmap(editorIcon("gamepad",QColor("#d190ff")).pixmap(19,19));
    previewCaption->addWidget(previewIcon);
    previewTitle_ = label({},17,true);
    previewCaption->addWidget(previewTitle_,1);
    recommended_ = label(tr("Recommended"),10,true);
    recommended_->setStyleSheet("color:#71f88c;background:#0c251a;border:1px solid #1c7d3b;border-radius:5px;padding:2px 5px;");
    previewCaption->addWidget(recommended_);
    showcase->addLayout(previewCaption);
    previewText_ = label({},12);
    previewText_->setWordWrap(true);
    showcase->addWidget(previewText_);
    previewFeatures_ = label({},11);
    previewFeatures_->setObjectName("templateFeatures");
    previewFeatures_->setTextFormat(Qt::RichText);
    previewFeatures_->setWordWrap(true);
    showcase->addWidget(previewFeatures_);
    showcase->addStretch(1);
    top->addWidget(showcase->parentWidget(),10);

    auto *bottom = new QHBoxLayout;
    bottom->setSpacing(12);
    content->addLayout(bottom);
    auto *details = surface();
    details->setContentsMargins(13,7,13,9);
    details->setSpacing(5);
    addSectionTitle(details,2,tr("Project Details"));
    auto *fields = new QGridLayout;
    fields->setSpacing(5);
    fields->addWidget(label(tr("Project Name *"),12,true),0,0);
    fields->addWidget(label(tr("Project Location"),12,true),2,0);
    name_ = new QLineEdit("WF_Project");
    name_->setMaxLength(64);
    name_->setAccessibleName(tr("Project name"));
    fields->addWidget(name_,1,0);
    auto *folderRow = new QHBoxLayout;
    folderRow->setSpacing(5);
    location_ = new QLineEdit;
    location_->setAccessibleName(tr("Project location"));
    location_->setReadOnly(true);
    location_->setMinimumWidth(0);
    folderRow->addWidget(location_,1);
    auto *browse = button({},"folder");
    browse->setAccessibleName(tr("Browse project folder"));
    browse->setToolTip(tr("Choose where to save the project"));
    browse->setFixedWidth(32);
    folderRow->addWidget(browse);
    fields->addLayout(folderRow,3,0);
    details->addLayout(fields);
    bottom->addWidget(details->parentWidget(),47);

    auto *setup = surface();
    setup->setContentsMargins(13,7,13,9);
    setup->setSpacing(5);
    addSectionTitle(setup,3,tr("Project Configuration"));
    auto *settings = new QGridLayout;
    settings->setHorizontalSpacing(9);
    settings->setVerticalSpacing(5);
    settings->setColumnStretch(0,2);
    settings->setColumnStretch(1,2);
    starterContent_ = new QComboBox;
    starterContent_->setAccessibleName(tr("Starter Content"));
    starterContent_->addItem(tr("Empty Scene"),"blank");
    starterContent_->addItem(tr("Rolling Hills with Trees"),"terrain");
    starterContent_->addItem(tr("Physics Lab"),"physics");
    starterContent_->addItem(tr("Playable Street · repair shop, traffic, mission"),"street");
    quality_ = new QComboBox;
    quality_->setAccessibleName(tr("Graphics quality"));
    quality_->addItem(tr("Standard (Default)"),true);
    quality_->addItem(tr("Performance · Sun shadows off"),false);
    settings->addWidget(label(tr("Graphics Quality"),12,true),0,0);
    settings->addWidget(quality_,1,0);
    settings->addWidget(label(tr("Starter Content"),12,true),2,0);
    settings->addWidget(starterContent_,3,0);
    settings->addWidget(label(tr("Target Platform"),12,true),0,1);
    auto *targets = new QHBoxLayout;
    targets->setSpacing(4);
    for (const QString &platform : {tr("Windows"),tr("macOS"),tr("Linux"),tr("Xbox"),tr("PlayStation")}) {
        const bool supported = platform == tr("Windows");
        auto *target = button(platform,supported ? "screen" : "");
        target->setProperty("primary",supported);
        target->setEnabled(supported);
        target->setStyleSheet("font-size:10px;padding:2px 4px;");
        target->setFocusPolicy(Qt::NoFocus);
        target->setAttribute(Qt::WA_TransparentForMouseEvents);
        targets->addWidget(target,1);
    }
    settings->addLayout(targets,1,1);
    settings->setRowStretch(4,1);
    setup->addLayout(settings);
    bottom->addWidget(setup->parentWidget(),53);
    creatorLayout->addWidget(scrollable(body),1);

    auto *footer = surface();
    footer->setContentsMargins(13,4,13,4);
    auto *actions = new QHBoxLayout;
    error_ = label({},12);
    error_->setProperty("error",true);
    error_->setWordWrap(true);
    error_->setAccessibleName(tr("Project creation status"));
    actions->addWidget(error_);
    actions->addStretch();
    auto *cancel = button(tr("Cancel"),"close");
    auto *create = button(tr("Create Project"),"check");
    create->setProperty("primary",true);
    cancel->setMinimumWidth(106);
    create->setMinimumWidth(152);
    for (auto *item : {cancel,create}) {
        item->setFixedHeight(32);
        actions->addWidget(item);
    }
    connect(cancel,&QPushButton::clicked,this,&ProjectLauncher::showRecent);
    footer->addLayout(actions);
    creatorLayout->addWidget(footer->parentWidget());

    auto *statusStrip = new QFrame;
    statusStrip->setObjectName("launcherStatus");
    statusStrip->setStyleSheet("QFrame#launcherStatus{background:#080d1a;border-top:1px solid #31254c;}");
    auto *status = new QHBoxLayout(statusStrip);
    status->setContentsMargins(15,4,15,4);
    status->setSpacing(14);
    status->addWidget(label(tr("Renderer: Vulkan  |  Target: Windows"),11));
    const auto memory = SystemMetrics().sample({},0,0).ramTotal;
    if (memory)
        status->addWidget(label(tr("RAM: %1 GiB").arg(double(*memory)/(1024*1024*1024),0,'f',1),11));
    auto *storageLabel = label({},11);
    status->addWidget(storageLabel);
    status->addStretch();
    creatorLayout->addWidget(statusStrip);
    pages_->addWidget(creator);

    auto *recentPage = new QWidget;
    recentPage->setObjectName("recentPage");
    auto *recentLayout = new QVBoxLayout(recentPage);
    recentLayout->setContentsMargins(28,24,28,26);
    recentLayout->setSpacing(16);
    auto *recentHeading = new QHBoxLayout;
    recentHeading->addWidget(label(tr("Recent Projects"),24,true),1);
    auto *newProject = button(tr("Create New Project"),"plus");
    newProject->setProperty("primary",true);
    recentHeading->addWidget(newProject);
    recentLayout->addLayout(recentHeading);
    connect(newProject,&QPushButton::clicked,this,&ProjectLauncher::showCreator);
    recent_ = new QListView;
    recent_->setAccessibleName(tr("Local projects"));
    recent_->setModel(projects);
    recent_->setAlternatingRowColors(false);
    recentLayout->addWidget(recent_,1);
    auto *emptyRecent = label(tr("Your projects will appear here after you create them."),14,false,true);
    emptyRecent->setAlignment(Qt::AlignCenter);
    recentLayout->addWidget(emptyRecent,1);
    auto *recentActions = new QHBoxLayout;
    auto *rename = button(tr("Rename"));
    auto *remove = button(tr("Delete"));
    auto *open = button(tr("Open Project"),"folder");
    open->setProperty("primary",true);
    recentActions->addWidget(rename);
    recentActions->addWidget(remove);
    recentActions->addStretch();
    recentActions->addWidget(open);
    recentLayout->addLayout(recentActions);
    pages_->addWidget(recentPage);
    const auto refreshRecent = [this,open,rename,remove,emptyRecent] {
        const bool available = recent_->model()->rowCount() > 0;
        recent_->setVisible(available);
        emptyRecent->setVisible(!available);
        if (available && !recent_->currentIndex().isValid())
            recent_->setCurrentIndex(recent_->model()->index(0,0));
        for (auto *control : {open,rename,remove})
            control->setEnabled(available);
    };
    connect(projects,&QAbstractItemModel::rowsInserted,this,refreshRecent);
    connect(projects,&QAbstractItemModel::rowsRemoved,this,refreshRecent);
    connect(recent_,&QListView::activated,this,&ProjectLauncher::openRequested);
    connect(open,&QPushButton::clicked,this,&ProjectLauncher::openRequested);
    connect(rename,&QPushButton::clicked,this,&ProjectLauncher::renameRequested);
    connect(remove,&QPushButton::clicked,this,&ProjectLauncher::deleteRequested);
    connect(create,&QPushButton::clicked,this,&ProjectLauncher::submit);
    connect(name_,&QLineEdit::returnPressed,this,&ProjectLauncher::submit);
    folder_ = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath("WorldForgeProjects");
    const auto updateStorage = [this,storageLabel] {
        QDir volume(folder_);
        while (!volume.exists() && volume.cdUp()) {}
        const QStorageInfo storage(volume.absolutePath());
        storageLabel->setVisible(storage.isReady() && storage.bytesAvailable() >= 0);
        if (storage.isReady() && storage.bytesAvailable() >= 0)
            storageLabel->setText(tr("Storage: %1 GiB free").arg(double(storage.bytesAvailable())/(1024*1024*1024),0,'f',1));
    };
    connect(browse,&QPushButton::clicked,this,[this,updateStorage] {
        const auto folder = QFileDialog::getExistingDirectory(this,tr("Choose parent folder"),folder_);
        if (!folder.isEmpty()) {
            folder_ = folder;
            updateLocation();
            updateStorage();
        }
    });
    connect(name_,&QLineEdit::textChanged,this,&ProjectLauncher::updateLocation);
    refreshRecent();
    updateLocation();
    updateStorage();
    selectTemplate(0);
}

void ProjectLauncher::showCreator() { pages_->setCurrentIndex(0); }
void ProjectLauncher::showRecent() { pages_->setCurrentIndex(1); }
void ProjectLauncher::showError(const QString &message)
{
    error_->setText(message);
    error_->setVisible(!message.isEmpty());
}

void ProjectLauncher::updateLocation()
{
    const auto path = QDir::toNativeSeparators(QDir(folder_).filePath(name_->text().trimmed()));
    location_->setText(path);
    location_->setCursorPosition(0);
    location_->setToolTip(path);
    showError({});
}

void ProjectLauncher::selectTemplate(int index)
{
    selected_ = index;
    cards_[index]->setChecked(true);
    const auto &starter = starters[index];
    static_cast<ArtworkButton *>(previewArt_)->setStarter(starter);
    previewTitle_->setText(tr(starter.title));
    previewText_->setText(tr(starter.summary));
    recommended_->setVisible(index == 0);
    play_->setVisible(*starter.art);
    const QStringList features{
        *starter.art ? tr(starter.artDetail) : tr("Empty scene"),
        tr("Windows Player export"),
        starter.content == 1 ? tr("Rolling hills and trees by default") : tr("Terrain available in editor"),
        tr("Dynamic sunlight and fog"),
        *starter.art ? tr("Animated artwork preview") : tr("Choose starter content"),
        tr("Vulkan viewport")
    };
    QString featureTable = "<table width='100%' cellspacing='0' cellpadding='2'>";
    for (int i = 0; i < features.size(); i += 2) {
        featureTable += QString("<tr><td width='50%'><span style='color:#69f684'>✓</span> %1</td>"
            "<td width='50%'><span style='color:#69f684'>✓</span> %2</td></tr>")
            .arg(features[i].toHtmlEscaped(),features[i+1].toHtmlEscaped());
    }
    previewFeatures_->setText(featureTable + "</table>");
    starterContent_->setCurrentIndex(starter.content);
    showError({});
}

void ProjectLauncher::preview(bool animated)
{
    const auto &starter = starters[selected_];
    if (!*starter.art)
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(animated ? tr("%1 · Artwork Preview").arg(tr(starter.title)) : tr(starter.title));
    auto *layout = new QVBoxLayout(&dialog);
    auto *image = new QLabel;
    const auto available = screen()->availableGeometry().size()*0.82;
    const auto pixmap = readArt(starter.art,available.width()*1.2);
    const QSize display = pixmap.size().scaled(available,Qt::KeepAspectRatio);
    image->setFixedSize(display);
    image->setPixmap(pixmap.scaled(display,Qt::KeepAspectRatio,Qt::SmoothTransformation));
    layout->addWidget(image);
    QVariantAnimation motion(&dialog);
    if (animated) {
        motion.setDuration(4200);
        motion.setStartValue(1.0);
        motion.setEndValue(1.15);
        motion.setEasingCurve(QEasingCurve::InOutSine);
        connect(&motion,&QVariantAnimation::valueChanged,image,[image,pixmap,display](const QVariant &value) {
            const auto cropSize = pixmap.size()/value.toDouble();
            const QRect crop(QPoint((pixmap.width()-cropSize.width())/2,(pixmap.height()-cropSize.height())/2),cropSize);
            QPixmap frame(display);
            QPainter painter(&frame);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawPixmap(frame.rect(),pixmap,crop);
            painter.end();
            image->setPixmap(frame);
        });
        motion.start();
    }
    dialog.exec();
}

void ProjectLauncher::submit()
{
    const auto &starter = starters[selected_];
    QJsonObject world;
    if (!quality_->currentData().toBool()) world.insert("shadows",false);
    emit createRequested(name_->text(),{{"location",QDir::fromNativeSeparators(location_->text())},
        {"template",starter.title},{"starter",starterContent_->currentData().toString()},
        {"category",category_},
        {"platform","Windows"},{"renderer","Vulkan"},{"quality",quality_->currentData().toBool() ? "high" : "performance"},
        {"worldDefaults",world}});
}
