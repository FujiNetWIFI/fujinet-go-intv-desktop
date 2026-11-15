/*
 * The debugger's Video tab (Qt6 Widgets).
 *
 * Everything that decides what to show lives in the core (intvvidtab.h): this file is widgets.
 * The picture comes from the controller's cached buffer already opaque (0xFFRRGGBB), so
 * Format_RGB32 is safe on KWin; a mouse move redraws only the marks, never re-renders.
 * Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "VideoTab.h"
#include "Accent.h"

extern "C" {
#include "intvvidcap.h"
}

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

QColor accentColor()
{
    return intvAccentColor();
}

QLabel *monoLabel()
{
    auto *l = new QLabel;
    QFont f = l->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    l->setFont(f);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    return l;
}

QTableWidget *makeTable(const QStringList &heads)
{
    auto *t = new QTableWidget(0, heads.size());
    t->setHorizontalHeaderLabels(heads);
    t->verticalHeader()->hide();
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionMode(QAbstractItemView::NoSelection);
    t->horizontalHeader()->setStretchLastSection(true);
    QFont f = t->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    t->setFont(f);
    return t;
}

QString hex(unsigned v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

void setCell(QTableWidget *t, int row, int col, const QString &text, bool hot)
{
    auto *it = t->item(row, col);
    if (!it) {
        it = new QTableWidgetItem;
        t->setItem(row, col, it);
    }
    it->setText(text);
    it->setBackground(hot ? QBrush(accentColor()) : QBrush());
    it->setForeground(hot ? QBrush(intvAccentInk()) : QBrush());
}

} // namespace

/* ---- the picture ------------------------------------------------------------ */

class VideoPicture : public QWidget
{
public:
    explicit VideoPicture(VideoTab *owner) : m_owner(owner)
    {
        setMouseTracking(true);
        setMinimumSize(320, 200);
    }
    /* the controller's buffer, or a message when this view cannot be drawn */
    void sync()
    {
        int width = 0, height = 0;
        const uint32_t *pixels = intvvid_tab_picture(m_owner->m_tab, &width, &height);
        if (pixels) {
            /* a view over the controller's buffer: valid until its next refresh */
            m_image = QImage(reinterpret_cast<const uchar *>(pixels), width, height, width * 4, QImage::Format_RGB32);
            m_msg.clear();
            setFixedSize(width, height);
        } else {
            m_image = QImage();
            const char *why = intvvid_tab_why(m_owner->m_tab);
            m_msg = QString::fromUtf8(why ? why : "Nothing to show");
            setMinimumSize(320, 200);
            setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(18, 18, 24));
        if (m_image.isNull()) {
            p.setPen(QColor(140, 140, 148));
            p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, m_msg);
            return;
        }
        p.drawImage(0, 0, m_image);
        int hover[4] = { 0 }, pin[4] = { 0 };
        const int marks = intvvid_tab_marks(m_owner->m_tab, hover, pin);
        if (marks & 1) {
            p.setPen(QPen(QColor(255, 255, 255, 230), 1));
            p.drawRect(hover[0] - 1, hover[1] - 1, hover[2] + 1, hover[3] + 1);   /* just outside the pixel */
        }
        if (marks & 2) {
            p.setPen(QPen(QColor(255, 217, 26), 2));
            p.drawRect(pin[0] - 1, pin[1] - 1, pin[2] + 1, pin[3] + 1);
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const QPoint pt = e->position().toPoint();
        if (intvvid_tab_hover(m_owner->m_tab, pt.x(), pt.y())) {
            update();
            m_owner->updatePixelSection();
        }
    }
    void leaveEvent(QEvent *) override
    {
        if (intvvid_tab_hover(m_owner->m_tab, -1, -1)) {
            update();
            m_owner->updatePixelSection();
        }
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const QPoint pt = e->position().toPoint();
        intvvid_tab_pin(m_owner->m_tab, pt.x(), pt.y());
        update();
        m_owner->updatePixelSection();
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const QPoint pt = e->position().toPoint();
        intvvid_tab_hover(m_owner->m_tab, pt.x(), pt.y());
        m_owner->breakOnPixel();
        update();
    }

private:
    VideoTab *m_owner;
    QImage m_image;
    QString m_msg;
};

/* ---- the tab ------------------------------------------------------------------ */

VideoTab::VideoTab(intvdebug *debugger, QWidget *parent) : QWidget(parent)
{
    m_tab = intvvid_tab_new(debugger);

    auto *root = new QVBoxLayout(this);

    /* ---- toolbar: view, Decoded source, zoom, break; then the overlays */
    auto *bar = new QHBoxLayout;
    m_view = new QComboBox;
    for (int i = 0; i < INTVVID_V_COUNT; ++i) m_view->addItem(QString::fromUtf8(intvvid_view_name(i)));
    m_view->setToolTip(QStringLiteral(
        "Display: the last published frame, border included.\n"
        "Decoded: one source (Source) redrawn from the STIC's registers, the BACKTAB and GRAM.\n"
        "Beam: the Display, the lines the beam has not reached yet dimmed (stopped only).\n"
        "Values: each BACKTAB word as a flat false colour of its value."));
    m_source = new QComboBox;
    for (int i = 0; i < INTVVID_SRC_COUNT; ++i) m_source->addItem(QString::fromUtf8(intvvid_source_name(i)));
    m_source->setToolTip(QStringLiteral("What the Decoded view redraws: the BACKTAB cards, the 8 MOBs, the GROM/GRAM card sheet or the palette"));
    m_zoom = new QComboBox;
    int zoomCount = 0;
    const int *zooms = intvvid_zooms(&zoomCount);
    for (int i = 0; i < zoomCount; ++i)
        m_zoom->addItem(zooms[i] ? QStringLiteral("%1×").arg(zooms[i]) : QStringLiteral("Fit"), zooms[i]);
    bar->addWidget(new QLabel(QStringLiteral("View")));
    bar->addWidget(m_view);
    bar->addWidget(new QLabel(QStringLiteral("Source")));
    bar->addWidget(m_source);
    bar->addWidget(new QLabel(QStringLiteral("Zoom")));
    bar->addWidget(m_zoom);
    m_breakBtn = new QPushButton(QStringLiteral("Break on writes"));
    m_breakBtn->setToolTip(QStringLiteral("A write breakpoint on what the inspected pixel is made from: the BACKTAB word, the GRAM card or the MOB registers"));
    bar->addWidget(m_breakBtn);
    bar->addStretch(1);
    root->addLayout(bar);

    auto *overlays = new QHBoxLayout;
    overlays->addWidget(new QLabel(QStringLiteral("Overlays")));
    int overlayCount = 0;
    const intvvid_overlay_info *overlayInfo = intvvid_overlays(&overlayCount);
    for (int i = 0; i < overlayCount; ++i) {
        auto *box = new QCheckBox(QString::fromUtf8(overlayInfo[i].label));
        box->setProperty("bit", overlayInfo[i].bit);
        m_overlays << box;
        overlays->addWidget(box);
    }
    overlays->addStretch(1);
    root->addLayout(overlays);

    m_status = monoLabel();
    root->addWidget(m_status);

    auto *split = new QSplitter(Qt::Horizontal);
    root->addWidget(split, 1);

    /* ---- left: picture */
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    m_scroll = new QScrollArea;
    m_pic = new VideoPicture(this);
    m_pic->setObjectName(QStringLiteral("videoPicture"));
    m_scroll->setWidget(m_pic);
    m_scroll->setWidgetResizable(false);
    m_scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    lv->addWidget(m_scroll, 1);
    m_note = new QLabel;
    lv->addWidget(m_note);
    split->addWidget(left);

    /* ---- right: sections, in intvvid_section_name order */
    m_sections = new QTabWidget;
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        m_pixelText = monoLabel();
        m_pixelText->setWordWrap(true);
        v->addWidget(m_pixelText);
        auto *btns = new QHBoxLayout;
        auto *brk = new QPushButton(QStringLiteral("Break on its writes"));
        auto *unpin = new QPushButton(QStringLiteral("Unpin"));
        btns->addWidget(brk);
        btns->addWidget(unpin);
        btns->addStretch(1);
        v->addLayout(btns);
        v->addStretch(1);
        connect(brk, &QPushButton::clicked, this, [this] { breakOnPixel(); });
        connect(unpin, &QPushButton::clicked, this, [this] {
            intvvid_tab_unpin(m_tab);
            m_pic->update();
            updatePixelSection();
        });
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(w);
        m_sections->addTab(scroll, QString::fromUtf8(intvvid_section_name(INTVVID_S_PIXEL)));
    }
    m_regs = makeTable({ QStringLiteral("Name"), QStringLiteral("Value"), QStringLiteral("Meaning") });
    m_sections->addTab(m_regs, QString::fromUtf8(intvvid_section_name(INTVVID_S_REGISTERS)));
    m_swatches = makeTable({ QString(), QStringLiteral("Colour"), QStringLiteral("RGB"), QStringLiteral("Used"), QStringLiteral("Role") });
    m_swatches->verticalHeader()->setDefaultSectionSize(26);
    m_sections->addTab(m_swatches, QString::fromUtf8(intvvid_section_name(INTVVID_S_COLOURS)));
    m_lines = makeTable({ QStringLiteral("Row"), QStringLiteral("Line"), QStringLiteral("Address"), QStringLiteral("Used"),
                          QStringLiteral("Changed"), QStringLiteral("Background"), QStringLiteral("Fetched"), QStringLiteral("Words") });
    m_sections->addTab(m_lines, QString::fromUtf8(intvvid_section_name(INTVVID_S_LINES)));
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        m_beam = new QProgressBar;
        m_beam->setRange(0, 1000);
        m_beam->setFormat(QStringLiteral("beam"));
        v->addWidget(m_beam);
        m_timingText = monoLabel();
        m_timingText->setWordWrap(true);
        v->addWidget(m_timingText);
        v->addStretch(1);
        m_sections->addTab(w, QString::fromUtf8(intvvid_section_name(INTVVID_S_TIMING)));
    }
    split->addWidget(m_sections);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    m_sections->setMinimumWidth(480);
    split->setSizes({ 640, 640 });

    /* start from the controller's state (the STUDIO2_DEBUGGER_VIDEO_* variables) */
    m_updating = true;
    m_view->setCurrentIndex(intvvid_tab_view(m_tab));
    m_source->setCurrentIndex(intvvid_tab_source(m_tab));
    m_source->setEnabled(intvvid_tab_view(m_tab) == INTVVID_V_DECODED);
    for (int i = 0; i < zoomCount; ++i)
        if (zooms[i] == intvvid_tab_zoom(m_tab)) m_zoom->setCurrentIndex(i);
    for (QCheckBox *box : m_overlays) box->setChecked((intvvid_tab_overlays(m_tab) & box->property("bit").toUInt()) != 0);
    m_sections->setCurrentIndex(intvvid_tab_env_section(m_tab));
    updateBreakEnabled();
    m_updating = false;

    connect(m_view, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_updating || i < 0) return;
        intvvid_tab_set_view(m_tab, i);
        m_source->setEnabled(i == INTVVID_V_DECODED);
        refresh(true);
    });
    connect(m_source, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_updating || i < 0) return;
        intvvid_tab_set_source(m_tab, i);
        refresh(true);
    });
    connect(m_zoom, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_updating || i < 0) return;
        intvvid_tab_set_zoom(m_tab, m_zoom->itemData(i).toInt());
        refresh(true);
    });
    for (QCheckBox *box : m_overlays)
        connect(box, &QCheckBox::toggled, this, [this](bool) {
            if (m_updating) return;
            unsigned mask = 0;
            for (QCheckBox *each : m_overlays)
                if (each->isChecked()) mask |= each->property("bit").toUInt();
            intvvid_tab_set_overlays(m_tab, mask);
            refresh(true);
        });
    connect(m_breakBtn, &QPushButton::clicked, this, [this] { breakOnPixel(); });
    connect(m_sections, &QTabWidget::currentChanged, this, [this](int) {
        m_regsSig.clear();
        m_linesSig.clear();
        refreshView(INTVVID_TAB_TABLES);
    });
}

VideoTab::~VideoTab()
{
    intvvid_tab_free(m_tab);
}

/* Break on writes needs a BACKTAB word, GRAM card or MOB under the inspected pixel. */
void VideoTab::updateBreakEnabled()
{
    m_breakBtn->setEnabled(intvvid_tab_can_break(m_tab) != 0);
}

void VideoTab::note(const QString &text)
{
    m_note->setText(text);
}

void VideoTab::breakOnPixel()
{
    const intvvid_pixel *pixel = intvvid_tab_pixel(m_tab);
    const int id = intvvid_tab_break_on_pixel(m_tab);
    if (id >= 0 && pixel && pixel->nbreak > 0)
        note(QStringLiteral("Break on writes to $%1-$%2 (see Breakpoints)")
                 .arg(hex(pixel->break_lo[0], 4), hex(pixel->break_hi[0], 4)));
    else if (id >= 0)
        note(QStringLiteral("Write breakpoint added (see Breakpoints)"));
    else
        note(QStringLiteral("Nothing to break on: hover or pin a card, a MOB or a GRAM card"));
    updatePixelSection();
}

void VideoTab::updatePixelSection()
{
    const intvvid_pixel *pixel = intvvid_tab_pixel(m_tab);
    updateBreakEnabled();
    m_pixelText->setText(pixel ? QString::fromUtf8(pixel->text)
                               : QStringLiteral("Move over the picture to inspect a pixel.\nClick to pin it; "
                                                "double-click to break on writes to it."));
}

void VideoTab::fillRegs()
{
    const intvvid_reg *regs;
    const int n = intvvid_tab_regs(m_tab, &regs);
    QString sig;
    for (int i = 0; i < n; ++i)
        sig += QString::fromUtf8(regs[i].value) + (regs[i].age < INTVVID_REG_FADE ? QLatin1Char('*') : QLatin1Char('.'));
    sig += QString::number(n);
    if (sig == m_regsSig) return;
    m_regsSig = sig;
    const int vpos = m_regs->verticalScrollBar()->value();
    m_regs->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        const bool hot = regs[i].age < INTVVID_REG_FADE;
        setCell(m_regs, i, 0, QString::fromUtf8(regs[i].name), hot);
        setCell(m_regs, i, 1, QString::fromUtf8(regs[i].value), hot);
        setCell(m_regs, i, 2, QString::fromUtf8(regs[i].meaning), hot);
    }
    m_regs->resizeColumnsToContents();
    m_regs->verticalScrollBar()->setValue(vpos);
}

/* The 16 colours, each with a swatch and what the registers use it for; the
 * one the inspected pixel has is marked. */
void VideoTab::fillColours()
{
    const intvvid_swatch *swatches;
    const int n = intvvid_tab_swatches(m_tab, &swatches);
    m_swatches->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        const bool inspected = swatches[i].inspected != 0;
        auto *colour = m_swatches->item(i, 0);
        if (!colour) {
            colour = new QTableWidgetItem;
            m_swatches->setItem(i, 0, colour);
        }
        colour->setBackground(QBrush(QColor::fromRgb(swatches[i].rgb & 0xFFFFFF)));
        setCell(m_swatches, i, 1, QString::fromUtf8(swatches[i].name), inspected);
        setCell(m_swatches, i, 2, QStringLiteral("#") + hex(swatches[i].rgb & 0xFFFFFF, 6), inspected);
        setCell(m_swatches, i, 3, QStringLiteral("%1 px").arg(swatches[i].uses), inspected);
        setCell(m_swatches, i, 4, QString::fromUtf8(swatches[i].role), inspected);
    }
    m_swatches->resizeColumnsToContents();
    m_swatches->setColumnWidth(0, 28);
}

/* The 12 card rows: where each starts, how many BACKTAB words are used and
 * changed, the colour stack background, and whether the STIC has fetched it
 * (a stopped machine). A row that changed is marked. */
void VideoTab::fillLines()
{
    const intvvid_line_row *rows;
    const int n = intvvid_tab_lines(m_tab, &rows);
    QString sig = QString::number(n);
    for (int i = 0; i < n; ++i) {
        sig += QStringLiteral("|%1:%2:%3:%4:%5:").arg(rows[i].row).arg(rows[i].used).arg(rows[i].changed)
                   .arg(rows[i].background).arg(rows[i].fetched);
        for (int k = 0; k < INTVVID_CARD_COLS; ++k) sig += hex(rows[i].word[k], 4);
    }
    if (sig == m_linesSig) return;
    m_linesSig = sig;
    const int vpos = m_lines->verticalScrollBar()->value();
    m_lines->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        const bool hot = rows[i].changed != 0;
        QString words;
        for (int k = 0; k < INTVVID_CARD_COLS; ++k)
            words += (k ? QStringLiteral(" ") : QString()) + hex(rows[i].word[k], 4);
        setCell(m_lines, i, 0, QString::number(rows[i].row), hot);
        setCell(m_lines, i, 1, QString::number(rows[i].line), hot);
        setCell(m_lines, i, 2, QStringLiteral("$") + hex(rows[i].addr, 4), hot);
        setCell(m_lines, i, 3, QString::number(rows[i].used), hot);
        setCell(m_lines, i, 4, QString::number(rows[i].changed), hot);
        setCell(m_lines, i, 5, rows[i].background >= 0 ? QString::number(rows[i].background) : QStringLiteral("-"), hot);
        setCell(m_lines, i, 6, rows[i].fetched ? QStringLiteral("yes") : QStringLiteral("not yet"), hot);
        setCell(m_lines, i, 7, words, hot);
    }
    m_lines->resizeColumnsToContents();
    m_lines->verticalScrollBar()->setValue(vpos);
}

void VideoTab::fillTiming()
{
    const char *const *lines;
    const int n = intvvid_tab_timing(m_tab, &lines);
    QString text;
    for (int i = 0; i < n; ++i) {
        const QString line = QString::fromUtf8(lines[i]);
        const int tab = line.indexOf(QLatin1Char('\t'));
        text += tab >= 0 ? line.left(tab).leftJustified(14) + QLatin1Char(' ') + line.mid(tab + 1) : line;
        text += QLatin1Char('\n');
    }
    m_timingText->setText(text.isEmpty() ? QStringLiteral("No timing yet") : text);
    m_beam->setValue(intvvid_tab_beam_permille(m_tab));
}

void VideoTab::refreshView(unsigned what)
{
    if (what & INTVVID_TAB_PICTURE) m_pic->sync();
    if (what & INTVVID_TAB_TABLES) {
        m_status->setText(QString::fromUtf8(intvvid_tab_status(m_tab)));
        switch (m_sections->currentIndex()) {
        case INTVVID_S_REGISTERS: fillRegs(); break;
        case INTVVID_S_COLOURS: fillColours(); break;
        case INTVVID_S_LINES: fillLines(); break;
        case INTVVID_S_TIMING: fillTiming(); break;
        default: break;
        }
        updatePixelSection();
    }
}

void VideoTab::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    /* Fit follows the room the picture has */
    if (intvvid_tab_zoom(m_tab) == 0) refresh(false);
}

void VideoTab::refresh(bool force)
{
    const QSize viewport = m_scroll->viewport()->size();
    if (viewport.width() > 4 && viewport.height() > 4)
        intvvid_tab_set_viewport(m_tab, viewport.width() - 2, viewport.height() - 2);
    unsigned what = intvvid_tab_refresh(m_tab, force);
    if (force) what |= INTVVID_TAB_PICTURE | INTVVID_TAB_TABLES;
    if (what & INTVVID_TAB_OFF) what |= INTVVID_TAB_PICTURE | INTVVID_TAB_TABLES;
    refreshView(what);
}
