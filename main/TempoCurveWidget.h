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

class TempoCurveWidget : public QFrame
{
    Q_OBJECT

public:
    TempoCurveWidget(QWidget *parent = 0);
    virtual ~TempoCurveWidget();

    void setCurveForAudio(sv::ModelId audioModel, sv::ModelId tempoModel);
    void unsetCurveForAudio(sv::ModelId audioModel);

public slots:
    void setHighlightedPosition(QString label);
    
protected:
    void paintEvent(QPaintEvent *e) override;

private:
    std::map<sv::ModelId, sv::ModelId> m_curves; // audio model -> tempo model
    std::map<sv::ModelId, QColor> m_colours; // audio model -> colour
    int m_colourCounter;
    double m_highlightedPosition;

    double barToX(double bar, double barStart, double barEnd) const;
    
    double labelToBarAndFraction(QString label, bool *ok) const;
    void paintBarAndBeatLines(double barStart, double barEnd,
                              int beatsPerBar); // 0 for no beats
    void paintCurve(std::shared_ptr<sv::SparseTimeValueModel> tempoCurveModel,
                    QColor colour, double barStart, double barEnd);
};

#endif
