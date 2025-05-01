/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Sonic Visualiser
    An audio file viewer and annotation editor.
    Centre for Digital Music, Queen Mary, University of London.
    This file copyright 2006-2007 Chris Cannam and QMUL.
    
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.
*/

#ifndef SV_MAIN_WINDOW_H
#define SV_MAIN_WINDOW_H

#include "framework/MainWindowBase.h"
#include "widgets/LayerTreeDialog.h"

#include "PreferencesDialog.h"
#include "Surveyer.h"

#include "TempoCurveWidget.h"
#include "ScoreWidget.h"
#include "Session.h"
#include "piano-aligner/Score.h"

class QFileSystemWatcher;
class QScrollArea;
class QToolButton;

namespace sv {
class VersionTester;
class ActivityLog;
class UnitConverter;
}

class Score;

class MainWindow : public sv::MainWindowBase
{
    Q_OBJECT

public:
    MainWindow(AudioMode audioMode, MIDIMode midiMode, bool withOSCSupport);
    virtual ~MainWindow();

signals:
    void canChangeSolo(bool);
    void canAlign(bool);
    void canSaveScoreAlignment(bool);
    void canSaveScoreAlignmentAs(bool);
    void canLoadScoreAlignment(bool);
    void canPropagateAlignment(bool);

public slots:
    void preferenceChanged(sv::PropertyContainer::PropertyName) override;
    virtual void coloursChanged();

    virtual bool commitData(bool mayAskUser);

    void goFullScreen();
    void endFullScreen();

protected slots:
    virtual void importAudio();
    virtual void importMoreAudio();
    virtual void replaceMainAudio();
    virtual void openSomething();
    virtual void openLocation();
    virtual void openRecentFile();
    virtual void applyTemplate();
    virtual void exportAudio();
    virtual void exportAudioData();
    virtual void convertAudio();
    virtual void loadScoreAlignment();
    virtual void saveScoreAlignment();
    virtual void saveScoreAlignmentAs();
    virtual void propagateAlignmentFromReference();
    virtual void importLayer();
    virtual void exportLayer();
    virtual void exportImage();
    virtual void exportSVG();
    virtual void browseRecordedAudio();
    virtual void saveSession();
    virtual void saveSessionAs();
    virtual void newSession();
    void closeSession() override;
    virtual void preferences();

    void sampleRateMismatch(sv::sv_samplerate_t, sv::sv_samplerate_t, bool) override;
    void audioOverloadPluginDisabled() override;

    virtual void toolNavigateSelected();
    virtual void toolSelectSelected();
    virtual void toolEditSelected();
    virtual void toolDrawSelected();
    virtual void toolEraseSelected();
    virtual void toolMeasureSelected();

    void documentModified() override;
    void documentRestored() override;
    virtual void documentReplaced();

    void updateMenuStates() override;
    void updateDescriptionLabel() override;
    void updateWindowTitle() override;

    virtual void setInstantsNumbering();
    virtual void setInstantsCounterCycle();
    virtual void setInstantsCounters();
    virtual void resetInstantsCounters();
    virtual void subdivideInstants();
    virtual void winnowInstants();

    void modelGenerationFailed(QString, QString) override;
    void modelGenerationWarning(QString, QString) override;
    void modelRegenerationFailed(QString, QString, QString) override;
    void modelRegenerationWarning(QString, QString, QString) override;
    void alignmentFailed(sv::ModelId, QString) override; // For audio-to-audio

    void paneRightButtonMenuRequested(sv::Pane *, QPoint point) override;
    void panePropertiesRightButtonMenuRequested(sv::Pane *, QPoint point) override;
    void layerPropertiesRightButtonMenuRequested(sv::Pane *, sv::Layer *, QPoint point) override;

    virtual void propertyStacksResized(int);

    virtual void addPane();
    virtual void addLayer();
    virtual void addLayer(QString transformId);
    virtual void renameCurrentLayer();

    virtual void findTransform();

    void paneAdded(sv::Pane *) override;
    void paneHidden(sv::Pane *) override;
    void paneAboutToBeDeleted(sv::Pane *) override;
    void paneDropAccepted(sv::Pane *, QStringList) override;
    void paneDropAccepted(sv::Pane *, QString) override;

    void paneCancelButtonPressed(sv::Layer *);
    void paneDeleteButtonClicked(sv::Pane *) override;

    virtual void setupRecentFilesMenu();
    virtual void setupRecentTransformsMenu();
    virtual void setupTemplatesMenu();

    void openScoreFile(); // from arbitrary MEI file
    void chooseScore(); // from a list of known scores

    void openScoreFile(QString scoreName, QString scoreFile);

    void viewManagerPlaybackFrameChanged(sv::sv_frame_t);
    void scoreInteractionModeChanged(ScoreWidget::InteractionMode);
    void scoreLocationHighlighted(Fraction, ScoreWidget::EventLabel, ScoreWidget::InteractionMode);
    void scoreLocationActivated(Fraction, ScoreWidget::EventLabel, ScoreWidget::InteractionMode);
    void actOnScoreLocation(Fraction, ScoreWidget::EventLabel, ScoreWidget::InteractionMode, bool activated);
    void scoreInteractionEnded(ScoreWidget::InteractionMode);
    void alignmentReadyForReview(sv::Pane *, sv::Layer *);
    void alignmentModified();
    void alignmentAccepted();
    void alignmentRejected();
    void alignmentEventIlluminated(sv::sv_frame_t, QString);
    void alignmentFailedToRun(QString);
    void populateScoreAlignerChoiceMenu();
    void scoreAlignerChosen(sv::TransformId);
    void highlightFrameInScore(sv::sv_frame_t);
    void highlightLabelInScore(QString);
    void highlightLabelInTempoCurve(QString);
    void activateLabelInScore(QString);
    void scoreSelectionChanged(Fraction, bool, ScoreWidget::EventLabel, Fraction, bool, ScoreWidget::EventLabel);
    void scorePageChanged(int page);
    void scorePageDownButtonClicked();
    void scorePageUpButtonClicked();
    void alignButtonClicked();
    void tempoCurveRequestedAudioModelChange(sv::ModelId audioModel);

    virtual void playSpeedChanged(int);
    void playSoloToggled() override;
    virtual void alignToggled();
    void followScoreToggled();

    void currentPaneChanged(sv::Pane *) override;

    virtual void speedUpPlayback();
    virtual void slowDownPlayback();
    virtual void restoreNormalPlayback();

    void monitoringLevelsChanged(float, float) override;

    void layerAdded(sv::Layer *) override;
    void layerRemoved(sv::Layer *) override;
    void layerInAView(sv::Layer *, bool) override;

    void mainModelChanged(sv::ModelId) override;
    virtual void mainModelGainChanged(float);
    virtual void mainModelPanChanged(float);
    void modelAdded(sv::ModelId) override;

    virtual void showLayerTree();
    virtual void showActivityLog();
    virtual void showUnitConverter();

    virtual void mouseEnteredWidget();
    virtual void mouseLeftWidget();

    void handleOSCMessage(const sv::OSCMessage &) override;
    virtual void midiEventsAvailable();
    virtual void playStatusChanged(bool);

    void installedTransformsPopulated();
    void populateTransformsMenu();
    
    virtual void betaReleaseWarning();
    virtual void pluginPopulationWarning(QString text);

    virtual void saveSessionAsTemplate();
    virtual void manageSavedTemplates();

    virtual QString getDefaultSessionTemplate() const override;

    virtual void website();
    virtual void help();
    virtual void introduction();
    virtual void about();
    virtual void whatsNew();
    virtual void keyReference();
    void newerVersionAvailable(QString) override;

protected:
    sv::Overview            *m_overview;
    sv::LevelPanToolButton  *m_mainLevelPan;
    sv::AudioDial           *m_playSpeed;
    QSplitter               *m_tempoCurveSplitter;
    TempoCurveWidget        *m_tempoCurveWidget;
    ScoreWidget             *m_scoreWidget;
    QScrollArea             *m_mainScroll;
    QPushButton             *m_alignButton;
    QPushButton             *m_alignerChoice;
    QWidget                 *m_alignCommands;
    QPushButton             *m_alignAcceptButton;
    QPushButton             *m_alignRejectButton;
    QWidget                 *m_alignAcceptReject;
    QPushButton             *m_scorePageDownButton;
    QPushButton             *m_scorePageUpButton;
    QLabel                  *m_scorePageLabel;
    QPushButton             *m_selectFromButton;
    QLabel                  *m_selectFrom;
    QPushButton             *m_selectToButton;
    QLabel                  *m_selectTo;
    QPushButton             *m_resetSelectionButton;

    bool                     m_mainMenusCreated;
    QMenu                   *m_paneMenu;
    QMenu                   *m_layerMenu;
    QMenu                   *m_transformsMenu;
    QMenu                   *m_playbackMenu;
    QMenu                   *m_existingLayersMenu;
    QMenu                   *m_sliceMenu;
    QMenu                   *m_recentFilesMenu;
    QMenu                   *m_recentTransformsMenu;
    QMenu                   *m_templatesMenu;
    QMenu                   *m_rightButtonMenu;
    QMenu                   *m_rightButtonLayerMenu;
    QMenu                   *m_rightButtonTransformsMenu;
    QMenu                   *m_rightButtonPlaybackMenu;
    QMenu                   *m_lastRightButtonPropertyMenu;

    QAction                 *m_deleteSelectedAction;
    QAction                 *m_soloAction;
    QAction                 *m_rwdStartAction;
    QAction                 *m_rwdSimilarAction;
    QAction                 *m_rwdAction;
    QAction                 *m_ffwdAction;
    QAction                 *m_ffwdSimilarAction;
    QAction                 *m_ffwdEndAction;
    QAction                 *m_playAction;
    QAction                 *m_recordAction;
    QAction                 *m_playSelectionAction;
    QAction                 *m_playLoopAction;
    QAction                 *m_manageTemplatesAction;
    QAction                 *m_zoomInAction;
    QAction                 *m_zoomOutAction;
    QAction                 *m_zoomFitAction;
    QAction                 *m_scrollLeftAction;
    QAction                 *m_scrollRightAction;
    QAction                 *m_showPropertyBoxesAction;
    QAction                 *m_chooseSmartCopyAction;

    bool                     m_soloModified;
    bool                     m_prevSolo;

    QFrame                  *m_playControlsSpacer;
    int                      m_playControlsWidth;

    QLabel                  *m_descriptionLabel;
    QLabel                  *m_currentLabel;

    QPointer<PreferencesDialog> m_preferencesDialog;
    QPointer<sv::LayerTreeDialog>   m_layerTreeDialog;

    sv::ActivityLog         *m_activityLog;
    sv::UnitConverter       *m_unitConverter;
    sv::KeyReference        *m_keyReference;

    QFileSystemWatcher      *m_templateWatcher;

    bool                     m_shouldStartOSCQueue;
    
    Surveyer                *m_surveyer;
    sv::VersionTester       *m_versionTester;
    QString                  m_newerVersionIs;

    QString                  m_scoreId;
    Session                  m_session;
    Score                    m_score;
    bool                     m_followScore;

    class ScoreBasedFrameAligner;
    ScoreBasedFrameAligner  *m_scoreBasedFrameAligner;

    std::vector<std::string> m_scoreFilesToDelete;
    void deleteTemporaryScoreFiles();
    
    struct LayerConfiguration {
        LayerConfiguration(sv::LayerFactory::LayerType _layer
                                               = sv::LayerFactory::TimeRuler,
                           sv::ModelId _source = sv::ModelId(),
                           int _channel = -1) :
            layer(_layer), sourceModel(_source), channel(_channel) { }
        sv::LayerFactory::LayerType layer;
        sv::ModelId sourceModel;
        int channel;
    };

    QString shortcutFor(sv::LayerFactory::LayerType, bool isPaneMenu);
    void updateLayerShortcutsFor(sv::ModelId);

    // Map from menu action to the resulting layer configurations
    // etc. These all used to be std::maps, but we sometimes want to
    // iterate through actions in order of creation, not in order of
    // arbitrary QAction pointer. And speed of random lookup is not
    // important.
    //
    // Some of these would still be fine as maps, but we might as well
    // consistently use the same arrangement throughout.
    
    typedef std::vector<std::pair<QAction *, LayerConfiguration>> PaneActions;
    PaneActions m_paneActions;

    typedef std::vector<std::pair<QAction *, LayerConfiguration>> LayerActions;
    LayerActions m_layerActions;

    typedef std::vector<std::pair<QAction *, sv::Layer *>> ExistingLayerActions;
    ExistingLayerActions m_existingLayerActions;
    ExistingLayerActions m_sliceActions;

    typedef std::vector<std::pair<sv::ViewManager::ToolMode, QAction *>> ToolActions;
    ToolActions m_toolActions;

    typedef std::vector<std::pair<QAction *, int>> NumberingActions;
    NumberingActions m_numberingActions;

    typedef std::vector<std::pair<QAction *, sv::TransformId>> TransformActions;
    TransformActions m_transformActions;

    // This one only makes sense as a map though
    typedef std::map<sv::TransformId, QAction *> TransformActionReverseMap;
    TransformActionReverseMap m_transformActionsReverse;

    QString getReleaseText() const;
    
    void setupMenus() override;

    void setupFileMenu();
    void setupEditMenu();
    void setupViewMenu();
    void setupPaneAndLayerMenus();
    void prepareTransformsMenu();
    void setupHelpMenu();
    void setupExistingLayersMenus();
    void setupToolbars();
    
    void addPane(const LayerConfiguration &configuration, QString text);

    void closeEvent(QCloseEvent *e) override;
    bool checkSaveModified() override;

    void exportAudio(bool asData);

    void updateVisibleRangeDisplay(sv::Pane *p) const override;
    void updatePositionStatusDisplays() const override;

    bool shouldCreateNewSessionForRDFAudio(bool *cancel) override;
    
    void connectLayerEditDialog(sv::ModelDataTableDialog *) override;

    bool m_subsetOfScoreSelected;
    void updateAlignButtonText();
};


#endif
