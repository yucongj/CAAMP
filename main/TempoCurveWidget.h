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

#include <map>

#include "data/model/Model.h"
#include "data/model/SparseTimeValueModel.h"

#include "layer/LayerGeometryProvider.h"
#include "layer/CoordinateScale.h"

#include "piano-aligner/Score.h"

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
                                                   
public slots:
    void setHighlightedPosition(QString label);
    void setCurrentAudioModel(sv::ModelId audioModel);
    void setAudioModelDisplayedRange(sv::sv_frame_t start, sv::sv_frame_t end);
    
protected:
    void paintEvent(QPaintEvent *e) override;

private:
    std::map<sv::ModelId, sv::ModelId> m_curves; // audio model -> tempo model
    std::map<sv::ModelId, QColor> m_colours; // audio model -> colour
    QString m_crotchet;
    sv::CoordinateScale m_coordinateScale;
    int m_colourCounter;
    int m_margin;
    double m_highlightedPosition;
    sv::ModelId m_currentAudioModel;
    sv::sv_frame_t m_audioModelDisplayStart;
    sv::sv_frame_t m_audioModelDisplayEnd;
    double m_barDisplayStart;
    double m_barDisplayEnd;
    Score::MusicalEventList m_musicalEvents;
    std::vector<std::pair<int, int>> m_timeSignatures; // index == bar no
    std::pair<int, int> getTimeSignature(int bar) const;

    double barToX(double bar, double barStart, double barEnd) const;

    void updateBarDisplayExtentsFromAudio();
    void ensureBarVisible(double bar);

    double frameToBarAndFraction(sv::sv_frame_t frame,
                                 sv::ModelId audioModel) const;
    double labelToBarAndFraction(QString label, bool *ok) const;
    void paintBarAndBeatLines(double barStart, double barEnd);
    void paintCurve(std::shared_ptr<sv::SparseTimeValueModel> tempoCurveModel,
                    QColor colour, double barStart, double barEnd);
    void paintLabels();

    void setPaintFont(QPainter &paint);
};

#endif
