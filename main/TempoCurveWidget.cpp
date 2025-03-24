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

#include <QPainter>

using namespace std;
using namespace sv;

//#define DEBUG_TEMPO_CURVE_WIDGET 1

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
    m_barDisplayStart(0),
    m_barDisplayEnd(0)
{
}

TempoCurveWidget::~TempoCurveWidget()
{
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
    int nbars = 0;
    for (const auto &e : m_musicalEvents) {
        int bar = e.measureInfo.measureNumber;
        pair<int, int> sig(e.meterNumer, e.meterDenom);
        // we want m_timeSignatures[bar] to be correct for bar, but
        // m_timeSignatures is zero-indexed and bar is not, so we need
        // one more than we might think - hence <= rather than < in
        // the following
        while (nbars <= bar) {
            m_timeSignatures.push_back(prev);
            ++nbars;
        }
        m_timeSignatures[bar] = sig;
        prev = sig;
    }
#ifdef DEBUG_TEMPO_CURVE_WIDGET
    SVDEBUG << "TempoCurveWidget::setMusicalEvents: time sigs:" << endl;
    for (int i = 0; i < m_timeSignatures.size(); ++i) {
        SVDEBUG << i << ": " << m_timeSignatures[i].first << "/"
                << m_timeSignatures[i].second << endl;
    }
#endif

    m_curves.clear();
    m_colours.clear();
    m_colourCounter = 0;

    updateBarDisplayExtentsFromAudio();
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

    updateBarDisplayExtentsFromAudio();
}

void
TempoCurveWidget::unsetCurveForAudio(sv::ModelId audioModel)
{
#ifdef DEBUG_TEMPO_CURVE_WIDGET
    SVDEBUG << "TempoCurveWidget::unsetCurveForAudio(" << audioModel
            << ")" << endl;
#endif

    m_curves.erase(audioModel);

    updateBarDisplayExtentsFromAudio();
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

    updateBarDisplayExtentsFromAudio();
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

    updateBarDisplayExtentsFromAudio();
}

void
TempoCurveWidget::updateBarDisplayExtentsFromAudio()
{
    m_barDisplayStart = frameToBarAndFraction(m_audioModelDisplayStart,
                                              m_currentAudioModel);
    m_barDisplayEnd = frameToBarAndFraction(m_audioModelDisplayEnd,
                                            m_currentAudioModel);

#ifdef DEBUG_TEMPO_CURVE_WIDGET
    SVDEBUG << "TempoCurveWidget::updateBarDisplayExtentsFromAudio: "
            << "m_barDisplayStart = " << m_barDisplayStart
            << ", m_barDisplayEnd = " << m_barDisplayEnd << endl;
#endif

    update();
}

void
TempoCurveWidget::ensureBarVisible(double bar)
{
#ifdef DEBUG_TEMPO_CURVE_WIDGET
    SVDEBUG << "TempoCurveWidget::ensureBarVisible(" << bar << ")" << endl;
#endif

    if (bar >= m_barDisplayStart && bar < m_barDisplayEnd) {
        if (barToX(bar, m_barDisplayStart, m_barDisplayEnd) < width() * 0.9) {
            return;
        }
    }
    double duration = m_barDisplayEnd - m_barDisplayStart;
    if (duration < 2.0) duration = 2.0;
    double proposedStart = floor(bar - 1.0);
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
#ifdef DEBUG_TEMPO_CURVE_WIDGET
    SVDEBUG << "TempoCurveWidget::paintEvent" << endl;
#endif
    
    QFrame::paintEvent(e);

    LinearNumericalScale scale;
    
    {
        QPainter paint(this);
        setPaintFont(paint);
        m_margin = scale.getWidth(this, paint);
        paint.fillRect(rect(), getBackground());
    }

    double barStart = m_barDisplayStart;
    double barEnd = m_barDisplayEnd;
    if (barEnd <= 1.0) {
#ifdef DEBUG_TEMPO_CURVE_WIDGET
        SVDEBUG << "TempoCurveWidget::paintEvent: barEnd = " << barEnd << ", returning early" << endl;
#endif
        return;
    }
    if (barStart < 1.0) {
        barStart = 1.0;
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
    if (w < 0.0) w = 1.0;
    return m_margin + w * ((bar - barStart) / (barEnd - barStart));
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
        if (bar > barEnd) {
            break;
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
        if (auto model = ModelById::get(c.first)) {
            QColor colour = c.second;
            QString label = model->objectName();
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
