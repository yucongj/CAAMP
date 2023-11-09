/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    SV Piano Precision
    
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.
*/

#include "Session.h"

#include "transform/TransformFactory.h"
#include "transform/ModelTransformer.h"
#include "layer/ColourDatabase.h"
#include "layer/ColourMapper.h"

#include <QMessageBox>

using namespace std;

Session::Session() :
    m_document(nullptr),
    m_topView(nullptr),
    m_bottomView(nullptr),
    m_timeRulerLayer(nullptr),
    m_waveformLayer(nullptr),
    m_spectrogramLayer(nullptr),
    m_onsetsLayer(nullptr),
    m_tempoLayer(nullptr)
{
    SVDEBUG << "Session::Session" << endl;
}

Session::~Session()
{
}

void
Session::setDocument(Document *doc,
                     View *topView,
                     View *bottomView,
                     Layer *timeRuler)
{
    SVDEBUG << "Session::setDocument(" << doc << ")" << endl;
    
    m_document = doc;
    m_scoreId = "";
    m_mainModel = {};
    m_topView = topView;
    m_bottomView = bottomView;
    m_timeRulerLayer = timeRuler;
    m_waveformLayer = nullptr;
    m_spectrogramLayer = nullptr;
    m_onsetsLayer = nullptr;
    m_onsetsModel = {};
    m_tempoLayer = nullptr;
    m_tempoModel = {};
}

void
Session::unsetDocument()
{
    setDocument(nullptr, nullptr, nullptr, nullptr);
}

void
Session::setMainModel(ModelId modelId, QString scoreId)
{
    SVDEBUG << "Session::setMainModel(" << modelId << ")" << endl;
    
    m_mainModel = modelId;
    m_scoreId = scoreId;

    if (!m_document) {
        SVDEBUG << "Session::setMainModel: WARNING: No document; one should have been set first" << endl;
        return;
    }

    if (m_waveformLayer) {
        //!!! Review this
        SVDEBUG << "Session::setMainModel: Waveform layer already exists - currently we expect a process by which the document and panes are created and then setMainModel called here only once per document" << endl;
        return;
    }

    m_document->addLayerToView(m_bottomView, m_timeRulerLayer);
    
    ColourDatabase *cdb = ColourDatabase::getInstance();

    m_waveformLayer = qobject_cast<WaveformLayer *>
        (m_document->createLayer(LayerFactory::Waveform));
    m_waveformLayer->setBaseColour(cdb->getColourIndex(tr("Orange")));
    
    m_document->addLayerToView(m_topView, m_waveformLayer);
    m_document->setModel(m_waveformLayer, modelId);

    m_spectrogramLayer = qobject_cast<SpectrogramLayer *>
        (m_document->createLayer(LayerFactory::MelodicRangeSpectrogram));
    m_spectrogramLayer->setColourMap(ColourMapper::BlackOnWhite);

    m_document->addLayerToView(m_bottomView, m_spectrogramLayer);
    m_document->setModel(m_spectrogramLayer, modelId);
}

void
Session::beginAlignment()
{
    if (m_mainModel.isNone()) {
        SVDEBUG << "Session::beginAlignment: WARNING: No main model; one should have been set first" << endl;
        return;
    }

    beginPartialAlignment(-1, -1, -1, -1);
}

void
Session::beginPartialAlignment(int scorePositionStart,
                               int scorePositionEnd,
                               sv_frame_t audioFrameStart,
                               sv_frame_t audioFrameEnd)
{
    if (m_mainModel.isNone()) {
        SVDEBUG << "Session::beginPartialAlignment: WARNING: No main model; one should have been set first" << endl;
        return;
    }

    //!!! race condition here - need to either cancel or count properly
    m_modelsReady = 0;
    
    ModelTransformer::Input input(m_mainModel);

    vector<pair<QString, View *>> layerDefinitions {
        { "vamp:score-aligner:pianoaligner:chordonsets", m_topView },
        { "vamp:score-aligner:pianoaligner:eventtempo", m_bottomView }
    };

    vector<Layer *> newLayers;

    sv_samplerate_t sampleRate = ModelById::get(m_mainModel)->getSampleRate();
    RealTime audioStart, audioEnd;
    if (audioFrameStart == -1) {
        audioStart = RealTime::fromSeconds(-1.0);
    } else {
        audioStart = RealTime::frame2RealTime(audioFrameStart, sampleRate);
    }
    if (audioFrameEnd == -1) {
        audioEnd = RealTime::fromSeconds(-1.0);
    } else {
        audioEnd = RealTime::frame2RealTime(audioFrameEnd, sampleRate);
    }

    SVDEBUG << "Session::beginPartialAlignment: score position start = "
            << scorePositionStart << ", end = " << scorePositionEnd
            << ", audio start = " << audioStart << ", end = "
            << audioEnd << endl;
    
    Transform::ParameterMap params {
        { "score-position-start", scorePositionStart },
        { "score-position-end", scorePositionEnd },
        { "audio-start", float(audioStart.toDouble()) },
        { "audio-end", float(audioEnd.toDouble()) }
    };
    
    for (auto defn : layerDefinitions) {
        
        Transform t = TransformFactory::getInstance()->
            getDefaultTransformFor(defn.first);

        t.setProgram(m_scoreId);
        t.setParameters(params);

        //!!! return error codes
    
        Layer *layer = m_document->createDerivedLayer(t, input);
        if (!layer) {
            SVDEBUG << "Session::setMainModel: Transform failed to initialise" << endl;
            return;
        }
        newLayers.push_back(layer);

        m_document->addLayerToView(defn.second, layer);

        Model *model = ModelById::get(layer->getModel()).get();
        if (!model) {
            SVDEBUG << "Session::setMainModel: Transform failed to create a model" << endl;
            return;
        }
                    
        connect(model, SIGNAL(ready(ModelId)),
                this, SLOT(modelReady(ModelId)));
    }
    
    m_onsetsLayer = qobject_cast<TimeValueLayer *>(newLayers[0]);
    m_tempoLayer = qobject_cast<TimeValueLayer *>(newLayers[1]);
    if (!m_onsetsLayer || !m_tempoLayer) {
        SVDEBUG << "Session::setMainModel: Layers are not of the expected type" << endl;
        return;
    }
    
    m_onsetsModel = m_onsetsLayer->getModel();
    m_tempoModel = m_tempoLayer->getModel();

    ColourDatabase *cdb = ColourDatabase::getInstance();
    
    m_onsetsLayer->setPlotStyle(TimeValueLayer::PlotSegmentation);
    m_onsetsLayer->setDrawSegmentDivisions(true);
    m_onsetsLayer->setFillSegments(false);

    m_tempoLayer->setPlotStyle(TimeValueLayer::PlotLines);
    m_tempoLayer->setBaseColour(cdb->getColourIndex(tr("Blue")));
}

void
Session::modelReady(ModelId id)
{
    SVDEBUG << "Session::modelReady: model is " << id << endl;

    if (++m_modelsReady < 2) {
        return;
    }

    if (QMessageBox::question
        (m_topView, tr("Save alignment?"),
         tr("<b>Alignment finished</b><p>Do you want to keep this alignment?"),
         QMessageBox::Save | QMessageBox::Cancel,
         QMessageBox::Save) ==
        QMessageBox::Save) {

        SVDEBUG << "Session::modelReady: Save chosen" << endl;
        
    } else {

        SVDEBUG << "Session::modelReady: Cancel chosen" << endl;
        
        m_document->deleteLayer(m_onsetsLayer, true);
        m_document->deleteLayer(m_tempoLayer, true);
    }
}

