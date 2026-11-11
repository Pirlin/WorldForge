#include "Workspaces/StudioViews.h"

#include "DesignSystem/Icons.h"
#include "Workspaces/WorkspaceCatalog.h"

#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QComboBox>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSlider>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QSyntaxHighlighter>
#include <QTableWidget>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {
const QList<QColor> SeriesColors{"#a56bff", "#57d9df", "#7fca79", "#f4b668"};

class LineNumbers : public QWidget {
public:
    using QWidget::QWidget;
    std::function<void(QPainter &)> draw;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#181b28"));
        if (draw)
            draw(painter);
    }
};

class StudioCodeEditor : public QPlainTextEdit {
public:
    explicit StudioCodeEditor(QWidget *parent) : QPlainTextEdit(parent), gutter_(new LineNumbers(this))
    {
        setViewportMargins(40, 0, 0, 0);
        gutter_->draw = [this](QPainter &painter) {
            painter.setPen(QColor("#a2a8ba"));
            painter.setFont(font());
            for (auto block = firstVisibleBlock(); block.isValid(); block = block.next()) {
                const auto bounds = blockBoundingGeometry(block).translated(contentOffset());
                if (bounds.top() >= gutter_->height())
                    break;
                if (block.isVisible() && bounds.bottom() >= 0)
                    painter.drawText(QRectF(0, bounds.top(), 34, blockBoundingRect(block).height()),
                                     Qt::AlignRight | Qt::AlignVCenter, QString::number(block.blockNumber() + 1));
            }
        };
        connect(this, &QPlainTextEdit::updateRequest, gutter_, [this] { gutter_->update(); });
        connect(this, &QPlainTextEdit::blockCountChanged, gutter_, [this] { gutter_->update(); });
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QPlainTextEdit::resizeEvent(event);
        const auto content = viewport()->geometry();
        gutter_->setGeometry(content.left() - 40, content.top(), 40, content.height());
    }

private:
    LineNumbers *gutter_;
};

QRect crop(const QJsonArray &values)
{
    if (values.size() != 4)
        return {};
    return {values.at(0).toInt(), values.at(1).toInt(), values.at(2).toInt(), values.at(3).toInt()};
}

class ReferencePreview : public QWidget {
public:
    ReferencePreview(QPixmap art, QWidget *parent) : QWidget(parent), art_(std::move(art))
    {
        setMinimumSize(65, 45);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#101421"));
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        if (art_.isNull())
            return;
        QRect target(QPoint(), art_.size().scaled(size(), Qt::KeepAspectRatio));
        target.moveCenter(rect().center());
        painter.drawPixmap(target, art_);
    }

private:
    QPixmap art_;
};

class GalleryTile : public QStyledItemDelegate {
public:
    GalleryTile(QSize size, QObject *parent) : QStyledItemDelegate(parent), size_(size) {}

protected:
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        const auto tile = option.rect.adjusted(3, 3, -3, -3);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        painter->setPen(QPen(selected ? QColor("#a660ff") : QColor("#292e40"), selected ? 1.5 : 1.0));
        painter->setBrush(selected ? QColor("#342052") : QColor("#202431"));
        painter->drawRoundedRect(tile, 5, 5);
        const auto imageRect = tile.adjusted(4, 4, -4, -27);
        const auto image = index.data(Qt::UserRole).value<QPixmap>();
        if (!image.isNull()) {
            QRect target(QPoint(), image.size().scaled(imageRect.size(), Qt::KeepAspectRatio));
            target.moveCenter(imageRect.center());
            painter->drawPixmap(target, image);
        } else {
            editorIcon("box", QColor("#bc93ff")).paint(painter, imageRect);
        }
        auto font = option.font;
        font.setPixelSize(10);
        painter->setFont(font);
        const auto name = painter->fontMetrics().elidedText(index.data().toString(), Qt::ElideRight, tile.width() - 5);
        painter->setPen(QColor("#e9e8f0"));
        painter->drawText(QRect(tile.left(), tile.bottom() - 25, tile.width(), 13), Qt::AlignCenter, name);
        painter->setPen(QColor("#a7a8bd"));
        painter->drawText(QRect(tile.left(), tile.bottom() - 12, tile.width(), 12), Qt::AlignCenter,
            index.data(Qt::UserRole + 1).toString());
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return size_; }

private:
    QSize size_;
};

QWidget *sampleGallery(const QJsonArray &view, const QString &reference, QWidget *parent)
{
    const auto rows = view.at(2).toArray();
    const auto dimensions = view.size() > 3 ? view.at(3).toArray() : QJsonArray{};
    const auto tileSize = dimensions.size() == 2 ? QSize(dimensions.at(0).toInt(), dimensions.at(1).toInt()) : QSize(89, 108);
    auto *pane = new QWidget(parent);
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(5, 4, 5, 4);
    layout->setSpacing(4);
    auto *search = new QLineEdit(pane);
    search->setPlaceholderText(QObject::tr("Search assets…"));
    search->setClearButtonEnabled(true);
    search->addAction(editorIcon("search"), QLineEdit::LeadingPosition);
    layout->addWidget(search);
    search->setVisible(view.size() < 5 || view.at(4).toBool());
    auto *gallery = new QListWidget(pane);
    gallery->setObjectName("studioGallery");
    gallery->setViewMode(QListView::IconMode);
    gallery->setResizeMode(QListView::Adjust);
    gallery->setMovement(QListView::Static);
    gallery->setGridSize(tileSize);
    gallery->setItemDelegate(new GalleryTile(tileSize, gallery));
    for (const auto &entry : rows) {
        const auto row = entry.toArray();
        auto *tile = new QListWidgetItem(row.at(0).toString(), gallery);
        tile->setData(Qt::UserRole, referenceArt(reference, crop(row.at(2).toArray())));
        tile->setData(Qt::UserRole + 1, row.at(1).toString());
        tile->setToolTip(row.at(0).toString());
    }
    if (gallery->count())
        gallery->setCurrentRow(0);
    layout->addWidget(gallery, 1);
    QObject::connect(search, &QLineEdit::textChanged, gallery, [gallery](const QString &query) {
        for (int row = 0; row < gallery->count(); ++row) {
            auto *item = gallery->item(row);
            item->setHidden(!item->text().contains(query, Qt::CaseInsensitive));
        }
    });
    return pane;
}

QWidget *splitView(const QJsonArray &view, const QString &reference, QWidget *parent)
{
    const auto orientation = view.size() > 3 && view.at(3).toString() == "vertical" ? Qt::Vertical : Qt::Horizontal;
    auto *split = new QSplitter(orientation, parent);
    split->setChildrenCollapsible(false);
    const auto children = view.at(2).toArray();
    const auto weights = view.size() > 4 ? view.at(4).toArray() : QJsonArray{};
    QList<int> sizes;
    for (qsizetype index = 0; index < children.size(); ++index) {
        const auto description = children.at(index).toArray();
        auto *pane = new QWidget(split);
        auto *layout = new QVBoxLayout(pane);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(1);
        auto *title = new QLabel(studioViewName(description), pane);
        title->setObjectName("studioPaneHeader");
        layout->addWidget(title);
        layout->addWidget(createStudioView(description, pane, reference), 1);
        split->addWidget(pane);
        sizes.append(index < weights.size() ? weights.at(index).toInt() : 100);
    }
    split->setSizes(sizes);
    return split;
}

QStringList strings(const QJsonArray &array)
{
    QStringList result;
    for (const auto &entry : array)
        result.append(entry.toString());
    return result;
}

QWidget *audioMixer(const QStringList &tracks, QWidget *parent)
{
    auto *pane = new QWidget(parent);
    auto *layout = new QHBoxLayout(pane);
    layout->setContentsMargins(3, 3, 3, 3);
    layout->setSpacing(3);
    for (const auto &name : tracks) {
        auto *channel = new QWidget(pane);
        channel->setObjectName("studioSectionBody");
        auto *column = new QVBoxLayout(channel);
        column->setContentsMargins(3, 3, 3, 3);
        column->setSpacing(3);
        auto *title = new QLabel(name, channel);
        title->setAlignment(Qt::AlignCenter);
        column->addWidget(title);
        auto *level = new QLabel(QStringLiteral("0 dB"), channel);
        level->setAlignment(Qt::AlignCenter);
        column->addWidget(level);
        auto *gain = new QSlider(Qt::Vertical, channel);
        gain->setRange(-60, 12);
        gain->setValue(0);
        gain->setAccessibleName(name + QObject::tr(" gain"));
        column->addWidget(gain, 1, Qt::AlignHCenter);
        QObject::connect(gain, &QSlider::valueChanged, level, [level](int value) {
            level->setText(QObject::tr("%1 dB").arg(value));
        });
        auto *controls = new QHBoxLayout;
        controls->setSpacing(2);
        for (const auto &label : {QString("M"), QString("S")}) {
            auto *button = new QToolButton(channel);
            button->setText(label);
            button->setAccessibleName(name + (label == "M" ? QObject::tr(" mute") : QObject::tr(" solo")));
            button->setCheckable(true);
            button->setFixedWidth(23);
            controls->addWidget(button);
        }
        column->addLayout(controls);
        layout->addWidget(channel, 1);
    }
    return pane;
}

class SizeBreakdown : public QWidget {
public:
    explicit SizeBreakdown(const QJsonArray &values, QWidget *parent) : QWidget(parent), values_(values)
    {
        setMinimumHeight(100);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const int diameter = std::max(24, std::min(height() - 18, width() / 2 - 8));
        QRectF disk(7, (height() - diameter) / 2.0, diameter, diameter);
        disk.adjust(6, 6, -6, -6);
        double total = 0;
        for (const auto &entry : values_)
            total += entry.toArray().at(1).toDouble();
        if (total <= 0)
            return;
        int angle = 90 * 16;
        int row = 0;
        for (const auto &entry : values_) {
            const auto fields = entry.toArray();
            const auto color = SeriesColors[row % SeriesColors.size()];
            const auto amount = fields.at(1).toDouble();
            const int span = -qRound(360 * 16 * amount / total);
            painter.setPen(QPen(color, 9));
            painter.drawArc(disk, angle, span);
            angle += span;
            const int y = 10 + row * 15;
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawEllipse(QPointF(diameter + 18, y + 5), 3, 3);
            painter.setPen(QColor("#e8e8f0"));
            painter.drawText(diameter + 26, y + 9, QObject::tr("%1  %2 GB").arg(fields.at(0).toString()).arg(amount));
            ++row;
        }
        painter.setPen(QColor("#e8e8f0"));
        painter.drawText(disk, Qt::AlignCenter, QObject::tr("%1 GB").arg(total, 0, 'f', 1));
    }

private:
    QJsonArray values_;
};

QWidget *sampleTable(const QJsonArray &columns, const QJsonArray &rows, QWidget *parent)
{
    auto *pane = new QWidget(parent);
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(6, 5, 6, 5);
    auto *search = new QLineEdit(pane);
    search->setPlaceholderText(QObject::tr("Filter rows…"));
    search->setAccessibleName(QObject::tr("Filter rows"));
    search->setClearButtonEnabled(true);
    search->addAction(editorIcon("search"), QLineEdit::LeadingPosition);
    layout->addWidget(search);

    auto *table = new QTableWidget(int(rows.size()), int(columns.size()), pane);
    table->setHorizontalHeaderLabels(strings(columns));
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(24);
    table->horizontalHeader()->setStretchLastSection(true);
    for (qsizetype row = 0; row < rows.size(); ++row) {
        const auto values = rows[row].toArray();
        for (qsizetype column = 0; column < std::min(values.size(), columns.size()); ++column)
            table->setItem(int(row), int(column), new QTableWidgetItem(values[column].toString()));
    }
    table->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
    table->setSortingEnabled(true);
    table->resizeColumnsToContents();
    layout->addWidget(table, 1);
    QObject::connect(search, &QLineEdit::textChanged, table, [table](const QString &query) {
        for (int row = 0; row < table->rowCount(); ++row) {
            bool visible = query.isEmpty();
            for (int column = 0; !visible && column < table->columnCount(); ++column) {
                const auto *cell = table->item(row, column);
                visible = cell && cell->text().contains(query, Qt::CaseInsensitive);
            }
            table->setRowHidden(row, !visible);
        }
    });
    return pane;
}

QWidget *assetFolders(const QJsonArray &rows, QWidget *parent)
{
    auto *tree = new QTreeWidget(parent);
    tree->setHeaderHidden(true);
    tree->setIndentation(12);
    tree->setIconSize({13, 13});
    const auto addRows = [&](auto &&self, QTreeWidgetItem *parentItem, const QJsonArray &entries) -> void {
        for (const auto &entry : entries) {
            const auto fields = entry.toArray();
            auto *item = new QTreeWidgetItem(parentItem, {fields.at(0).toString()});
            item->setIcon(0, editorIcon("folderFill", QColor("#e8c353")));
            if (fields.size() > 1)
                self(self, item, fields.at(1).toArray());
            if (fields.at(0).toString() == "Models")
                tree->setCurrentItem(item);
        }
    };
    addRows(addRows, tree->invisibleRootItem(), rows);
    tree->expandAll();
    return tree;
}

QWidget *textureLayers(const QJsonArray &rows, const QString &reference, QWidget *parent)
{
    auto *table = new QTableWidget(int(rows.size()), 3, parent);
    table->setHorizontalHeaderLabels({QObject::tr("Layer"), QObject::tr("Blend"), QObject::tr("Opacity")});
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(27);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    table->horizontalHeader()->resizeSection(1, 72);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    table->horizontalHeader()->resizeSection(2, 91);
    table->setIconSize({24, 24});
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    for (qsizetype row = 0; row < rows.size(); ++row) {
        const auto fields = rows.at(row).toArray();
        const auto label = fields.at(0).toString();
        auto *item = new QTableWidgetItem(QIcon(referenceArt(reference, crop(fields.at(3).toArray()))), label);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        table->setItem(int(row), 0, item);
        auto *blend = new QComboBox(table);
        blend->setObjectName("layerBlend");
        blend->addItems({"Normal", "Overlay", "Add", "Multiply"});
        blend->setCurrentText(fields.at(1).toString());
        blend->setAccessibleName(label + QObject::tr(" blend"));
        table->setCellWidget(int(row), 1, blend);
        auto *opacity = new QWidget(table);
        auto *controls = new QHBoxLayout(opacity);
        controls->setContentsMargins(4, 0, 4, 0);
        controls->setSpacing(3);
        auto *slider = new QSlider(Qt::Horizontal, opacity);
        slider->setRange(0, 100);
        slider->setValue(fields.at(2).toInt());
        slider->setAccessibleName(label + QObject::tr(" opacity"));
        auto *value = new QLabel(QString::number(slider->value()) + "%", opacity);
        QObject::connect(slider, &QSlider::valueChanged, value, [value](int amount) {
            value->setText(QString::number(amount) + "%");
        });
        controls->addWidget(slider, 1);
        controls->addWidget(value);
        table->setCellWidget(int(row), 2, opacity);
    }
    if (table->rowCount())
        table->setCurrentCell(0, 0);
    return table;
}

class SampleHighlighter : public QSyntaxHighlighter {
public:
    explicit SampleHighlighter(QTextDocument *document) : QSyntaxHighlighter(document) {}

protected:
    void highlightBlock(const QString &text) override
    {
        static const QRegularExpression keywords(R"(\b(class|struct|float|double|bool|void|public|private|const|return|if|else|true|false|auto)\b)");
        static const QRegularExpression numbers(R"(\b\d+(?:\.\d+)?f?\b)");
        const auto paintMatches = [&](const QRegularExpression &pattern, const QColor &color) {
            auto matches = pattern.globalMatch(text);
            while (matches.hasNext()) {
                const auto match = matches.next();
                setFormat(int(match.capturedStart()), int(match.capturedLength()), color);
            }
        };
        paintMatches(keywords, QColor("#cb95ff"));
        paintMatches(numbers, QColor("#f3c484"));
        const auto comment = text.indexOf("//");
        if (comment >= 0)
            setFormat(int(comment), int(text.size() - comment), QColor("#83a889"));
    }
};

class GraphNode : public QGraphicsRectItem {
public:
    GraphNode(const QString &name, const QStringList &pins, const QColor &color)
        : QGraphicsRectItem(0, 0, 144, std::max(53, 29 + int(pins.size()) * 17))
    {
        setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
        setBrush(QColor("#1b2130"));
        setPen(QPen(color.lighter(155)));
        auto *header = new QGraphicsRectItem(1, 1, rect().width() - 2, 25, this);
        header->setPen(Qt::NoPen);
        header->setBrush(color);
        auto *label = new QGraphicsSimpleTextItem(QFontMetrics(QFont("Segoe UI", 9))
            .elidedText(name, Qt::ElideRight, 132), this);
        label->setFont(QFont("Segoe UI", 9));
        label->setBrush(QColor("#f2e9fc"));
        label->setPos(6, 4);
        setToolTip(name);
        for (const qreal x : {0.0, rect().width()}) {
            auto *pin = new QGraphicsEllipseItem(x - 3, 37, 6, 6, this);
            pin->setPen(Qt::NoPen);
            pin->setBrush(color.lighter(195));
        }
        for (qsizetype index = 0; index < pins.size(); ++index) {
            auto *detail = new QGraphicsSimpleTextItem(pins[index], this);
            detail->setFont(QFont("Segoe UI", 8));
            detail->setBrush(QColor("#ccd2e5"));
            detail->setPos(10, 31 + 17 * index);
        }
    }

    std::function<void()> moved;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override
    {
        if (change == ItemPositionHasChanged && moved)
            moved();
        return QGraphicsRectItem::itemChange(change, value);
    }
};

class StudioGraphView : public QGraphicsView {
public:
    using QGraphicsView::QGraphicsView;

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QGraphicsView::resizeEvent(event);
        if (scene() && !scene()->sceneRect().isEmpty()) {
            resetTransform();
            fitInView(scene()->sceneRect(), Qt::KeepAspectRatio);
        }
    }

    void drawBackground(QPainter *painter, const QRectF &bounds) override
    {
        painter->fillRect(bounds, QColor("#101421"));
        for (const int interval : {16, 80}) {
            painter->setPen(QPen(QColor(interval == 80 ? "#273049" : "#1c2335"), 0));
            for (qreal x = std::floor(bounds.left() / interval) * interval; x < bounds.right(); x += interval)
                painter->drawLine(QLineF(x, bounds.top(), x, bounds.bottom()));
            for (qreal y = std::floor(bounds.top() / interval) * interval; y < bounds.bottom(); y += interval)
                painter->drawLine(QLineF(bounds.left(), y, bounds.right(), y));
        }
    }
};

QWidget *nodeGraph(const QStringList &labels, const QJsonObject &layout, QWidget *parent)
{
    auto *view = new StudioGraphView(parent);
    auto *scene = new QGraphicsScene(view);
    view->setScene(scene);
    view->setRenderHint(QPainter::Antialiasing);
    view->setDragMode(QGraphicsView::ScrollHandDrag);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QList<GraphNode *> nodes;
    const auto positions = layout.value("positions").toArray();
    const auto pins = layout.value("pins").toArray();
    const auto colors = layout.value("colors").toArray();
    for (qsizetype index = 0; index < labels.size(); ++index) {
        const auto color = QColor(index < colors.size() ? colors.at(index).toString() : QString("#4e2c76"));
        auto *node = new GraphNode(labels[index], index < pins.size() ? strings(pins.at(index).toArray()) : QStringList{}, color);
        scene->addItem(node);
        if (index < positions.size()) {
            const auto point = positions.at(index).toArray();
            node->setPos(point.at(0).toDouble(), point.at(1).toDouble());
        } else {
            node->setPos(24 + 171 * (index % 3), 23 + 85 * (index / 3));
        }
        nodes.append(node);
    }
    const QRectF content = scene->itemsBoundingRect().adjusted(-20, -20, 20, 20);
    scene->setSceneRect(content);
    struct Edge { GraphNode *from; GraphNode *to; QGraphicsPathItem *path; };
    QList<Edge> edges;
    const auto addEdge = [&](int from, int to) {
        if (from < 0 || to < 0 || from >= nodes.size() || to >= nodes.size())
            return;
        auto *edge = scene->addPath(QPainterPath(), QPen(QColor("#b5a5d4"), 1.6));
        edge->setZValue(-1);
        edges.append({nodes[from], nodes[to], edge});
    };
    if (layout.contains("edges")) {
        for (const auto &edge : layout.value("edges").toArray()) {
            const auto link = edge.toArray();
            addEdge(link.at(0).toInt(), link.at(1).toInt());
        }
    } else {
        for (int index = 1; index < nodes.size(); ++index)
            addEdge(index - 1, index);
    }
    const auto connectNodes = [edges] {
        for (const auto &edge : edges) {
            const auto a = edge.from->pos() + QPointF(edge.from->rect().width(), 40);
            const auto b = edge.to->pos() + QPointF(0, 40);
            QPainterPath path(a);
            path.cubicTo(a + QPointF(52, 0), b - QPointF(52, 0), b);
            edge.path->setPath(path);
        }
    };
    for (auto *node : nodes)
        node->moved = connectNodes;
    connectNodes();
    return view;
}

class SampleChart : public QWidget {
public:
    enum class Mode { Metrics, Timeline, Waveform };

    SampleChart(QStringList tracks, Mode mode, QWidget *parent)
        : QWidget(parent), tracks_(std::move(tracks)), mode_(mode)
    {
        setMinimumHeight(95);
    }

    void setFrame(int frame) { frame_ = frame; update(); }
    std::function<void(int)> scrubbed;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor("#121622"));
        const bool transport = mode_ != Mode::Metrics;
        const QRect grid(transport ? 114 : 16, 21, width() - (transport ? 127 : 30), height() - 28);
        if (grid.width() <= 0 || grid.height() <= 0)
            return;
        const int rows = std::max(1, int(tracks_.size()));
        painter.setFont(QFont("Segoe UI", 8));
        for (int tick = 0; tick <= 6; ++tick) {
            const int x = grid.left() + tick * grid.width() / 6;
            painter.setPen(QColor("#30354a"));
            painter.drawLine(x, grid.top(), x, grid.bottom());
            painter.setPen(QColor("#a8b0c5"));
            if (tick < 6)
                painter.drawText(x + 3, 15, QString::number(tick * 20));
        }
        for (int row = 0; row < rows; ++row) {
            const auto color = SeriesColors[row % SeriesColors.size()];
            const int y = grid.top() + row * grid.height() / rows;
            const int band = grid.height() / rows;
            painter.setPen(QColor("#30354a"));
            painter.drawLine(grid.left(), y, grid.right(), y);
            if (transport) {
                painter.setPen(QColor("#d4d7e5"));
                painter.drawText(QRect(6, y, 105, band), Qt::AlignVCenter, tracks_.value(row));
            }
            if (mode_ == Mode::Timeline) {
                const QRect clip(grid.left() + 7 + row * 10, y + 4, (grid.width() * (10 - row) / 13), std::max(3, band - 8));
                painter.setPen(Qt::NoPen);
                painter.setBrush(color.darker(164));
                painter.drawRoundedRect(clip, 3, 3);
                painter.setBrush(color);
                for (int key = 0; key < 4; ++key) {
                    const QPointF p(clip.left() + (key + 1) * clip.width() / 5, clip.center().y());
                    painter.drawPolygon(QPolygonF({p + QPointF(0, -4), p + QPointF(4, 0), p + QPointF(0, 4), p + QPointF(-4, 0)}));
                }
            } else if (mode_ == Mode::Waveform) {
                const auto middle = y + band * 0.5;
                painter.setPen(QPen(color, 1));
                for (int x = 0; x < grid.width(); x += 2) {
                    const double phase = x * 0.17 + row * 2;
                    const double beat = std::abs(std::sin(phase * 0.09));
                    const double peak = band * 0.45 * (0.14 + 0.86 * beat) * std::abs(std::sin(phase) * std::cos(phase * 1.7));
                    painter.drawLine(QPointF(grid.left() + x, middle - peak), QPointF(grid.left() + x, middle + peak));
                }
            } else {
                QPainterPath path;
                for (int n = 0; n <= 90; ++n) {
                    const double wave = 0.48 + 0.23 * std::sin(n * 0.16 + row) + 0.13 * std::sin(n * 0.53 + row * 2);
                    const QPointF p(grid.left() + n * grid.width() / 90., y + band * wave);
                    n == 0 ? path.moveTo(p) : path.lineTo(p);
                }
                painter.setPen(QPen(color, 1.8));
                painter.drawPath(path);
                painter.drawText(grid.left() + 4, y + 13, tracks_.value(row));
            }
        }
        if (transport) {
            const int x = grid.left() + frame_ * grid.width() / 120;
            painter.setPen(QPen(QColor("#f1e0ff"), 1));
            painter.drawLine(x, grid.top(), x, grid.bottom());
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (mode_ != Mode::Metrics && scrubbed && event->button() == Qt::LeftButton && width() > 127)
            scrubbed(std::clamp(qRound((event->position().x() - 114) * 120 / (width() - 127)), 0, 120));
    }

private:
    QStringList tracks_;
    Mode mode_;
    int frame_ = 0;
};

QWidget *sampleTimeline(const QStringList &tracks, SampleChart::Mode mode, QWidget *parent)
{
    auto *pane = new QWidget(parent);
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(4, 3, 4, 4);
    auto *bar = new QHBoxLayout;
    auto *play = new QToolButton(pane);
    play->setCheckable(true);
    play->setIcon(editorIcon("play"));
    play->setToolTip(QObject::tr("Play simulated timeline"));
    play->setAccessibleName(QObject::tr("Play simulated timeline"));
    auto *frame = new QLabel(pane);
    auto *slider = new QSlider(Qt::Horizontal, pane);
    slider->setRange(0, 120);
    slider->setAccessibleName(QObject::tr("Timeline frame"));
    bar->addWidget(play);
    bar->addWidget(frame);
    bar->addWidget(slider, 1);
    layout->addLayout(bar);
    auto *chart = new SampleChart(tracks, mode, pane);
    layout->addWidget(chart, 1);
    const auto refresh = [chart, frame](int number) {
        frame->setText(QObject::tr("Frame %1 / 120").arg(number));
        chart->setFrame(number);
    };
    QObject::connect(slider, &QSlider::valueChanged, chart, refresh);
    chart->scrubbed = [slider](int value) { slider->setValue(value); };
    refresh(0);
    auto *timer = new QTimer(pane);
    timer->setInterval(33);
    QObject::connect(timer, &QTimer::timeout, pane, [pane, slider, play] {
        if (!pane->isVisible()) {
            play->setChecked(false);
            return;
        }
        slider->setValue((slider->value() + 1) % 121);
    });
    QObject::connect(play, &QToolButton::toggled, pane, [timer, play](bool running) {
        play->setIcon(editorIcon(running ? "stop" : "play"));
        running ? timer->start() : timer->stop();
    });
    return pane;
}
}

QString studioViewName(const QJsonArray &view)
{
    return view.at(0).toString();
}

QWidget *createStudioView(const QJsonArray &view, QWidget *parent, const QString &reference)
{
    const auto kind = view.at(1).toString();
    const auto values = view.size() > 2 ? view.at(2).toArray() : QJsonArray{};
    QWidget *widget = nullptr;
    if (kind == "table") {
        widget = sampleTable(values, view.at(3).toArray(), parent);
    } else if (kind == "gallery") {
        widget = sampleGallery(view, reference, parent);
    } else if (kind == "folders") {
        widget = assetFolders(values, parent);
    } else if (kind == "layers") {
        widget = textureLayers(values, reference, parent);
    } else if (kind == "image") {
        widget = new ReferencePreview(referenceArt(reference, crop(values)), parent);
    } else if (kind == "split") {
        widget = splitView(view, reference, parent);
    } else if (kind == "mixer") {
        widget = audioMixer(strings(values), parent);
    } else if (kind == "size") {
        widget = new SizeBreakdown(values, parent);
    } else if (kind == "graph") {
        widget = nodeGraph(strings(values), view.size() > 3 ? view.at(3).toObject() : QJsonObject{}, parent);
    } else if (kind == "timeline" || kind == "waveform") {
        widget = sampleTimeline(strings(values), kind == "waveform" ? SampleChart::Mode::Waveform
                                                                    : SampleChart::Mode::Timeline, parent);
    } else if (kind == "metrics") {
        widget = new SampleChart(strings(values), SampleChart::Mode::Metrics, parent);
        widget->setToolTip(QObject::tr("Simulated readings"));
    } else {
        auto *text = kind == "code" ? new StudioCodeEditor(parent) : new QPlainTextEdit(parent);
        text->setStyleSheet("font-family: Consolas; font-size: 13px");
        text->setReadOnly(kind != "code");
        text->setPlainText(strings(values).join('\n'));
        if (kind == "code") {
            text->setLineWrapMode(QPlainTextEdit::NoWrap);
            new SampleHighlighter(text->document());
        }
        widget = text;
    }
    widget->setAccessibleName(studioViewName(view));
    return widget;
}
