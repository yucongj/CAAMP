/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Performance Precision
    
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.
*/

#include "Session.h"

#include "ScoreAlignmentTransform.h"

#include "transform/TransformFactory.h"
#include "transform/ModelTransformer.h"
#include "layer/ColourDatabase.h"
#include "layer/ColourMapper.h"

#include "data/fileio/CSVFormat.h"
#include "data/fileio/CSVFileReader.h"
#include "data/fileio/CSVFileWriter.h"

#include "base/TempWriteFile.h"
#include "base/StringBits.h"

#include <QMessageBox>
#include <QFileInfo>

using namespace std;
using namespace sv;

const TransformId
Session::smartCopyTransformId = "*smartcopy*";

Session::Session() :
    m_pendingOnsetsPane(nullptr),
    m_pendingOnsetsLayer(nullptr)
{
    SVDEBUG << "Session::Session" << endl;
    setDocument(nullptr, nullptr, nullptr, nullptr);
}

Session::~Session()
{
}

void
Session::setDocument(Document *doc,
                     Pane *mainAudioPane,
                     Pane *featurePane,
                     Layer *timeRuler)
{
    SVDEBUG << "Session::setDocument(" << doc << ")" << endl;

    if (m_pendingOnsetsLayer) {
        emit alignmentRejected();
    }
    
    m_document = doc;
    m_scoreId = "";
    m_mainModel = {};

    m_audioPanes.clear();
    if (mainAudioPane) {
        m_audioPanes.push_back(mainAudioPane);
    }
    m_featurePane = featurePane;
    m_activePane = mainAudioPane;
    m_timeRulerLayer = timeRuler;

    m_partialAlignmentAudioStart = -1;
    m_partialAlignmentAudioEnd = -1;

    m_pendingOnsetsPane = nullptr;
    m_pendingOnsetsLayer = nullptr;
    m_audioModelForPendingOnsets = {};
    
    m_featureData.clear();
    m_inEditMode = false;
}

void
Session::unsetDocument()
{
    setDocument(nullptr, nullptr, nullptr, nullptr);
}

TimeInstantLayer *
Session::getOnsetsLayer()
{
    return getOnsetsLayerFromPane(getPaneContainingOnsetsLayer(),
                                  OnsetsLayerSelection::PermitPendingOnsets);
}

Pane *
Session::getPaneContainingOnsetsLayer()
{
    return getAudioPaneForAudioModel(getActiveAudioModel());
}

Pane *
Session::getReferencePane() const
{
    return getAudioPaneForAudioModel(m_mainModel);
}

TimeValueLayer *
Session::getTempoLayerForAudioModel(ModelId model)
{
    if (m_featureData.find(model) != m_featureData.end()) {
        return m_featureData.at(model).tempoLayer;
    } else {
        return nullptr;
    }
}

Pane *
Session::getPaneContainingTempoLayers()
{
    return m_featurePane;
}

void
Session::setMainModel(ModelId modelId, QString scoreId)
{
    SVDEBUG << "Session::setMainModel(" << modelId << ")" << endl;
    
    m_mainModel = modelId;
    m_scoreId = scoreId;

    if (!m_document) {
        if (m_mainModel.isNone()) {
            SVDEBUG << "Session::setMainModel: NOTE: Cleared main model and no document set" << endl;
        } else {
            SVDEBUG << "Session::setMainModel: WARNING: No document; one should have been set first" << endl;
        }
        return;
    } else if (m_mainModel.isNone()) {
        SVDEBUG << "Session::setMainModel: WARNING: Cleared main model, but there is a document active" << endl;
        return;
    } else if (m_audioPanes.empty() || !m_featurePane) {
        SVDEBUG << "Session::setMainModel: WARNING: Set a main model but we have no audio panes and/or no feature pane" << endl;
        return;
    }

    m_document->addLayerToView(m_featurePane, m_timeRulerLayer);
    
    ColourDatabase *cdb = ColourDatabase::getInstance();

    WaveformLayer *waveformLayer = qobject_cast<WaveformLayer *>
        (m_document->createLayer(LayerFactory::Waveform));
    waveformLayer->setBaseColour(cdb->getColourIndex(tr("Orange")));
    
    m_document->addLayerToView(m_featurePane, waveformLayer);
    m_document->setModel(waveformLayer, modelId);

    SpectrogramLayer *spectrogramLayer = qobject_cast<SpectrogramLayer *>
        (m_document->createLayer(LayerFactory::MelodicRangeSpectrogram));
    spectrogramLayer->setBinScale(BinScale::Linear);
    spectrogramLayer->setColourMap(ColourMapper::Green);
    spectrogramLayer->setColourScale(ColourScaleType::Log);
    spectrogramLayer->setColourScaleMultiple(2.0);

    m_document->addLayerToView(m_audioPanes[0], spectrogramLayer);
    m_document->setModel(spectrogramLayer, modelId);

    resetAlignmentEntriesFor(modelId);
}

ModelId
Session::getAudioModelFromPane(Pane *pane) const
{
    if (!pane) {
        return {};
    }
    
    int n = pane->getLayerCount();

    for (int i = n-1; i >= 0; --i) {

        // Reverse order, to find whichever is visible (since in the
        // feature pane there could be more than one audio layer)
        
        auto layer = pane->getLayer(i);

        auto waveformLayer = qobject_cast<WaveformLayer *>(layer);
        if (waveformLayer) {
            return waveformLayer->getModel();
        }
        
        auto spectrogramLayer = qobject_cast<SpectrogramLayer *>(layer);
        if (spectrogramLayer) {
            return spectrogramLayer->getModel();
        }
    }

    return {};
}

TimeInstantLayer *
Session::getOnsetsLayerFromPane(Pane *pane) const
{
    return getOnsetsLayerFromPane(pane, OnsetsLayerSelection::PermitPendingOnsets);
}

TimeInstantLayer *
Session::getOnsetsLayerFromPane(Pane *pane, OnsetsLayerSelection selection) const
{
    if (!pane) {
        return nullptr;
    }
    
    int n = pane->getLayerCount();

    // Prefer topmost non-dormant layer if more than one matches the
    // selection

    vector<TimeInstantLayer *> candidates;
    
    for (int i = n-1; i >= 0; --i) {
        auto layer = qobject_cast<TimeInstantLayer *>(pane->getLayer(i));
        if (!layer) {
            continue;
        }
        if (layer == m_pendingOnsetsLayer &&
            selection != OnsetsLayerSelection::PermitPendingOnsets) {
            continue;
        }
        candidates.push_back(layer);
    }

    TimeInstantLayer *any = nullptr;
    TimeInstantLayer *nonDormant = nullptr;
    
    for (auto layer : candidates) {
        if (!layer->isLayerDormant(pane)) {
            if (!nonDormant) {
                nonDormant = layer;
            }
        }
        if (!any) {
            any = layer;
        }
    }

    if (nonDormant) {
        return nonDormant;
    } else {
        return any;
    }
}

void
Session::addFurtherAudioPane(Pane *audioPane)
{
    m_audioPanes.push_back(audioPane);

    ModelId modelId = getAudioModelFromPane(audioPane);

    if (modelId.isNone()) {
        SVDEBUG << "Session::addAudioPane: WARNING: Unable to retrieve audio model from pane" << endl;
        return;
    }

    // This pane should already have a waveform, so we move that to
    // the feature pane. If we don't find one, we have to make a new
    // one.

    WaveformLayer *waveformLayer = nullptr;
    
    int n = audioPane->getLayerCount();
    for (int i = 0; i < n; ++i) {
        auto layer = audioPane->getLayer(i);
        waveformLayer = qobject_cast<WaveformLayer *>(layer);
        if (waveformLayer) break;
    }

    if (waveformLayer) {
        m_document->removeLayerFromView(audioPane, waveformLayer);
        m_document->addLayerToView(m_featurePane, waveformLayer);
    }
    
    SpectrogramLayer *spectrogramLayer = qobject_cast<SpectrogramLayer *>
        (m_document->createLayer(LayerFactory::MelodicRangeSpectrogram));
    spectrogramLayer->setBinScale(BinScale::Linear);
    spectrogramLayer->setColourMap(ColourMapper::Green);
    spectrogramLayer->setColourScale(ColourScaleType::Log);
    spectrogramLayer->setColourScaleMultiple(2.0);

    m_document->addLayerToView(audioPane, spectrogramLayer);
    m_document->setModel(spectrogramLayer, modelId);

    resetAlignmentEntriesFor(modelId);
}

void
Session::setActivePane(Pane *pane)
{
    SVDEBUG << "Session::setActivePane(" << pane << ")" << endl;
    
    m_activePane = pane;

    if (!pane) {
        return;
    }

    if (!m_document) {
        // May be exiting
        SVDEBUG << "Session::setActivePane: No document, ignoring" << endl;
        return;
    }
    
    if (pane == m_featurePane) {
        return;
    }

    auto audioModel = getAudioModelFromPane(pane);
    if (audioModel.isNone()) {
        return;
    }
    
    // Select the associated waveform and tempo curve in the feature
    // pane, hide the rest
    
    int n = m_featurePane->getLayerCount();
    for (int i = 0; i < n; ++i) {

        auto layer = m_featurePane->getLayer(i);

        auto waveform = qobject_cast<WaveformLayer *>(layer);
        if (waveform) {
            waveform->showLayer
                (m_featurePane, waveform->getModel() == audioModel);
        }

        auto tempo = qobject_cast<TimeValueLayer *>(layer);
        if (tempo) {
            tempo->showLayer
                (m_featurePane, tempo->getSourceModel() == audioModel);
        }
    }
}

QString
Session::getActiveAudioTitle() const
{
    auto modelId = getActiveAudioModel();
    auto model = ModelById::getAs<RangeSummarisableTimeValueModel>(modelId);
    if (model) {
        return model->getTitle();
    } else {
        return {};
    }
}    

ModelId
Session::getActiveAudioModel() const
{
    SVDEBUG << "Session::getActiveAudioModel: we have " << m_audioPanes.size()
            << " audio panes" << endl;
    
    if (m_activePane) {

        // Check against the audio panes, because it might not be one
        for (auto p: m_audioPanes) {
            if (m_activePane == p) {
                auto modelId = getAudioModelFromPane(p);
                SVDEBUG << "Session::getActiveAudioModel: Returning model "
                        << modelId << " from active pane " << p << endl;
                return modelId;
            }
        }

        SVDEBUG << "Session::getActiveAudioModel: Returning main model "
                << m_mainModel << " as active pane is not an audio one" << endl;
        return m_mainModel;
    }

    if (m_audioPanes.empty()) {
        SVDEBUG << "Session::getActiveAudioModel: Returning main model "
                << m_mainModel << " as we have no active pane" << endl;
        return m_mainModel;
    } else {
        auto modelId = getAudioModelFromPane(m_audioPanes[0]);
        SVDEBUG << "Session::getActiveAudioModel: Returning model "
                << modelId << " from first pane " << m_audioPanes[0] << endl;
        return modelId;
    }
}

Pane *
Session::getAudioPaneForAudioModel(ModelId modelId) const
{
    if (modelId.isNone()) {
        return nullptr;
    }
            
    for (auto pane : m_audioPanes) {

        int n = pane->getLayerCount();

        for (int i = 0; i < n; ++i) {

            auto layer = pane->getLayer(i);

            // We are only interested in a pane with a spectrogram in
            // it; the feature pane may contain any number of
            // waveforms
        
            auto spectrogramLayer = qobject_cast<SpectrogramLayer *>(layer);
            if (spectrogramLayer && spectrogramLayer->getModel() == modelId) {
                return pane;
            }
        }
    }

    return nullptr;
}

void
Session::setAlignmentTransformId(TransformId alignmentTransformId)
{
    SVDEBUG << "Session::setAlignmentTransformId: Setting to \""
            << alignmentTransformId << "\"" << endl;
    m_alignmentTransformId = alignmentTransformId;
}

void
Session::beginAlignment()
{
    beginPartialAlignment(-1, -1, -1, -1, -1, -1);
}

void
Session::beginPartialAlignment(int scorePositionStartNumerator,
                               int scorePositionStartDenominator,
                               int scorePositionEndNumerator,
                               int scorePositionEndDenominator,
                               sv_frame_t audioFrameStart,
                               sv_frame_t audioFrameEnd)
{
    if (m_mainModel.isNone()) {
        SVDEBUG << "Session::beginPartialAlignment: ERROR: No main model; one should have been set first" << endl;
        return;
    }
    if (m_audioPanes.empty()) {
        SVDEBUG << "Session::beginPartialAlignment: ERROR: No audio panes" << endl;
        return;
    }

    ModelId activeModelId = getActiveAudioModel();
    Pane *activeAudioPane = getAudioPaneForAudioModel(activeModelId);

    if (!activeAudioPane) {
        SVDEBUG << "Session::beginPartialAlignment: ERROR: Failed to find audio pane for active model " << activeModelId << endl;
        return;
    }

    TransformId alignmentTransformId = m_alignmentTransformId;
    if (alignmentTransformId == "") {
        alignmentTransformId =
            ScoreAlignmentTransform::getDefaultAlignmentTransform();
    }

    if (alignmentTransformId == smartCopyTransformId) {
        propagateAlignmentFromMain();
        return;
    }
    
    ModelTransformer::Input input(activeModelId);

    if (alignmentTransformId == "") {
        SVDEBUG << "Session::beginPartialAlignment: ERROR: No alignment transform found" << endl;
        emit alignmentFailedToRun("No suitable score alignment plugin found");
        return;
    }

    vector<pair<QString, pair<Pane *, TimeInstantLayer **>>> layerDefinitions {
        { alignmentTransformId,
          { activeAudioPane, &m_pendingOnsetsLayer }
        }
    };

    vector<Layer *> newLayers;

    sv_samplerate_t sampleRate = ModelById::get(activeModelId)->getSampleRate();
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
            << scorePositionStartNumerator << "/"
            << scorePositionStartDenominator
            << ", end = " << scorePositionEndNumerator << "/"
            << scorePositionEndDenominator
            << ", audio start = " << audioStart << ", end = "
            << audioEnd << endl;

    // Hide the existing layers

    auto onsetsLayer = getOnsetsLayerFromPane
        (activeAudioPane, OnsetsLayerSelection::ExcludePendingOnsets);
    if (onsetsLayer) {
        onsetsLayer->showLayer(activeAudioPane, false);
    }
    if (m_featureData.find(activeModelId) != m_featureData.end()) {
        m_featurePane->removeLayer(m_featureData.at(activeModelId).tempoLayer);
    }
    
    Transform::ParameterMap params {
        { "score-position-start-numerator", scorePositionStartNumerator },
        { "score-position-start-denominator", scorePositionStartDenominator },
        { "score-position-end-numerator", scorePositionEndNumerator },
        { "score-position-end-denominator", scorePositionEndDenominator },
        { "audio-start", float(audioStart.toDouble()) },
        { "audio-end", float(audioEnd.toDouble()) }
    };

    // General principle is to create new layers using
    // m_document->createDerivedLayer, which creates and attaches a
    // model and runs a transform in the background.
    //
    // If we have an existing layer of the same type already, we don't
    // delete it but we do temporarily hide it.
    //
    // When the model is complete, our callback is called; at this
    // moment, if we had a layer which is now hidden, we merge its
    // model with the new one (into the new layer, not the old) and
    // ask the user if they want to keep the new one. If so, we delete
    // the old; if not, we restore the old and delete the new.
    //
    //!!! What should we do if the user requests an alignment when we
    //!!! are already waiting for one to complete?
    
    for (auto defn : layerDefinitions) {

        auto transformId = defn.first;
        auto pane = defn.second.first;
        auto layerPtr = defn.second.second;
        
        Transform t = TransformFactory::getInstance()->
            getDefaultTransformFor(transformId);

        SVDEBUG << "Session::beginPartialAlignment: Setting plugin's program to \"" << m_scoreId << "\"" << endl;
            
        t.setProgram(m_scoreId);
        t.setParameters(params);

        Layer *layer = m_document->createDerivedLayer(t, input);
        if (!layer) {
            SVDEBUG << "Session::beginPartialAlignment: Transform failed to initialise" << endl;
            emit alignmentFailedToRun(QString("Unable to initialise score alignment plugin \"%1\"").arg(transformId));
            return;
        }
        if (layer->getModel().isNone()) {
            SVDEBUG << "Session::beginPartialAlignment: Transform failed to create a model" << endl;
            emit alignmentFailedToRun(QString("Score alignment plugin \"%1\" did not produce the expected output").arg(transformId));
            return;
        }

        TimeInstantLayer *tl = qobject_cast<TimeInstantLayer *>(layer);
        if (!tl) {
            SVDEBUG << "Session::beginPartialAlignment: Transform resulted in wrong layer type" << endl;
            emit alignmentFailedToRun(QString("Score alignment plugin \"%1\" did not produce the expected output format").arg(transformId));
            return;
        }

        if (*layerPtr) {
            m_document->deleteLayer(*layerPtr, true);
        }
            
        *layerPtr = tl;
        
        m_document->addLayerToView(pane, layer);

        ModelId modelId = layer->getModel();
        auto model = ModelById::get(modelId);
        if (model->isReady(nullptr)) {
            modelReady(modelId);
        } else {
            connect(model.get(), SIGNAL(ready(ModelId)),
                    this, SLOT(modelReady(ModelId)));
        }
    }
        
    setOnsetsLayerProperties(m_pendingOnsetsLayer);

    m_partialAlignmentAudioStart = audioFrameStart;
    m_partialAlignmentAudioEnd = audioFrameEnd;
    
    m_pendingOnsetsPane = activeAudioPane;
    m_audioModelForPendingOnsets = activeModelId;
}

void
Session::setOnsetsLayerProperties(TimeInstantLayer *onsetsLayer)
{
//    onsetsLayer->setPlotStyle(TimeInstantLayer::PlotSegmentation);
//    onsetsLayer->setFillSegments(false);

    connect(onsetsLayer, &TimeInstantLayer::frameIlluminated,
            this, &Session::alignmentFrameIlluminated);

    auto playParams = PlayParameterRepository::getInstance()->getPlayParameters
        (onsetsLayer->getModel().untyped);
    if (playParams) {
        playParams->setPlayGain(0.1);
    }
}

void
Session::modelReady(ModelId id)
{
    SVDEBUG << "Session::modelReady: model is " << id << endl;

    if (m_pendingOnsetsLayer && id == m_pendingOnsetsLayer->getModel()) {
        alignmentComplete();
    }
}

void
Session::modelChanged(ModelId id)
{
    SVDEBUG << "Session::modelChanged: model is " << id << endl;

    auto mainOnsetsLayer = getOnsetsLayerFromPane
        (getAudioPaneForAudioModel(m_mainModel),
         OnsetsLayerSelection::PermitPendingOnsets);
    
    if (mainOnsetsLayer && id == mainOnsetsLayer->getModel()) {
        recalculateTempoLayerFor(id);
    }

    emit alignmentModified();
}

void
Session::alignmentComplete()
{
    SVDEBUG << "Session::alignmentComplete" << endl;

    recalculateTempoLayerFor(m_audioModelForPendingOnsets);
    updateOnsetColours();
    
    emit alignmentReadyForReview(m_pendingOnsetsPane, m_pendingOnsetsLayer);
}

void
Session::propagateAlignmentFromMain()
{
    propagatePartialAlignmentFromMain(-1, -1);
}

void
Session::propagatePartialAlignmentFromMain(sv_frame_t audioFrameStartInMain,
                                           sv_frame_t audioFrameEndInMain)
{
    SVDEBUG << "Session::propagatePartialAlignmentFromMain("
            << audioFrameStartInMain << ", " << audioFrameEndInMain << ")"
            << endl;
    
    auto mainOnsetsLayer = getOnsetsLayerFromPane
        (getAudioPaneForAudioModel(m_mainModel),
         OnsetsLayerSelection::ExcludePendingOnsets);
    if (!mainOnsetsLayer) {
        SVDEBUG << "Session::propagateAlignmentFromMain: No onsets layer found for main model " << m_mainModel << endl;
        return;
    }

    shared_ptr<SparseOneDimensionalModel> mainOnsetsModel =
        ModelById::getAs<SparseOneDimensionalModel>
        (mainOnsetsLayer->getModel());
    if (!mainOnsetsModel) {
        SVDEBUG << "Session::propagateAlignmentFromMain: No onsets model found for main model" << endl;
        return;
    }

    ModelId activeModelId = getActiveAudioModel();
    auto activeModel = ModelById::getAs<RangeSummarisableTimeValueModel>
        (activeModelId);
    if (!activeModel) {
        SVDEBUG << "Session::propagateAlignmentFromMain: No active audio model" << endl;
        return;
    }

    Pane *pane = getAudioPaneForAudioModel(activeModelId);
    if (!pane) {
        SVDEBUG << "Session::propagateAlignmentFromMain: No pane for active model" << endl;
        return;
    }
    
    if (m_pendingOnsetsLayer) {
        m_document->deleteLayer(m_pendingOnsetsLayer, true);
    }

    m_pendingOnsetsLayer = qobject_cast<TimeInstantLayer *>
        (m_document->createEmptyLayer(LayerFactory::TimeInstants));
    
    shared_ptr<SparseOneDimensionalModel> pendingOnsetsModel =
        ModelById::getAs<SparseOneDimensionalModel>
        (m_pendingOnsetsLayer->getModel());

    m_document->addLayerToView(pane, m_pendingOnsetsLayer);
    setOnsetsLayerProperties(m_pendingOnsetsLayer);

    EventVector events;

    if (audioFrameEndInMain > audioFrameStartInMain) {
        SVDEBUG << "selecting events from " << audioFrameStartInMain
                << " to " << audioFrameEndInMain << endl;
        events = mainOnsetsModel->getEventsWithin
            (audioFrameStartInMain, audioFrameEndInMain - audioFrameStartInMain);
    } else {
        events = mainOnsetsModel->getAllEvents();
    }

    for (auto e : events) {
        sv_frame_t mapped = activeModel->alignFromReference(e.getFrame());
        SVDEBUG << "mapped event frame " << e.getFrame() << " to "
                << mapped << endl;
        pendingOnsetsModel->add(Event(mapped, e.getLabel()));
    }

    m_partialAlignmentAudioStart = -1;
    m_partialAlignmentAudioEnd = -1;

    m_pendingOnsetsPane = pane;
    m_audioModelForPendingOnsets = activeModelId;
    
    alignmentComplete();
}

void
Session::rejectAlignment()
{
    SVDEBUG << "Session::rejectAlignment" << endl;
    
    if (!m_pendingOnsetsLayer) {
        SVDEBUG << "Session::rejectAlignment: No alignment waiting to be rejected" << endl;
        return;
    }        

    m_document->deleteLayer(m_pendingOnsetsLayer, true);

    if (!m_audioModelForPendingOnsets.isNone()) {
        auto pane = getAudioPaneForAudioModel(m_audioModelForPendingOnsets);
        auto previousOnsets = getOnsetsLayerFromPane
            (pane, OnsetsLayerSelection::ExcludePendingOnsets);
        if (previousOnsets) {
            previousOnsets->showLayer(pane, true);
        }
    }

    m_pendingOnsetsLayer = nullptr;
    
    recalculateTempoLayerFor(m_audioModelForPendingOnsets);
    updateOnsetColours();
    
    m_audioModelForPendingOnsets = {};
    
    emit alignmentRejected();
}

void
Session::acceptAlignment()
{
    SVDEBUG << "Session::acceptAlignment" << endl;
    
    if (!m_pendingOnsetsLayer || m_audioModelForPendingOnsets.isNone()) {
        SVDEBUG << "Session::acceptAlignment: No alignment waiting to be accepted" << endl;
        return;
    }        

    auto pane = getAudioPaneForAudioModel(m_audioModelForPendingOnsets);
    auto previousOnsets = getOnsetsLayerFromPane
        (pane, OnsetsLayerSelection::ExcludePendingOnsets);
    
    if (previousOnsets && m_partialAlignmentAudioEnd >= 0) {
        mergeLayers(previousOnsets, m_pendingOnsetsLayer,
                    m_partialAlignmentAudioStart, m_partialAlignmentAudioEnd);
    }
    
    if (previousOnsets) {
        m_document->deleteLayer(previousOnsets, true);
    }

    connect(ModelById::get(m_pendingOnsetsLayer->getModel()).get(),
            SIGNAL(modelChanged(ModelId)), this, SLOT(modelChanged(ModelId)));
    
    m_pendingOnsetsLayer = nullptr;
    
    recalculateTempoLayerFor(m_audioModelForPendingOnsets);
    updateOnsetColours();
    
    emit alignmentAccepted();
}

void
Session::signifyEditMode()
{
    m_inEditMode = true;
    updateOnsetColours();
}

void
Session::signifyNavigateMode()
{
    m_inEditMode = false;
    updateOnsetColours();
}

void
Session::mergeLayers(TimeInstantLayer *from, TimeInstantLayer *to,
                     sv_frame_t overlapStart, sv_frame_t overlapEnd)
{
    // Currently the way we are handling this is by having "to"
    // contain *only* the new events, within overlapStart to
    // overlapEnd.  So the merge just copies all events outside that
    // range from "from" to "to". There are surely cleverer ways

    //!!! We should also use a command
    
    auto fromModel = ModelById::getAs<SparseOneDimensionalModel>(from->getModel());
    auto toModel = ModelById::getAs<SparseOneDimensionalModel>(to->getModel());

    EventVector beforeOverlap = fromModel->getEventsWithin(0, overlapStart);
    EventVector afterOverlap = fromModel->getEventsWithin
        (overlapEnd, fromModel->getEndFrame() - overlapEnd);

    for (auto e : beforeOverlap) toModel->add(e);
    for (auto e : afterOverlap) toModel->add(e);
}

bool
Session::canExportAlignment() const
{
    auto modelId = getActiveAudioModel();
    return (!modelId.isNone() &&
            m_featureData.find(modelId) != m_featureData.end());
}

bool
Session::exportAlignmentTo(QString path)
{
    if (QFileInfo(path).suffix() == "") {
        path += ".csv";
    }
    
    bool success = updateAlignmentEntriesFor(getActiveAudioModel());
    if (success) {
        success = exportAlignmentEntries(getActiveAudioModel(), path);
    }
    return success;
}

bool
Session::exportAlignmentEntries(ModelId modelId, QString path)
{
    if (modelId.isNone() || m_featureData.find(modelId) == m_featureData.end()) {
        return false;
    }
    
    sv_samplerate_t sampleRate = ModelById::get(modelId)->getSampleRate();

    // Write to a temporary file and then move it into place at the
    // end, so as to avoid overwriting existing file if for any reason
    // the write fails
    
    TempWriteFile temp(path);
    QFile file(temp.getTemporaryFilename());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        SVCERR << "Session::exportAlignmentEntriesTo: Failed to open file "
               << temp.getTemporaryFilename() << " for writing" << endl;
        return false;
    }
    
    QTextStream out(&file);

    out << "LABEL,TIME,FRAME\n";
    
    for (const auto &entry : m_featureData.at(modelId).alignmentEntries) {
        QVector<QString> columns;
        columns << QString::fromStdString(entry.label);
        auto frame = entry.frame;
        if (frame < 0) {
            columns << "N" << "N";
        } else {
            columns << QString("%1").arg(RealTime::frame2RealTime
                                         (frame, sampleRate).toDouble())
                    << QString("%1").arg(frame);
        }
        out << StringBits::joinDelimited(columns, ",") << '\n';
    }

    file.close();
    temp.moveToTarget();
    return true;
}

bool
Session::importAlignmentFrom(QString path)
{
    SVDEBUG << "Session::importAlignmentFrom(" << path << ")" << endl;

    auto audioModelId = getActiveAudioModel();
    auto audioModel = ModelById::get(audioModelId);
    if (!audioModel) {
        SVDEBUG << "Session::importAlignmentFrom: No active audio model" << endl;
        return false;
    }

    // We support two different CSV formats:
    //
    // * The one we export is is LABEL,TIME,FRAME where LABEL is text,
    // TIME is a number in seconds (not an integer), and FRAME is an
    // integer audio sample frame number. We use FRAME as the
    // authoritative timestamp. The TIME column was derived from FRAME
    // and should not be imported.
    //
    // * We also support a simpler two-column format LABEL,TIME where
    // LABEL is text, TIME is a number in seconds. Here we use TIME as
    // the timestamp and convert it back to frame ourselves.
    //
    // Either way we want to import to an onsets layer whose contents
    // are time instants indexed by audio sample frame, with a label
    // taken from LABEL.

    bool haveFrame = (CSVFormat(path).getColumnCount() > 2);
    
    CSVFormat format;
    
    format.setSeparator(QChar(','));
    format.setHeaderStatus(CSVFormat::HeaderPresent);
    format.setModelType(CSVFormat::OneDimensionalModel);
    format.setTimingType(CSVFormat::ExplicitTiming);

    if (haveFrame) {
        SVDEBUG << "Session::importAlignmentFrom: Have [at least] 3 columns, assuming we have label, [derived] time, and [authoritative] frame" << endl;
        format.setColumnCount(3);
        format.setTimeUnits(CSVFormat::TimeAudioFrames);
        QList<CSVFormat::ColumnPurpose> purposes {
            CSVFormat::ColumnLabel,    // LABEL
            CSVFormat::ColumnUnknown,  // TIME - Derived column, don't import
            CSVFormat::ColumnStartTime // FRAME
        };
        format.setColumnPurposes(purposes);
    } else {
        SVDEBUG << "Session::importAlignmentFrom: Have fewer than 3 columns, assuming we have label and time" << endl;
        format.setColumnCount(2);
        format.setTimeUnits(CSVFormat::TimeSeconds);
        QList<CSVFormat::ColumnPurpose> purposes {
            CSVFormat::ColumnLabel,    // LABEL
            CSVFormat::ColumnStartTime // TIME
        };
        format.setColumnPurposes(purposes);
    }
    
    CSVFileReader reader(path, format, audioModel->getSampleRate(), nullptr);

    if (!reader.isOK()) {
        SVDEBUG << "Session::importAlignmentFrom: Failed to construct CSV reader: " << reader.getError() << endl;
        return false;
    }

    Model *imported = reader.load();
    if (!imported) {
        SVDEBUG << "Session::importAlignmentFrom: Failed to import model from CSV file" << endl;
        return false;
    }

    auto stvm = qobject_cast<SparseOneDimensionalModel *>(imported);
    if (!stvm) {
        SVDEBUG << "Session::importAlignmentFrom: Imported model is of the wrong type" << endl;
        delete imported;
        return false;
    }

    auto pane = getAudioPaneForAudioModel(audioModelId);
    if (!pane) {
        SVDEBUG << "Session::importAlignmentFrom: No audio pane for model "
                << audioModel << endl;
        return false;
    }
    
    auto onsetsLayer = getOnsetsLayerFromPane
        (pane, OnsetsLayerSelection::ExcludePendingOnsets);
    
    if (!onsetsLayer) {
        onsetsLayer = dynamic_cast<TimeInstantLayer *>
            (m_document->createEmptyLayer(LayerFactory::TimeInstants));
        m_document->addLayerToView(pane, onsetsLayer);
        setOnsetsLayerProperties(onsetsLayer);
    }
    
    auto existingModel = ModelById::getAs<SparseOneDimensionalModel>
        (onsetsLayer->getModel());
    if (!existingModel) {
        SVDEBUG << "Session::importAlignmentFrom: Internal error: onsets layer has no model!" << endl;
        delete imported;
        return false;
    }

    disconnect(existingModel.get(), SIGNAL(modelChanged(ModelId)),
               this, nullptr);
    
    EventVector oldEvents = existingModel->getAllEvents();
    EventVector newEvents = stvm->getAllEvents();
    
    for (auto e : oldEvents) {
        existingModel->remove(e);
    }
    for (auto e : newEvents) {
        existingModel->add(e);
    }

    delete imported;

    recalculateTempoLayerFor(audioModelId);
    updateOnsetColours();
    emit alignmentAccepted();    

    connect(existingModel.get(), SIGNAL(modelChanged(ModelId)),
            this, SLOT(modelChanged(ModelId)));

    return true;
}

void
Session::setMusicalEvents(const Score::MusicalEventList &musicalEvents)
{
    m_musicalEvents = musicalEvents;
    resetAllAlignmentEntries();
}

void
Session::resetAllAlignmentEntries()
{
    for (auto fd : m_featureData) {
        resetAlignmentEntriesFor(fd.first);
    }
}

void
Session::resetAlignmentEntriesFor(ModelId model)
{
    if (m_featureData.find(model) == m_featureData.end()) {
        m_featureData[model] = { {}, nullptr };
    } else {
        m_featureData.at(model).alignmentEntries.clear();
    }
    // Calculating the mapping from score musical events to m_alignmentEntries
    for (auto &event : m_musicalEvents) {
        Score::MeasureInfo info = event.measureInfo;
        std::string label = info.toLabel();
        m_featureData.at(model).alignmentEntries.push_back
            (AlignmentEntry(label, -1)); // -1 is placeholder
    }
}

bool
Session::updateAlignmentEntriesFor(ModelId audioModelId)
{
    auto pane = getAudioPaneForAudioModel(audioModelId);
    if (!pane) {
        SVDEBUG << "Session::updateAlignmentEntriesFor: No audio pane for model "
                << audioModelId << endl;
        return false;
    }

    if (m_featureData.find(audioModelId) == m_featureData.end()) {
        resetAlignmentEntriesFor(audioModelId);
    }
    
    auto onsetsLayer = getOnsetsLayerFromPane
        (pane, OnsetsLayerSelection::ExcludePendingOnsets);
    if (!onsetsLayer) {
        SVDEBUG << "Session::updateAlignmentEntriesFor: NOTE: No onsets layer for model " << audioModelId << endl;
        // This is actually fine, it just means the alignment is
        // effectively empty
        return true;
    }

    shared_ptr<SparseOneDimensionalModel> onsetsModel =
        ModelById::getAs<SparseOneDimensionalModel>(onsetsLayer->getModel());
    if (!onsetsModel) {
        SVDEBUG << "Session::updateAlignmentEntriesFor: Onsets layer for model "
                << audioModelId << " lacks onsets model itself" << endl;
        return false;
    }

    auto &alignmentEntries = m_featureData.at(audioModelId).alignmentEntries;
    int n = alignmentEntries.size();
    
    // Overwriting the frame values
    auto onsets = onsetsModel->getAllEvents();
    for (auto onset : onsets) {
        // finding the alignment entry with the same label
        std::string target = onset.getLabel().toStdString();
        bool found = false;
        int i = 0;
        while (!found && i < n) {
            if (alignmentEntries[i].label == target) {
                found = true;
            } else {
                i++;
            }
        }
        if (!found) {
            SVCERR << "ERROR: In Session::updateAlignmentEntries, label "
                   << target << " not found!" << endl;
            return false;
        }
        alignmentEntries[i].frame = onset.getFrame();
    }

    return true;
}

void
Session::recalculateTempoLayerFor(ModelId audioModel)
{
    if (audioModel.isNone()) return;

    TimeValueLayer *tempoLayer = nullptr;
    
    if (m_featureData.find(audioModel) == m_featureData.end()) {
        m_featureData[audioModel] = { {}, nullptr };
    }

    if (!m_featureData.at(audioModel).tempoLayer) {
        tempoLayer = qobject_cast<TimeValueLayer *>
            (m_document->createLayer(LayerFactory::TimeValues));
        ColourDatabase *cdb = ColourDatabase::getInstance();
        tempoLayer->setBaseColour(cdb->getColourIndex(tr("Blue")));
        m_document->addLayerToView(m_featurePane, tempoLayer);
        m_featureData[audioModel].tempoLayer = tempoLayer;
    } else {
        tempoLayer = m_featureData.at(audioModel).tempoLayer;
    }
        
    sv_samplerate_t sampleRate = ModelById::get(audioModel)->getSampleRate();
    auto tempoModel = make_shared<SparseTimeValueModel>(sampleRate, 1);
    ModelId tempoModelId = ModelById::add(tempoModel);
    tempoModel->setSourceModel(audioModel);
    m_document->addNonDerivedModel(tempoModelId);
    m_document->setModel(tempoLayer, tempoModelId);

    auto audioPane = getAudioPaneForAudioModel(audioModel);
    if (!audioPane) {
        SVDEBUG << "Session::recalculateTempoLayer: No audio pane for model "
                << audioModel << endl;
        return;
    }
    
    auto onsetsLayer = getOnsetsLayerFromPane
        (audioPane, OnsetsLayerSelection::PermitPendingOnsets);
    if (!onsetsLayer) {
        SVDEBUG << "Session::recalculateTempoLayer: No onsets layer in pane for audio model "
                << audioModel << endl;
        return;
    }

    if (!updateAlignmentEntriesFor(audioModel)) {
        SVDEBUG << "Session::recalculateTempoLayer: Failed to update alignment entries" << endl;
        return;
    }
        
    const auto &alignmentEntries = m_featureData.at(audioModel).alignmentEntries;
    int n = alignmentEntries.size();
    
    int start = -1, end = -2;
    bool stop = false;
    while (!stop && end <= n - 4) {
        start = end + 2;
        while (alignmentEntries[start].frame < 0) {
            start++;
            if (start >= int(alignmentEntries.size()) - 1) {
                stop = true; // no more aligned sections
                break;
            }
        }
        end = start;
        while (!stop && alignmentEntries[end+1].frame >= 0) {
            end++;
            if (end >= int(alignmentEntries.size()) - 1) {
                stop = true; // reached the last event
                break;
            }
        }
        for (int i = start; i < end; ++i) {
            auto thisFrame = alignmentEntries[i].frame;
            auto nextFrame = alignmentEntries[i+1].frame;
            auto thisSec = RealTime::frame2RealTime(thisFrame, sampleRate).toDouble();
            auto nextSec = RealTime::frame2RealTime(nextFrame, sampleRate).toDouble();
            Fraction dur = m_musicalEvents[i].duration;
            if (abs(nextSec - thisSec) > 0) {
                double tempo = (4. * dur.numerator / dur.denominator) * 60. / (nextSec - thisSec); // num of quarter notes per minutes
                Event tempoEvent(thisFrame, float(tempo), QString());
                tempoModel->add(tempoEvent);
            }
        }
    }
}

void
Session::updateOnsetColours()
{
    for (auto pane : m_audioPanes) {

        auto onsetsLayer = getOnsetsLayerFromPane
            (pane, OnsetsLayerSelection::PermitPendingOnsets);

        if (!onsetsLayer) {
            continue;
        }

        bool isPending =
            (m_pendingOnsetsLayer &&
             pane == getAudioPaneForAudioModel(m_audioModelForPendingOnsets));
        
        QString colour;

        if (isPending) {
            colour = "Bright Red";
        } else if (m_inEditMode) {
            colour = "Orange";
        } else {
            colour = "Purple";
        }
    
        ColourDatabase *cdb = ColourDatabase::getInstance();
        onsetsLayer->setBaseColour(cdb->getColourIndex(colour));
    }
}


