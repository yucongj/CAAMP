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
#include "svgui/widgets/RangeInputDialog.h"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QGridLayout>
#include <QMenu>
#include <QAction>
#include <QActionGroup>

using namespace std;
using namespace sv;

//#define DEBUG_TEMPO_CURVE_WIDGET 1

static double defaultTempoMin = 40.0;
static double defaultTempoMax = 200.0;

// for the displayed range; we don't care if actual values are outside
// this range, we just can't show them:
static double overallTempoMin = 4.0;
static double overallTempoMax = 400.0;

TempoCurveWidget::TempoCurveWidget(QWidget *parent) :
    QFrame(parent),
    m_crotchet(QChar(0x2669)),
    m_coordinateScale(CoordinateScale::Direction::Vertical,
                      QString("%1/min").arg(m_crotchet), // unit
                      false,                             // logarithmic
                      defaultTempoMin,
                      defaultTempoMax),
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
    m_resolution(TempoResolution::perNote),
    m_clickedInRange(false),
    m_dragMode(UnresolvedDrag),
    m_releasing(false),
    m_pendingWheelAngle(0),
    m_headsUpDisplay(nullptr),
    m_hthumb(nullptr),
    m_reset(nullptr)
{
    setMouseTracking(true);
    updateHeadsUpDisplay();

    m_contextMenu = new QMenu(this);
    QActionGroup *tempoGroup = new QActionGroup(this);
    vector<pair<QString, TempoResolution>> resolutions {
        { tr("Tempo per Note"), TempoResolution::perNote },
        { tr("Tempo per Beat"), TempoResolution::perBeat },
        { tr("Tempo per Bar"), TempoResolution::perBar }
    };
    for (const auto &r : resolutions) {
        QAction *action =
            m_contextMenu->addAction(r.first, this,
                                     [=]() {
                                         changeTempoResolution(r.second);
                                     });
        action->setCheckable(true);
        tempoGroup->addAction(action);
        if (r.second == m_resolution) {
            action->setChecked(true);
        }
    }
    m_contextMenu->addSeparator();
    m_contextMenu->addAction(tr("Set Tempo Scale Extents..."),
                             this, &TempoCurveWidget::changeTempoScaleExtents);

    setTempoScaleExtents(m_coordinateScale.getDisplayMinimum(),
                         m_coordinateScale.getDisplayMaximum(),
                         true); // make sure wheel is updated by a single route
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

        m_vthumb = new Thumbwheel(Qt::Vertical);
        m_vthumb->setObjectName(tr("Vertical Zoom"));
        m_vthumb->setCursor(Qt::ArrowCursor);
        layout->addWidget(m_vthumb, 0, 2);
        m_vthumb->setMinimumValue(1);
        m_vthumb->setMaximumValue(100);
        m_vthumb->setDefaultValue(40);
        m_vthumb->setFixedWidth(ViewManager::scalePixelSize(16));
        m_vthumb->setFixedHeight(ViewManager::scalePixelSize(70));
        connect(m_vthumb, SIGNAL(valueChanged(int)), this, 
                SLOT(verticalThumbwheelMoved(int)));

        m_reset = new NotifyingPushButton;
        m_reset->setFlat(true);
        m_reset->setCursor(Qt::ArrowCursor);
        m_reset->setFixedHeight(ViewManager::scalePixelSize(16));
        m_reset->setFixedWidth(ViewManager::scalePixelSize(16));
        m_reset->setIcon(IconLoader().load("zoom-reset"));
        m_reset->setToolTip(tr("Reset zoom to default"));
        layout->addWidget(m_reset, 1, 2);
        
        layout->setColumnStretch(0, 20);

        connect(m_reset, &NotifyingPushButton::clicked,
                [=]() {
                    m_hthumb->resetToDefault();
                    setTempoScaleExtents(defaultTempoMin, defaultTempoMax, true);
                });
    }
    
    if (!m_headsUpDisplay->isVisible()) {
        m_headsUpDisplay->show();
    }
        
    int shift = ViewManager::scalePixelSize(86);
    m_headsUpDisplay->setFixedHeight(m_vthumb->height() + m_hthumb->height());
    m_headsUpDisplay->move(width() - shift, height() - shift);
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

    m_tempoModels.clear();
    m_curves.clear();
    m_colours.clear();
    m_labelToBarCache.clear();
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
    
    m_tempoModels[audioModel] = tempoModel;
    m_curves[audioModel] = extractCurve(tempoModel);
    
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

    m_tempoModels.erase(audioModel);
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
        if (barToX(bar) < width() * 0.9) {
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
    if (barToXWith(bar, proposedStart, proposedEnd) > width() / 2) {
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
    if (barEnd < m_firstBar) {
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
    
    for (auto c: m_tempoModels) {
        paintCurve(c.first, m_colours[c.first], barStart, barEnd,
                   c.second == m_closeTempoModel);
    }

    paintLabels();

    if (m_highlightedPosition >= 0.0) {
        double x = barToX(m_highlightedPosition);
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
    if (m_tempoModels.find(audioModelId) == m_tempoModels.end()) {
        return 0.0;
    }
    auto tempoModel = ModelById::getAs<SparseTimeValueModel>
        (m_tempoModels.at(audioModelId));
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
    double barAndFraction = -1.0;

    auto itr = m_labelToBarCache.find(label);
    if (itr != m_labelToBarCache.end()) {
        barAndFraction = itr.value();
        if (okp) {
            *okp = (barAndFraction >= 0.0);
        }
    } else {
        barAndFraction = labelToBarAndFractionUncached(label, okp);
        m_labelToBarCache[label] = barAndFraction;
    }

    return barAndFraction;
}

double
TempoCurveWidget::labelToBarAndFractionUncached(QString label, bool *okp) const
{
    bool okv = false;
    bool &ok = (okp ? *okp : okv);

    double badValue = -1.0;
    
    ok = false;

    QStringList barAndFraction = label.split("+");
    if (barAndFraction.size() != 2) return badValue;

    int bar = barAndFraction[0].toInt(&ok);
    if (!ok) return badValue;

    auto sig = getTimeSignature(bar);

#ifdef DEBUG_TEMPO_CURVE_WIDGET
    SVDEBUG << "TempoCurveWidget::labelToBarAndFraction: label = " << label
            << ", sig = " << sig.first << "/" << sig.second << endl;
#endif
    
    QStringList numAndDenom = barAndFraction[1].split("/");
    if (numAndDenom.size() != 2) return badValue;

    int num = numAndDenom[0].toInt(&ok);
    if (!ok) return badValue;
    
    int denom = numAndDenom[1].toInt(&ok);
    if (!ok) return badValue;

    double pos = double(num) / (denom > 0 ? double(denom) : 1.0);
    double len = double(sig.first) / (sig.second > 0 ? double(sig.second) : 1.0);

    double result = double(bar);
    if (len > 0.0) result += pos / len;
    
    ok = true;
    return result;
}

double
TempoCurveWidget::barToX(double bar) const
{
    double barStart = max(m_barDisplayStart, double(m_firstBar));
    double barEnd = min(m_barDisplayEnd, m_lastBar + 1.0);
    return barToXWith(bar, barStart, barEnd);
}

double
TempoCurveWidget::xToBar(double x) const
{
    double barStart = max(m_barDisplayStart, double(m_firstBar));
    double barEnd = min(m_barDisplayEnd, m_lastBar + 1.0);
    return xToBarWith(x, barStart, barEnd);
}

double
TempoCurveWidget::barToXWith(double bar, double barStart, double barEnd) const
{
    double w = width() - m_margin;
    if (w <= 0.0) w = 1.0;
    return m_margin + w * ((bar - barStart) / (barEnd - barStart));
}

double
TempoCurveWidget::xToBarWith(double x, double barStart, double barEnd) const
{
    double w = width() - m_margin;
    if (w <= 0.0) w = 1.0;
    return barStart + ((x - m_margin) / w) * (barEnd - barStart);
}

EventVector
TempoCurveWidget::extractCurve(ModelId tempoCurveModelId) const
{
    auto model = ModelById::getAs<SparseTimeValueModel>(tempoCurveModelId);
    if (!model) {
        return {};
    }

    EventVector original = model->getAllEvents();
    
    if (m_resolution == TempoResolution::perNote) {
        return original;
    }

    EventVector synthetic;
    sv_frame_t syntheticFrame = 0;
    
    int bar = 0;
    int beat = 0;
    int num = 1;
    int denom = 1;
    
    double prevPos = 0.0;
    double prevValue = 0.0;
    double firstNotePos = 0.0;

    double eps = 1.0e-6;

    double acc = 0.0;

    for (const auto &ev : original) {

        double value = ev.getValue();
        QString label = ev.getLabel();
        bool ok = false;
        double pos = labelToBarAndFraction(label, &ok);
        if (!ok) continue;

#ifdef DEBUG_TEMPO_CURVE_WIDGET
        SVDEBUG << "TempoCurveWidget::extractCurve: label " << label
                << ", pos " << pos << ", value " << value << endl;
#endif

        if (value <= 0.0) {
#ifdef DEBUG_TEMPO_CURVE_WIDGET
            SVDEBUG << "TempoCurveWidget::extractCurve: disregarding event with value " << value << endl;
#endif
            continue;
        }

        bool isFirstNote = (prevValue == 0.0);
            
        while (true) {

            // A note may continue for several beats: tally up each
            // beat until we run out of note, then break from the loop
            // so as to go on to the next note 

            if (beat == 0 && m_resolution == TempoResolution::perBeat) {
                // First beat of bar: get the time signature. (In
                // perBar mode the logic is identical but we have
                // effectively 1/1 signature)
                auto sig = getTimeSignature(bar);
                num = sig.first;
                denom = sig.second;
            }

            // Our position value is bar + beat / numerator (of time
            // sig), not denominator.  e.g. in 3/4 time the beats land
            // at 1.0, 1.333, 1.666 etc.
            
            double nextBeatPos = double(bar) + double(beat + 1) / double(num);

            if (isFirstNote) {
                firstNotePos = pos;
#ifdef DEBUG_TEMPO_CURVE_WIDGET
                SVDEBUG << "TempoCurveWidget::extractCurve: This is the first note, setting firstNotePos to " << pos << endl;
#endif
            }
        
            if (pos + eps < nextBeatPos) { // prev note ends before next beat
                if (!isFirstNote) {
                    acc += (pos - prevPos) * (1.0 / prevValue);
#ifdef DEBUG_TEMPO_CURVE_WIDGET
                    SVDEBUG << "TempoCurveWidget::extractCurve: added "
                            << (pos - prevPos) << " * " << (1.0 / prevValue)
                            << " to acc, is now " << acc << endl;
#endif
                }
                break;
            }

#ifdef DEBUG_TEMPO_CURVE_WIDGET
            SVDEBUG << "TempoCurveWidget::extractCurve: surpassed next beat "
                    << beat+1 << " of bar " << bar << " (per bar = " << num
                    << ") at label = " << label << ", pos = " << pos << endl;
#endif

            if (isFirstNote && firstNotePos < prevPos) {
                // This is the first note but a previous beat has
                // already been surpassed during it so we need an
                // event for that beat
#ifdef DEBUG_TEMPO_CURVE_WIDGET
                SVDEBUG << "TempoCurveWidget::extractCurve: The first note spans a beat, permitting an event for prev beat" << endl;
#endif
                prevValue = value;
            }

            if (prevValue == 0.0) {
                // NB we test prevValue here, not isFirstNote, because
                // the above check may have changed our view of it
#ifdef DEBUG_TEMPO_CURVE_WIDGET
                SVDEBUG << "TempoCurveWidget::extractCurve: This is the first note, not adding an event for prev note" << endl;
#endif
            } else {

                acc += (nextBeatPos - prevPos) * (1.0 / prevValue);

                double beatDuration = 1.0 / double(num); // in bars

                if (firstNotePos > nextBeatPos - beatDuration &&
                    firstNotePos < nextBeatPos) {
#ifdef DEBUG_TEMPO_CURVE_WIDGET
                    SVDEBUG << "TempoCurveWidget::extractCurve: this is a partial beat with firstNotePos at "
                            << firstNotePos << ", adjusting beat duration from "
                            << beatDuration << " to "
                            << nextBeatPos - firstNotePos << endl;
#endif
                    beatDuration = nextBeatPos - firstNotePos;
                }

                double syntheticValue = beatDuration / acc;
                QString syntheticLabel =
                    QString("%1+%2/%3").arg(bar).arg(beat).arg(denom);

#ifdef DEBUG_TEMPO_CURVE_WIDGET
                SVDEBUG << "TempoCurveWidget::extractCurve: finalised acc with "
                        << (nextBeatPos - prevPos) << " * " << (1.0 / prevValue)
                        << " -> now " << acc << ", beat duration = "
                        << beatDuration << ", beat/acc = "
                        << syntheticValue << " for label "
                        << syntheticLabel << " at index " << syntheticFrame
                        << endl;
#endif

                Event s(syntheticFrame, syntheticValue, syntheticLabel);
                synthetic.push_back(s);

                ++syntheticFrame;
            
                prevPos = nextBeatPos;
                prevValue = value;

                acc = 0.0;
            }
            
            if (++beat >= num) {
                ++bar;
                beat = 0;
            }

#ifdef DEBUG_TEMPO_CURVE_WIDGET
            SVDEBUG << "TempoCurveWidget::extractCurve: bar and beat now "
                    << bar << " and " << beat << endl;
#endif
        }

        if (prevPos < pos) { // (It may have been advanced to an
                             // interim beat if the note was long)
            prevPos = pos;
        }
        
        prevValue = value;
    }

    return synthetic;
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
        
        double x = barToX(bar);
        paint.setPen(getForeground());
        paint.drawLine(x, 0, x, height());

        //!!! +font
        paint.drawText(x + 5, 5 + paint.fontMetrics().ascent(),
                       QString("%1").arg(int(bar)));
        
        for (int i = 1; i < sig.first; ++i) {
            double barFrac = bar + double(i) / double(sig.first);
            x = barToX(barFrac);
            paint.setPen(Qt::gray);
            paint.drawLine(x, 0, x, height());
        }
    }
}

void
TempoCurveWidget::paintCurve(ModelId audioModelId, QColor colour,
                             double barStart, double barEnd,
                             bool isCloseTempoModel)
{
    if (m_curves.find(audioModelId) == m_curves.end()) {
        return;
    }
    
    QPainter paint(this);
    paint.setRenderHint(QPainter::Antialiasing, true);
    paint.setBrush(Qt::NoBrush);

    ModelId tempoModelId = m_tempoModels.at(audioModelId);
    auto model = ModelById::getAs<SparseTimeValueModel>(tempoModelId);
    if (!model) {
        SVDEBUG << "TempoCurveWidget::paintCurve: Tempo model " << tempoModelId
                << " not found" << endl;
        return;
    }
    
    const EventVector &points = m_curves.at(audioModelId);

    double maxValue = model->getValueMaximum();
    double minValue = model->getValueMinimum();
    if (maxValue <= minValue) maxValue = minValue + 1.0;

    double px = 0.0;
    double py = 0.0;
    bool first = true;

    QPen pointPen(colour, 4.0);
    pointPen.setCapStyle(Qt::RoundCap);

    QPen closePointPen(colour, 8.0);
    closePointPen.setCapStyle(Qt::RoundCap);

    QPen linePen(colour, 1.0);
    
    for (auto p : points) {
        
        bool ok = false;
        QString label = p.getLabel();
        double tempo = p.getValue();
        
        double bar = labelToBarAndFraction(label, &ok);
        
        if (!ok) {
            SVDEBUG << "TempoCurveWidget::paintCurve: Failed to parse bar and fraction \"" << label << "\"" << endl;
            continue;
        }
        if (bar + 1.0 < barStart) {
            continue;
        }
        if (bar > barEnd + 1.0) {
            continue;
        }

        double x = barToX(bar);

        double y = m_coordinateScale.getCoordForValue(this, tempo);

#ifdef DEBUG_TEMPO_CURVE_WIDGET
        SVDEBUG << "TempoCurveWidget::paintCurve: frame = "
                << p.getFrame() << ", label = " << label
                << ", bar = " << bar << ", value = " << p.getValue()
                << ", minValue = " << minValue << ", maxValue = "
                << maxValue << ", x = " << x << ", y = " << y << endl;
#endif
        
        if (!first) {
            paint.setPen(linePen);
            paint.drawLine(px, py, x, y);
        }

        if (isCloseTempoModel && label == m_closeLabel) {
            paint.setPen(closePointPen);
        } else {
            paint.setPen(pointPen);
        }
        
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
        if (m_tempoModels.find(audioModelId) == m_tempoModels.end()) {
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
    m_clickTempoMin = m_coordinateScale.getDisplayMinimum();
    m_clickTempoMax = m_coordinateScale.getDisplayMaximum();
    m_clickedInRange = true;
    m_dragMode = UnresolvedDrag;
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

        if (m_dragMode == UnresolvedDrag) {
            mouseClickedOnly(e);
        }
    }

    m_dragMode = UnresolvedDrag;
    m_clickedInRange = false;
}

void
TempoCurveWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (!e || (e->buttons() & Qt::RightButton)) {
        return;
    }

    QPoint pos = e->pos();

    if (!m_clickedInRange) {
        if (identifyClosePoint(e->pos())) {
            emit highlightLabel(m_closeLabel);
            update();
        }
        return;
    }
    
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

    double distx = pos.x() - m_clickPos.x();
    double disty = pos.y() - m_clickPos.y();
    double threshold = 4.0;

    if (m_dragMode == UnresolvedDrag) {
        if (fabs(distx) > threshold) {
            m_dragMode = HorizontalDrag;
        } else if (fabs(disty) > threshold) {
            m_dragMode = VerticalDrag;
        } else {
            return;
        }
    }

    if (m_dragMode == HorizontalDrag) {

        double clickAvgBarWidth = width();
        if (m_barDisplayEnd > m_barDisplayStart) {
            clickAvgBarWidth /= (m_barDisplayEnd - m_barDisplayStart);
        }

        double barDist = distx / clickAvgBarWidth;
        m_barDisplayStart = m_clickBarDisplayStart - barDist;
        m_barDisplayEnd = m_clickBarDisplayEnd - barDist;

    } else if (m_dragMode == VerticalDrag) {

        double prop = disty / height();
        double centre = (m_clickTempoMin + m_clickTempoMax) / 2.0;
        double extent = m_clickTempoMax - m_clickTempoMin;
        double newCentre = centre + extent * prop;
        setTempoScaleExtents(newCentre - extent/2.0, newCentre + extent/2.0,
                             true);
    }
        
    update();
}

void
TempoCurveWidget::mouseClickedOnly(QMouseEvent *e)
{
    if (!identifyClosePoint(e->pos())) {
        return;
    }

    for (auto c : m_tempoModels) {
        if (c.second == m_closeTempoModel) {
            SVDEBUG << "TempoCurveWidget::mouseClickedOnly: asking to change to model " << c.first << endl;
            emit changeCurrentAudioModel(c.first);
            emit activateLabel(m_closeLabel);
            break;
        }
    }
}

void
TempoCurveWidget::contextMenuEvent(QContextMenuEvent *e)
{
    m_contextMenu->popup(mapToGlobal(e->pos()));
}

void
TempoCurveWidget::changeTempoResolution(TempoResolution resolution)
{
    SVDEBUG << "TempoResolution::changeTempoResolution: "
            << int(resolution) << endl;
    m_resolution = resolution;

    for (const auto &m : m_tempoModels) {
        m_curves[m.first] = extractCurve(m.second);
    }
    
    update();
}

bool
TempoCurveWidget::identifyClosePoint(QPoint pos)
{
    double threshold = ViewManager::scalePixelSize(15);
    double closest = threshold;

    m_closeTempoModel = {};
    m_closeLabel = {};

    double x = pos.x();
    double y = pos.y();
    
    for (auto c : m_curves) {

        ModelId audioModelId = c.first;
        ModelId tempoModelId = m_tempoModels.at(audioModelId);
        
        const EventVector &points(c.second);
        
        for (auto p : points) {
        
            double py = m_coordinateScale.getCoordForValue(this, p.getValue());
            if (py < 0 || py > height() || fabs(py - y) > threshold) {
                continue;
            }

            QString label = p.getLabel();
            
            bool ok = false;
            double bar = labelToBarAndFraction(label, &ok);
            if (!ok) continue;

            double px = barToX(bar);
            if (px < 0) continue;

            double dist = sqrt((px - x) * (px - x) + (py - y) * (py - y));
            if (dist < closest) {
                m_closeTempoModel = tempoModelId;
                m_closeLabel = label;
                closest = dist;
            }

            if (px > x) {
                break;
            }
        }
    }

    return !m_closeTempoModel.isNone();
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
    if (m_closeLabel != "") {
        m_closeTempoModel = {};
        m_closeLabel = {};
        update();
    }
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

void
TempoCurveWidget::verticalThumbwheelMoved(int value)
{
    SVDEBUG << "TempoCurveWidget::verticalThumbwheelMoved: " << value << endl;
    
    double centre = (m_coordinateScale.getDisplayMinimum() +
                     m_coordinateScale.getDisplayMaximum()) / 2.0;

    double dist = (102.0 - value);
    
    double min = centre - dist * 2;
    if (min < overallTempoMin) min = overallTempoMin;
    
    double max = centre + dist * 2;
    if (max > overallTempoMax) max = overallTempoMax;

    SVDEBUG << "TempoCurveWidget::verticalThumbwheelMoved: centre "
            << centre << ", dist " << dist << ", min " << min << ", max "
            << max << endl;
    
    setTempoScaleExtents(min, max, false);
}

void
TempoCurveWidget::setTempoScaleExtents(double min, double max, bool updateWheel)
{
    SVDEBUG << "TempoCurveWidget::setTempoScaleExtents: " << min << " to "
            << max << ", updateWheel = " << updateWheel << endl;
    if (min < overallTempoMin) min = overallTempoMin;
    if (max > overallTempoMax) max = overallTempoMax;
    if (max < min + 1.0) {
        max = min + 1.0;
    }

    if (updateWheel) {
        double dist = (max - min) / 4.0;
        double wheelValue = round(102.0 - dist);
        SVDEBUG << "TempoCurveWidget::setTempoScaleExtents: dist = " << dist
                << ", changing wheel from "
                << m_vthumb->getValue() << " to " << wheelValue << endl;
        m_vthumb->setValue(wheelValue);
    }

    m_coordinateScale = m_coordinateScale.withDisplayExtents(min, max);
    update();
}    

void
TempoCurveWidget::changeTempoScaleExtents()
{
    QString unit = "bpm";
    RangeInputDialog dialog(tr("Enter tempo range"),
                            tr("New tempo display range, from %1 to %2 %3:")
                            .arg(overallTempoMin).arg(overallTempoMax).arg(unit),
                            unit, overallTempoMin, overallTempoMax, this);

    dialog.setRange(m_coordinateScale.getDisplayMinimum(),
                    m_coordinateScale.getDisplayMaximum());

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    float newmin, newmax;
    dialog.getRange(newmin, newmax);
    setTempoScaleExtents(newmin, newmax, true);
}

