/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Performance Precision
    
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.
*/

#ifndef SV_TEMPO_CURVE_WIDGET_H
#define SV_TEMPO_CURVE_WIDGET_H

#include <QTemporaryDir>
#include <QFrame>
#include <QTimer>
#include <QColor>
#include <QMenu>

#include <map>

#include "data/model/Model.h"
#include "data/model/SparseTimeValueModel.h"

#include "layer/LayerGeometryProvider.h"
#include "layer/CoordinateScale.h"

#include "piano-aligner/Score.h"

namespace sv {
class Thumbwheel;
class NotifyingPushButton;
}

class TempoCurveWidget : public QFrame, public sv::LayerDimensionProvider
{
    Q_OBJECT

public:
    TempoCurveWidget(QWidget *parent = 0);
    virtual ~TempoCurveWidget();
    
    void setMusicalEvents(const Score::MusicalEventList &musicalEvents);

    void setCurveForAudio(sv::ModelId audioModel, sv::ModelId tempoModel);
    void unsetCurveForAudio(sv::ModelId audioModel);

    // LayerDimensionProvider methods
    QRect getPaintRect() const override { return rect(); }
    bool hasLightBackground() const override { return true; }
    QColor getForeground() const override { return Qt::black; }
    QColor getBackground() const override { return Qt::white; }

    enum class TempoResolution { perNote, perBeat, perBar };

signals:
    void changeCurrentAudioModel(sv::ModelId toAudioModel);
    void highlightLabel(QString label);
    void activateLabel(QString label);

public slots:
    void setHighlightedPosition(QString label);
    void setCurrentAudioModel(sv::ModelId audioModel);
    void setAudioModelDisplayedRange(sv::sv_frame_t start, sv::sv_frame_t end);
    void zoomIn();
    void zoomOut();
    void zoom(bool in);
    void zoomTo(double duration);
    void horizontalThumbwheelMoved(int value);
    void verticalThumbwheelMoved(int value);
    void setTempoScaleExtents(double min, double max, bool updateWheel);
    void changeTempoScaleExtents(); // asking the user
    void changeTempoResolution(TempoResolution);
    
protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void wheelVertical(int sign, Qt::KeyboardModifiers);
    void wheelHorizontal(int sign, Qt::KeyboardModifiers);

private:
    // m_tempoModels contains the original models; m_curves contains
    // synthetic events generated from each model at the currently
    // active resolution. (If the resolution is perNote, the curves
    // are the same as the events found in the corresponding models.)
    // In all cases the map key is the audio model id.
    std::map<sv::ModelId, sv::ModelId> m_tempoModels;
    std::map<sv::ModelId, sv::EventVector> m_curves;
    std::map<sv::ModelId, QColor> m_colours;
    mutable QHash<QString, double> m_labelToBarCache;
    QString m_crotchet;
    sv::CoordinateScale m_coordinateScale;
    int m_colourCounter;
    int m_margin;
    double m_highlightedPosition;
    sv::ModelId m_currentAudioModel;
    sv::sv_frame_t m_audioModelDisplayStart;
    sv::sv_frame_t m_audioModelDisplayEnd;
    int m_defaultBarCount;
    double m_barDisplayStart;
    double m_barDisplayEnd;
    Score::MusicalEventList m_musicalEvents;
    int m_firstBar;
    int m_lastBar;
    TempoResolution m_resolution;

    QPoint m_clickPos;
    double m_clickBarDisplayStart;
    double m_clickBarDisplayEnd;
    double m_clickTempoMin;
    double m_clickTempoMax;
    bool m_clickedInRange;

    enum DragMode {
        UnresolvedDrag,
        VerticalDrag,
        HorizontalDrag
    };
    DragMode m_dragMode;
    
    bool m_releasing;
    int m_pendingWheelAngle;

    sv::ModelId m_closeTempoModel;
    QString m_closeLabel;

    QMenu *m_contextMenu;
    
    QWidget *m_headsUpDisplay;
    sv::Thumbwheel *m_hthumb;
    sv::Thumbwheel *m_vthumb;
    sv::NotifyingPushButton *m_reset;
    void updateHeadsUpDisplay();

    void mouseClickedOnly(QMouseEvent *);
    bool identifyClosePoint(QPoint pos);
    
    std::vector<std::pair<int, int>> m_timeSignatures; // index == bar no
    std::pair<int, int> getTimeSignature(int bar) const;

    double barToX(double bar, double barStart, double barEnd) const;
    double xToBar(double x, double barStart, double barEnd) const;

    sv::EventVector extractCurve(sv::ModelId tempoCurveModelId) const;
    
    bool isBarVisible(double bar);
    void ensureBarVisible(double bar);

    double frameToBarAndFraction(sv::sv_frame_t frame,
                                 sv::ModelId audioModel) const;
    double labelToBarAndFraction(QString label, bool *ok) const;
    double labelToBarAndFractionUncached(QString label, bool *ok) const;
    void paintBarAndBeatLines(double barStart, double barEnd);
    void paintCurve(sv::ModelId audioModelId, QColor colour,
                    double barStart, double barEnd, bool isCloseTempoModel);
    void paintLabels();

    void setPaintFont(QPainter &paint);
};

#endif
