/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Performance Precision
    
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.
*/

#include "TempoCurveWidget.h"

#include "svgui/layer/ColourDatabase.h"
#include "svgui/layer/LinearNumericalScale.h"
#include "svgui/layer/PaintAssistant.h"
#include "svgui/widgets/TextAbbrev.h"
#include "svcore/base/Preferences.h"
#include "svgui/widgets/Thumbwheel.h"
#include "svgui/widgets/NotifyingPushButton.h"
#include "svgui/view/ViewManager.h"
#include "svgui/widgets/IconLoader.h"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QGridLayout>

using namespace std;
using namespace sv;

#define DEBUG_TEMPO_CURVE_WIDGET 1

TempoCurveWidget::TempoCurveWidget(QWidget *parent) :
    QFrame(parent),
    m_crotchet(QChar(0x2669)),
    m_coordinateScale(CoordinateScale::Direction::Vertical,
                      QString("%1/min").arg(m_crotchet), // unit
                      false,                             // logarithmic
                      40.0,
                      200.0),
    m_colourCounter(0),
    m_margin(0),
    m_highlightedPosition(-1.0),
    m_audioModelDisplayStart(0),
    m_audioModelDisplayEnd(0),
    m_defaultBarCount(8),
    m_barDisplayStart(0),
    m_barDisplayEnd(m_defaultBarCount),
    m_firstBar(1),
    m_lastBar(1),
    m_clickedInRange(false),
    m_dragging(false),
    m_releasing(false),
    m_pendingWheelAngle(0),
    m_headsUpDisplay(nullptr),
    m_hthumb(nullptr),
    m_reset(nullptr)
{
    updateHeadsUpDisplay();
}

TempoCurveWidget::~TempoCurveWidget()
{
}

void
TempoCurveWidget::updateHeadsUpDisplay()
{
    if (!m_headsUpDisplay) {
        
        m_headsUpDisplay = new QFrame(this);

        QGridLayout *layout = new QGridLayout;
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        m_headsUpDisplay->setLayout(layout);
        
        m_hthumb = new Thumbwheel(Qt::Horizontal);
        m_hthumb->setObjectName(tr("Horizontal Zoom"));
        m_hthumb->setCursor(Qt::ArrowCursor);
        layout->addWidget(m_hthumb, 1, 0, 1, 2);
        m_hthumb->setFixedWidth(ViewManager::scalePixelSize(70));
        m_hthumb->setFixedHeight(ViewManager::scalePixelSize(16));
        m_hthumb->setMinimumValue(1);
        m_hthumb->setMaximumValue(100);
        m_hthumb->setDefaultValue(100 - m_defaultBarCount);
        m_hthumb->setSpeed(0.6f);
        connect(m_hthumb, SIGNAL(valueChanged(int)), this, 
                SLOT(horizontalThumbwheelMoved(int)));

        m_reset = new NotifyingPushButton;
        m_reset->setFlat(true);
        m_reset->setCursor(Qt::ArrowCursor);
        m_reset->setFixedHeight(ViewManager::scalePixelSize(16));
        m_reset->setFixedWidth(ViewManager::scalePixelSize(16));
        m_reset->setIcon(IconLoader().load("zoom-reset"));
        m_reset->setToolTip(tr("Reset zoom to default"));
        layout->addWidget(m_reset, 1, 2);
        
        layout->setColumnStretch(0, 20);

        connect(m_reset, SIGNAL(clicked()), m_hthumb, SLOT(resetToDefault()));
    }
    
    if (!m_headsUpDisplay->isVisible()) {
        m_headsUpDisplay->show();
    }
        
    int shift = ViewManager::scalePixelSize(86);
    m_headsUpDisplay->setFixedHeight(m_hthumb->height());
    m_headsUpDisplay->move(width() - shift,
                           height() - ViewManager::scalePixelSize(16));
}

void
TempoCurveWidget::setMusicalEvents(const Score::MusicalEventList &musicalEvents)
{
#ifdef DEBUG_TEMPO_CURVE_WIDGET
    SVDEBUG << "TempoCurveWidget::setMusicalEvents: " << musicalEvents.size() << " events" << endl;
#endif
    
    m_musicalEvents = musicalEvents;
    m_timeSignatures.clear();
    pair<int, int> prev(4, 4);
    // We aim for m_timeSignatures[bar] to record the time sig for
    // that bar number. Bar numbers usually start at 1 (in which case
    // the first entry in the vector is unused) but start at 0 if
    // there is a pick-up bar.
    for (const auto &e : m_musicalEvents) {
        int bar = e.measureInfo.measureNumber;
        if (m_timeSignatures.empty()) {
            m_firstBar = bar;
        }
        m_lastBar = bar;
        while (int(m_timeSignatures.size()) <= bar) {
            m_timeSignatures.push_back(prev);
        }
        pair<int, int> sig(e.meterNumer, e.meterDenom);
        m_timeSignatures[bar] = sig;
        prev = sig;
    }
#ifdef DEBUG_TEMPO_CURVE_WIDGET
    SVDEBUG << "TempoCurveWidget::setMusicalEvents: time sigs:" << endl;
    for (int i = 0; i < int(m_timeSignatures.size()); ++i) {
        SVDEBUG << i << ": " << m_timeSignatures[i].first << "/"
                << m_timeSignatures[i].second << endl;
    }
#endif

    m_curves.clear();
    m_colours.clear();
    m_colourCounter = 0;

    update();
}

void
TempoCurveWidget::setCurveForAudio(sv::ModelId audioModel,
                                   sv::ModelId tempoModel)
{
#ifdef DEBUG_TEMPO_CURVE_WIDGET
    SVDEBUG << "TempoCurveWidget::setCurveForAudio(" << audioModel << ", "
            << tempoModel << ")" << endl;
#endif
    
    m_curves[audioModel] = tempoModel;

    if (m_colours.find(audioModel) == m_colours.end()) {
    
        ColourDatabase *cdb = ColourDatabase::getInstance();
        QColor colour = Qt::black;

        while (colour == Qt::black || colour == Qt::white) {
            colour = cdb->getColour(m_colourCounter % cdb->getColourCount());
            ++m_colourCounter;
        }
    
        m_colours[audioModel] = colour;
    }

    update();
}

void
TempoCurveWidget::unsetCurveForAudio(sv::ModelId audioModel)
{
#ifdef DEBUG_TEMPO_CURVE_WIDGET
    SVDEBUG << "TempoCurveWidget::unsetCurveForAudio(" << audioModel
            << ")" << endl;
#endif

    m_curves.erase(audioModel);

    update();
}

pair<int, int>
TempoCurveWidget::getTimeSignature(int bar) const
{
    pair<int, int> sig { 4, 4 };
    if (in_range_for(m_timeSignatures, bar)) {
        sig = m_timeSignatures.at(bar);
    } else if (!m_timeSignatures.empty()) {
        sig = *m_timeSignatures.rbegin();
    }
    return sig;
}

void
TempoCurveWidget::setHighlightedPosition(QString label)
{
#ifdef DEBUG_TEMPO_CURVE_WIDGET
    SVDEBUG << "TempoCurveWidget::setHighlightedPosition("
            << label << ")" << endl;
#endif

    bool ok = false;
    double bar = labelToBarAndFraction(label, &ok);
    if (!ok) {
        SVDEBUG << "TempoCurveWidget::setHighlightedPosition: unable to parse "
                << "label \"" << label << "\"" << endl;
        return;
    }

    m_highlightedPosition = bar;
    ensureBarVisible(bar);
    update();
}

void
TempoCurveWidget::setCurrentAudioModel(ModelId model)
{
#ifdef DEBUG_TEMPO_CURVE_WIDGET
    SVDEBUG << "TempoCurveWidget::setCurrentAudioModel(" << model
            << ")" << endl;
#endif

    m_currentAudioModel = model;

    update();
}

void
TempoCurveWidget::setAudioModelDisplayedRange(sv_frame_t start, sv_frame_t end)
{
#ifdef DEBUG_TEMPO_CURVE_WIDGET
    SVDEBUG << "TempoCurveWidget::setAudioModelDisplayedRange(" << start
            << ", " << end << ") [m_currentAudioModel = " << m_currentAudioModel
            << "]" << endl;
#endif

    m_audioModelDisplayStart = start;
    m_audioModelDisplayEnd = end;

    update();
}

bool
TempoCurveWidget::isBarVisible(double bar)
{
    return (bar >= m_barDisplayStart && bar < m_barDisplayEnd);
}

void
TempoCurveWidget::ensureBarVisible(double bar)
{
#ifdef DEBUG_TEMPO_CURVE_WIDGET
    SVDEBUG << "TempoCurveWidget::ensureBarVisible(" << bar << ")" << endl;
#endif

    if (isBarVisible(bar)) {
        if (barToX(bar, m_barDisplayStart, m_barDisplayEnd) < width() * 0.9) {
            return;
        }
    }

    double duration = m_barDisplayEnd - m_barDisplayStart;
    if (duration < 1.0) {
        duration = 1.0;
    }
    double proposedStart = floor(bar);
    if (bar < m_barDisplayStart) {
        proposedStart = proposedStart - 1.0;
    }
    double proposedEnd = proposedStart + duration;
    if (barToX(bar, proposedStart, proposedEnd) > width() / 2) {
        proposedStart = bar;
        proposedEnd = proposedStart + duration;
    }
    m_barDisplayStart = proposedStart;
    m_barDisplayEnd = proposedEnd;
    update();
}

void
TempoCurveWidget::paintEvent(QPaintEvent *e)
{
    QFrame::paintEvent(e);

    LinearNumericalScale scale;
    
    {
        QPainter paint(this);
        setPaintFont(paint);
        m_margin = scale.getWidth(this, paint);
        paint.fillRect(rect(), getBackground());
    }

#ifdef DEBUG_TEMPO_CURVE_WIDGET
    SVDEBUG << "TempoCurveWidget::paintEvent: m_barDisplayStart = " << m_barDisplayStart << ", m_barDisplayEnd = " << m_barDisplayEnd << ", m_firstBar = " << m_firstBar << ", m_lastBar = " << m_lastBar << endl;
#endif

    double barStart = m_barDisplayStart;
    double barEnd = m_barDisplayEnd;
    if (barEnd <= 1.0) {
#ifdef DEBUG_TEMPO_CURVE_WIDGET
        SVDEBUG << "TempoCurveWidget::paintEvent: barEnd = " << barEnd << ", returning early" << endl;
#endif
        return;
    }
    if (barStart < m_firstBar) {
        barStart = m_firstBar;
    }
    if (barEnd > m_lastBar + 1) {
        barEnd = m_lastBar + 1;
    }
    
    paintBarAndBeatLines(barStart, barEnd);
    
    for (auto c: m_curves) {
        if (auto model = ModelById::getAs<SparseTimeValueModel>(c.second)) {
            paintCurve(model, m_colours[c.first], barStart, barEnd);
        }
    }

    paintLabels();

    if (m_highlightedPosition >= 0.0) {
        double x = barToX(m_highlightedPosition, barStart, barEnd);
        QPainter paint(this);
        QColor highlightColour("#59c4df");
        highlightColour.setAlpha(160);
        paint.setPen(Qt::NoPen);
        paint.setBrush(highlightColour);
        paint.drawRect(QRectF(x, 0.0, 10.0, height()));
    }

    {
        QPainter paint(this);
        setPaintFont(paint);
        paint.setPen(getForeground());
        paint.fillRect(QRectF(0.0, 0.0, m_margin, height()), getBackground());
        scale.paintVertical(this, m_coordinateScale, paint, 0);
        paint.drawText(5, height() - paint.fontMetrics().descent(),
                       QString("%1 =").arg(m_crotchet));
        paint.drawLine(m_margin, 0, m_margin, height());
    }
}

double
TempoCurveWidget::frameToBarAndFraction(sv_frame_t frame, ModelId audioModelId)
    const
{
    if (m_curves.find(audioModelId) == m_curves.end()) {
        return 0.0;
    }
    auto tempoModel = ModelById::getAs<SparseTimeValueModel>
        (m_curves.at(audioModelId));
    if (!tempoModel) {
        return 0.0;
    }
    Event event;
    if (!tempoModel->getNearestEventMatching(frame,
                                             [](Event) { return true; },
                                             EventSeries::Backward,
                                             event)) {
        return 0.0;
    }
    bool ok = false;
    double bar = labelToBarAndFraction(event.getLabel(), &ok);
    if (!ok) {
        return 0.0;
    }
    return bar;
}

double
TempoCurveWidget::labelToBarAndFraction(QString label, bool *okp) const
{
    bool okv = false;
    bool &ok = (okp ? *okp : okv);
    
    ok = false;

    QStringList barAndFraction = label.split("+");
    if (barAndFraction.size() != 2) return 0.0;

    int bar = barAndFraction[0].toInt(&ok);
    if (!ok) return 0.0;

    auto sig = getTimeSignature(bar);

#ifdef DEBUG_TEMPO_CURVE_WIDGET
    SVDEBUG << "TempoCurveWidget::labelToBarAndFraction: label = " << label
            << ", sig = " << sig.first << "/" << sig.second << endl;
#endif
    
    QStringList numAndDenom = barAndFraction[1].split("/");
    if (numAndDenom.size() != 2) return 0.0;

    int num = numAndDenom[0].toInt(&ok);
    if (!ok) return 0.0;
    
    int denom = numAndDenom[1].toInt(&ok);
    if (!ok) return 0.0;

    double pos = double(num) / (denom > 0 ? double(denom) : 1.0);
    double len = double(sig.first) / (sig.second > 0 ? double(sig.second) : 1.0);

    double result = double(bar);
    if (len > 0.0) result += pos / len;
    
    ok = true;
    return result;
}

double
TempoCurveWidget::barToX(double bar, double barStart, double barEnd) const
{
    double w = width() - m_margin;
    if (w <= 0.0) w = 1.0;
    return m_margin + w * ((bar - barStart) / (barEnd - barStart));
}

double
TempoCurveWidget::xToBar(double x, double barStart, double barEnd) const
{
    double w = width() - m_margin;
    if (w <= 0.0) w = 1.0;
    return barStart + ((x - m_margin) / w) * (barEnd - barStart);
}

void
TempoCurveWidget::paintBarAndBeatLines(double barStart, double barEnd)
{
    QPainter paint(this);
    setPaintFont(paint);
    paint.setRenderHint(QPainter::Antialiasing, true);
    paint.setBrush(Qt::NoBrush);
    
    for (int ibar = int(floor(barStart)); ibar <= int(ceil(barEnd)); ++ibar) {

        auto sig = getTimeSignature(ibar);
        
        double bar(ibar);
        
        double x = barToX(bar, barStart, barEnd);
        paint.setPen(getForeground());
        paint.drawLine(x, 0, x, height());

        //!!! +font
        paint.drawText(x + 5, 5 + paint.fontMetrics().ascent(),
                       QString("%1").arg(int(bar)));
        
        for (int i = 1; i < sig.first; ++i) {
            double barFrac = bar + double(i) / double(sig.first);
            x = barToX(barFrac, barStart, barEnd);
            paint.setPen(Qt::gray);
            paint.drawLine(x, 0, x, height());
        }
    }
}

void
TempoCurveWidget::paintCurve(shared_ptr<SparseTimeValueModel> model,
                             QColor colour, double barStart, double barEnd)
{
    QPainter paint(this);
    paint.setRenderHint(QPainter::Antialiasing, true);
    paint.setBrush(Qt::NoBrush);
     
    EventVector points(model->getAllEvents()); //!!! for now...

    double maxValue = model->getValueMaximum();
    double minValue = model->getValueMinimum();
    if (maxValue <= minValue) maxValue = minValue + 1.0;

    double px = 0.0;
    double py = 0.0;
    bool first = true;

    QPen pointPen(colour, 4.0);
    pointPen.setCapStyle(Qt::RoundCap);
    QPen linePen(colour, 1.0);
    
    for (auto p : points) {
        
        bool ok = false;
        double bar = labelToBarAndFraction(p.getLabel(), &ok);
        
        if (!ok) {
            SVDEBUG << "TempoCurveWidget::paintCurve: Failed to parse bar and fraction \"" << p.getLabel() << "\"" << endl;
            continue;
        }
        if (bar < barStart) {
            continue;
        }

        double x = barToX(bar, barStart, barEnd);

        double y = m_coordinateScale.getCoordForValue(this, p.getValue());

#ifdef DEBUG_TEMPO_CURVE_WIDGET
        SVDEBUG << "TempoCurveWidget::paintCurve: frame = "
                << p.getFrame() << ", label = " << p.getLabel()
                << ", bar = " << bar << ", value = " << p.getValue()
                << ", minValue = " << minValue << ", maxValue = "
                << maxValue << ", x = " << x << ", y = " << y << endl;
#endif
        
        if (!first) {
            paint.setPen(linePen);
            paint.drawLine(px, py, x, y);
        }
            
        paint.setPen(pointPen);
        paint.drawPoint(x, y);

        px = x;
        py = y;
        first = false;
    }

}

void
TempoCurveWidget::paintLabels()
{
    // Partly borrowed from Pane::drawLayerNames
    
    QPainter paint(this);
    setPaintFont(paint);
    paint.setPen(getForeground());
    
    auto fontHeight = paint.fontMetrics().height();
    auto fontAscent = paint.fontMetrics().ascent();
    
    QStringList texts;
    vector<QPixmap> pixmaps;
    
    for (auto c: m_colours) {
        auto audioModelId = c.first;
        if (m_curves.find(audioModelId) == m_curves.end()) {
            continue;
        }
        if (auto audioModel = ModelById::get(audioModelId)) {
            QColor colour = c.second;
            QString label = audioModel->objectName();
            QPixmap pixmap = ColourDatabase::getInstance()->
                getExamplePixmap(colour, QSize(fontAscent, fontAscent), false);
            texts.push_back(label);
            pixmaps.push_back(pixmap);
        }
    }

    int maxTextWidth = width() / 3;
    texts = TextAbbrev::abbreviate(texts, paint.fontMetrics(), maxTextWidth,
                                   TextAbbrev::ElideEndAndCommonPrefixes);

    int llx = width() - maxTextWidth - 5;
    int lly = height() - 6 - fontHeight * texts.size();
    
    for (int i = 0; i < texts.size(); ++i) {

#ifdef DEBUG_TEMPO_CURVE_WIDGET
        SVDEBUG << "TempoCurveWidget::paintLabels: text " << i << " = \""
                << texts[i] << "\", llx = " << llx << ", lly = " << lly
                << endl;
#endif
        
        PaintAssistant::drawVisibleText(this, paint, llx,
                                        lly - fontHeight + fontAscent,
                                        texts[i],
                                        PaintAssistant::OutlinedText);

        paint.drawPixmap(llx - fontAscent - 3,
                         lly - fontHeight + (fontHeight-fontAscent)/2,
                         pixmaps[i]);
            
        lly += fontHeight;
    }
}

void
TempoCurveWidget::setPaintFont(QPainter &paint)
{
    // From View::setPaintFont
    
    int scaleFactor = 1;
    int dpratio = int(ceil(devicePixelRatioF()));
    if (dpratio > 1) {
        QPaintDevice *dev = paint.device();
        if (dynamic_cast<QPixmap *>(dev) || dynamic_cast<QImage *>(dev)) {
            scaleFactor = dpratio;
        }
    }

    QFont font(paint.font());
    int pointSize = Preferences::getInstance()->getViewFontSize() * scaleFactor;
    font.setPointSize(pointSize);

    int h = height();
    int fh = QFontMetrics(font).height();
    if (pointSize > 6) {
        if (h < fh * 2.1) {
            font.setPointSize(pointSize - 2);
        } else if (h < fh * 3.1) {
            font.setPointSize(pointSize - 1);
        }
    }
    
    paint.setFont(font);
}

void
TempoCurveWidget::mousePressEvent(QMouseEvent *e)
{
    if (e->buttons() & Qt::RightButton) {
        // (context menu)
        return;
    }

    m_clickPos = e->pos();
    m_clickBarDisplayStart = m_barDisplayStart;
    m_clickBarDisplayEnd = m_barDisplayEnd;
    m_clickedInRange = true;
    m_dragging = false;
    m_releasing = false;
}

void
TempoCurveWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (e && (e->buttons() & Qt::RightButton)) {
        return;
    }

    if (m_clickedInRange) {
        m_releasing = true;
        mouseMoveEvent(e);
        m_releasing = false;

        if (!m_dragging) {
            mouseClickedOnly(e);
        }
    }

    m_dragging = false;
    m_clickedInRange = false;
}

void
TempoCurveWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (!e || (e->buttons() & Qt::RightButton)) {
        return;
    }

    QPoint pos = e->pos();

    if (m_clickedInRange && !m_releasing) {

        // if no buttons pressed, and not called from
        // mouseReleaseEvent, we want to reset clicked-ness (to avoid
        // annoying continual drags when we moved the mouse outside
        // the window after pressing button first time).

        if (!(e->buttons() & Qt::LeftButton) &&
            !(e->buttons() & Qt::MiddleButton)) {
            m_clickedInRange = false;
            return;
        }
    }

    double dist = pos.x() - m_clickPos.x();
    double threshold = 2.0;
    if (fabs(dist) < threshold) {
        return;
    }

    m_dragging = true;
    
    double clickAvgBarWidth = width();
    if (m_barDisplayEnd > m_barDisplayStart) {
        clickAvgBarWidth /= (m_barDisplayEnd - m_barDisplayStart);
    }

    double barDist = dist / clickAvgBarWidth;
    m_barDisplayStart = m_clickBarDisplayStart - barDist;
    m_barDisplayEnd = m_clickBarDisplayEnd - barDist;
    
    update();
}

void
TempoCurveWidget::mouseClickedOnly(QMouseEvent *e)
{
/*
  if (m_curves.find(m_currentAudioModel) != m_curves.end()) {
        if (checkCloseTo(e->pos().x(), e->pos().y(),
                         m_curves.at(m_currentAudioModel))) {
            return;
        }
    }
*/  
    for (auto c : m_curves) {
        if (c.first != m_currentAudioModel &&
            checkCloseTo(e->pos().x(), e->pos().y(), c.second)) {
            SVDEBUG << "TempoCurveWidget::mouseClickedOnly: asking to change to model " << c.first << endl;
            emit changeCurrentAudioModel(c.first);
        }
    }
}

bool
TempoCurveWidget::checkCloseTo(double x, double y, ModelId tempoModel)
{
    auto model = ModelById::getAs<SparseTimeValueModel>(tempoModel);
    if (!model) return false;

    EventVector points(model->getAllEvents()); //!!! for now...

    double threshold = ViewManager::scalePixelSize(10); 
        
    for (auto p : points) {
        
        bool ok = false;
        double bar = labelToBarAndFraction(p.getLabel(), &ok);
        if (!ok) continue;

        double px = barToX(bar, m_barDisplayStart, m_barDisplayEnd);

        if (px < 0) {
            continue;
        }
        
        double py = m_coordinateScale.getCoordForValue(this, p.getValue());

        if (fabs(px - x) < threshold && fabs(py - y) < threshold) {
            return true;
        }

        if (px > x) {
            break;
        }
    }

    return false;
}

void
TempoCurveWidget::mouseDoubleClickEvent(QMouseEvent *)
{
}

void
TempoCurveWidget::enterEvent(QEnterEvent *)
{
}

void
TempoCurveWidget::leaveEvent(QEvent *)
{
}

void
TempoCurveWidget::wheelEvent(QWheelEvent *e)
{
    e->accept();
    
    int dx = e->angleDelta().x();
    int dy = e->angleDelta().y();

    if (dx == 0 && dy == 0) {
        return;
    }

    int d = dy;
    bool horizontal = false;

    if (abs(dx) > abs(dy)) {
        d = dx;
        horizontal = true;
    }        

    if (e->phase() == Qt::ScrollBegin) {
        if (d < 0) m_pendingWheelAngle = -120;
        else if (d > 0) m_pendingWheelAngle = 120;
    } else if (std::abs(d) >= 120 ||
               (d > 0 && m_pendingWheelAngle < 0) ||
               (d < 0 && m_pendingWheelAngle > 0)) {
        m_pendingWheelAngle = d;
    } else {
        m_pendingWheelAngle += d;
    }

    if (m_pendingWheelAngle > 600) {
        m_pendingWheelAngle = 600;
    }
    if (m_pendingWheelAngle < -600) {
        m_pendingWheelAngle = -600;
    }

    while (abs(m_pendingWheelAngle) >= 120) {
        
        int sign = (m_pendingWheelAngle < 0 ? -1 : 1);
        
        if (horizontal) {
            wheelHorizontal(sign, e->modifiers());
        } else {
            wheelVertical(sign, e->modifiers());
        }
        
        m_pendingWheelAngle -= sign * 120;
    }
}

void
TempoCurveWidget::wheelVertical(int sign, Qt::KeyboardModifiers)
{
    if (sign < 0) {
        zoomIn();
    } else {
        zoomOut();
    }
}

void
TempoCurveWidget::wheelHorizontal(int sign, Qt::KeyboardModifiers)
{
}

void
TempoCurveWidget::resizeEvent(QResizeEvent *)
{
    updateHeadsUpDisplay();
}

void
TempoCurveWidget::zoomIn()
{
    zoom(true);
}

void
TempoCurveWidget::zoomOut()
{
    zoom(false);
}

void
TempoCurveWidget::zoom(bool in)
{
    double duration = m_barDisplayEnd - m_barDisplayStart;
    if (duration < 1.0) {
        duration = 1.0;
    }
    
    double adjusted = duration;
    if (in) {
        adjusted *= 1.41;
    } else {
        adjusted /= 1.41;
    }
    if (adjusted < 1.0) {
        adjusted = 1.0;
    }

    zoomTo(adjusted);
}

void
TempoCurveWidget::zoomTo(double duration)
{
    double from = m_barDisplayEnd - m_barDisplayStart;
    if (from < 1.0) {
        from = 1.0;
    }
    
    bool highlightVisible = isBarVisible(m_highlightedPosition);
    
    double middle;
    if (highlightVisible) {
        double frac = (m_highlightedPosition - m_barDisplayStart) / from;
        m_barDisplayStart = m_highlightedPosition - frac * duration;
        m_barDisplayEnd = m_highlightedPosition + (1.0 - frac) * duration;
    } else {
        double middle = m_barDisplayStart + (from / 2.0);
        m_barDisplayStart = middle - duration/2.0;
        m_barDisplayEnd = middle + duration/2.0;
    }
    
    if (m_barDisplayStart < m_firstBar) {
        m_barDisplayStart = m_firstBar;
    }

    if (highlightVisible) {
        ensureBarVisible(m_highlightedPosition);
    }
    
    update();
}

void
TempoCurveWidget::horizontalThumbwheelMoved(int value)
{
    zoomTo(100 - value);
}    

