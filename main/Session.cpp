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
    setDocument(nullptr, nullptr, nullptr, nullptr, nullptr);
}

Session::~Session()
{
    for (auto f : m_featureData) {
        ModelById::release(f.second.tempoModel);
    }
}

void
Session::setDocument(Document *doc,
                     Pane *mainAudioPane,
                     TempoCurveWidget *tempoCurveWidget,
                     View *overview,
                     Layer *timeRuler)
{
    SVDEBUG << "Session::setDocument(" << doc << ")" << endl;

    if (m_pendingOnsetsLayer) {
        emit alignmentRejected();
    }

    // Don't reset the score id or musical events - they can outlast
    // the document, and indeed are usually present before the
    // document is first set
    
    m_document = doc;
    m_mainModel = {};

    m_audioPanes.clear();
    if (mainAudioPane) {
        m_audioPanes.push_back(mainAudioPane);
    }
    m_tempoCurveWidget = tempoCurveWidget;
    m_overview = overview;
    m_activePane = mainAudioPane;
    m_timeRulerLayer = timeRuler;

    m_partialAlignmentAudioStart = -1;
    m_partialAlignmentAudioEnd = -1;

    m_pendingOnsetsPane = nullptr;
    m_pendingOnsetsLayer = nullptr;
    m_audioModelForPendingOnsets = {};

    m_featureData.clear();
    m_inEditMode = false;

    if (mainAudioPane) {
        connect(mainAudioPane, &View::centreFrameChanged,
                this, &Session::paneCentreOrZoomChanged);
        connect(mainAudioPane, &View::zoomLevelChanged,
                this, &Session::paneCentreOrZoomChanged);
    }
}

void
Session::unsetDocument()
{
    setDocument(nullptr, nullptr, nullptr, nullptr, nullptr);
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

void
Session::setMainModel(ModelId modelId)
{
    SVDEBUG << "Session::setMainModel(" << modelId << ")" << endl;

    if (m_mainModel == modelId) {
        SVDEBUG << "Session::setMainModel: we already have it" << endl;
        return;
    }
    
    m_mainModel = modelId;

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
    } else if (m_audioPanes.empty()) {
        SVDEBUG << "Session::setMainModel: WARNING: Set a main model but we lack any audio panes" << endl;
        return;
    }

    ColourDatabase *cdb = ColourDatabase::getInstance();
    auto waveColour = cdb->getColourIndex(tr("Orange"));

    WaveformLayer *overviewLayer = nullptr;
    
    if (m_overview) {
        overviewLayer = qobject_cast<WaveformLayer *>
            (m_document->createLayer(LayerFactory::Waveform));
        overviewLayer->setChannelMode(WaveformLayer::MergeChannels);
        overviewLayer->setAggressiveCacheing(true);
        overviewLayer->setBaseColour(waveColour);
        m_document->addLayerToView(m_overview, overviewLayer);
        m_document->setModel(overviewLayer, m_mainModel);
    }

    SpectrogramLayer *spectrogramLayer = qobject_cast<SpectrogramLayer *>
        (m_document->createLayer(LayerFactory::MelodicRangeSpectrogram));
    spectrogramLayer->setBinScale(BinScale::Linear);
    spectrogramLayer->setColourMap(ColourMapper::Green);
    spectrogramLayer->setColourScale(ColourScaleType::Log);
    spectrogramLayer->setColourScaleMultiple(2.0);
    
    m_document->addLayerToView(m_audioPanes[0], spectrogramLayer);
    m_document->setModel(spectrogramLayer, m_mainModel);

    m_featureData[m_mainModel] = {
        {},                 // alignmentEntries
        {},                 // tempoModel
        overviewLayer,      // overviewLayer
        {},                 // lastExportedTo
        false               // alignmentModified
    };
}

void
Session::paneRemoved(Pane *pane)
{
    vector<Pane *> remainingPanes;
    ModelId modelToBeDeleted = getAudioModelFromPane(pane);
    bool isReference = (modelToBeDeleted == m_mainModel);

    SVDEBUG << "Session::paneRemoved: pane = " << pane
            << ", modelToBeDeleted = " << modelToBeDeleted
            << ", isReference = " << isReference << endl;
    
    ModelId newMainModel = m_mainModel;

    for (Pane *p : m_audioPanes) {
        if (p != pane) {
            remainingPanes.push_back(p);
            if (newMainModel == modelToBeDeleted) {
                auto modelId = getAudioModelFromPane(p);
                if (modelId != modelToBeDeleted) {
                    newMainModel = modelId;
                }
            }
        }
    }

    m_audioPanes = remainingPanes;

    vector<View *> views = { m_overview };

    for (auto view : views) {
        if (!view) continue;
        vector<Layer *> layersToRemove;
        for (int i = 0; i < view->getLayerCount(); ++i) {
            Layer *layer = view->getLayer(i);
            if (layer->getModel() == modelToBeDeleted) {
                layersToRemove.push_back(layer);
            }
        }
        for (auto layer : layersToRemove) {
            m_document->removeLayerFromView(view, layer);
        }
    }

    if (m_tempoCurveWidget) {
        m_tempoCurveWidget->unsetCurveForAudio(modelToBeDeleted);
    }
    
    if (newMainModel == modelToBeDeleted) {
        newMainModel = {};
        SVDEBUG << "Session::paneRemoved: it's the main model pane being deleted, but we have no other model to be the main one" << endl;
    }

    SVDEBUG << "Session::paneRemoved: switching main model to " << newMainModel << endl;
    m_mainModel = newMainModel;
    if (m_document) {
        m_document->switchMainModel(m_mainModel);
        m_document->realignModels();
    }
    
    if (m_activePane == pane) {
        m_activePane = nullptr;
    }
}

ModelId
Session::getAudioModelFromPane(Pane *pane) const
{
    if (!pane) {
        return {};
    }
    
    int n = pane->getLayerCount();

//    SVDEBUG << "Session::getAudioModelFromPane: pane = " << pane
//            << ", n = " << n << endl;
    
    for (int i = n-1; i >= 0; --i) {

        // Reverse order, to find whichever is visible (since in the
        // feature pane there could be more than one audio layer)
        
        auto layer = pane->getLayer(i);

//        SVDEBUG << "layer " << i << ": " << layer->getLayerPresentationName()
//                << endl;
        
        auto waveformLayer = qobject_cast<WaveformLayer *>(layer);
        if (waveformLayer && !waveformLayer->isLayerDormant(pane)) {
            return waveformLayer->getModel();
        }
        
        auto spectrogramLayer = qobject_cast<SpectrogramLayer *>(layer);
        if (spectrogramLayer && !spectrogramLayer->isLayerDormant(pane)) {
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

    // This pane should already have a waveform; we put the
    // spectrogram on top

    WaveformLayer *waveformLayer = nullptr;
    WaveformLayer *overviewLayer = nullptr;
    
    int n = audioPane->getLayerCount();
    for (int i = 0; i < n; ++i) {
        auto layer = audioPane->getLayer(i);
        waveformLayer = qobject_cast<WaveformLayer *>(layer);
        if (waveformLayer) break;
    }
    
    if (waveformLayer) {
        if (m_overview) {
            overviewLayer = qobject_cast<WaveformLayer *>
                (m_document->createLayer(LayerFactory::Waveform));
            overviewLayer->setChannelMode(WaveformLayer::MergeChannels);
            overviewLayer->setAggressiveCacheing(true);
            overviewLayer->setBaseColour(waveformLayer->getBaseColour());
            m_document->addLayerToView(m_overview, overviewLayer);
            m_document->setModel(overviewLayer, modelId);
        }
    }
    
    SpectrogramLayer *spectrogramLayer = qobject_cast<SpectrogramLayer *>
        (m_document->createLayer(LayerFactory::MelodicRangeSpectrogram));
    spectrogramLayer->setBinScale(BinScale::Linear);
    spectrogramLayer->setColourMap(ColourMapper::Green);
    spectrogramLayer->setColourScale(ColourScaleType::Log);
    spectrogramLayer->setColourScaleMultiple(2.0);

    m_document->addLayerToView(audioPane, spectrogramLayer);
    m_document->setModel(spectrogramLayer, modelId);
    
    m_featureData[modelId] = {
        {},                 // alignmentEntries
        {},                 // tempoModel
        overviewLayer,      // overviewLayer
        {},                 // lastExportedTo
        false               // alignmentModified
    };

    connect(audioPane, &View::centreFrameChanged,
            this, &Session::paneCentreOrZoomChanged);
    connect(audioPane, &View::zoomLevelChanged,
            this, &Session::paneCentreOrZoomChanged);
}

void
Session::setActivePane(Pane *pane)
{
    SVDEBUG << "Session::setActivePane(" << pane << ")" << endl;
    
    if (!m_document) {
        // If we're closing a session or exiting entirely, then it can
        // happen that our document is cleared and setActivePane is
        // subsequently called simply as an artifact of an active pane
        // being deleted (so some other pane being made active). In
        // this situation we want to avoid setting the active pane to
        // anything, because it isn't going to persist
        SVDEBUG << "Session::setActivePane: No document, resetting" << endl;
        m_activePane = nullptr;
        return;
    }
    
    m_activePane = pane;

    if (!pane) {
        return;
    }

    ModelId audioModelId = getAudioModelFromPane(pane);
    if (audioModelId.isNone()) {
        return;
    }
    
    // Select the associated waveform and tempo curves in the feature
    // pane and overview, hide the rest

    vector<View *> views = { m_overview };
    
    for (auto view : views) {
        if (!view) continue;
        int n = view->getLayerCount();
        for (int i = 0; i < n; ++i) {
            auto layer = view->getLayer(i);
            auto waveform = qobject_cast<WaveformLayer *>(layer);
            if (waveform) {
                waveform->showLayer
                    (view, waveform->getModel() == audioModelId);
            }
            auto tempo = qobject_cast<TimeValueLayer *>(layer);
            if (tempo) {
                tempo->showLayer
                    (view, tempo->getSourceModel() == audioModelId);
            }
        }
    }

    updateTempoCurveExtentsFromActivePane();
}

void
Session::paneCentreOrZoomChanged()
{
    Pane *pane = dynamic_cast<Pane *>(sender());
    if (!pane) {
        SVDEBUG << "Session::paneCentreOrZoomChanged: sender is not a pane"
                << endl;
        return;
    }
    if (pane == m_activePane) {
        updateTempoCurveExtentsFromActivePane();
    }
}

void
Session::updateTempoCurveExtentsFromActivePane()
{
    if (!m_activePane) {
        return;
    }
    
    ModelId audioModelId = getAudioModelFromPane(m_activePane);
    if (audioModelId.isNone()) {
        return;
    }
    
    if (m_tempoCurveWidget) {
        m_tempoCurveWidget->setCurrentAudioModel(audioModelId);
        m_tempoCurveWidget->setAudioModelDisplayedRange
            (m_activePane->getStartFrame(), m_activePane->getEndFrame());
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
    // We used to laboriously check that the active pane was an audio
    // pane and return the main model if it wasn't, but
    // getActiveAudioModel is clever enough to return the right model
    // also when the feature pane is active, so this should be fine
    
    return getAudioModelFromPane(m_activePane);
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
            this, &Session::frameIlluminated);

    auto playParams = PlayParameterRepository::getInstance()->getPlayParameters
        (onsetsLayer->getModel().untyped);
    if (playParams) {
        playParams->setPlayGain(0.1);
    } else {
        SVDEBUG << "Session::setOnsetsLayerProperties: WARNING: No play parameters found for model " << onsetsLayer->getModel().untyped << endl;
    }
}

void
Session::frameIlluminated(sv_frame_t frame)
{
    TimeInstantLayer *layer = dynamic_cast<TimeInstantLayer *>(sender());
    if (!layer) return;

    auto model = ModelById::getAs<SparseOneDimensionalModel>(layer->getModel());
    if (!model) return;

    SVDEBUG << "Session::frameIlluminated(" << frame << ") from layer "
            << layer << " with model id " << layer->getModel() << endl;
    
    auto events = model->getEventsStartingAt(frame);
    QString label;

    if (events.empty()) {
        SVDEBUG << "Session::frameIlluminated: no event found at frame "
                << frame << ", emitting with frame only?" << endl;
    } else {
        label = events[0].getLabel();
    }

    emit alignmentEventIlluminated(frame, label);
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

    for (auto &p : m_audioPanes) {

        auto onsetsLayer =
            getOnsetsLayerFromPane(p, OnsetsLayerSelection::PermitPendingOnsets);
        auto audioModelId =
            getAudioModelFromPane(p);

        if (onsetsLayer && !audioModelId.isNone()) {
            recalculateTempoCurveFor(audioModelId);
            emit alignmentModified();
        }
    }
}

void
Session::modelChangedWithin(ModelId id, sv_frame_t, sv_frame_t)
{
    modelChanged(id);
}

void
Session::alignmentComplete()
{
    SVDEBUG << "Session::alignmentComplete" << endl;

    recalculateTempoCurveFor(m_audioModelForPendingOnsets);
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
            (audioFrameStartInMain,
             // +1 because end point is exclusive and we don't want it to be
             audioFrameEndInMain - audioFrameStartInMain + 1);
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

    if (audioFrameStartInMain >= 0) {
        m_partialAlignmentAudioStart =
            activeModel->alignFromReference(audioFrameStartInMain);
    }
    if (audioFrameEndInMain >= 0) {
        m_partialAlignmentAudioEnd =
            activeModel->alignFromReference(audioFrameEndInMain);
    }
    
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
    
    recalculateTempoCurveFor(m_audioModelForPendingOnsets);
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
            &Model::modelChanged, this, &Session::modelChanged);

    connect(ModelById::get(m_pendingOnsetsLayer->getModel()).get(),
            &Model::modelChangedWithin, this, &Session::modelChangedWithin);
    
    m_pendingOnsetsLayer = nullptr;
    
    recalculateTempoCurveFor(m_audioModelForPendingOnsets);
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
    if (m_scoreId == "") {
//        SVDEBUG << "Session::canExportAlignment: No, no score ID set" << endl;
        return false;
    }
    if (m_musicalEvents.empty()) {
//        SVDEBUG << "Session::canExportAlignment: No, no musical events set" << endl;
        return false;
    }
    auto modelId = getActiveAudioModel();
    if (modelId.isNone()) {
//        SVDEBUG << "Session::canExportAlignment: No, no active audio model" << endl;
        return false;
    }
    if (m_featureData.find(modelId) == m_featureData.end()) {
//        SVDEBUG << "Session::canExportAlignment: No, no feature data" << endl;
        return false;
    }
//    SVDEBUG << "Session::canExportAlignment: Yes" << endl;
    return true;
}

bool
Session::canReExportAlignment() const
{
    if (!canExportAlignment()) {
        return false;
    }
    auto modelId = getActiveAudioModel();
    if (m_featureData.at(modelId).lastExportedTo == "") {
//        SVDEBUG << "Session::canReExportAlignment: No, we have no filename"
//                << endl;
        return false;
    }
    if (!m_featureData.at(modelId).alignmentModified) {
//        SVDEBUG << "Session::canReExportAlignment: No, it hasn't been modified"
//                << endl;
        return false;
    }
    return true;
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
Session::reExportAlignment()
{
    auto modelId = getActiveAudioModel();
    if (m_featureData.find(modelId) == m_featureData.end()) {
        SVDEBUG << "Session::reExportAlignment: No feature data found for audio model" << endl;
        return false;
    }
    if (m_featureData.at(modelId).lastExportedTo == "") {
        SVDEBUG << "Session::reExportAlignment: No filename" << endl;
        return false;
    }
    return exportAlignmentTo(m_featureData.at(modelId).lastExportedTo);
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

    m_featureData.at(modelId).lastExportedTo = path;
    m_featureData.at(modelId).alignmentModified = false;
        
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

    disconnect(existingModel.get(), &Model::modelChanged, this, nullptr);
    disconnect(existingModel.get(), &Model::modelChangedWithin, this, nullptr);
    
    EventVector oldEvents = existingModel->getAllEvents();
    EventVector newEvents = stvm->getAllEvents();
    
    for (auto e : oldEvents) {
        existingModel->remove(e);
    }
    for (auto e : newEvents) {
        existingModel->add(e);
    }

    delete imported;

    recalculateTempoCurveFor(audioModelId);
    updateOnsetColours();
    emit alignmentAccepted();    

    connect(existingModel.get(),
            &Model::modelChanged, this, &Session::modelChanged);

    connect(existingModel.get(),
            &Model::modelChangedWithin, this, &Session::modelChangedWithin);
        
    return true;
}

void
Session::setMusicalEvents(QString scoreId,
                          const Score::MusicalEventList &musicalEvents)
{
    m_scoreId = scoreId;
    m_musicalEvents = musicalEvents;

    for (auto fd : m_featureData) {
        fd.second.alignmentEntries.clear();
    }
}

bool
Session::updateAlignmentEntriesFor(ModelId audioModelId)
{
    if (m_featureData.find(audioModelId) == m_featureData.end()) {
        SVDEBUG << "Session::updateAlignmentEntriesFor: No feature data record found" << endl;
        return false;
    }
    
    auto pane = getAudioPaneForAudioModel(audioModelId);
    if (!pane) {
        SVDEBUG << "Session::updateAlignmentEntriesFor: No audio pane for model "
                << audioModelId << endl;
        return false;
    }

    std::map<std::string, sv_frame_t> labelFrameMap;
    
    auto onsetsLayer = getOnsetsLayerFromPane
        (pane, OnsetsLayerSelection::ExcludePendingOnsets);

    if (onsetsLayer) {
        shared_ptr<SparseOneDimensionalModel> onsetsModel =
            ModelById::getAs<SparseOneDimensionalModel>(onsetsLayer->getModel());
        if (onsetsModel) {
            auto onsets = onsetsModel->getAllEvents();
            for (auto onset : onsets) {
                labelFrameMap[onset.getLabel().toStdString()] = onset.getFrame();
            }
        } else {
            SVDEBUG << "Session::updateAlignmentEntriesFor: WARNING: Onsets layer for model " << audioModelId << " lacks onsets model itself" << endl;
        }
    } else {
        SVDEBUG << "Session::updateAlignmentEntriesFor: NOTE: No onsets layer for model " << audioModelId << endl;
        // This is actually fine, it just means the alignment is
        // effectively empty (full of -1s)
    }

    auto &alignmentEntries = m_featureData.at(audioModelId).alignmentEntries;
    alignmentEntries.clear();
    
    for (auto &event : m_musicalEvents) {
        Score::MeasureInfo info = event.measureInfo;
        std::string label = info.toLabel();
        auto itr = labelFrameMap.find(label);
        if (itr == labelFrameMap.end()) {
            // No onset has this musical event's label
            alignmentEntries.push_back(AlignmentEntry(label, -1));
        } else {
            // An onset has this label
            alignmentEntries.push_back(AlignmentEntry(label, itr->second));
        }
    }        

    return true;
}

void
Session::recalculateTempoCurveFor(ModelId audioModel)
{
    if (audioModel.isNone()) return;

    if (m_featureData.find(audioModel) == m_featureData.end()) {
        m_featureData[audioModel] = {
            {},                 // alignmentEntries
            {},                 // tempoModel
            nullptr,            // overviewLayer
            {},                 // lastExportedTo
            false               // alignmentModified
        };
    } else {
        ModelById::release(m_featureData[audioModel].tempoModel);
    }
    
    m_featureData.at(audioModel).alignmentModified = true;

    sv_samplerate_t sampleRate = ModelById::get(audioModel)->getSampleRate();
    auto tempoModel = make_shared<SparseTimeValueModel>(sampleRate, 1);
    ModelId tempoModelId = ModelById::add(tempoModel);
    m_featureData[audioModel].tempoModel = tempoModelId;
    tempoModel->setSourceModel(audioModel);
    
    auto audioPane = getAudioPaneForAudioModel(audioModel);
    if (!audioPane) {
        SVDEBUG << "Session::recalculateTempoCurve: No audio pane for model "
                << audioModel << endl;
        return;
    }
    
    auto onsetsLayer = getOnsetsLayerFromPane
        (audioPane, OnsetsLayerSelection::PermitPendingOnsets);
    if (!onsetsLayer) {
        SVDEBUG << "Session::recalculateTempoCurve: No onsets layer in pane for audio model "
                << audioModel << endl;
        return;
    }

    if (!updateAlignmentEntriesFor(audioModel)) {
        SVDEBUG << "Session::recalculateTempoCurve: Failed to update alignment entries" << endl;
        return;
    }
        
    const auto &alignmentEntries = m_featureData.at(audioModel).alignmentEntries;
    int n = alignmentEntries.size();

    int start = -1, end = -2;
    bool stop = false;
    sv_frame_t prev = -1;
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
                if (prev > 0) {
                    tempoModel->add({ prev + 1, 0.0, QString() });
                    tempoModel->add({ thisFrame - 1, 0.0, QString() });
                    prev = -1;
                }
                double tempo = (4. * dur.numerator / dur.denominator) * 60. / (nextSec - thisSec); // num of quarter notes per minutes
                Event tempoEvent(thisFrame, float(tempo),
                                 QString::fromStdString(alignmentEntries[i].label));
                tempoModel->add(tempoEvent);
            }
            if (i + 1 == end) {
                prev = thisFrame;
            }
        }
    }

    // We must do this after adding all the events, otherwise we get
    // mired in a series of very slow updates from each time an event
    // is added
    if (m_tempoCurveWidget) {
        m_tempoCurveWidget->setCurveForAudio(audioModel, tempoModelId);
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


