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

TempoCurveWidget::TempoCurveWidget(QWidget *parent) :
    QFrame(parent)
{
}

TempoCurveWidget::~TempoCurveWidget()
{
}

void
TempoCurveWidget::addCurve(sv::ModelId audioModel, sv::ModelId tempoModel)
{
    m_curves[audioModel] = tempoModel;
}

void
TempoCurveWidget::removeCurve(sv::ModelId audioModel)
{
    m_curves.erase(audioModel);
}

