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

#include <map>

#include "data/model/Model.h"

class TempoCurveWidget : public QFrame
{
    Q_OBJECT

public:
    TempoCurveWidget(QWidget *parent = 0);
    virtual ~TempoCurveWidget();

    void addCurve(sv::ModelId audioModel, sv::ModelId tempoModel);
    void removeCurve(sv::ModelId audioModel);

private:
    std::map<sv::ModelId, sv::ModelId> m_curves; // audio model -> tempo model

};

#endif
