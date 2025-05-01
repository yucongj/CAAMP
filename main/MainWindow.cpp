/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Sonic Visualiser
    An audio file viewer and annotation editor.
    Centre for Digital Music, Queen Mary, University of London.
    This file copyright 2006-2023 Chris Cannam and QMUL.
    
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.
*/

#include "../version.h"

#include "MainWindow.h"
#include "PreferencesDialog.h"
#include "ScoreWidget.h"
#include "ScoreFinder.h"
#include "ScoreParser.h"
#include "ScoreAlignmentTransform.h"
#include "Session.h"
#include "piano-aligner/Score.h"
#include "align/Align.h"

#include "view/Pane.h"
#include "view/PaneStack.h"
#include "data/model/WaveFileModel.h"
#include "data/model/SparseOneDimensionalModel.h"
#include "data/model/RangeSummarisableTimeValueModel.h"
#include "data/model/NoteModel.h"
#include "data/model/AggregateWaveModel.h"
#include "data/model/Labeller.h"
#include "data/osc/OSCQueue.h"
#include "framework/Document.h"
#include "framework/TransformUserConfigurator.h"
#include "view/ViewManager.h"
#include "base/Preferences.h"
#include "base/ResourceFinder.h"
#include "base/RecordDirectory.h"
#include "layer/WaveformLayer.h"
#include "layer/TimeRulerLayer.h"
#include "layer/TimeInstantLayer.h"
#include "layer/TimeValueLayer.h"
#include "layer/NoteLayer.h"
#include "layer/Colour3DPlotLayer.h"
#include "layer/SliceLayer.h"
#include "layer/SliceableLayer.h"
#include "layer/ImageLayer.h"
#include "layer/RegionLayer.h"
#include "view/Overview.h"
#include "widgets/PropertyBox.h"
#include "widgets/PropertyStack.h"
#include "widgets/AudioDial.h"
#include "widgets/LevelPanWidget.h"
#include "widgets/LevelPanToolButton.h"
#include "widgets/IconLoader.h"
#include "widgets/LayerTreeDialog.h"
#include "widgets/ListInputDialog.h"
#include "widgets/SubdividingMenu.h"
#include "widgets/NotifyingPushButton.h"
#include "widgets/KeyReference.h"
#include "widgets/TransformFinder.h"
#include "widgets/LabelCounterInputDialog.h"
#include "widgets/ActivityLog.h"
#include "widgets/UnitConverter.h"
#include "widgets/ProgressDialog.h"
#include "widgets/CSVAudioFormatDialog.h"
#include "audio/AudioCallbackPlaySource.h"
#include "audio/AudioCallbackRecordTarget.h"
#include "audio/PlaySpeedRangeMapper.h"
#include "data/fileio/AudioFileReaderFactory.h"
#include "data/fileio/DataFileReaderFactory.h"
#include "data/fileio/PlaylistFileReader.h"
#include "data/fileio/WavFileWriter.h"
#include "data/fileio/CSVFileWriter.h"
#include "data/fileio/MIDIFileWriter.h"
#include "data/fileio/BZipFileDevice.h"
#include "data/fileio/FileSource.h"
#include "data/midi/MIDIInput.h"
#include "base/RecentFiles.h"
#include "plugin/PluginScan.h"
#include "transform/TransformFactory.h"
#include "transform/ModelTransformerFactory.h"
#include "base/XmlExportable.h"
#include "widgets/CommandHistory.h"
#include "base/Profiler.h"
#include "base/Clipboard.h"
#include "base/UnitDatabase.h"
#include "layer/ColourDatabase.h"
#include "widgets/ModelDataTableDialog.h"
#include "widgets/CSVExportDialog.h"
#include "widgets/MenuTitle.h"
#include "rdf/PluginRDFIndexer.h"

#include "Surveyer.h"
#include "NetworkPermissionTester.h"
#include "framework/VersionTester.h"

// For version information
#include <vamp/vamp.h>
#include <vamp-hostsdk/PluginBase.h>
#include "plugin/api/ladspa.h"
#include "plugin/api/dssi.h"

#include <bqaudioio/SystemPlaybackTarget.h>
#include <bqaudioio/SystemAudioIO.h>

#include <QApplication>
#include <QMessageBox>
#include <QGridLayout>
#include <QLabel>
#include <QAction>
#include <QMenuBar>
#include <QToolBar>
#include <QInputDialog>
#include <QStatusBar>
#include <QTreeView>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>
#include <QProcess>
#include <QShortcut>
#include <QSettings>
#include <QDateTime>
#include <QProcess>
#include <QCheckBox>
#include <QRegularExpression>
#include <QScrollArea>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QFileSystemWatcher>
#include <QTextEdit>
#include <QWidgetAction>
#include <QGroupBox>
#include <QButtonGroup>
#include <QActionGroup>
#include <QFileDialog>
#include <QDockWidget>
#include <QSplitter>

#include <iostream>
#include <cstdio>
#include <errno.h>

using std::map;
using std::set;
using std::vector;
using std::string;

using namespace sv;

class MainWindow::ScoreBasedFrameAligner : public View::PlaybackFrameAligner {
public:
    ScoreBasedFrameAligner(Session *session) : m_session(session) { }

    sv_frame_t map(const View *forView, sv_frame_t frame) const override
    {
        auto sourcePane = m_session->getPaneContainingOnsetsLayer();
        if (forView == sourcePane) {
            return frame;
        }

        Pane *targetPane =
            const_cast<Pane *>(dynamic_cast<const Pane *>(forView));
        auto targetLayer = m_session->getOnsetsLayerFromPane(targetPane);
        if (!targetLayer) {
            return frame;
        }

        QString label;
        double proportion;

        mapToScoreLabelAndProportion(m_session->getOnsetsLayer(),
                                     frame, label, proportion);

        mapFromScoreLabelAndProportion(targetLayer,
                                       label, proportion, frame);
        
        return frame;
    }

    QString mapToScoreLabel(sv_frame_t frame) const
    {
        TimeInstantLayer *layer = m_session->getOnsetsLayer();
        if (!layer) {
            return {};
        }
        
        QString label;
        double proportion;
        mapToScoreLabelAndProportion(layer, frame, label, proportion);
        return label;
    }

    void mapToScoreLabelAndProportion(Layer *layer,
                                      sv_frame_t frame,
                                      QString &label,
                                      double &proportion) const
    {
        //!!! Much too slow - rework with search & cacheing

        label = "";
        proportion = 0.0;
        if (!layer) {
            return;
        }
        
        ModelId targetId = layer->getModel();
        auto targetModel = ModelById::getAs<SparseOneDimensionalModel>(targetId);
        auto events = targetModel->getAllEvents();
        if (events.empty()) {
            return;
        }
        
        label = events[0].getLabel();
        bool found = false;
        int eventCount = targetModel->getEventCount();
        for (int i = 1; i < eventCount; ++i) {
            sv_frame_t eventFrame = events[i].getFrame();
            if (frame < eventFrame) {
                label = events[i-1].getLabel();
                sv_frame_t priorEventFrame = events[i-1].getFrame();
                if (priorEventFrame < eventFrame) {
                    proportion = double(frame - priorEventFrame) /
                        double(eventFrame - priorEventFrame);
                }
                found = true;
                break;
            } else if (frame == eventFrame) {
                label = events[i].getLabel();
                found = true;
                break;
            }
        }
        if (!found && eventCount > 0) {
            label = events[eventCount-1].getLabel();
        }
    }

    void mapFromScoreLabelAndProportion(Layer *layer,
                                        QString label,
                                        double proportion,
                                        sv_frame_t &frame) const
    {
        //!!! miserably slow (I assume! - measure it - but at any rate
        //!!! this is doing the same query as in
        //!!! mapToScoreLabelAndProportion all over again)

        if (!layer) {
            return; // leave frame unchanged
        }
        
        frame = 0;
        
        ModelId targetId = layer->getModel();
        mapFromScoreLabelAndProportion(targetId, label, proportion, frame);
    }

    void mapFromScoreLabelAndProportion(ModelId targetModelId,
                                        QString label,
                                        double proportion,
                                        sv_frame_t &frame) const
    {
        frame = 0;
        auto targetModel = ModelById::getAs<SparseOneDimensionalModel>(targetModelId);
        if (!targetModel) {
            SVDEBUG << "ERROR: mapFromScoreLabelAndProportion: model is not a SparseOneDimensionalModel" << endl;
            return;
        }
        auto events = targetModel->getAllEvents();
        int eventCount = targetModel->getEventCount();
        bool found = false;
        for (int i = 0; i < eventCount; ++i) {
            if (label == events[i].getLabel()) {
                found = true;
                sv_frame_t eventFrame = events[i].getFrame();
                if (proportion == 0.0 || i + 1 == eventCount) {
                    frame = eventFrame;
                    break;
                } else {
                    frame = sv_frame_t
                        (round(eventFrame + proportion *
                               (events[i+1].getFrame() - eventFrame)));
                    break;
                }
            }
        }
        if (!found) {
            for (int i = 0; i < eventCount; ++i) {
                frame = events[i].getFrame();
                if (labelLessThan(label, events[i].getLabel())) {
                    if (i > 0) {
                        frame = events[i-1].getFrame();
                    }
                    return;
                }
            }
        }
    }
    
private:
    Session *m_session;

    static bool labelLessThan(QString first, QString second) {
        if (first == second) {
            return false;
        }
        static QRegularExpression punct("[^0-9]");
        QStringList firstBits = first.split(punct, Qt::SkipEmptyParts);
        QStringList secondBits = second.split(punct, Qt::SkipEmptyParts);
        if (firstBits.size() < 2 || secondBits.size() < 2) {
            return false;
        }
        int i0 = firstBits[0].toInt(), i1 = secondBits[0].toInt();
        if (i0 > i1) {
            return false;
        } else if (i0 == i1) {
            if (firstBits[1].toInt() >= secondBits[1].toInt()) {
                return false;
            }
        }
        return true;
    }
};

MainWindow::MainWindow(AudioMode audioMode, MIDIMode midiMode, bool withOSCSupport) :
    MainWindowBase(audioMode, midiMode, int(PaneStack::Option::Default)),
    m_overview(nullptr),
    m_mainMenusCreated(false),
    m_paneMenu(nullptr),
    m_layerMenu(nullptr),
    m_transformsMenu(nullptr),
    m_playbackMenu(nullptr),
    m_existingLayersMenu(nullptr),
    m_sliceMenu(nullptr),
    m_recentFilesMenu(nullptr),
    m_recentTransformsMenu(nullptr),
    m_templatesMenu(nullptr),
    m_rightButtonMenu(nullptr),
    m_rightButtonLayerMenu(nullptr),
    m_rightButtonTransformsMenu(nullptr),
    m_rightButtonPlaybackMenu(nullptr),
    m_lastRightButtonPropertyMenu(nullptr),
    m_soloAction(nullptr),
    m_rwdStartAction(nullptr),
    m_rwdSimilarAction(nullptr),
    m_rwdAction(nullptr),
    m_ffwdAction(nullptr),
    m_ffwdSimilarAction(nullptr),
    m_ffwdEndAction(nullptr),
    m_playAction(nullptr),
    m_recordAction(nullptr),
    m_playSelectionAction(nullptr),
    m_playLoopAction(nullptr),
    m_chooseSmartCopyAction(nullptr),
    m_soloModified(false),
    m_prevSolo(false),
    m_playControlsSpacer(nullptr),
    m_playControlsWidth(0),
    m_preferencesDialog(nullptr),
    m_layerTreeDialog(nullptr),
    m_activityLog(new ActivityLog()),
    m_unitConverter(new UnitConverter()),
    m_keyReference(new KeyReference()),
    m_templateWatcher(nullptr),
    m_shouldStartOSCQueue(false),
    m_followScore(true),
    m_scoreBasedFrameAligner(new ScoreBasedFrameAligner(&m_session))
{
    Profiler profiler("MainWindow::MainWindow");

    SVDEBUG << "MainWindow: " << getReleaseText() << endl;

    setWindowTitle(QApplication::applicationName());

    UnitDatabase *udb = UnitDatabase::getInstance();
    udb->registerUnit("");
    udb->registerUnit("Hz");
    udb->registerUnit("dB");
    udb->registerUnit("s");
    udb->registerUnit("V");

    ColourDatabase *cdb = ColourDatabase::getInstance();
    cdb->addColour(Qt::black, tr("Black"));
    cdb->addColour(Qt::darkRed, tr("Red"));
    cdb->addColour(Qt::darkBlue, tr("Blue"));
    cdb->addColour(Qt::darkGreen, tr("Green"));
    cdb->addColour(QColor(200, 50, 255), tr("Purple"));
    cdb->addColour(QColor(255, 150, 50), tr("Orange"));
    cdb->setUseDarkBackground(cdb->addColour(Qt::white, tr("White")), true);
    cdb->setUseDarkBackground(cdb->addColour(Qt::red, tr("Bright Red")), true);
    cdb->setUseDarkBackground(cdb->addColour(QColor(30, 150, 255), tr("Bright Blue")), true);
    cdb->setUseDarkBackground(cdb->addColour(QColor(20, 255, 90), tr("Bright Green")), true);
    cdb->setUseDarkBackground(cdb->addColour(QColor(225, 74, 255), tr("Bright Purple")), true);
    cdb->setUseDarkBackground(cdb->addColour(QColor(255, 188, 80), tr("Bright Orange")), true);

    SVDEBUG << "MainWindow: Creating main user interface layout" << endl;

    // For Performance Precision, we want to constrain playback to selection
    // by default, and we don't want to play multiple recordings at once
    m_viewManager->setPlaySelectionMode(true);
    m_viewManager->setPlaySoloMode(true);

    QFrame *frame = new QFrame;
    setCentralWidget(frame);

    QGridLayout *layout = new QGridLayout;

    m_descriptionLabel = new QLabel;

    QDockWidget *scoreWidgetDock = new QDockWidget(this);
    scoreWidgetDock->setAllowedAreas(Qt::LeftDockWidgetArea |
                                     Qt::RightDockWidgetArea);
    scoreWidgetDock->setFeatures(QDockWidget::DockWidgetMovable |
                                 QDockWidget::DockWidgetFloatable);
    scoreWidgetDock->setWindowTitle(tr("Score"));
    
    QWidget *scoreWidgetContainer = new QWidget(scoreWidgetDock);
    
    QGridLayout *scoreWidgetLayout = new QGridLayout;

    m_scoreWidget = new ScoreWidget(true, scoreWidgetContainer);
    m_scoreWidget->setInteractionMode(ScoreWidget::InteractionMode::Navigate);
    connect(m_scoreWidget, &ScoreWidget::scoreLocationHighlighted,
            this, &MainWindow::scoreLocationHighlighted);
    connect(m_scoreWidget, &ScoreWidget::scoreLocationActivated,
            this, &MainWindow::scoreLocationActivated);
    connect(m_scoreWidget, &ScoreWidget::interactionModeChanged,
            this, &MainWindow::scoreInteractionModeChanged);
    connect(m_scoreWidget, &ScoreWidget::interactionEnded,
            this, &MainWindow::scoreInteractionEnded);
    connect(m_scoreWidget, &ScoreWidget::selectionChanged,
            this, &MainWindow::scoreSelectionChanged);
    connect(m_scoreWidget, &ScoreWidget::pageChanged,
            this, &MainWindow::scorePageChanged);

    int alignButtonWidth = 50 + QFontMetrics(font()).horizontalAdvance
        (tr("Align Selection of Score with All of Audio"));
    m_alignButton = new QPushButton();
    m_alignButton->setIcon(IconLoader().load("align"));
    m_alignButton->setMinimumWidth(alignButtonWidth);
    connect(m_alignButton, SIGNAL(clicked()),
            this, SLOT(alignButtonClicked()));
    connect(this, SIGNAL(canAlign(bool)),
            m_alignButton, SLOT(setEnabled(bool)));
    m_alignButton->setEnabled(false);
    m_subsetOfScoreSelected = false;

    m_alignerChoice = new QPushButton();
    QChar dot(0x00b7);
    m_alignerChoice->setText(QString("%1%2%3").arg(dot).arg(dot).arg(dot));
    
    m_alignCommands = new QWidget;
    QHBoxLayout *aclayout = new QHBoxLayout;
    aclayout->addWidget(m_alignButton);
    aclayout->addWidget(m_alignerChoice);
    aclayout->setContentsMargins(0, 2, 0, 0);
    aclayout->setSpacing(3);
    m_alignCommands->setLayout(aclayout);
    
    m_alignAcceptButton = new QPushButton
        (IconLoader().load("dataaccept"), tr("Accept Alignment"));
    connect(m_alignAcceptButton, SIGNAL(clicked()),
            &m_session, SLOT(acceptAlignment()));

    m_alignRejectButton = new QPushButton
        (IconLoader().load("datadelete"), tr("Reject Alignment"));
    connect(m_alignRejectButton, SIGNAL(clicked()),
            &m_session, SLOT(rejectAlignment()));

    m_alignAcceptReject = new QWidget;
    QHBoxLayout *aalayout = new QHBoxLayout;
    aalayout->addWidget(m_alignAcceptButton);
    aalayout->addWidget(m_alignRejectButton);
    aalayout->setContentsMargins(0, 0, 0, 0);
    aalayout->setSpacing(3);
    m_alignAcceptReject->setLayout(aalayout);
    
    m_scorePageDownButton = new QPushButton("<<");
    connect(m_scorePageDownButton, SIGNAL(clicked()),
            this, SLOT(scorePageDownButtonClicked()));
    m_scorePageUpButton = new QPushButton(">>");
    connect(m_scorePageUpButton, SIGNAL(clicked()),
            this, SLOT(scorePageUpButtonClicked()));
    m_scorePageLabel = new QLabel(tr("Page"));
    m_scorePageLabel->setAlignment(Qt::AlignHCenter);

    scoreWidgetLayout->addWidget(m_scoreWidget, 0, 0, 1, 3);
    scoreWidgetLayout->setRowStretch(0, 10);
    
    scoreWidgetLayout->addWidget(m_alignCommands, 1, 0, 1, 3, Qt::AlignHCenter);
    scoreWidgetLayout->addWidget(m_alignAcceptReject, 1, 0, 1, 3, Qt::AlignHCenter);
    m_alignAcceptReject->hide();
    scoreWidgetLayout->addWidget(m_scorePageDownButton, 2, 0);
    scoreWidgetLayout->addWidget(m_scorePageLabel, 2, 1, Qt::AlignCenter);
    scoreWidgetLayout->addWidget(m_scorePageUpButton, 2, 2);

    QGroupBox *selectionGroupBox = new QGroupBox(tr("Selection within Score"));
    QGridLayout *selectionLayout = new QGridLayout;

    QButtonGroup *selectGroup = new QButtonGroup;
    selectGroup->setExclusive(false); // want to allow nothing to be checked
    
    QLabel *selectFromLabel = new QLabel(tr("From:"));
    m_selectFrom = new QLabel(tr("Start"));
    m_selectFromButton = new QPushButton(tr("Choose"));
    m_selectFromButton->setCheckable(true);
    selectGroup->addButton(m_selectFromButton);

    QLabel *selectToLabel = new QLabel(tr("To:"));
    m_selectTo = new QLabel(tr("End"));
    m_selectToButton = new QPushButton(tr("Choose"));
    m_selectToButton->setCheckable(true);
    selectGroup->addButton(m_selectToButton);

    connect(m_selectFromButton, &QPushButton::toggled, [this] (bool checked) {
        SVDEBUG << "selectFromButton toggled: checked = " << checked << endl;
        if (checked) {
            m_scoreWidget->setInteractionMode(ScoreWidget::InteractionMode::SelectStart);
        } else {
            m_scoreWidget->setInteractionMode(ScoreWidget::InteractionMode::Navigate);
        }
    });
    connect(m_selectToButton, &QPushButton::toggled, [this] (bool checked) {
        SVDEBUG << "m_selectToButton toggled: checked = " << checked << endl;
        if (checked) {
            m_scoreWidget->setInteractionMode(ScoreWidget::InteractionMode::SelectEnd);
        } else {
            m_scoreWidget->setInteractionMode(ScoreWidget::InteractionMode::Navigate);
        }
    });

    m_resetSelectionButton = new QPushButton(tr("Reset"));
    connect(m_resetSelectionButton, SIGNAL(clicked()),
            m_scoreWidget, SLOT(clearSelection()));
    m_resetSelectionButton->setEnabled(false);
    
    selectionLayout->addWidget(new QLabel(" "), 0, 0);
    selectionLayout->addWidget(selectFromLabel, 0, 1, Qt::AlignRight);
    selectionLayout->addWidget(m_selectFromButton, 0, 2);
    selectionLayout->addWidget(m_selectFrom, 0, 3);
    selectionLayout->addWidget(selectToLabel, 1, 1, Qt::AlignRight);
    selectionLayout->addWidget(m_selectToButton, 1, 2);
    selectionLayout->addWidget(m_selectTo, 1, 3);
    selectionLayout->addWidget(m_resetSelectionButton, 1, 4);
    selectionLayout->setColumnStretch(3, 10);

    selectionGroupBox->setLayout(selectionLayout);

    scoreWidgetLayout->addWidget(selectionGroupBox, 3, 0, 1, 3);

    scoreWidgetContainer->setLayout(scoreWidgetLayout);
    scoreWidgetDock->setWidget(scoreWidgetContainer);
    addDockWidget(Qt::LeftDockWidgetArea, scoreWidgetDock);

    m_mainScroll = new QScrollArea(frame);
    m_mainScroll->setWidgetResizable(true);
    m_mainScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_mainScroll->setFrameShape(QFrame::NoFrame);

    m_mainScroll->setWidget(m_paneStack);

    m_tempoCurveWidget = new TempoCurveWidget(frame);
    connect(m_tempoCurveWidget, &TempoCurveWidget::changeCurrentAudioModel,
            this, &MainWindow::tempoCurveRequestedAudioModelChange);
    connect(m_tempoCurveWidget, &TempoCurveWidget::highlightLabel,
            this, &MainWindow::highlightLabelInScore);
    connect(m_tempoCurveWidget, &TempoCurveWidget::activateLabel,
            this, &MainWindow::activateLabelInScore);
    
    m_overview = new Overview(frame);
    m_overview->setViewManager(m_viewManager);
    int overviewHeight = m_viewManager->scalePixelSize(35);
    if (overviewHeight < 40) overviewHeight = 40;
    m_overview->setFixedHeight(overviewHeight);
#ifndef _WIN32
    // For some reason, the contents of the overview never appear if we
    // make this setting on Windows.  I have no inclination at the moment
    // to track down the reason why.
    m_overview->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
#endif
    connect(m_overview, SIGNAL(contextHelpChanged(const QString &)),
            this, SLOT(contextHelpChanged(const QString &)));

    coloursChanged(); // sets pan layer colour from preferences

    m_playSpeed = new AudioDial(frame);
    m_playSpeed->setMinimum(0);
    m_playSpeed->setMaximum(120);
    m_playSpeed->setValue(60);
    m_playSpeed->setFixedWidth(overviewHeight);
    m_playSpeed->setFixedHeight(overviewHeight);
    m_playSpeed->setNotchesVisible(true);
    m_playSpeed->setPageStep(10);
    m_playSpeed->setObjectName(tr("Playback Speed"));
    m_playSpeed->setRangeMapper(new PlaySpeedRangeMapper);
    m_playSpeed->setDefaultValue(60);
    m_playSpeed->setShowToolTip(true);
    connect(m_playSpeed, SIGNAL(valueChanged(int)),
            this, SLOT(playSpeedChanged(int)));
    connect(m_playSpeed, SIGNAL(mouseEntered()), this, SLOT(mouseEnteredWidget()));
    connect(m_playSpeed, SIGNAL(mouseLeft()), this, SLOT(mouseLeftWidget()));

    m_mainLevelPan = new LevelPanToolButton(frame);
    connect(m_mainLevelPan, SIGNAL(mouseEntered()), this, SLOT(mouseEnteredWidget()));
    connect(m_mainLevelPan, SIGNAL(mouseLeft()), this, SLOT(mouseLeftWidget()));
    m_mainLevelPan->setFixedHeight(overviewHeight);
    m_mainLevelPan->setFixedWidth(overviewHeight);
    m_mainLevelPan->setImageSize((overviewHeight * 3) / 4);
    m_mainLevelPan->setBigImageSize(overviewHeight * 3);

    layout->setSpacing(m_viewManager->scalePixelSize(4));

    m_tempoCurveSplitter = new QSplitter;
    m_tempoCurveSplitter->setOrientation(Qt::Vertical);
    m_tempoCurveSplitter->addWidget(m_mainScroll);
    m_tempoCurveSplitter->addWidget(m_tempoCurveWidget);
    m_tempoCurveSplitter->setSizes({ 120, 80 });
    layout->addWidget(m_tempoCurveSplitter, 0, 0, 1, 3);
    
    layout->addWidget(m_overview, 1, 0);
    layout->addWidget(m_playSpeed, 1, 1);
    layout->addWidget(m_mainLevelPan, 1, 2);

    m_playControlsWidth = 
        m_mainLevelPan->width() + m_playSpeed->width() + layout->spacing() * 2;

    m_paneStack->setPropertyStackMinWidth(m_playControlsWidth
                                          + 2 + layout->spacing());

    layout->setColumnStretch(0, 10);

    connect(m_paneStack, SIGNAL(propertyStacksResized(int)),
            this, SLOT(propertyStacksResized(int)));

    frame->setLayout(layout);

    SVDEBUG << "MainWindow: Creating menus and toolbars" << endl;

#ifdef Q_OS_MAC
    // Mac doesn't align menu labels when icons are shown: result is messy
    QApplication::setAttribute(Qt::AA_DontShowIconsInMenus);
    setIconsVisibleInMenus(false);
#endif

    setupMenus();
    setupToolbars();
    setupHelpMenu();

    statusBar();
    m_currentLabel = new QLabel;
    statusBar()->addPermanentWidget(m_currentLabel);

    finaliseMenus();

    connect(m_viewManager, SIGNAL(activity(QString)),
            m_activityLog, SLOT(activityHappened(QString)));
    connect(m_playSource, SIGNAL(activity(QString)),
            m_activityLog, SLOT(activityHappened(QString)));
    connect(CommandHistory::getInstance(), SIGNAL(activity(QString)),
            m_activityLog, SLOT(activityHappened(QString)));
    connect(this, SIGNAL(activity(QString)),
            m_activityLog, SLOT(activityHappened(QString)));
    connect(this, SIGNAL(replacedDocument()), this, SLOT(documentReplaced()));

    m_activityLog->hide();

    m_unitConverter->hide();

    setAudioRecordMode(RecordCreateAdditionalModel);

    SVDEBUG << "MainWindow: Creating new session" << endl;

    newSession();

    connect(m_midiInput, SIGNAL(eventsAvailable()),
            this, SLOT(midiEventsAvailable()));

    SVDEBUG << "MainWindow: Creating network permission tester" << endl;

    NetworkPermissionTester tester(withOSCSupport);
    bool networkPermission = tester.havePermission();
    if (networkPermission) {
        SVDEBUG << "MainWindow: Starting uninstalled-transform population thread" << endl;
        TransformFactory::getInstance()->startPopulatingUninstalledTransforms();

        m_surveyer = nullptr;

        SVDEBUG << "MainWindow: Creating version tester" << endl;
        m_versionTester = new VersionTester
            ("sonicvisualiser.org", "latest-pp-version.txt", SV_VERSION);
        connect(m_versionTester, SIGNAL(newerVersionAvailable(QString)),
                this, SLOT(newerVersionAvailable(QString)));
    } else {
        m_surveyer = nullptr;
        m_versionTester = nullptr;
    }

    m_shouldStartOSCQueue = (withOSCSupport && networkPermission);
    
    if (QString(SV_VERSION).contains("-")) {
        QTimer::singleShot(500, this, SLOT(betaReleaseWarning()));
    }

    connect(m_viewManager, SIGNAL(playbackFrameChanged(sv_frame_t)),
            this, SLOT(viewManagerPlaybackFrameChanged(sv_frame_t)));
    
    m_showPropertyBoxesAction->trigger();

    connect(&m_session, &Session::alignmentReadyForReview,
            this, &MainWindow::alignmentReadyForReview);
    connect(&m_session, &Session::alignmentModified,
            this, &MainWindow::alignmentModified);
    connect(&m_session, &Session::alignmentAccepted,
            this, &MainWindow::alignmentAccepted);
    connect(&m_session, &Session::alignmentRejected,
            this, &MainWindow::alignmentRejected);
    connect(&m_session, &Session::alignmentEventIlluminated,
            this, &MainWindow::alignmentEventIlluminated);
    connect(&m_session, &Session::alignmentFailedToRun,
            this, &MainWindow::alignmentFailedToRun);

    QTimer::singleShot(250, this, &MainWindow::introduction);

    SVDEBUG << "MainWindow: Constructor done" << endl;
}

MainWindow::~MainWindow()
{
//    SVDEBUG << "MainWindow::~MainWindow" << endl;

    delete m_scoreBasedFrameAligner;
    
    deleteTemporaryScoreFiles();

    delete m_keyReference;
    delete m_activityLog;
    delete m_unitConverter;
    delete m_preferencesDialog;
    delete m_layerTreeDialog;
    delete m_versionTester;
    delete m_surveyer;
//    SVDEBUG << "MainWindow::~MainWindow finishing" << endl;
}

void
MainWindow::setupMenus()
{
    SVDEBUG << "MainWindow::setupMenus" << endl;
    
    if (!m_mainMenusCreated) {

#ifdef Q_OS_LINUX
        // In Ubuntu 14.04 the window's menu bar goes missing entirely
        // if the user is running any desktop environment other than Unity
        // (in which the faux single-menubar appears). The user has a
        // workaround, to remove the appmenu-qt5 package, but that is
        // awkward and the problem is so severe that it merits disabling
        // the system menubar integration altogether. Like this:
        menuBar()->setNativeMenuBar(false);  // fix #1039
#endif
        
        m_rightButtonMenu = new QMenu();

        // We don't want tear-off enabled on the right-button menu.
        // If it is enabled, then simply right-clicking and releasing
        // will pop up the menu, activate the tear-off, and leave the
        // torn-off menu window in front of the main window.  That
        // isn't desirable.
        m_rightButtonMenu->setTearOffEnabled(false);
    }

    if (m_rightButtonTransformsMenu) {
        m_rightButtonTransformsMenu->clear();
    } else {
        m_rightButtonTransformsMenu = m_rightButtonMenu->addMenu(tr("&Transform"));
        m_rightButtonTransformsMenu->setTearOffEnabled(true);
        m_rightButtonMenu->addSeparator();
    }

    // This will be created (if not found) or cleared (if found) in
    // setupPaneAndLayerMenus, but we want to ensure it's created
    // sooner so it can go nearer the top of the right button menu
    if (m_rightButtonLayerMenu) {
        m_rightButtonLayerMenu->clear();
    } else {
        m_rightButtonLayerMenu = m_rightButtonMenu->addMenu(tr("&Layer"));
        m_rightButtonLayerMenu->setTearOffEnabled(true);
        m_rightButtonMenu->addSeparator();
    }

    if (!m_mainMenusCreated) {
        CommandHistory::getInstance()->registerMenu(m_rightButtonMenu);
        m_rightButtonMenu->addSeparator();
    }

    setupFileMenu();
    setupEditMenu();
    setupViewMenu();
    setupPaneAndLayerMenus();
    prepareTransformsMenu();

    m_mainMenusCreated = true;

    SVDEBUG << "MainWindow::setupMenus: done" << endl;
}

void
MainWindow::goFullScreen()
{
    if (m_viewManager->getZoomWheelsEnabled()) {
        // The wheels seem to end up in the wrong place in full-screen mode
        toggleZoomWheels();
    }

    QWidget *ps = m_mainScroll->takeWidget();
    ps->setParent(nullptr);

    QShortcut *sc;

    sc = new QShortcut(QKeySequence("Esc"), ps);
    connect(sc, SIGNAL(activated()), this, SLOT(endFullScreen()));

    sc = new QShortcut(QKeySequence("F11"), ps);
    connect(sc, SIGNAL(activated()), this, SLOT(endFullScreen()));

    QAction *acts[] = {
        m_playAction, m_zoomInAction, m_zoomOutAction, m_zoomFitAction,
        m_scrollLeftAction, m_scrollRightAction, m_showPropertyBoxesAction
    };

    for (int i = 0; i < int(sizeof(acts)/sizeof(acts[0])); ++i) {
        sc = new QShortcut(acts[i]->shortcut(), ps);
        connect(sc, SIGNAL(activated()), acts[i], SLOT(trigger()));
    }

    ps->showFullScreen();
}

void
MainWindow::endFullScreen()
{
    // these were only created in goFullScreen:
    QObjectList cl = m_paneStack->children();
    for (QObject *o : cl) {
        QShortcut *sc = qobject_cast<QShortcut *>(o);
        if (sc) delete sc;
    }

    m_paneStack->showNormal();
    m_mainScroll->setWidget(m_paneStack);
}

void
MainWindow::setupFileMenu()
{
    SVDEBUG << "MainWindow::setupFileMenu" << endl;
    
    if (m_mainMenusCreated) return;

    QMenu *menu = menuBar()->addMenu(tr("&File"));
    menu->setTearOffEnabled(true);
    QToolBar *toolbar = addToolBar(tr("File Toolbar"));

    m_keyReference->setCategory(tr("File and Session Management"));

    IconLoader il;

    QIcon icon;
    QAction *action = nullptr;
/*!!!    
    QIcon icon = il.load("filenew");
    QAction *action = new QAction(icon, tr("&New Session"), this);
    action->setShortcut(tr("Ctrl+N"));
    action->setStatusTip(tr("Abandon the current %1 session and start a new one").arg(QApplication::applicationName()));
    connect(action, SIGNAL(triggered()), this, SLOT(newSession()));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
    toolbar->addAction(action);
*/
    // Added by YJ: Ocs 5, 2021
    icon = il.load("chooseScore");
    action = new QAction(icon, tr("&Choose Score..."), this);
    action->setShortcut(tr("Ctrl+E"));
    action->setStatusTip(tr("Choose a new score"));
    connect(action, SIGNAL(triggered()), this, SLOT(openScoreFile()));
    // m_keyReference->registerShortcut(action);
    menu->addAction(action);
    toolbar->addAction(action);
    // end of YJ

/*!!!
    icon = il.load("fileopen");
    action = new QAction(icon, tr("&Open..."), this);
    action->setShortcut(tr("Ctrl+O"));
    action->setStatusTip(tr("Open a session file, audio file, or layer"));
    connect(action, SIGNAL(triggered()), this, SLOT(openSomething()));
    m_keyReference->registerShortcut(action);
    toolbar->addAction(action);
    menu->addAction(action);
*/

    icon = il.load("fileopenaudio");
    action = new QAction(icon, tr("&Open Recording..."), this);
    action->setShortcut(tr("Ctrl+O"));
    action->setStatusTip(tr("Open an audio recording"));
    connect(action, SIGNAL(triggered()), this, SLOT(importAudio()));
    m_keyReference->registerShortcut(action);
    toolbar->addAction(action);
    menu->addAction(action);

    icon = il.load("fileopenmoreaudio");
    QAction *iaction = new QAction(icon, tr("Open Another Recording..."), this);
    iaction->setShortcut(tr("Ctrl+I"));
    iaction->setStatusTip(tr("Import an extra audio file into a new pane"));
    connect(iaction, SIGNAL(triggered()), this, SLOT(importMoreAudio()));
    connect(this, SIGNAL(canImportMoreAudio(bool)), iaction, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(iaction);
    toolbar->addAction(iaction);
    menu->addAction(iaction);
    
/*!!!
    // We want this one to go on the toolbar now, if we add it at all,
    // but on the menu later
    QAction *raction = new QAction(tr("Replace &Main Audio..."), this);
    raction->setStatusTip(tr("Replace the main audio file of the session with a different file"));
    connect(raction, SIGNAL(triggered()), this, SLOT(replaceMainAudio()));
    connect(this, SIGNAL(canReplaceMainAudio(bool)), raction, SLOT(setEnabled(bool)));
*/
    action = new QAction(tr("Open Lo&cation..."), this);
    action->setShortcut(tr("Ctrl+Shift+O"));
    action->setStatusTip(tr("Open or import a file from a remote URL"));
    connect(action, SIGNAL(triggered()), this, SLOT(openLocation()));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    m_recentFilesMenu = menu->addMenu(tr("Open &Recent"));
    m_recentFilesMenu->setTearOffEnabled(true);
    setupRecentFilesMenu();
    connect(&m_recentFiles, SIGNAL(recentChanged()),
            this, SLOT(setupRecentFilesMenu()));

    menu->addSeparator();
/*!!!
    icon = il.load("filesave");
    action = new QAction(icon, tr("&Save Session"), this);
    action->setShortcut(tr("Ctrl+S"));
    action->setStatusTip(tr("Save the current session into a %1 session file").arg(QApplication::applicationName()));
    connect(action, SIGNAL(triggered()), this, SLOT(saveSession()));
    connect(this, SIGNAL(canSave(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
    toolbar->addAction(action);
        
    icon = il.load("filesaveas");
    action = new QAction(icon, tr("Save Session &As..."), this);
    action->setShortcut(tr("Ctrl+Shift+S"));
    action->setStatusTip(tr("Save the current session into a new %1 session file").arg(QApplication::applicationName()));
    connect(action, SIGNAL(triggered()), this, SLOT(saveSessionAs()));
    menu->addAction(action);
    toolbar->addAction(action);

    menu->addSeparator();

    // the Replace action we made earlier
    menu->addAction(raction);

    // the Import action we made earlier
    menu->addAction(iaction);

    action = new QAction(tr("&Export Audio File..."), this);
    action->setStatusTip(tr("Export selection as an audio file"));
    connect(action, SIGNAL(triggered()), this, SLOT(exportAudio()));
    connect(this, SIGNAL(canExportAudio(bool)), action, SLOT(setEnabled(bool)));
    menu->addAction(action);

    menu->addSeparator();
*/
    icon = il.load("fileopen");
    action = new QAction(icon, tr("Load Score Alignment..."), this);
    action->setStatusTip(tr("Import score alignment data from a previously-saved file"));
    connect(action, SIGNAL(triggered()), this, SLOT(loadScoreAlignment()));
    connect(this, SIGNAL(canLoadScoreAlignment(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    toolbar->addAction(action);
    menu->addAction(action);

    icon = il.load("filesave");
    action = new QAction(icon, tr("Save Score Alignment"), this);
    action->setStatusTip(tr("Save modified score alignment data to the same file as previously"));
    connect(action, SIGNAL(triggered()), this, SLOT(saveScoreAlignment()));
    connect(this, SIGNAL(canSaveScoreAlignment(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    toolbar->addAction(action);
    menu->addAction(action);

    icon = il.load("filesaveas");
    action = new QAction(icon, tr("Save Score Alignment As..."), this);
    action->setStatusTip(tr("Save score alignment data to a new file"));
    connect(action, SIGNAL(triggered()), this, SLOT(saveScoreAlignmentAs()));
    connect(this, SIGNAL(canSaveScoreAlignmentAs(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    toolbar->addAction(action);
    menu->addAction(action);

    menu->addSeparator();

    action = new QAction(tr("Import Annotation &Layer..."), this);
    action->setShortcut(tr("Ctrl+L"));
    action->setStatusTip(tr("Import layer data from an existing file"));
    connect(action, SIGNAL(triggered()), this, SLOT(importLayer()));
    connect(this, SIGNAL(canImportLayer(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    action = new QAction(tr("Export Annotation La&yer..."), this);
    action->setShortcut(tr("Ctrl+Y"));
    action->setStatusTip(tr("Export layer data to a file"));
    connect(action, SIGNAL(triggered()), this, SLOT(exportLayer()));
    connect(this, SIGNAL(canExportLayer(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
    
    menu->addSeparator();
    
    action = new QAction(tr("Convert Audio from Data File..."), this);
    action->setStatusTip(tr("Convert and import audio sample values from a CSV data file"));
    connect(action, SIGNAL(triggered()), this, SLOT(convertAudio()));
    menu->addAction(action);
    
    action = new QAction(tr("Export Audio to Data File..."), this);
    action->setStatusTip(tr("Export audio from selection into a CSV data file"));
    connect(action, SIGNAL(triggered()), this, SLOT(exportAudioData()));
    connect(this, SIGNAL(canExportAudio(bool)), action, SLOT(setEnabled(bool)));
    menu->addAction(action);

    menu->addSeparator();

    action = new QAction(tr("Export Image File..."), this);
    action->setStatusTip(tr("Export a single pane to an image file"));
    connect(action, SIGNAL(triggered()), this, SLOT(exportImage()));
    connect(this, SIGNAL(canExportImage(bool)), action, SLOT(setEnabled(bool)));
    menu->addAction(action);

    action = new QAction(tr("Export SVG File..."), this);
    action->setStatusTip(tr("Export a single pane to a scalable SVG image file"));
    connect(action, SIGNAL(triggered()), this, SLOT(exportSVG()));
    connect(this, SIGNAL(canExportImage(bool)), action, SLOT(setEnabled(bool)));
    menu->addAction(action);

    menu->addSeparator();

    action = new QAction(tr("Browse Recorded and Converted Audio"), this);
    action->setStatusTip(tr("Open the Recorded Audio folder in the system file browser"));
    connect(action, SIGNAL(triggered()), this, SLOT(browseRecordedAudio()));
    menu->addAction(action);

    menu->addSeparator();

    QString templatesMenuLabel = tr("Apply Session Template");
    m_templatesMenu = menu->addMenu(templatesMenuLabel);
    m_templatesMenu->setTearOffEnabled(true);
    // Formerly we enabled or disabled this menu according to whether
    // we had a main model or not, since applying a template requires
    // a main model. But the menu also contains the option to set the
    // default, and that's useful regardless, so we now have the menu
    // always enabled

    // Set up the menu in a moment, after m_manageTemplatesAction constructed

    action = new QAction(tr("Export Session as Template..."), this);
    connect(action, SIGNAL(triggered()), this, SLOT(saveSessionAsTemplate()));
    // We need to have something in the session for this to be useful:
    // canDeleteCurrentLayer captures that
    connect(this, SIGNAL(canExportAudio(bool)), action, SLOT(setEnabled(bool)));
    menu->addAction(action);

    m_manageTemplatesAction = new QAction(tr("Manage Exported Templates"), this);
    connect(m_manageTemplatesAction, SIGNAL(triggered()), this, SLOT(manageSavedTemplates()));
    menu->addAction(m_manageTemplatesAction);

    setupTemplatesMenu();

    action = new QAction(tr("&Preferences..."), this);
    action->setStatusTip(tr("Adjust the application preferences"));
    connect(action, SIGNAL(triggered()), this, SLOT(preferences()));
    menu->addAction(action);
        
    menu->addSeparator();
    action = new QAction(il.load("exit"), tr("&Quit"), this);
    action->setShortcut(tr("Ctrl+Q"));
    action->setStatusTip(tr("Exit %1").arg(QApplication::applicationName()));
    connect(action, SIGNAL(triggered()), qApp, SLOT(closeAllWindows()));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
}

void
MainWindow::setupEditMenu()
{
    SVDEBUG << "MainWindow::setupEditMenu" << endl;
    
    if (m_mainMenusCreated) return;

    QMenu *menu = menuBar()->addMenu(tr("&Edit"));
    menu->setTearOffEnabled(true);
    CommandHistory::getInstance()->registerMenu(menu);

    m_keyReference->setCategory(tr("Editing"));

    menu->addSeparator();

    IconLoader il;

    QAction *action = new QAction(il.load("editcut"),
                                  tr("Cu&t"), this);
    action->setShortcut(tr("Ctrl+X"));
    action->setStatusTip(tr("Cut the selection from the current layer to the clipboard"));
    connect(action, SIGNAL(triggered()), this, SLOT(cut()));
    connect(this, SIGNAL(canEditSelection(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
    m_rightButtonMenu->addAction(action);

    action = new QAction(il.load("editcopy"),
                         tr("&Copy"), this);
    action->setShortcut(tr("Ctrl+C"));
    action->setStatusTip(tr("Copy the selection from the current layer to the clipboard"));
    connect(action, SIGNAL(triggered()), this, SLOT(copy()));
    connect(this, SIGNAL(canEditSelection(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
    m_rightButtonMenu->addAction(action);

    action = new QAction(il.load("editpaste"),
                         tr("&Paste"), this);
    action->setShortcut(tr("Ctrl+V"));
    action->setStatusTip(tr("Paste from the clipboard to the current layer"));
    connect(action, SIGNAL(triggered()), this, SLOT(paste()));
    connect(this, SIGNAL(canPaste(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
    m_rightButtonMenu->addAction(action);

    action = new QAction(tr("Paste at Playback Position"), this);
    action->setShortcut(tr("Ctrl+Shift+V"));
    action->setStatusTip(tr("Paste from the clipboard to the current layer, placing the first item at the playback position"));
    connect(action, SIGNAL(triggered()), this, SLOT(pasteAtPlaybackPosition()));
    connect(this, SIGNAL(canPaste(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
    m_rightButtonMenu->addAction(action);

    m_deleteSelectedAction = new QAction(tr("&Delete Selected Items"), this);
    m_deleteSelectedAction->setShortcut(tr("Del"));
    m_deleteSelectedAction->setStatusTip(tr("Delete items in current selection from the current layer"));
    connect(m_deleteSelectedAction, SIGNAL(triggered()), this, SLOT(deleteSelected()));
    connect(this, SIGNAL(canDeleteSelection(bool)), m_deleteSelectedAction, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(m_deleteSelectedAction);
    menu->addAction(m_deleteSelectedAction);
    m_rightButtonMenu->addAction(m_deleteSelectedAction);

    menu->addSeparator();
    m_rightButtonMenu->addSeparator();

    m_keyReference->setCategory(tr("Selection"));

    action = new QAction(tr("Select &All"), this);
    action->setShortcut(tr("Ctrl+A"));
    action->setStatusTip(tr("Select the whole duration of the current session"));
    connect(action, SIGNAL(triggered()), this, SLOT(selectAll()));
    connect(this, SIGNAL(canSelect(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
    m_rightButtonMenu->addAction(action);
        
    action = new QAction(tr("Select &Visible Range"), this);
    action->setShortcut(tr("Ctrl+Shift+A"));
    action->setStatusTip(tr("Select the time range corresponding to the current window width"));
    connect(action, SIGNAL(triggered()), this, SLOT(selectVisible()));
    connect(this, SIGNAL(canSelect(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
        
    action = new QAction(tr("Select to &Start"), this);
    action->setShortcut(tr("Shift+Left"));
    action->setStatusTip(tr("Select from the start of the session to the current playback position"));
    connect(action, SIGNAL(triggered()), this, SLOT(selectToStart()));
    connect(this, SIGNAL(canSelect(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
        
    action = new QAction(tr("Select to &End"), this);
    action->setShortcut(tr("Shift+Right"));
    action->setStatusTip(tr("Select from the current playback position to the end of the session"));
    connect(action, SIGNAL(triggered()), this, SLOT(selectToEnd()));
    connect(this, SIGNAL(canSelect(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    action = new QAction(tr("C&lear Selection"), this);
    action->setShortcut(tr("Esc"));
    action->setStatusTip(tr("Clear the selection"));
    connect(action, SIGNAL(triggered()), this, SLOT(clearSelection()));
    connect(this, SIGNAL(canClearSelection(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
    m_rightButtonMenu->addAction(action);

    menu->addSeparator();

    m_keyReference->setCategory(tr("Tapping Time Instants"));

    action = new QAction(tr("&Insert Instant at Playback Position"), this);
    action->setShortcut(tr(";"));
    action->setStatusTip(tr("Insert a new time instant at the current playback position, in a new layer if necessary"));
    connect(action, SIGNAL(triggered()), this, SLOT(insertInstant()));
    connect(this, SIGNAL(canInsertInstant(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    // Historically this was the main shortcut for "Insert Instant at
    // Playback Position". Note that Enter refers to the keypad key,
    // rather than the Return key, so this doesn't actually exist on
    // many keyboards now. Accordingly the alternative shortcut ";"
    // has been promoted to primary, listed above. Same goes for the
    // shifted version below
    QString shortcut(tr("Enter"));
    connect(new QShortcut(shortcut, this), SIGNAL(activated()),
            this, SLOT(insertInstant()));
    m_keyReference->registerAlternativeShortcut(action, shortcut);

    action = new QAction(tr("Insert Instants at Selection &Boundaries"), this);
    action->setShortcut(tr("Shift+;"));
    action->setStatusTip(tr("Insert new time instants at the start and end of the current selected regions, in a new layer if necessary"));
    connect(action, SIGNAL(triggered()), this, SLOT(insertInstantsAtBoundaries()));
    connect(this, SIGNAL(canInsertInstantsAtBoundaries(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    shortcut = QString(tr("Shift+Enter"));
    connect(new QShortcut(shortcut, this), SIGNAL(activated()),
            this, SLOT(insertInstantsAtBoundaries()));
    m_keyReference->registerAlternativeShortcut(action, shortcut);

    // The previous two actions used shortcuts with the (keypad) Enter
    // key, while this one I (bizarrely) switched from Enter to Return
    // in September 2014. Let's make it consistent with the above by
    // making the primary shortcut for it Ctrl+Shift+; and keeping
    // both Return and Enter as synonyms for ;
    action = new QAction(tr("Insert Item at Selection"), this);
    action->setShortcut(tr("Ctrl+Shift+;"));
    action->setStatusTip(tr("Insert a new note or region item corresponding to the current selection"));
    connect(action, SIGNAL(triggered()), this, SLOT(insertItemAtSelection()));
    connect(this, SIGNAL(canInsertItemAtSelection(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    shortcut = QString(tr("Ctrl+Shift+Enter"));
    connect(new QShortcut(shortcut, this), SIGNAL(activated()),
            this, SLOT(insertItemAtSelection()));
    m_keyReference->registerAlternativeShortcut(action, shortcut);

    shortcut = QString(tr("Ctrl+Shift+Return"));
    connect(new QShortcut(shortcut, this), SIGNAL(activated()),
            this, SLOT(insertItemAtSelection()));
    // we had that one for historical compatibility, but let's not
    // register it publicly; having three shortcuts for such an
    // obscure function is really over-egging it

    menu->addSeparator();

    QMenu *numberingMenu = menu->addMenu(tr("Number New Instants with"));
    numberingMenu->setTearOffEnabled(true);
    QActionGroup *numberingGroup = new QActionGroup(this);
    m_numberingActions.clear();

    Labeller::TypeNameMap types = m_labeller->getTypeNames();
    for (Labeller::TypeNameMap::iterator i = types.begin(); i != types.end(); ++i) {

        if (i->first == Labeller::ValueFromLabel ||
            i->first == Labeller::ValueFromExistingNeighbour) continue;

        action = new QAction(i->second, this);
        connect(action, SIGNAL(triggered()), this, SLOT(setInstantsNumbering()));
        action->setCheckable(true);
        action->setChecked(m_labeller->getType() == i->first);
        numberingGroup->addAction(action);
        numberingMenu->addAction(action);
        m_numberingActions.push_back({ action, (int)i->first });

        if (i->first == Labeller::ValueFromTwoLevelCounter) {

            QMenu *cycleMenu = numberingMenu->addMenu(tr("Cycle size"));
            QActionGroup *cycleGroup = new QActionGroup(this);

            int cycles[] = { 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 16 };
            for (int i = 0; i < int(sizeof(cycles)/sizeof(cycles[0])); ++i) {
                action = new QAction(QString("%1").arg(cycles[i]), this);
                connect(action, SIGNAL(triggered()), this, SLOT(setInstantsCounterCycle()));
                action->setCheckable(true);
                action->setChecked(cycles[i] == m_labeller->getCounterCycleSize());
                cycleGroup->addAction(action);
                cycleMenu->addAction(action);
            }
        }

        if (i->first == Labeller::ValueNone ||
            i->first == Labeller::ValueFromTwoLevelCounter ||
            i->first == Labeller::ValueFromRealTime) {
            numberingMenu->addSeparator();
        }
    }

    action = new QAction(tr("Reset Numbering Counters"), this);
    action->setStatusTip(tr("Reset to 1 all the counters used for counter-based labelling"));
    connect(action, SIGNAL(triggered()), this, SLOT(resetInstantsCounters()));
    connect(this, SIGNAL(replacedDocument()), action, SLOT(trigger()));
    menu->addAction(action);

    action = new QAction(tr("Set Numbering Counters..."), this);
    action->setStatusTip(tr("Set the counters used for counter-based labelling"));
    connect(action, SIGNAL(triggered()), this, SLOT(setInstantsCounters()));
    menu->addAction(action);
            
    action = new QAction(tr("Renumber Selected Instants"), this);
    action->setStatusTip(tr("Renumber the selected instants using the current labelling scheme"));
    connect(action, SIGNAL(triggered()), this, SLOT(renumberInstants()));
    connect(this, SIGNAL(canRenumberInstants(bool)), action, SLOT(setEnabled(bool)));
//    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    menu->addSeparator();
    
    action = new QAction(tr("Subdivide Selected Instants..."), this);
    action->setStatusTip(tr("Add new instants at regular intervals between the selected instants"));
    connect(action, SIGNAL(triggered()), this, SLOT(subdivideInstants()));
    connect(this, SIGNAL(canSubdivideInstants(bool)), action, SLOT(setEnabled(bool)));
    menu->addAction(action);
            
    action = new QAction(tr("Winnow Selected Instants..."), this);
    action->setStatusTip(tr("Remove subdivisions, leaving only every Nth instant"));
    connect(action, SIGNAL(triggered()), this, SLOT(winnowInstants()));
    connect(this, SIGNAL(canWinnowInstants(bool)), action, SLOT(setEnabled(bool)));
    menu->addAction(action);
}

void
MainWindow::setupViewMenu()
{
    SVDEBUG << "MainWindow::setupViewMenu" << endl;
    
    if (m_mainMenusCreated) return;

    IconLoader il;

    QAction *action = nullptr;

    m_keyReference->setCategory(tr("Panning and Navigation"));

    QMenu *menu = menuBar()->addMenu(tr("&View"));
    menu->setTearOffEnabled(true);
    m_scrollLeftAction = new QAction(tr("Scroll &Left"), this);
    m_scrollLeftAction->setShortcut(tr("Left"));
    m_scrollLeftAction->setStatusTip(tr("Scroll the current pane to the left"));
    connect(m_scrollLeftAction, SIGNAL(triggered()), this, SLOT(scrollLeft()));
    connect(this, SIGNAL(canScroll(bool)), m_scrollLeftAction, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(m_scrollLeftAction);
    menu->addAction(m_scrollLeftAction);
        
    m_scrollRightAction = new QAction(tr("Scroll &Right"), this);
    m_scrollRightAction->setShortcut(tr("Right"));
    m_scrollRightAction->setStatusTip(tr("Scroll the current pane to the right"));
    connect(m_scrollRightAction, SIGNAL(triggered()), this, SLOT(scrollRight()));
    connect(this, SIGNAL(canScroll(bool)), m_scrollRightAction, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(m_scrollRightAction);
    menu->addAction(m_scrollRightAction);
        
    action = new QAction(tr("&Jump Left"), this);
    action->setShortcut(tr("Ctrl+Left"));
    action->setStatusTip(tr("Scroll the current pane a big step to the left"));
    connect(action, SIGNAL(triggered()), this, SLOT(jumpLeft()));
    connect(this, SIGNAL(canScroll(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
        
    action = new QAction(tr("J&ump Right"), this);
    action->setShortcut(tr("Ctrl+Right"));
    action->setStatusTip(tr("Scroll the current pane a big step to the right"));
    connect(action, SIGNAL(triggered()), this, SLOT(jumpRight()));
    connect(this, SIGNAL(canScroll(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    action = new QAction(tr("Peek Left"), this);
    action->setShortcut(tr("Alt+Left"));
    action->setStatusTip(tr("Scroll the current pane to the left without moving the playback cursor or other panes"));
    connect(action, SIGNAL(triggered()), this, SLOT(peekLeft()));
    connect(this, SIGNAL(canScroll(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
        
    action = new QAction(tr("Peek Right"), this);
    action->setShortcut(tr("Alt+Right"));
    action->setStatusTip(tr("Scroll the current pane to the right without moving the playback cursor or other panes"));
    connect(action, SIGNAL(triggered()), this, SLOT(peekRight()));
    connect(this, SIGNAL(canScroll(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    menu->addSeparator();

    m_keyReference->setCategory(tr("Zoom"));

    m_zoomInAction = new QAction(il.load("zoom-in"),
                                 tr("Zoom &In"), this);
    m_zoomInAction->setShortcut(tr("Up"));
    m_zoomInAction->setStatusTip(tr("Increase the zoom level"));
    connect(m_zoomInAction, SIGNAL(triggered()), this, SLOT(zoomIn()));
    connect(this, SIGNAL(canZoom(bool)), m_zoomInAction, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(m_zoomInAction);
    menu->addAction(m_zoomInAction);
        
    m_zoomOutAction = new QAction(il.load("zoom-out"),
                                  tr("Zoom &Out"), this);
    m_zoomOutAction->setShortcut(tr("Down"));
    m_zoomOutAction->setStatusTip(tr("Decrease the zoom level"));
    connect(m_zoomOutAction, SIGNAL(triggered()), this, SLOT(zoomOut()));
    connect(this, SIGNAL(canZoom(bool)), m_zoomOutAction, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(m_zoomOutAction);
    menu->addAction(m_zoomOutAction);
        
    action = new QAction(tr("Restore &Default Zoom"), this);
    action->setStatusTip(tr("Restore the zoom level to the default"));
    connect(action, SIGNAL(triggered()), this, SLOT(zoomDefault()));
    connect(this, SIGNAL(canZoom(bool)), action, SLOT(setEnabled(bool)));
    menu->addAction(action);

    m_zoomFitAction = new QAction(il.load("zoom-fit"),
                                  tr("Zoom to &Fit"), this);
    m_zoomFitAction->setShortcut(tr("F"));
    m_zoomFitAction->setStatusTip(tr("Zoom to show the whole file"));
    connect(m_zoomFitAction, SIGNAL(triggered()), this, SLOT(zoomToFit()));
    connect(this, SIGNAL(canZoom(bool)), m_zoomFitAction, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(m_zoomFitAction);
    menu->addAction(m_zoomFitAction);

    menu->addSeparator();

    m_keyReference->setCategory(tr("Display Features"));

    // For Performance Precision
    m_viewManager->setShowCentreLine(false);
    
    action = new QAction(tr("Show &Centre Line"), this);
    action->setShortcut(tr("'"));
    action->setStatusTip(tr("Show or hide the centre line"));
    connect(action, SIGNAL(triggered()), this, SLOT(toggleCentreLine()));
    action->setCheckable(true);
    action->setChecked(m_viewManager->shouldShowCentreLine());
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    action = new QAction(tr("Toggle All Time Rulers"), this);
    action->setShortcut(tr("#"));
    action->setStatusTip(tr("Show or hide all time rulers"));
    connect(action, SIGNAL(triggered()), this, SLOT(toggleTimeRulers()));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    menu->addSeparator();

    QActionGroup *overlayGroup = new QActionGroup(this);
        
    ViewManager::OverlayMode mode = m_viewManager->getOverlayMode();

    action = new QAction(tr("Show &No Overlays"), this);
    action->setShortcut(tr("0"));
    action->setStatusTip(tr("Hide times, layer names, and scale"));
    connect(action, SIGNAL(triggered()), this, SLOT(showNoOverlays()));
    action->setCheckable(true);
    action->setChecked(mode == ViewManager::NoOverlays);
    overlayGroup->addAction(action);
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
        
    action = new QAction(tr("Show &Minimal Overlays"), this);
    action->setShortcut(tr("9"));
    action->setStatusTip(tr("Show times and basic scale"));
    connect(action, SIGNAL(triggered()), this, SLOT(showMinimalOverlays()));
    action->setCheckable(true);
    action->setChecked(mode == ViewManager::StandardOverlays);
    overlayGroup->addAction(action);
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
        
    action = new QAction(tr("Show &All Overlays"), this);
    action->setShortcut(tr("8"));
    action->setStatusTip(tr("Show times, layer names, and scale"));
    connect(action, SIGNAL(triggered()), this, SLOT(showAllOverlays()));
    action->setCheckable(true);
    action->setChecked(mode == ViewManager::AllOverlays);
    overlayGroup->addAction(action);
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    menu->addSeparator();
        
    action = new QAction(tr("Show &Zoom Wheels"), this);
    action->setShortcut(tr("Z"));
    action->setStatusTip(tr("Show thumbwheels for zooming horizontally and vertically"));
    connect(action, SIGNAL(triggered()), this, SLOT(toggleZoomWheels()));
    action->setCheckable(true);
    action->setChecked(m_viewManager->getZoomWheelsEnabled());
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    m_showPropertyBoxesAction = new QAction(tr("Show Property Bo&xes"), this);
    m_showPropertyBoxesAction->setShortcut(tr("X"));
    m_showPropertyBoxesAction->setStatusTip(tr("Show the layer property boxes at the side of the main window"));
    connect(m_showPropertyBoxesAction, SIGNAL(triggered()), this, SLOT(togglePropertyBoxes()));
    m_showPropertyBoxesAction->setCheckable(true);
    m_showPropertyBoxesAction->setChecked(true);
    m_keyReference->registerShortcut(m_showPropertyBoxesAction);
    menu->addAction(m_showPropertyBoxesAction);

    action = new QAction(tr("Show Status &Bar"), this);
    action->setStatusTip(tr("Show context help information in the status bar at the bottom of the window"));
    connect(action, SIGNAL(triggered()), this, SLOT(toggleStatusBar()));
    action->setCheckable(true);
    action->setChecked(true);
    menu->addAction(action);

    QSettings settings;
    settings.beginGroup("MainWindow");
    bool sb = settings.value("showstatusbar", true).toBool();
    if (!sb) {
        action->setChecked(false);
        statusBar()->hide();
    }
    settings.endGroup();

    menu->addSeparator();

    action = new QAction(tr("Show La&yer Summary"), this);
    action->setShortcut(tr("Y"));
    action->setStatusTip(tr("Open a window displaying the hierarchy of panes and layers in this session"));
    connect(action, SIGNAL(triggered()), this, SLOT(showLayerTree()));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    action = new QAction(tr("Show Acti&vity Log"), this);
    action->setStatusTip(tr("Open a window listing interactions and other events"));
    connect(action, SIGNAL(triggered()), this, SLOT(showActivityLog()));
    menu->addAction(action);

    action = new QAction(tr("Show &Unit Converter"), this);
    action->setStatusTip(tr("Open a window of pitch and timing conversion utilities"));
    connect(action, SIGNAL(triggered()), this, SLOT(showUnitConverter()));
    menu->addAction(action);

    menu->addSeparator();

#ifndef Q_OS_MAC
    // Only on non-Mac platforms -- on the Mac this interacts very
    // badly with the "native" full-screen mode
    action = new QAction(tr("Go Full-Screen"), this);
    action->setShortcut(tr("F11"));
    action->setStatusTip(tr("Expand the pane area to the whole screen"));
    connect(action, SIGNAL(triggered()), this, SLOT(goFullScreen()));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
#endif
}

QString
MainWindow::shortcutFor(LayerFactory::LayerType layer, bool isPaneMenu)
{
    QString shortcutText;
    
#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wswitch-enum"
#endif
    
    switch (layer) {
    case LayerFactory::Waveform:
        if (isPaneMenu) {
            shortcutText = tr("W");
        } else {
            shortcutText = tr("Shift+W");
        }
        break;
                
    case LayerFactory::Spectrogram:
        if (isPaneMenu) {
            shortcutText = tr("G");
        } else {
            shortcutText = tr("Shift+G");
        }
        break;
                
    case LayerFactory::MelodicRangeSpectrogram:
        if (isPaneMenu) {
            shortcutText = tr("M");
        } else {
            shortcutText = tr("Shift+M");
        }
        break;
                
    case LayerFactory::PeakFrequencySpectrogram:
        if (isPaneMenu) {
            shortcutText = tr("K");
        } else {
            shortcutText = tr("Shift+K");
        }
        break;
                
    case LayerFactory::Spectrum:
        if (isPaneMenu) {
            shortcutText = tr("U");
        } else {
            shortcutText = tr("Shift+U");
        }
        break;

    default:
        break;
    }

    return shortcutText;
}

void
MainWindow::setupPaneAndLayerMenus()
{
    SVDEBUG << "MainWindow::setupPaneAndLayerMenus" << endl;
    
    Profiler profiler("MainWindow::setupPaneAndLayerMenus");
    
    if (m_paneMenu) {
        m_paneMenu->clear();
        for (auto a: m_paneActions) {
            delete a.first;
        }
        m_paneActions.clear();
    } else {
        m_paneMenu = menuBar()->addMenu(tr("&Pane"));
        m_paneMenu->setTearOffEnabled(true);
    }

    if (m_rightButtonLayerMenu) {
        m_rightButtonLayerMenu->clear();
    } else {
        m_rightButtonLayerMenu = m_rightButtonMenu->addMenu(tr("&Layer"));
        m_rightButtonLayerMenu->setTearOffEnabled(true);
        m_rightButtonMenu->addSeparator();
    }

    if (m_layerMenu) {
        m_layerMenu->clear();
        for (auto a: m_layerActions) {
            delete a.first;
        }
        m_layerActions.clear();
    } else {
        m_layerMenu = menuBar()->addMenu(tr("&Layer"));
        m_layerMenu->setTearOffEnabled(true);
    }

    QMenu *menu = m_paneMenu;

    IconLoader il;

    m_keyReference->setCategory(tr("Managing Panes and Layers"));

    m_paneActions.clear();
    m_layerActions.clear();
    
    QAction *action = new QAction(il.load("pane"), tr("Add &New Pane"), this);
    action->setShortcut(tr("N"));
    action->setStatusTip(tr("Add a new pane containing only a time ruler"));
    connect(action, SIGNAL(triggered()), this, SLOT(addPane()));
    connect(this, SIGNAL(canAddPane(bool)), action, SLOT(setEnabled(bool)));
    m_paneActions.push_back({ action, LayerConfiguration(LayerFactory::TimeRuler) });
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    menu->addSeparator();

    menu = m_layerMenu;

    LayerFactory::LayerTypeSet emptyLayerTypes =
        LayerFactory::getInstance()->getValidEmptyLayerTypes();

    for (LayerFactory::LayerTypeSet::iterator i = emptyLayerTypes.begin();
         i != emptyLayerTypes.end(); ++i) {
        
        QIcon icon;
        QString mainText, tipText, channelText;
        LayerFactory::LayerType type = *i;
        QString name = LayerFactory::getInstance()->getLayerPresentationName(type);
        
        icon = il.load(LayerFactory::getInstance()->getLayerIconName(type));

        mainText = tr("Add New %1 Layer").arg(name);
        tipText = tr("Add a new empty layer of type %1").arg(name);

        action = new QAction(icon, mainText, this);
        action->setStatusTip(tipText);

        if (type == LayerFactory::Text) {
            action->setShortcut(tr("T"));
            m_keyReference->registerShortcut(action);
        }

        connect(action, SIGNAL(triggered()), this, SLOT(addLayer()));
        connect(this, SIGNAL(canAddLayer(bool)), action, SLOT(setEnabled(bool)));
        m_layerActions.push_back({ action, LayerConfiguration(type) });
        menu->addAction(action);
        m_rightButtonLayerMenu->addAction(action);
    }
    
    m_rightButtonLayerMenu->addSeparator();
    menu->addSeparator();

    LayerFactory::LayerType backgroundTypes[] = {
        LayerFactory::Waveform,
        LayerFactory::Spectrogram,
        LayerFactory::MelodicRangeSpectrogram,
        LayerFactory::PeakFrequencySpectrogram,
        LayerFactory::Spectrum
    };
    int backgroundTypeCount = int(sizeof(backgroundTypes) /
                                  sizeof(backgroundTypes[0]));

    vector<ModelId> models;
    if (m_document) models = m_document->getTransformInputModels();
    bool plural = (models.size() > 1);
    if (models.empty()) {
        models.push_back(getMainModelId()); // probably None at this point
    }

    for (int i = 0; i < backgroundTypeCount; ++i) {

        const int paneMenuType = 0, layerMenuType = 1;

        for (int menuType = paneMenuType; menuType <= layerMenuType; ++menuType) {

            if (menuType == paneMenuType) menu = m_paneMenu;
            else menu = m_layerMenu;

            QMenu *submenu = nullptr;

            QIcon icon;
            QString mainText, tipText, channelText;
            LayerFactory::LayerType type = backgroundTypes[i];
            bool mono = true;

            QString shortcutText = shortcutFor(type, menuType == paneMenuType);

// Avoid warnings/errors with -Wextra because we aren't explicitly
// handling all layer types (-Wall is OK with this because of the
// default but the stricter level insists)
#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wswitch-enum"
#endif
            
            switch (type) {
                    
            case LayerFactory::Waveform:
                icon = il.load("waveform");
                mainText = tr("Add &Waveform");
                if (menuType == paneMenuType) {
                    tipText = tr("Add a new pane showing a waveform view");
                } else {
                    tipText = tr("Add a new layer showing a waveform view");
                }
                mono = false;
                break;
                
            case LayerFactory::Spectrogram:
                icon = il.load("spectrogram");
                mainText = tr("Add Spectro&gram");
                if (menuType == paneMenuType) {
                    tipText = tr("Add a new pane showing a spectrogram");
                } else {
                    tipText = tr("Add a new layer showing a spectrogram");
                }
                break;
                
            case LayerFactory::MelodicRangeSpectrogram:
                icon = il.load("spectrogram");
                mainText = tr("Add &Melodic Range Spectrogram");
                if (menuType == paneMenuType) {
                    tipText = tr("Add a new pane showing a spectrogram set up for an overview of note pitches");
                } else {
                    tipText = tr("Add a new layer showing a spectrogram set up for an overview of note pitches");
                }
                break;
                
            case LayerFactory::PeakFrequencySpectrogram:
                icon = il.load("spectrogram");
                mainText = tr("Add Pea&k Frequency Spectrogram");
                if (menuType == paneMenuType) {
                    tipText = tr("Add a new pane showing a spectrogram set up for tracking frequencies");
                } else {
                    tipText = tr("Add a new layer showing a spectrogram set up for tracking frequencies");
                }
                break;
                
            case LayerFactory::Spectrum:
                icon = il.load("spectrum");
                mainText = tr("Add Spectr&um");
                if (menuType == paneMenuType) {
                    tipText = tr("Add a new pane showing a frequency spectrum");
                } else {
                    tipText = tr("Add a new layer showing a frequency spectrum");
                }
                break;
                
            default: break;
            }

            vector<ModelId> candidateModels = models;
            if (candidateModels.empty()) {
                throw std::logic_error("candidateModels should not be empty");
            }
            
            for (auto modelId: candidateModels) {

                auto model = ModelById::get(modelId);
                
                int channels = 0;
                if (model) {
                    if (auto dtvm = ModelById::getAs<DenseTimeValueModel>
                        (modelId)) {
                        channels = dtvm->getChannelCount();
                    }
                }
                if (channels < 1 && getMainModel()) {
                    channels = getMainModel()->getChannelCount();
                }
                if (channels < 1) channels = 1;

                for (int c = 0; c <= channels; ++c) {

                    if (c == 1 && channels == 1) continue;
                    bool isDefault = (c == 0);
                    bool isOnly = (isDefault && (channels == 1));

                    if (isOnly && !plural) {

                        action = new QAction(icon, mainText, this);

                        action->setShortcut(shortcutText);
                        action->setStatusTip(tipText);
                        if (menuType == paneMenuType) {
                            connect(action, SIGNAL(triggered()),
                                    this, SLOT(addPane()));
                            connect(this, SIGNAL(canAddPane(bool)),
                                    action, SLOT(setEnabled(bool)));
                            m_paneActions.push_back
                                ({ action, LayerConfiguration(type, modelId) });
                        } else {
                            connect(action, SIGNAL(triggered()),
                                    this, SLOT(addLayer()));
                            connect(this, SIGNAL(canAddLayer(bool)),
                                    action, SLOT(setEnabled(bool)));
                            m_layerActions.push_back
                                ({ action, LayerConfiguration(type, modelId) });
                        }
                        if (shortcutText != "") {
                            m_keyReference->registerShortcut(action);
                        }
                        menu->addAction(action);
                        
                    } else {
                        
                        if (!submenu) {
                            submenu = menu->addMenu(mainText);
                            submenu->setTearOffEnabled(true);
                        } else if (isDefault) {
                            submenu->addSeparator();
                        }

                        QString actionText;
                        if (c == 0) {
                            if (mono) {
                                actionText = tr("&All Channels Mixed");
                            } else {
                                actionText = tr("&All Channels");
                            }
                        } else {
                            actionText = tr("Channel &%1").arg(c);
                        }

                        if (model) {
                            actionText = tr("%1: %2")
                                .arg(model->objectName())
                                .arg(actionText);
                        }

                        if (isDefault) {
                            action = new QAction(icon, actionText, this);
                            if (!model || modelId == getMainModelId()) {
                                // Default for the shortcut is to
                                // attach to an action that uses the
                                // main model as input. But this may
                                // change when the user selects a
                                // different pane - see
                                // updateLayerShortcutsFor() below.
                                action->setShortcut(shortcutText); 
                            }
                        } else {
                            action = new QAction(actionText, this);
                        }

                        action->setStatusTip(tipText);

                        if (menuType == paneMenuType) {
                            connect(action, SIGNAL(triggered()),
                                    this, SLOT(addPane()));
                            connect(this, SIGNAL(canAddPane(bool)),
                                    action, SLOT(setEnabled(bool)));
                            m_paneActions.push_back
                                ({ action, LayerConfiguration(type, modelId, c - 1) });
                        } else {
                            connect(action, SIGNAL(triggered()),
                                    this, SLOT(addLayer()));
                            connect(this, SIGNAL(canAddLayer(bool)),
                                    action, SLOT(setEnabled(bool)));
                            m_layerActions.push_back
                                ({ action, LayerConfiguration(type, modelId, c - 1) });
                        }

                        submenu->addAction(action);
                    }

                    if (isDefault && menuType == layerMenuType &&
                        modelId == *candidateModels.begin()) {
                        // only add for one model, one channel, one menu on
                        // right button -- the action itself will discover
                        // which model is the correct one (based on pane)
                        action = new QAction(icon, mainText, this);
                        action->setStatusTip(tipText);
                        connect(action, SIGNAL(triggered()),
                                this, SLOT(addLayer()));
                        connect(this, SIGNAL(canAddLayer(bool)),
                                action, SLOT(setEnabled(bool)));
                        m_layerActions.push_back
                            ({ action, LayerConfiguration(type, ModelId(), 0) });
                        m_rightButtonLayerMenu->addAction(action);
                    }
                }
            }
        }
    }

    m_rightButtonLayerMenu->addSeparator();

    menu = m_paneMenu;
    menu->addSeparator();

    action = new QAction(tr("Switch to Previous Pane"), this);
    action->setShortcut(tr("["));
    action->setStatusTip(tr("Make the next pane up in the pane stack current"));
    connect(action, SIGNAL(triggered()), this, SLOT(previousPane()));
    connect(this, SIGNAL(canSelectPreviousPane(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    action = new QAction(tr("Switch to Next Pane"), this);
    action->setShortcut(tr("]"));
    action->setStatusTip(tr("Make the next pane down in the pane stack current"));
    connect(action, SIGNAL(triggered()), this, SLOT(nextPane()));
    connect(this, SIGNAL(canSelectNextPane(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    menu->addSeparator();

    action = new QAction(il.load("editdelete"), tr("&Delete Pane"), this);
    action->setShortcut(tr("Ctrl+Shift+D"));
    action->setStatusTip(tr("Delete the currently active pane"));
    connect(action, SIGNAL(triggered()), this, SLOT(deleteCurrentPane()));
    connect(this, SIGNAL(canDeleteCurrentPane(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    menu = m_layerMenu;

    action = new QAction(il.load("timeruler"), tr("Add &Time Ruler"), this);
    action->setStatusTip(tr("Add a new layer showing a time ruler"));
    connect(action, SIGNAL(triggered()), this, SLOT(addLayer()));
    connect(this, SIGNAL(canAddLayer(bool)), action, SLOT(setEnabled(bool)));
    m_layerActions.push_back({ action, LayerConfiguration(LayerFactory::TimeRuler) });
    menu->addAction(action);

    menu->addSeparator();

    m_existingLayersMenu = menu->addMenu(tr("Add &Existing Layer"));
    m_existingLayersMenu->setTearOffEnabled(true);
    m_rightButtonLayerMenu->addMenu(m_existingLayersMenu);

    m_sliceMenu = menu->addMenu(tr("Add S&lice of Layer"));
    m_sliceMenu->setTearOffEnabled(true);
    m_rightButtonLayerMenu->addMenu(m_sliceMenu);

    setupExistingLayersMenus();

    menu->addSeparator();

    action = new QAction(tr("Switch to Previous Layer"), this);
    action->setShortcut(tr("{"));
    action->setStatusTip(tr("Make the previous layer in the pane current"));
    connect(action, SIGNAL(triggered()), this, SLOT(previousLayer()));
    connect(this, SIGNAL(canSelectPreviousLayer(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    action = new QAction(tr("Switch to Next Layer"), this);
    action->setShortcut(tr("}"));
    action->setStatusTip(tr("Make the next layer in the pane current"));
    connect(action, SIGNAL(triggered()), this, SLOT(nextLayer()));
    connect(this, SIGNAL(canSelectNextLayer(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);

    m_rightButtonLayerMenu->addSeparator();
    menu->addSeparator();

    QAction *raction = new QAction(tr("&Rename Layer..."), this);
    raction->setShortcut(tr("R"));
    raction->setStatusTip(tr("Rename the currently active layer"));
    connect(raction, SIGNAL(triggered()), this, SLOT(renameCurrentLayer()));
    connect(this, SIGNAL(canRenameLayer(bool)), raction, SLOT(setEnabled(bool)));
    menu->addAction(raction);
    m_rightButtonLayerMenu->addAction(raction);

    QAction *eaction = new QAction(tr("Edit Layer Data"), this);
    eaction->setShortcut(tr("E"));
    eaction->setStatusTip(tr("Edit the currently active layer as a data grid"));
    connect(eaction, SIGNAL(triggered()), this, SLOT(editCurrentLayer()));
    connect(this, SIGNAL(canEditLayerTabular(bool)), eaction, SLOT(setEnabled(bool)));
    menu->addAction(eaction);
    m_rightButtonLayerMenu->addAction(eaction);

    action = new QAction(il.load("editdelete"), tr("&Delete Layer"), this);
    action->setShortcut(tr("Ctrl+D"));
    action->setStatusTip(tr("Delete the currently active layer"));
    connect(action, SIGNAL(triggered()), this, SLOT(deleteCurrentLayer()));
    connect(this, SIGNAL(canDeleteCurrentLayer(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
    m_rightButtonLayerMenu->addAction(action);

    m_keyReference->registerShortcut(raction); // rename after delete, so delete layer goes next to delete pane
    m_keyReference->registerShortcut(eaction); // edit also after delete

    finaliseMenus();
}

void
MainWindow::updateLayerShortcutsFor(ModelId modelId)
{
    // Called when e.g. the current pane has changed, to ensure the
    // various layer shortcuts select an action whose input model is
    // the active one in this pane
    
    set<LayerFactory::LayerType> seen;
    
    for (auto &a : m_paneActions) {
        if (a.second.sourceModel.isNone()) {
            continue; // empty pane/layer shortcut
        }
        auto type = a.second.layer;
        if (a.second.sourceModel == modelId && seen.find(type) == seen.end()) {
            a.first->setShortcut(shortcutFor(type, true));
            seen.insert(type);
        } else {
            a.first->setShortcut(QString());
        }
    }

    seen.clear();
    
    for (auto &a : m_layerActions) {
        if (a.second.sourceModel.isNone()) {
            continue; // empty pane/layer shortcut
        }
        auto type = a.second.layer;
        if (a.second.sourceModel == modelId && seen.find(type) == seen.end()) {
            a.first->setShortcut(shortcutFor(type, false));
            seen.insert(type);
        } else {
            a.first->setShortcut(QString());
        }
    }
}

void
MainWindow::prepareTransformsMenu()
{
    SVDEBUG << "MainWindow::prepareTransformsMenu" << endl;

    if (m_transformsMenu) {
        return;
    }
    
    m_transformsMenu = menuBar()->addMenu(tr("&Transform")); 
    m_transformsMenu->setTearOffEnabled(true);
    m_transformsMenu->setSeparatorsCollapsible(true);

    auto pending = m_transformsMenu->addAction(tr("Scanning plugins..."));
    pending->setEnabled(false);
    
    SVDEBUG << "MainWindow::prepareTransformsMenu: Starting installed-transform population thread" << endl;

    connect(TransformFactory::getInstance(),
            SIGNAL(installedTransformsPopulated()),
            this,
            SLOT(installedTransformsPopulated()));

    set<Transform::Type> restrictedTo;
    restrictedTo.insert(Transform::FeatureExtraction);
    TransformFactory::getInstance()->restrictTransformTypes(restrictedTo);
    
    QTimer::singleShot(150, TransformFactory::getInstance(),
                       SLOT(startPopulatingInstalledTransforms()));
}

void
MainWindow::installedTransformsPopulated()
{
    populateTransformsMenu();
    populateScoreAlignerChoiceMenu();

    if (m_shouldStartOSCQueue) {
        SVDEBUG << "MainWindow: Creating OSC queue with network port"
                << endl;
        startOSCQueue(true);
    } else {
        SVDEBUG << "MainWindow: Creating internal-only OSC queue without port"
                << endl;
        startOSCQueue(false);
    }
}

void
MainWindow::populateTransformsMenu()
{
    SVDEBUG << "MainWindow::populateTransformsMenu" << endl;

    if (m_transformsMenu) {
        m_transformsMenu->clear();
        m_rightButtonTransformsMenu->clear();
        m_transformActionsReverse.clear();
        m_transformActions.clear();
        for (auto a: m_transformActions) {
            delete a.first;
        }
    } else {
        m_transformsMenu = menuBar()->addMenu(tr("&Transform")); 
        m_transformsMenu->setTearOffEnabled(true);
        m_transformsMenu->setSeparatorsCollapsible(true);
    }

    TransformFactory *factory = TransformFactory::getInstance();

    TransformList transforms = factory->getInstalledTransformDescriptions();
    
    // We have two possible sources of error here: plugin scan
    // warnings, and transform factory startup errors.
    //
    // In the Piper world, a plugin scan warning is typically
    // non-fatal (it means a plugin is being ignored) but a transform
    // factory startup error is fatal (it means no plugins can be used
    // at all, or at least no feature extraction plugins, and probably
    // indicates an installation problem with SV itself rather than
    // with a plugin).
    //
    // If we have both types of error text, we should either show both
    // (which we don't do as we haven't designed a way to do that
    // tidily) or else only show the transform factory one.
    //
    QString warning = factory->getStartupFailureReport();
    if (warning != "") {
        SVDEBUG << "MainWindow::populateTransformsMenu: Transform population yielded errors" << endl;
        pluginPopulationWarning(warning);
    } else {
        warning = PluginScan::getInstance()->getStartupFailureReport();
        if (warning != "") {
            SVDEBUG << "MainWindow::populateTransformsMenu: Plugin scan yielded errors" << endl;
            pluginPopulationWarning(warning);
        }
    }
    
    vector<TransformDescription::Type> types = factory->getTransformTypes();

    map<TransformDescription::Type, map<QString, SubdividingMenu *> > categoryMenus;
    map<TransformDescription::Type, map<QString, SubdividingMenu *> > makerMenus;

    map<TransformDescription::Type, SubdividingMenu *> byPluginNameMenus;
    map<TransformDescription::Type, map<QString, QMenu *> > pluginNameMenus;

    set<SubdividingMenu *> pendingMenus;

    m_recentTransformsMenu = m_transformsMenu->addMenu(tr("&Recent Transforms"));
    m_recentTransformsMenu->setTearOffEnabled(true);
    m_rightButtonTransformsMenu->addMenu(m_recentTransformsMenu);
    connect(&m_recentTransforms, SIGNAL(recentChanged()),
            this, SLOT(setupRecentTransformsMenu()));

    m_transformsMenu->addSeparator();
    m_rightButtonTransformsMenu->addSeparator();
    
    for (vector<TransformDescription::Type>::iterator i = types.begin();
         i != types.end(); ++i) {

        if (i != types.begin()) {
            m_transformsMenu->addSeparator();
            m_rightButtonTransformsMenu->addSeparator();
        }

        QString byCategoryLabel = tr("%1 by Category")
            .arg(factory->getTransformTypeName(*i));
        SubdividingMenu *byCategoryMenu = new SubdividingMenu(byCategoryLabel,
                                                              20, 40);
        byCategoryMenu->setTearOffEnabled(true);
        m_transformsMenu->addMenu(byCategoryMenu);
        m_rightButtonTransformsMenu->addMenu(byCategoryMenu);
        pendingMenus.insert(byCategoryMenu);

        vector<QString> categories = factory->getTransformCategories(*i);

        for (vector<QString>::iterator j = categories.begin();
             j != categories.end(); ++j) {

            QString category = *j;
            if (category == "") category = tr("Unclassified");

            if (categories.size() < 2) {
                categoryMenus[*i][category] = byCategoryMenu;
                continue;
            }

            QStringList components = category.split(" > ");
            QString key;

            for (QStringList::iterator k = components.begin();
                 k != components.end(); ++k) {

                QString parentKey = key;
                if (key != "") key += " > ";
                key += *k;

                if (categoryMenus[*i].find(key) == categoryMenus[*i].end()) {
                    SubdividingMenu *m = new SubdividingMenu(*k, 20, 40);
                    m->setTearOffEnabled(true);
                    pendingMenus.insert(m);
                    categoryMenus[*i][key] = m;
                    if (parentKey == "") {
                        byCategoryMenu->addMenu(m);
                    } else {
                        categoryMenus[*i][parentKey]->addMenu(m);
                    }
                }
            }
        }

        QString byPluginNameLabel = tr("%1 by Plugin Name")
            .arg(factory->getTransformTypeName(*i));
        byPluginNameMenus[*i] = new SubdividingMenu(byPluginNameLabel);
        byPluginNameMenus[*i]->setTearOffEnabled(true);
        m_transformsMenu->addMenu(byPluginNameMenus[*i]);
        m_rightButtonTransformsMenu->addMenu(byPluginNameMenus[*i]);
        pendingMenus.insert(byPluginNameMenus[*i]);

        QString byMakerLabel = tr("%1 by Maker")
            .arg(factory->getTransformTypeName(*i));
        SubdividingMenu *byMakerMenu = new SubdividingMenu(byMakerLabel, 20, 40);
        byMakerMenu->setTearOffEnabled(true);
        m_transformsMenu->addMenu(byMakerMenu);
        m_rightButtonTransformsMenu->addMenu(byMakerMenu);
        pendingMenus.insert(byMakerMenu);

        vector<QString> makers = factory->getTransformMakers(*i);

        for (vector<QString>::iterator j = makers.begin();
             j != makers.end(); ++j) {

            QString maker = *j;
            if (maker == "") maker = tr("Unknown");
            maker.replace(QRegularExpression(tr(" [\\(<].*$")), "");

            makerMenus[*i][maker] = new SubdividingMenu(maker, 30, 40);
            makerMenus[*i][maker]->setTearOffEnabled(true);
            byMakerMenu->addMenu(makerMenus[*i][maker]);
            pendingMenus.insert(makerMenus[*i][maker]);
        }
    }

    // Names should only be duplicated here if they have the same
    // plugin name, output name and maker but are in different library
    // .so names -- that won't happen often I hope
    map<QString, QString> idNameSonameMap;
    set<QString> seenNames, duplicateNames;
    for (int i = 0; in_range_for(transforms, i); ++i) {
        QString name = transforms[i].name;
        if (seenNames.find(name) != seenNames.end()) {
            duplicateNames.insert(name);
        } else {
            seenNames.insert(name);
        }
    }

    m_transformActions.clear();
    m_transformActionsReverse.clear();
    
    for (int i = 0; in_range_for(transforms, i); ++i) {
        
        QString name = transforms[i].name;
        if (name == "") name = transforms[i].identifier;

//        cerr << "Plugin Name: " << name << endl;

        TransformDescription::Type type = transforms[i].type;
        QString typeStr = factory->getTransformTypeName(type);

        QString category = transforms[i].category;
        if (category == "") category = tr("Unclassified");

        QString maker = transforms[i].maker;
        if (maker == "") maker = tr("Unknown");
        maker.replace(QRegularExpression(tr(" [\\(<].*$")), "");

        QString pluginName = name.section(": ", 0, 0);
        QString output = name.section(": ", 1);

        if (duplicateNames.find(pluginName) != duplicateNames.end()) {
            pluginName = QString("%1 <%2>")
                .arg(pluginName)
                .arg(transforms[i].identifier.section(':', 1, 1));
            if (output == "") name = pluginName;
            else name = QString("%1: %2")
                .arg(pluginName)
                .arg(output);
        }

        QAction *action = new QAction(tr("%1...").arg(name), this);
        connect(action, SIGNAL(triggered()), this, SLOT(addLayer()));
        m_transformActions.push_back({ action, transforms[i].identifier });
        m_transformActionsReverse[transforms[i].identifier] = action;
        connect(this, SIGNAL(canAddLayer(bool)), action, SLOT(setEnabled(bool)));

        action->setStatusTip(transforms[i].longDescription);

        if (categoryMenus[type].find(category) == categoryMenus[type].end()) {
            SVCERR << "WARNING: MainWindow::setupMenus: Internal error: "
                      << "No category menu for transform \""
                      << name << "\" (category = \""
                      << category << "\")" << endl;
        } else {
            categoryMenus[type][category]->addAction(action);
        }

        if (makerMenus[type].find(maker) == makerMenus[type].end()) {
            SVCERR << "WARNING: MainWindow::setupMenus: Internal error: "
                      << "No maker menu for transform \""
                      << name << "\" (maker = \""
                      << maker << "\")" << endl;
        } else {
            makerMenus[type][maker]->addAction(action);
        }

        action = new QAction(tr("%1...").arg(output == "" ? pluginName : output), this);
        connect(action, SIGNAL(triggered()), this, SLOT(addLayer()));
        m_transformActions.push_back({ action, transforms[i].identifier });
        connect(this, SIGNAL(canAddLayer(bool)), action, SLOT(setEnabled(bool)));
        action->setStatusTip(transforms[i].longDescription);

//        cerr << "Transform: \"" << name << "\": plugin name \"" << pluginName << "\"" << endl;

        if (pluginNameMenus[type].find(pluginName) ==
            pluginNameMenus[type].end()) {

            SubdividingMenu *parentMenu = byPluginNameMenus[type];
            parentMenu->setTearOffEnabled(true);

            if (output == "") {
                parentMenu->addAction(pluginName, action);
            } else {
                pluginNameMenus[type][pluginName] = 
                    parentMenu->addMenu(pluginName);
                connect(this, SIGNAL(canAddLayer(bool)),
                        pluginNameMenus[type][pluginName],
                        SLOT(setEnabled(bool)));
            }
        }

        if (pluginNameMenus[type].find(pluginName) !=
            pluginNameMenus[type].end()) {
            pluginNameMenus[type][pluginName]->addAction(action);
        }
    }

    for (set<SubdividingMenu *>::iterator i = pendingMenus.begin();
         i != pendingMenus.end(); ++i) {
        (*i)->entriesAdded();
    }

    m_transformsMenu->addSeparator();
    m_rightButtonTransformsMenu->addSeparator();

    QAction *action = new QAction(tr("Find a Transform..."), this);
    action->setStatusTip(tr("Search for a transform from the installed plugins, by name or description"));
    action->setShortcut(tr("Ctrl+M"));
    connect(action, SIGNAL(triggered()), this, SLOT(findTransform()));
//    connect(this, SIGNAL(canAddLayer(bool)), action, SLOT(setEnabled(bool)));
    m_keyReference->registerShortcut(action);
    m_transformsMenu->addAction(action);
    m_rightButtonTransformsMenu->addAction(action);

    setupRecentTransformsMenu();

    updateMenuStates();
}

void
MainWindow::setupHelpMenu()
{
    SVDEBUG << "MainWindow::setupHelpMenu" << endl;
    
    QMenu *menu = menuBar()->addMenu(tr("&Help"));
    menu->setTearOffEnabled(true);
    
    m_keyReference->setCategory(tr("Help"));

    IconLoader il;

    QString name = QApplication::applicationName();

    QAction *action = nullptr;

    action = new QAction(tr("&Instructions for %1").arg(name), this); 
    action->setStatusTip(tr("Show instructions for using %1").arg(name)); 
    connect(action, SIGNAL(triggered()), this, SLOT(introduction()));
    menu->addAction(action);

/*!!!    
    action = new QAction(il.load("help"), tr("&Help Reference"), this); 
    action->setShortcut(tr("F1"));
    action->setStatusTip(tr("Open the %1 reference manual").arg(name)); 
    connect(action, SIGNAL(triggered()), this, SLOT(help()));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
*/
    action = new QAction(tr("&Key and Mouse Reference"), this);
    action->setShortcut(tr("F2"));
    action->setStatusTip(tr("Open a window showing the keystrokes you can use in %1").arg(name));
    connect(action, SIGNAL(triggered()), this, SLOT(keyReference()));
    m_keyReference->registerShortcut(action);
    menu->addAction(action);
/*!!!
    action = new QAction(tr("What's &New In This Release?"), this); 
    action->setStatusTip(tr("List the changes in this release (and every previous release) of %1").arg(name)); 
    connect(action, SIGNAL(triggered()), this, SLOT(whatsNew()));
    menu->addAction(action);
*/    
    action = new QAction(tr("&About %1").arg(name), this); 
    action->setStatusTip(tr("Show information about %1").arg(name)); 
    connect(action, SIGNAL(triggered()), this, SLOT(about()));
    menu->addAction(action);
}

void
MainWindow::setupRecentFilesMenu()
{
    SVDEBUG << "MainWindow::setupRecentFilesMenu" << endl;
    
    m_recentFilesMenu->clear();
    vector<QString> files = m_recentFiles.getRecent();
    for (size_t i = 0; i < files.size(); ++i) {
        QString path = files[i];
        QAction *action = m_recentFilesMenu->addAction(path);
        action->setObjectName(path);
        connect(action, SIGNAL(triggered()), this, SLOT(openRecentFile()));
        if (i == 0) {
            action->setShortcut(tr("Ctrl+R"));
            m_keyReference->registerShortcut
                (tr("Re-open"),
                 action->shortcut().toString(),
                 tr("Re-open the current or most recently opened file"));
        }
    }
}

void
MainWindow::setupTemplatesMenu()
{
    SVDEBUG << "MainWindow::setupTemplatesMenu" << endl;
    
    m_templatesMenu->clear();

    QAction *defaultAction = m_templatesMenu->addAction(tr("Standard Waveform"));
    defaultAction->setObjectName("default");
    // All the apply actions need to have a main model to be useful:
    // canExportAudio captures that
    connect(this, SIGNAL(canExportAudio(bool)), defaultAction, SLOT(setEnabled(bool)));

    connect(defaultAction, SIGNAL(triggered()), this, SLOT(applyTemplate()));

    m_templatesMenu->addSeparator();

    QAction *action = nullptr;

    QStringList templates = ResourceFinder().getResourceFiles("templates", "svt");

    bool havePersonal = false;

    // (ordered by name)
    set<QString> byName;
    for (QString t : templates) {
        if (!t.startsWith(":")) havePersonal = true;
        byName.insert(QFileInfo(t).baseName());
    }

    for (QString t : byName) {
        if (t.toLower() == "default") continue;
        action = m_templatesMenu->addAction(t);
        // All the apply actions need to have a main model to be
        // useful: canExportAudio captures that
        connect(this, SIGNAL(canExportAudio(bool)), action, SLOT(setEnabled(bool)));
        connect(action, SIGNAL(triggered()), this, SLOT(applyTemplate()));
    }

    if (!templates.empty()) m_templatesMenu->addSeparator();

    if (!m_templateWatcher) {
        m_templateWatcher = new QFileSystemWatcher(this);
        m_templateWatcher->addPath(ResourceFinder().getResourceSaveDir("templates"));
        connect(m_templateWatcher, SIGNAL(directoryChanged(const QString &)),
                this, SLOT(setupTemplatesMenu()));
    }

    m_templatesMenu->addSeparator();

    QAction *setDefaultAction = m_templatesMenu->addAction(tr("Choose Default Template..."));
    setDefaultAction->setObjectName("set_default_template");
    connect(setDefaultAction, SIGNAL(triggered()), this, SLOT(preferences()));

    m_manageTemplatesAction->setEnabled(havePersonal);
}

void
MainWindow::chooseScore() // Added by YJ Oct 5, 2021
{
    m_scorePageDownButton->setEnabled(false);
    m_scorePageUpButton->setEnabled(false);

    auto scores = ScoreFinder::getScoreNames();
    set<string> byName;
    for (auto s: scores) {
        byName.insert(s);
    }

    QStringList items;
    for (auto n: byName) items << QString::fromStdString(n);

    if (items.empty()) {
        auto bundled = ScoreFinder::getBundledScoreDirectory();
        if (bundled != "") {
            QMessageBox::warning(this,
                                 tr("No score files found"),
                                 tr("No score files were found in the installed application bundle or in the scores directory \"%1\"").arg(QString::fromStdString(ScoreFinder::getUserScoreDirectory())),
                             QMessageBox::Ok);
        } else {
            QMessageBox::warning(this,
                                 tr("No score files found"),
                                 tr("No score files were found in the scores directory \"%1\"").arg(QString::fromStdString(ScoreFinder::getUserScoreDirectory())),
                                 QMessageBox::Ok);
        }
        return;
    }

    bool ok = false;
    QString scoreName = ListInputDialog::getItem
        (this, tr("Select a score"),
         tr("Please select the score of your recording:"),
         items, 0, &ok);

    if (!ok) {
        // user clicked Cancel
        return;
    }

    openScoreFile(scoreName, {});
}

void
MainWindow::openScoreFile()
{
    m_scorePageDownButton->setEnabled(false);
    m_scorePageUpButton->setEnabled(false);

    QString scoreDir =
        QString::fromStdString(ScoreFinder::getUserScoreDirectory());
    
    QFileDialog dialog(this);
    dialog.setNameFilter(tr("MEI score files (*.mei)"));
    dialog.setWindowTitle(tr("Choose a score file"));
    dialog.setDirectory(scoreDir);
    dialog.setAcceptMode(QFileDialog::AcceptOpen);
    dialog.setFileMode(QFileDialog::ExistingFile);
    if (!dialog.exec() || dialog.selectedFiles().empty()) return; // cancel

    QString scoreFile = dialog.selectedFiles()[0];
    QString scoreName = QFileInfo(scoreFile).completeBaseName();

    openScoreFile(scoreName, scoreFile);
}

void
MainWindow::deleteTemporaryScoreFiles()
{
    // Delete in reverse order of creation, so as to delete any
    // resulting empty directory after its contents
    for (auto itr = m_scoreFilesToDelete.rbegin();
         itr != m_scoreFilesToDelete.rend();
         ++itr) {
        auto f = *itr;
        std::error_code ec;
        SVDEBUG << "MainWindow::deleteTemporaryScoreFiles: Removing file \""
                << f << "\"" << endl;
        if (!std::filesystem::remove(f, ec)) {
            SVDEBUG << "MainWindow::deleteTemporaryScoreFiles: "
                    << "Failed to remove generated file \""
                    << f << "\": " << ec.message() << endl;
        }
    }
    m_scoreFilesToDelete.clear();
}

void
MainWindow::openScoreFile(QString scoreName, QString scoreFile)
{
    QString errorString;
    
    if (scoreFile == "") {
        scoreFile = QString::fromStdString
            (ScoreFinder::getScoreFile(scoreName.toStdString(), "mei"));
        if (scoreFile == "") {
            QMessageBox::warning(this,
                                 tr("Unable to load score"),
                                 tr("Unable to load score \"%1\": Score file (.mei) not found!")
                                 .arg(scoreName),
                                 QMessageBox::Ok);
            return;
        }
    }
        
    if (!m_scoreWidget->loadScoreFile(scoreName, scoreFile, errorString)) {
        QMessageBox::warning(this,
                             tr("Unable to load score"),
                             tr("Unable to load score \"%1\": %2")
                             .arg(scoreName).arg(errorString),
                             QMessageBox::Ok);
        return;
    }

    deleteTemporaryScoreFiles();
    
    m_scoreWidget->setInteractionMode(ScoreWidget::InteractionMode::Navigate);
    
    m_scoreId = scoreName;

    QSettings settings;
    settings.beginGroup("MainWindow");
    settings.setValue("sessiontemplate", "");
    settings.endGroup();

    newSession();
    m_score = Score();

    // Creating score structure
    string sname = scoreName.toStdString();
    string scoreDir = ScoreFinder::getUserScoreDirectory() + "/" + sname;

    if (!std::filesystem::exists(scoreDir)) {
        if (!QDir().mkpath(QString::fromStdString(scoreDir))) {
            SVCERR << "MainWindow::chooseScore: Failed to create score directory \"" << scoreDir << "\" for generated files" << endl;
            return;
        }
        m_scoreFilesToDelete.push_back(scoreDir);
    }

    auto generatedFiles = ScoreParser::generateScoreFiles
        (scoreDir, scoreName.toStdString(), scoreFile.toStdString());
    if (generatedFiles.empty()) {
        SVCERR << "MainWindow::chooseScore: Failed to generate score files in directory \"" << scoreDir << "\" from MEI file \"" << scoreFile << "\"" << endl;
        return;
    }
    m_scoreFilesToDelete.insert(m_scoreFilesToDelete.end(),
                                generatedFiles.begin(), generatedFiles.end());
    
    string soloPath = ScoreFinder::getScoreFile(sname, "solo");
    string meterPath = ScoreFinder::getScoreFile(sname, "meter");
    if (!m_score.initialize(soloPath)) {
        SVCERR << "MainWindow::chooseScore: Failed to load score data from solo file path \"" << soloPath << "\"" << endl;
        return;
    }
    if (!m_score.readMeter(meterPath)) {
        SVCERR << "MainWindow::chooseScore: Failed to load meter data from meter file path \"" << meterPath << "\"" << endl;
        return;
    }
    auto musicalEvents = m_score.getMusicalEvents();
    m_session.setMusicalEvents(m_scoreId, musicalEvents);
    m_scoreWidget->setMusicalEvents(musicalEvents);
    m_tempoCurveWidget->setMusicalEvents(musicalEvents);

    auto recordingDirectory =
        ScoreFinder::getUserRecordingDirectory(scoreName.toStdString(), false);
    if (recordingDirectory != "") {
        RecordDirectory::setRecordContainerDirectory
            (QString::fromStdString(recordingDirectory));
    }

    auto bundledRecordingDirectory =
        ScoreFinder::getBundledRecordingDirectory(scoreName.toStdString());
    if (bundledRecordingDirectory == "") {
        SVDEBUG << "MainWindow::chooseScore: Note: no bundled recording directory returned for score " << scoreName << endl;
        return;
    }

    // If we have a bundled recording directory and there are no audio
    // files in the user recording directory, override the find-file
    // dialog so that it looks in the bundled directory

    bool haveUserRecordings = false;
    if (recordingDirectory != "") {
        QDir dir(QString::fromUtf8(recordingDirectory.c_str()));
        QStringList nameFilters =
            AudioFileReaderFactory::getKnownExtensions().split(" ");
        if (!dir.entryList(nameFilters).empty()) {
            haveUserRecordings = true;
        } else {
            for (auto sub: dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                QDir subdir(dir.filePath(sub));
                if (!subdir.entryList(nameFilters).empty()) {
                    haveUserRecordings = true;
                    break;
                }
            }
        }
    }

    settings.beginGroup("FileFinder");
    settings.remove("audiopath");
    settings.remove("lastpath");
    settings.endGroup();

    SVDEBUG << "MainWindow::chooseScore: haveUserRecordings = "
            << haveUserRecordings << ", recordingDirectory = "
            << recordingDirectory << ", bundledRecordingDirectory = "
            << bundledRecordingDirectory << endl;
    
    // These aren't files, but they work anyway with the current file
    // finder
    if (haveUserRecordings) {
        m_audioFile = QString::fromStdString(recordingDirectory);
    } else {
        m_audioFile = QString::fromStdString(bundledRecordingDirectory);
    }
}

void
MainWindow::viewManagerPlaybackFrameChanged(sv_frame_t frame)
{
    if (m_followScore) {
        highlightFrameInScore(frame);
    }
}

void
MainWindow::highlightFrameInScore(sv_frame_t frame)
{
    QString label = m_scoreBasedFrameAligner->mapToScoreLabel(frame);
    if (label == "") {
        SVDEBUG << "highlightFrameInScore: Unable to map frame "
                << frame << " to a score label" << endl;
        return;
    }
    highlightLabelInScore(label);
    highlightLabelInTempoCurve(label);
}

void
MainWindow::highlightLabelInTempoCurve(QString label)
{
    m_tempoCurveWidget->setHighlightedPosition(label);
}

void
MainWindow::highlightLabelInScore(QString label)
{
    m_scoreWidget->setHighlightEventByLabel(label.toStdString());
    scoreInteractionEnded(m_scoreWidget->getInteractionMode());
}

void
MainWindow::activateLabelInScore(QString label)
{
    m_scoreWidget->activateEventByLabel(label.toStdString());
    scoreInteractionEnded(m_scoreWidget->getInteractionMode());
}

void
MainWindow::scoreSelectionChanged(Fraction start, bool atStart,
                                  ScoreWidget::EventLabel startLabel,
                                  Fraction end, bool atEnd,
                                  ScoreWidget::EventLabel endLabel)
{
    SVDEBUG << "MainWindow::scoreSelectionChanged: start = " << start
            << ", atStart = " << atStart << ", startLabel = " << startLabel
            << ", end = " << end << ", atEnd = " << atEnd << ", endLabel = "
            << endLabel << endl;

    QString qStartLabel = QString::fromStdString(startLabel);
    QString qEndLabel = QString::fromStdString(endLabel);
    
    if (atStart) {
        qStartLabel = tr("Start");
    }

    if (atEnd) {
        qEndLabel = tr("End"); 
    }

    m_selectFrom->setText(qStartLabel);
    m_selectTo->setText(qEndLabel);
    m_subsetOfScoreSelected = (!atStart || !atEnd);
    m_resetSelectionButton->setEnabled(m_subsetOfScoreSelected);
    updateAlignButtonText();
}

void
MainWindow::scorePageChanged(int page)
{
    SVDEBUG << "MainWindow::scorePageChanged(" << page << ")" << endl;
    int n = m_scoreWidget->getPageCount();
    m_scorePageDownButton->setEnabled(page > 0);
    m_scorePageUpButton->setEnabled(page + 1 < n);
    m_scorePageLabel->setText(tr("Page %1 of %2").arg(page + 1).arg(n));
}

void
MainWindow::scorePageDownButtonClicked()
{
    SVDEBUG << "MainWindow::scorePageDownButtonClicked" << endl;
    int page = m_scoreWidget->getCurrentPage();
    if (page > 0) {
        m_scoreWidget->showPage(page - 1);
    }
}

void
MainWindow::scorePageUpButtonClicked()
{
    SVDEBUG << "MainWindow::scorePageUpButtonClicked" << endl;
    int page = m_scoreWidget->getCurrentPage();
    if (page + 1 < m_scoreWidget->getPageCount()) {
        m_scoreWidget->showPage(page + 1);
    }
}

void
MainWindow::alignButtonClicked()
{
    if (m_chooseSmartCopyAction && m_chooseSmartCopyAction->isChecked()) {
        propagateAlignmentFromReference();
        return;
    }
    
    Fraction start, end;
    ScoreWidget::EventLabel startLabel, endLabel;
    sv_frame_t audioFrameStart = -1, audioFrameEnd = -1;
    
    if (m_subsetOfScoreSelected) {
        m_scoreWidget->getSelection(start, startLabel, end, endLabel);
    }

    if (!m_viewManager->getSelections().empty()) {
        m_viewManager->getSelection().getExtents(audioFrameStart, audioFrameEnd);
    }

    m_alignButton->setEnabled(false);

    if (m_subsetOfScoreSelected) {
        m_session.beginPartialAlignment(start.numerator, start.denominator,
                                        end.numerator, end.denominator,
                                        audioFrameStart, audioFrameEnd);
    } else {
        m_session.beginPartialAlignment(-1, -1, -1, -1,
                                        audioFrameStart, audioFrameEnd);
    }
}

void
MainWindow::scoreInteractionModeChanged(ScoreWidget::InteractionMode mode)
{
    SVDEBUG << "MainWindow::scoreInteractionModeChanged: mode = " << int(mode)
            << endl;
    
    ViewManager::ToolMode toolMode = ViewManager::NavigateMode;
    
    if (mode == ScoreWidget::InteractionMode::Edit) {
        toolMode = ViewManager::EditMode;
    } else if (mode == ScoreWidget::InteractionMode::SelectStart ||
               mode == ScoreWidget::InteractionMode::SelectEnd) {
        toolMode = ViewManager::SelectMode;
    }
    
    for (auto &p : m_toolActions) {
        if (p.first == toolMode && !p.second->isChecked()) {
            p.second->trigger();
            break;
        }
    }

    if (mode == ScoreWidget::InteractionMode::Edit) {
        m_session.signifyEditMode();
    } else {
        m_session.signifyNavigateMode();
    }

    m_selectFromButton->blockSignals(true);
    m_selectToButton->blockSignals(true);
    
    m_selectFromButton->setChecked
        (mode == ScoreWidget::InteractionMode::SelectStart);
    m_selectToButton->setChecked
        (mode == ScoreWidget::InteractionMode::SelectEnd);

    m_selectFromButton->blockSignals(false);
    m_selectToButton->blockSignals(false);
}

void
MainWindow::scoreLocationHighlighted(Fraction location,
                                     ScoreWidget::EventLabel label,
                                     ScoreWidget::InteractionMode mode)
{
    actOnScoreLocation(location, label, mode, false);
}

void
MainWindow::scoreLocationActivated(Fraction location,
                                   ScoreWidget::EventLabel label,
                                   ScoreWidget::InteractionMode mode)
{
    actOnScoreLocation(location, label, mode, true);
}

void
MainWindow::followScoreToggled()
{
    QAction *a = qobject_cast<QAction *>(sender());
    if (a) {
        m_followScore = a->isChecked();
    }
}

void
MainWindow::actOnScoreLocation(Fraction location,
                               ScoreWidget::EventLabel label,
                               ScoreWidget::InteractionMode mode,
                               bool activated)
{
    SVDEBUG << "MainWindow::actOnScoreLocation(" << location << ", " << label << ", " << int(mode) << ", " << activated << ")" << endl;
    
    // See comments in highlightFrameInScore above

    TimeInstantLayer *targetLayer = m_session.getOnsetsLayer();
    Pane *targetPane = m_session.getPaneContainingOnsetsLayer();

    if (!targetLayer || !targetPane || !m_viewManager) {
        SVDEBUG << "MainWindow::actOnScorePosition: missing either target layer or view manager" << endl;
        return;
    }

//    targetLayer->setPermitValueEditOfSegmentation(false);
    m_paneStack->setCurrentLayer(targetPane, targetLayer);

    ModelId targetId = targetLayer->getModel();
    const auto targetModel = ModelById::getAs<SparseOneDimensionalModel>(targetId);
    if (!targetModel) {
        SVDEBUG << "MainWindow::actOnScorePosition: missing target model" << endl;
        return;
    }

    sv_frame_t frame = 0;
    m_scoreBasedFrameAligner->mapFromScoreLabelAndProportion
        (targetId, QString::fromStdString(label), 0.0, frame);

    SVDEBUG << "MainWindow::actOnScorePosition: mapped location " << location
            << ", label " << label << " to frame " << frame << endl;

    targetLayer->overrideHighlightForPointsAt(frame);
    
    if (activated && m_followScore) {
        m_viewManager->setGlobalCentreFrame(frame);
        m_viewManager->setPlaybackFrame(frame);
    }
}

void
MainWindow::scoreInteractionEnded(ScoreWidget::InteractionMode)
{
    TimeInstantLayer *targetLayer = m_session.getOnsetsLayer();
    if (targetLayer) {
        targetLayer->removeOverrideHighlight();
    }
}

void
MainWindow::alignmentEventIlluminated(sv_frame_t frame, QString label)
{
    SVDEBUG << "MainWindow::alignmentEventIlluminated("
            << frame << ", " << label << ")" << endl;
    
    if (m_scoreWidget->getInteractionMode() ==
        ScoreWidget::InteractionMode::Edit) {

        if (label == "") {
            highlightFrameInScore(frame);
        } else {
            highlightLabelInScore(label);
        }
    }
}

void
MainWindow::alignmentFailedToRun(QString message)
{
    // NB we also have alignmentFailed which is received when the
    // "classic SV" audio-to-audio alignment fails. This is for
    // audio-to-score

    QMessageBox::warning
        (this,
         tr("Unable to calculate alignment"),
         tr("<b>Alignment calculation failed</b><p>Failed to align audio with score:<p>%1")
         .arg(message),
         QMessageBox::Ok);
}

void
MainWindow::populateScoreAlignerChoiceMenu()
{
    delete m_alignerChoice->menu();
    m_alignerChoice->setMenu(nullptr);
    
    auto transforms =
        ScoreAlignmentTransform::getAvailableAlignmentTransforms();

    SVDEBUG << "MainWindow::populateScoreAlignerChoiceMenu: Found "
            << transforms.size() << " transforms" << endl;

    if (transforms.empty()) {
        QMessageBox::warning
            (this,
             tr("No suitable alignment plugins found"),
             tr("<b>No alignment plugins found</b><p>Failed to find any suitable plugins for audio to score alignment. Alignment will not be available"),
             QMessageBox::Ok);
        return;
    }

    QSettings settings;
    settings.beginGroup("ScoreAlignment");
    QString preferredTransformKey = "transformId";
    TransformId defaultId =
        ScoreAlignmentTransform::getDefaultAlignmentTransform();

    SVDEBUG << "MainWindow::populateScoreAlignerChoiceMenu: Default transform is \"" << defaultId << "\"" << endl;
    
    if (settings.contains(preferredTransformKey)) {
        TransformId id = settings.value
            (preferredTransformKey, defaultId).toString();
        if (id == Session::smartCopyTransformId) {
            id = defaultId;
        }
        bool found = false;
        for (const auto &t : transforms) {
            if (t.identifier == id) {
                found = true;
                break;
            }
        }
        if (found) {
            SVDEBUG << "MainWindow::populateScoreAlignerChoiceMenu: Saved transform is \"" << id << "\"" << endl;
            defaultId = id;
            m_session.setAlignmentTransformId(id);
        } else {
            QMessageBox::warning
                (this,
                 tr("Previous alignment plugin not found"),
                 tr("<b>The previously-selected alignment plugin was not found</b><p>The previously-selected alignment plugin transform \"%1\" was not found on the system, using the default setting \"%2\"").arg(id).arg(defaultId),
                 QMessageBox::Ok);
        }
    }
    settings.endGroup();
    
    QMenu *menu = new QMenu(this);
    QActionGroup *alignerGroup = new QActionGroup(menu);
    for (const auto &t : transforms) {
        QString label = tr("%1 by %2").arg(t.pluginName).arg(t.maker);
        auto action = menu->addAction(label, [=]() {
            scoreAlignerChosen(t.identifier);
        });
        action->setData(t.identifier);
        action->setCheckable(true);
        action->setChecked(t.identifier == defaultId);
        alignerGroup->addAction(action);
    }
    m_chooseSmartCopyAction = menu->addAction(tr("Smart Copy from First Recording"), [=]() {
        scoreAlignerChosen(Session::smartCopyTransformId);
    });
    m_chooseSmartCopyAction->setData(Session::smartCopyTransformId);
    m_chooseSmartCopyAction->setCheckable(true);
    m_chooseSmartCopyAction->setChecked(false);
    alignerGroup->addAction(m_chooseSmartCopyAction);
    m_alignerChoice->setMenu(menu);
}

void
MainWindow::scoreAlignerChosen(TransformId id)
{
    SVDEBUG << "MainWindow::scoreAlignerChosen: Chosen transform is \"" << id << "\"" << endl;

    m_session.setAlignmentTransformId(id);

    QSettings settings;
    settings.beginGroup("ScoreAlignment");
    QString preferredTransformKey = "transformId";
    settings.setValue(preferredTransformKey, id);
    settings.endGroup();

    updateMenuStates();
}

void
MainWindow::layerAdded(Layer *)
{
    SVDEBUG << "MainWindow::layerAdded" << endl;
}

void
MainWindow::alignmentReadyForReview(Pane *onsetsPane, Layer *onsetsLayer)
{
    SVDEBUG << "MainWindow::alignmentReadyForReview" << endl;

    if (!onsetsPane || !onsetsLayer) {
        SVDEBUG << "MainWindow::alignmentReadyForReview: no pane and/or layer provided" << endl;
        return;
    }

    m_paneStack->setCurrentLayer(onsetsPane, onsetsLayer);

    m_alignAcceptReject->setFixedSize(m_alignCommands->size());
    
    m_alignCommands->hide();
    m_alignAcceptReject->show();

    m_tempoCurveWidget->update();
    
    updateMenuStates();
}

void
MainWindow::alignmentModified()
{
    SVDEBUG << "MainWindow::alignmentModified" << endl;

    m_tempoCurveWidget->update();
    
    updateMenuStates();
}

void
MainWindow::alignmentAccepted()
{
    SVDEBUG << "MainWindow::alignmentAccepted" << endl;

    m_alignAcceptReject->hide();
    m_alignCommands->show();
    m_alignButton->setEnabled(true);

    TimeInstantLayer *onsetsLayer = m_session.getOnsetsLayer();
    Pane *onsetsPane = m_session.getPaneContainingOnsetsLayer();
    if (!onsetsLayer) {
        SVDEBUG << "MainWindow::alignmentAccepted: can't find an onsets layer!" << endl;
        return;
    }

    m_paneStack->setCurrentLayer(onsetsPane, onsetsLayer);

    m_tempoCurveWidget->update();
    
    updateMenuStates();
}

void
MainWindow::alignmentRejected()
{
    SVDEBUG << "MainWindow::alignmentRejected" << endl;

    m_alignAcceptReject->hide();
    m_alignCommands->show();
    m_alignButton->setEnabled(true);

    TimeInstantLayer *onsetsLayer = m_session.getOnsetsLayer();
    Pane *onsetsPane = m_session.getPaneContainingOnsetsLayer();
    if (!onsetsLayer) {
        SVDEBUG << "MainWindow::alignmentRejected: can't find an onsets layer!" << endl;
        return;
    }

    m_paneStack->setCurrentLayer(onsetsPane, onsetsLayer);

    m_tempoCurveWidget->update();
    
    updateMenuStates();
}

void
MainWindow::setupRecentTransformsMenu()
{
    SVDEBUG << "MainWindow::setupRecentTransformsMenu" << endl;
    
    m_recentTransformsMenu->clear();
    vector<QString> transforms = m_recentTransforms.getRecent();
    for (size_t i = 0; i < transforms.size(); ++i) {
        TransformActionReverseMap::iterator ti =
            m_transformActionsReverse.find(transforms[i]);
        if (ti == m_transformActionsReverse.end()) {
            SVCERR << "WARNING: MainWindow::setupRecentTransformsMenu: "
                   << "Unknown transform \"" << transforms[i]
                   << "\" in recent transforms list" << endl;
            continue;
        }
        if (i == 0) {
            ti->second->setShortcut(tr("Ctrl+T"));
            m_keyReference->registerShortcut
                (tr("Repeat Transform"),
                 ti->second->shortcut().toString(),
                 tr("Re-select the most recently run transform"));
        } else {
            ti->second->setShortcut(QString(""));
        }
        m_recentTransformsMenu->addAction(ti->second);
    }
}

void
MainWindow::setupExistingLayersMenus()
{
    SVDEBUG << "MainWindow::setupExistingLayersMenus" << endl;
    
    if (!m_existingLayersMenu) return; // should have been created by setupMenus

//    SVDEBUG << "MainWindow::setupExistingLayersMenu" << endl;

    Profiler profiler1("MainWindow::setupExistingLayersMenu");

    m_existingLayersMenu->clear();
    for (auto a: m_existingLayerActions) {
        delete a.first;
    }
    m_existingLayerActions.clear();

    m_sliceMenu->clear();
    for (auto a: m_sliceActions) {
        delete a.first;
    }
    m_sliceActions.clear();

    IconLoader il;

    vector<Layer *> orderedLayers;
    set<Layer *> observedLayers;
    set<Layer *> sliceableLayers;

    LayerFactory *factory = LayerFactory::getInstance();

    for (int i = 0; i < m_paneStack->getPaneCount(); ++i) {

        Pane *pane = m_paneStack->getPane(i);
        if (!pane) continue;

        for (int j = 0; j < pane->getLayerCount(); ++j) {

            Layer *layer = pane->getLayer(j);
            if (!layer) continue;
            if (observedLayers.find(layer) != observedLayers.end()) {
//                cerr << "found duplicate layer " << layer << endl;
                continue;
            }

//            cerr << "found new layer " << layer << " (name = " 
//                      << layer->getLayerPresentationName() << ")" << endl;

            orderedLayers.push_back(layer);
            observedLayers.insert(layer);

            if (factory->isLayerSliceable(layer)) {
                sliceableLayers.insert(layer);
            }
        }
    }

    Profiler profiler3("MainWindow::setupExistingLayersMenu: after sorting");
    
    map<QString, int> observedNames;

    for (size_t i = 0; i < orderedLayers.size(); ++i) {
        
        Layer *layer = orderedLayers[i];

        QString name = layer->getLayerPresentationName();
        int n = ++observedNames[name];
        if (n > 1) name = QString("%1 <%2>").arg(name).arg(n);

        QIcon icon = il.load(factory->getLayerIconName
                             (factory->getLayerType(layer)));

        QAction *action = new QAction(icon, name, this);
        connect(action, SIGNAL(triggered()), this, SLOT(addLayer()));
        connect(this, SIGNAL(canAddLayer(bool)), action, SLOT(setEnabled(bool)));
        m_existingLayerActions.push_back({ action, layer });

        m_existingLayersMenu->addAction(action);

        if (sliceableLayers.find(layer) != sliceableLayers.end()) {
            action = new QAction(icon, name, this);
            connect(action, SIGNAL(triggered()), this, SLOT(addLayer()));
            connect(this, SIGNAL(canAddLayer(bool)), action, SLOT(setEnabled(bool)));
            m_sliceActions.push_back({ action, layer });
            m_sliceMenu->addAction(action);
        }
    }

    m_sliceMenu->setEnabled(!m_sliceActions.empty());
}

void
MainWindow::setupToolbars()
{
    SVDEBUG << "MainWindow::setupToolbars" << endl;
    
    m_keyReference->setCategory(tr("Playback and Transport Controls"));

    IconLoader il;

    QMenu *menu = m_playbackMenu = menuBar()->addMenu(tr("Play&back"));
    menu->setTearOffEnabled(true);
    m_rightButtonMenu->addSeparator();
    m_rightButtonPlaybackMenu = m_rightButtonMenu->addMenu(tr("Playback"));

    QToolBar *toolbar = addToolBar(tr("Playback Toolbar"));

    m_rwdStartAction = toolbar->addAction(il.load("rewind-start"),
                                          tr("Rewind to Start"));
    m_rwdStartAction->setShortcut(tr("Home"));
    m_rwdStartAction->setStatusTip(tr("Rewind to the start"));
    connect(m_rwdStartAction, SIGNAL(triggered()), this, SLOT(rewindStart()));
    connect(this, SIGNAL(canPlay(bool)), m_rwdStartAction, SLOT(setEnabled(bool)));

    m_rwdAction = toolbar->addAction(il.load("rewind"), tr("Rewind"));
    m_rwdAction->setShortcut(tr("PgUp"));
    m_rwdAction->setStatusTip(tr("Rewind to the previous time instant or time ruler notch"));
    connect(m_rwdAction, SIGNAL(triggered()), this, SLOT(rewind()));
    connect(this, SIGNAL(canRewind(bool)), m_rwdAction, SLOT(setEnabled(bool)));

    m_rwdSimilarAction = new QAction(tr("Rewind to Similar Point"), this);
    m_rwdSimilarAction->setShortcut(tr("Shift+PgUp"));
    m_rwdSimilarAction->setStatusTip(tr("Rewind to the previous similarly valued time instant"));
    connect(m_rwdSimilarAction, SIGNAL(triggered()), this, SLOT(rewindSimilar()));
    connect(this, SIGNAL(canRewind(bool)), m_rwdSimilarAction, SLOT(setEnabled(bool)));

    m_playAction = toolbar->addAction(il.load("playpause"),
                                      tr("Play / Pause"));
    m_playAction->setCheckable(true);

    /*: This text is a shortcut label referring to the space-bar on
        the keyboard. It probably should not be translated, and
        certainly should not be translated as if referring to an empty
        void or to the extra-terrestrial universe.
     */
    m_playAction->setShortcut(tr("Space"));

    m_playAction->setStatusTip(tr("Start or stop playback from the current position"));
    connect(m_playAction, SIGNAL(triggered()), this, SLOT(play()));
    connect(m_playSource, SIGNAL(playStatusChanged(bool)),
            m_playAction, SLOT(setChecked(bool)));
    connect(m_playSource, SIGNAL(playStatusChanged(bool)),
            this, SLOT(playStatusChanged(bool)));
    connect(this, SIGNAL(canPlay(bool)), m_playAction, SLOT(setEnabled(bool)));

    m_ffwdAction = toolbar->addAction(il.load("ffwd"),
                                      tr("Fast Forward"));
    m_ffwdAction->setShortcut(tr("PgDown"));
    m_ffwdAction->setStatusTip(tr("Fast-forward to the next time instant or time ruler notch"));
    connect(m_ffwdAction, SIGNAL(triggered()), this, SLOT(ffwd()));
    connect(this, SIGNAL(canFfwd(bool)), m_ffwdAction, SLOT(setEnabled(bool)));

    m_ffwdSimilarAction = new QAction(tr("Fast Forward to Similar Point"), this);
    m_ffwdSimilarAction->setShortcut(tr("Shift+PgDown"));
    m_ffwdSimilarAction->setStatusTip(tr("Fast-forward to the next similarly valued time instant"));
    connect(m_ffwdSimilarAction, SIGNAL(triggered()), this, SLOT(ffwdSimilar()));
    connect(this, SIGNAL(canFfwd(bool)), m_ffwdSimilarAction, SLOT(setEnabled(bool)));

    m_ffwdEndAction = toolbar->addAction(il.load("ffwd-end"),
                                         tr("Fast Forward to End"));
    m_ffwdEndAction->setShortcut(tr("End"));
    m_ffwdEndAction->setStatusTip(tr("Fast-forward to the end"));
    connect(m_ffwdEndAction, SIGNAL(triggered()), this, SLOT(ffwdEnd()));
    connect(this, SIGNAL(canPlay(bool)), m_ffwdEndAction, SLOT(setEnabled(bool)));

    m_recordAction = toolbar->addAction(il.load("record"),
                                        tr("Record"));
    m_recordAction->setCheckable(true);
    m_recordAction->setShortcut(tr("Ctrl+Space"));
    m_recordAction->setStatusTip(tr("Record a new audio file"));
    connect(m_recordAction, SIGNAL(triggered()), this, SLOT(record()));
    connect(m_recordTarget, SIGNAL(recordStatusChanged(bool)),
            m_recordAction, SLOT(setChecked(bool)));
    connect(this, SIGNAL(canRecord(bool)),
            m_recordAction, SLOT(setEnabled(bool)));

    toolbar = addToolBar(tr("Play Mode Toolbar"));
    
    m_playSelectionAction = toolbar->addAction(il.load("playselection"),
                                               tr("Constrain Playback to Selection"));
    m_playSelectionAction->setCheckable(true);
    m_playSelectionAction->setChecked(m_viewManager->getPlaySelectionMode());
    m_playSelectionAction->setShortcut(tr("s"));
    m_playSelectionAction->setStatusTip(tr("Constrain playback to the selected regions"));
    connect(m_viewManager, SIGNAL(playSelectionModeChanged(bool)),
            m_playSelectionAction, SLOT(setChecked(bool)));
    connect(m_playSelectionAction, SIGNAL(triggered()), this, SLOT(playSelectionToggled()));
    connect(this, SIGNAL(canPlaySelection(bool)), m_playSelectionAction, SLOT(setEnabled(bool)));

    m_playLoopAction = toolbar->addAction(il.load("playloop"),
                                          tr("Loop Playback"));
    m_playLoopAction->setCheckable(true);
    m_playLoopAction->setChecked(m_viewManager->getPlayLoopMode());
    m_playLoopAction->setShortcut(tr("l"));
    m_playLoopAction->setStatusTip(tr("Loop playback"));
    connect(m_viewManager, SIGNAL(playLoopModeChanged(bool)),
            m_playLoopAction, SLOT(setChecked(bool)));
    connect(m_playLoopAction, SIGNAL(triggered()), this, SLOT(playLoopToggled()));
    connect(this, SIGNAL(canPlay(bool)), m_playLoopAction, SLOT(setEnabled(bool)));

    m_soloAction = toolbar->addAction(il.load("solo"),
                                      tr("Solo Current Pane"));
    m_soloAction->setCheckable(true);
    m_soloAction->setChecked(m_viewManager->getPlaySoloMode());
    m_prevSolo = m_viewManager->getPlaySoloMode();
    m_soloAction->setShortcut(tr("o"));
    m_soloAction->setStatusTip(tr("Solo the current pane during playback"));
    connect(m_viewManager, SIGNAL(playSoloModeChanged(bool)),
            m_soloAction, SLOT(setChecked(bool)));
    connect(m_soloAction, SIGNAL(triggered()), this, SLOT(playSoloToggled()));
    connect(this, SIGNAL(canChangeSolo(bool)), m_soloAction, SLOT(setEnabled(bool)));

    QAction *alAction = toolbar->addAction(il.load("align"),
                                           tr("Link Audio and Score Positions"));
    alAction->setCheckable(true);
    alAction->setChecked(m_followScore);
    alAction->setStatusTip(tr("Track the score position in the audio panes"));
    alAction->setEnabled(true);
    connect(alAction, SIGNAL(triggered()), this, SLOT(followScoreToggled()));

    m_keyReference->registerShortcut(m_playAction);
    m_keyReference->registerShortcut(m_recordAction);
    m_keyReference->registerShortcut(m_playSelectionAction);
    m_keyReference->registerShortcut(m_playLoopAction);
    m_keyReference->registerShortcut(m_soloAction);
    m_keyReference->registerShortcut(alAction);
    m_keyReference->registerShortcut(m_rwdAction);
    m_keyReference->registerShortcut(m_ffwdAction);
    m_keyReference->registerShortcut(m_rwdSimilarAction);
    m_keyReference->registerShortcut(m_ffwdSimilarAction);
    m_keyReference->registerShortcut(m_rwdStartAction);
    m_keyReference->registerShortcut(m_ffwdEndAction);

    menu->addAction(m_playAction);
    menu->addAction(m_recordAction);
    menu->addAction(m_playSelectionAction);
    menu->addAction(m_playLoopAction);
    menu->addAction(m_soloAction);
    menu->addAction(alAction);
    menu->addSeparator();
    menu->addAction(m_rwdAction);
    menu->addAction(m_ffwdAction);
    menu->addSeparator();
    menu->addAction(m_rwdSimilarAction);
    menu->addAction(m_ffwdSimilarAction);
    menu->addSeparator();
    menu->addAction(m_rwdStartAction);
    menu->addAction(m_ffwdEndAction);
    menu->addSeparator();
    menu->addAction(m_recordAction);
    menu->addSeparator();

    m_rightButtonPlaybackMenu->addAction(m_playAction);
    m_rightButtonPlaybackMenu->addAction(m_playSelectionAction);
    m_rightButtonPlaybackMenu->addAction(m_playLoopAction);
    m_rightButtonPlaybackMenu->addAction(m_soloAction);
    m_rightButtonPlaybackMenu->addAction(alAction);
    m_rightButtonPlaybackMenu->addSeparator();
    m_rightButtonPlaybackMenu->addAction(m_rwdAction);
    m_rightButtonPlaybackMenu->addAction(m_ffwdAction);
    m_rightButtonPlaybackMenu->addSeparator();
    m_rightButtonPlaybackMenu->addAction(m_rwdStartAction);
    m_rightButtonPlaybackMenu->addAction(m_ffwdEndAction);
    m_rightButtonPlaybackMenu->addSeparator();
    m_rightButtonPlaybackMenu->addAction(m_recordAction);
    m_rightButtonPlaybackMenu->addSeparator();

    QAction *fastAction = menu->addAction(tr("Speed Up"));
    fastAction->setShortcut(tr("Ctrl+PgUp"));
    fastAction->setStatusTip(tr("Time-stretch playback to speed it up without changing pitch"));
    connect(fastAction, SIGNAL(triggered()), this, SLOT(speedUpPlayback()));
    connect(this, SIGNAL(canSpeedUpPlayback(bool)), fastAction, SLOT(setEnabled(bool)));
    
    QAction *slowAction = menu->addAction(tr("Slow Down"));
    slowAction->setShortcut(tr("Ctrl+PgDown"));
    slowAction->setStatusTip(tr("Time-stretch playback to slow it down without changing pitch"));
    connect(slowAction, SIGNAL(triggered()), this, SLOT(slowDownPlayback()));
    connect(this, SIGNAL(canSlowDownPlayback(bool)), slowAction, SLOT(setEnabled(bool)));

    QAction *normalAction = menu->addAction(tr("Restore Normal Speed"));
    normalAction->setShortcut(tr("Ctrl+Home"));
    normalAction->setStatusTip(tr("Restore non-time-stretched playback"));
    connect(normalAction, SIGNAL(triggered()), this, SLOT(restoreNormalPlayback()));
    connect(this, SIGNAL(canChangePlaybackSpeed(bool)), normalAction, SLOT(setEnabled(bool)));

    m_keyReference->registerShortcut(fastAction);
    m_keyReference->registerShortcut(slowAction);
    m_keyReference->registerShortcut(normalAction);

    m_rightButtonPlaybackMenu->addAction(fastAction);
    m_rightButtonPlaybackMenu->addAction(slowAction);
    m_rightButtonPlaybackMenu->addAction(normalAction);

    toolbar = addToolBar(tr("Edit Toolbar"));
    CommandHistory::getInstance()->registerToolbar(toolbar);

    toolbar = addToolBar(tr("Tools Toolbar"));
    QActionGroup *group = new QActionGroup(this);
    m_toolActions.clear();
    
    m_keyReference->setCategory(tr("Tool Selection"));
    QAction *action = toolbar->addAction(il.load("navigate"), tr("Navigate"));
    action->setCheckable(true);
    action->setChecked(true);
    action->setShortcut(tr("1"));
    action->setStatusTip(tr("Navigate"));
    connect(action, SIGNAL(triggered()), this, SLOT(toolNavigateSelected()));
    connect(this, SIGNAL(replacedDocument()), action, SLOT(trigger()));
    group->addAction(action);
    m_keyReference->registerShortcut(action);
    m_toolActions.push_back({ ViewManager::NavigateMode, action });
    
    m_keyReference->setCategory
        (tr("Navigate Tool Mouse Actions"));
    m_keyReference->registerShortcut
        (tr("Navigate"), tr("Left"), 
         tr("Click left button and drag to move around"));
    m_keyReference->registerShortcut
        (tr("Zoom to Area"), tr("Shift+Left"), 
         tr("Shift-click left button and drag to zoom to a rectangular area"));
    m_keyReference->registerShortcut
        (tr("Relocate"), tr("Double-Click Left"), 
         tr("Double-click left button to jump to clicked location"));
    m_keyReference->registerShortcut
        (tr("Edit"), tr("Double-Click Left"), 
         tr("Double-click left button on an item to edit it"));

    m_keyReference->setCategory(tr("Tool Selection"));
    action = toolbar->addAction(il.load("select"), tr("Select"));
    action->setCheckable(true);
    action->setShortcut(tr("2"));
    action->setStatusTip(tr("Select ranges"));
    connect(action, SIGNAL(triggered()), this, SLOT(toolSelectSelected()));
    group->addAction(action);
    m_keyReference->registerShortcut(action);
    m_toolActions.push_back({ ViewManager::SelectMode, action });
        
    m_keyReference->setCategory
        (tr("Select Tool Mouse Actions"));
    m_keyReference->registerShortcut
        (tr("Select"), tr("Left"), 
         tr("Click left button and drag to select region; drag region edge to resize"));
#ifdef Q_OS_MAC
    m_keyReference->registerShortcut
        (tr("Multi Select"), tr("Ctrl+Left"), 
         tr("Cmd-click left button and drag to select an additional region"));
#else
    m_keyReference->registerShortcut
        (tr("Multi Select"), tr("Ctrl+Left"), 
         tr("Ctrl-click left button and drag to select an additional region"));
#endif
    m_keyReference->registerShortcut
        (tr("Fine Select"), tr("Shift+Left"), 
        tr("Shift-click left button and drag to select without snapping to items or grid"));

    m_keyReference->setCategory(tr("Tool Selection"));
    action = toolbar->addAction(il.load("move"), tr("Edit"));
    action->setCheckable(true);
    action->setShortcut(tr("3"));
    action->setStatusTip(tr("Edit items in layer"));
    connect(action, SIGNAL(triggered()), this, SLOT(toolEditSelected()));
    connect(this, SIGNAL(canEditLayer(bool)), action, SLOT(setEnabled(bool)));
    group->addAction(action);
    m_keyReference->registerShortcut(action);
    m_toolActions.push_back({ ViewManager::EditMode, action });
    
    m_keyReference->setCategory
        (tr("Edit Tool Mouse Actions"));
    m_keyReference->registerShortcut
        (tr("Move"), tr("Left"), 
        tr("Click left button on an item or selected region and drag to move"));
    m_keyReference->registerShortcut
        (tr("Edit"), tr("Double-Click Left"), 
        tr("Double-click left button on an item to edit it"));

    m_keyReference->setCategory(tr("Tool Selection"));

    /*!!!
    action = toolbar->addAction(il.load("draw"), tr("Draw"));
    action->setCheckable(true);
    action->setShortcut(tr("4"));
    action->setStatusTip(tr("Draw new items in layer"));
    connect(action, SIGNAL(triggered()), this, SLOT(toolDrawSelected()));
    connect(this, SIGNAL(canEditLayer(bool)), action, SLOT(setEnabled(bool)));
    group->addAction(action);
    m_keyReference->registerShortcut(action);
    m_toolActions.push_back({ ViewManager::DrawMode, action });

    m_keyReference->setCategory
        (tr("Draw Tool Mouse Actions"));
    m_keyReference->registerShortcut
        (tr("Draw"), tr("Left"), 
        tr("Click left button and drag to create new item"));

    m_keyReference->setCategory(tr("Tool Selection"));
    action = toolbar->addAction(il.load("erase"), tr("Erase"));
    action->setCheckable(true);
    action->setShortcut(tr("5"));
    action->setStatusTip(tr("Erase items from layer"));
    connect(action, SIGNAL(triggered()), this, SLOT(toolEraseSelected()));
    connect(this, SIGNAL(canEditLayer(bool)), action, SLOT(setEnabled(bool)));
    group->addAction(action);
    m_keyReference->registerShortcut(action);
    m_toolActions.push_back({ ViewManager::EraseMode, action });

    m_keyReference->setCategory
        (tr("Erase Tool Mouse Actions"));
    m_keyReference->registerShortcut
        (tr("Erase"), tr("Left"), 
        tr("Click left button on an item to remove it from the layer"));
    */
    m_keyReference->setCategory(tr("Tool Selection"));
    action = toolbar->addAction(il.load("measure"), tr("Measure"));
    action->setCheckable(true);
    action->setShortcut(tr("6"));
    action->setStatusTip(tr("Make measurements in layer"));
    connect(action, SIGNAL(triggered()), this, SLOT(toolMeasureSelected()));
    connect(this, SIGNAL(canMeasureLayer(bool)), action, SLOT(setEnabled(bool)));
    group->addAction(action);
    m_keyReference->registerShortcut(action);
    m_toolActions.push_back({ ViewManager::MeasureMode, action });

    m_keyReference->setCategory
        (tr("Measure Tool Mouse Actions"));
    m_keyReference->registerShortcut
        (tr("Measure Area"), tr("Left"), 
        tr("Click left button and drag to measure a rectangular area"));
    m_keyReference->registerShortcut
        (tr("Measure Item"), tr("Double-Click Left"), 
        tr("Click left button and drag to measure extents of an item or shape"));
    m_keyReference->registerShortcut
        (tr("Zoom to Area"), tr("Shift+Left"), 
        tr("Shift-click left button and drag to zoom to a rectangular area"));

    toolNavigateSelected();

    Pane::registerShortcuts(*m_keyReference);
}

void
MainWindow::connectLayerEditDialog(ModelDataTableDialog *dialog)
{
    MainWindowBase::connectLayerEditDialog(dialog);
    QToolBar *toolbar = dialog->getPlayToolbar();
    if (toolbar) {
        toolbar->addAction(m_rwdStartAction);
        toolbar->addAction(m_rwdAction);
        toolbar->addAction(m_playAction);
        toolbar->addAction(m_ffwdAction);
        toolbar->addAction(m_ffwdEndAction);
    }
}

void
MainWindow::updateMenuStates()
{
//    SVDEBUG << "MainWindow::updateMenuStates" << endl;
    
    MainWindowBase::updateMenuStates();

    Pane *currentPane = nullptr;
    Layer *currentLayer = nullptr;

    if (m_paneStack) currentPane = m_paneStack->getCurrentPane();
    if (currentPane) currentLayer = currentPane->getSelectedLayer();

    bool haveCurrentPane =
        (currentPane != nullptr);
    bool haveCurrentLayer =
        (haveCurrentPane &&
         (currentLayer != nullptr));
    bool havePlayTarget =
        (m_playTarget != nullptr || m_audioIO != nullptr);
    bool haveSelection = 
        (m_viewManager &&
         !m_viewManager->getSelections().empty());
    bool haveCurrentEditableLayer =
        (haveCurrentLayer &&
         currentLayer->isLayerEditable());
    bool haveCurrentTimeInstantsLayer = 
        (haveCurrentLayer &&
         dynamic_cast<TimeInstantLayer *>(currentLayer));
    bool haveCurrentTimeValueLayer = 
        (haveCurrentLayer &&
         dynamic_cast<TimeValueLayer *>(currentLayer));
    
    bool alignMode = m_viewManager && m_viewManager->getAlignMode();
    emit canChangeSolo(havePlayTarget && !alignMode);

    emit canChangePlaybackSpeed(true);
    int v = m_playSpeed->value();
    emit canSpeedUpPlayback(v < m_playSpeed->maximum());
    emit canSlowDownPlayback(v > m_playSpeed->minimum());

    if (m_viewManager && 
        (m_viewManager->getToolMode() == ViewManager::MeasureMode)) {
        emit canDeleteSelection(haveCurrentLayer);
        m_deleteSelectedAction->setText(tr("&Delete Current Measurement"));
        m_deleteSelectedAction->setStatusTip(tr("Delete the measurement currently under the mouse pointer"));
    } else {
        emit canDeleteSelection(haveSelection && haveCurrentEditableLayer);
        m_deleteSelectedAction->setText(tr("&Delete Selected Items"));
        m_deleteSelectedAction->setStatusTip(tr("Delete items in current selection from the current layer"));
    }

    if (m_ffwdAction && m_rwdAction) {
        if (haveCurrentTimeInstantsLayer) {
            m_ffwdAction->setText(tr("Fast Forward to Next Instant"));
            m_ffwdAction->setStatusTip(tr("Fast forward to the next time instant in the current layer"));
            m_rwdAction->setText(tr("Rewind to Previous Instant"));
            m_rwdAction->setStatusTip(tr("Rewind to the previous time instant in the current layer"));
        } else if (haveCurrentTimeValueLayer) {
            m_ffwdAction->setText(tr("Fast Forward to Next Point"));
            m_ffwdAction->setStatusTip(tr("Fast forward to the next point in the current layer"));
            m_rwdAction->setText(tr("Rewind to Previous Point"));
            m_rwdAction->setStatusTip(tr("Rewind to the previous point in the current layer"));
        } else {
            m_ffwdAction->setText(tr("Fast Forward"));
            m_ffwdAction->setStatusTip(tr("Fast forward"));
            m_rwdAction->setText(tr("Rewind"));
            m_rwdAction->setStatusTip(tr("Rewind"));
        }
    }

    auto mainModelId = getMainModelId();
    auto activeModelId = m_session.getActiveAudioModel();
    
    bool haveMainModel =
        (!mainModelId.isNone());

    bool haveScore =
        m_scoreId != "";

    // NB this does not depend on actually having an alignment - we
    // want to be able to export an empty alignment (with every row
    // showing N/N)
    if (m_session.canReExportAlignment()) {
        emit canSaveScoreAlignmentAs(true);
        emit canSaveScoreAlignment(true);
    } else {
        emit canSaveScoreAlignmentAs(m_session.canExportAlignment());
        emit canSaveScoreAlignment(false);
    }
    
    emit canLoadScoreAlignment(true);

    // This refers to the audio-to-audio alignment using MATCH
    bool activeModelAlignmentComplete = false;
    if (!activeModelId.isNone()) {
        auto activeModel = ModelById::get(activeModelId);
        if (!activeModel->getAlignment().isNone()) {
            activeModelAlignmentComplete =
                (activeModel->getAlignmentCompletion() == 100);
        }
    }

//    SVDEBUG << "for canPropagateAlignment: activeModelId = " << activeModelId << ", mainModelId = " << mainModelId << ", activeModelAlignmentComplete = " << activeModelAlignmentComplete << endl;

    bool canPropagate =
        haveScore &&
        haveMainModel &&
        !activeModelId.isNone() &&
        activeModelId != mainModelId &&
        activeModelAlignmentComplete;
    
    emit canPropagateAlignment(canPropagate);
    
    if (m_chooseSmartCopyAction && m_chooseSmartCopyAction->isChecked()) {
        emit canAlign(canPropagate);
    } else {
        emit canAlign(haveScore && haveMainModel);
    }
    
    updateAlignButtonText();
}

void
MainWindow::updateWindowTitle()
{
    QString title = QApplication::applicationName();

    if (m_scoreId != "") {
        title += ": " + m_scoreId;
    }

    QString recordingTitle = m_session.getActiveAudioTitle();
    if (recordingTitle != "") {
        title += ": " + recordingTitle;
    } else if (m_originalLocation != "") {
        title += ": " + QFileInfo(m_originalLocation).completeBaseName();
    }

    setWindowTitle(title);
}

void
MainWindow::updateDescriptionLabel()
{
    if (!getMainModel()) {
        m_descriptionLabel->setText(tr("No audio file loaded."));
        return;
    }

    QString description;

//!!!???
    
    sv_samplerate_t ssr = getMainModel()->getSampleRate();
    sv_samplerate_t tsr = ssr;
    if (m_playSource) tsr = m_playSource->getDeviceSampleRate();

    if (ssr != tsr) {
        description = tr("%1Hz (resampling to %2Hz)").arg(ssr).arg(tsr);
    } else {
        description = QString("%1Hz").arg(ssr);
    }

    description = QString("%1 - %2")
        .arg(RealTime::frame2RealTime(getMainModel()->getEndFrame(), ssr)
             .toText(false).c_str())
        .arg(description);

    m_descriptionLabel->setText(description);
}

void
MainWindow::documentModified()
{
    //!!!
    MainWindowBase::documentModified();
}

void
MainWindow::documentRestored()
{
    //!!!
    MainWindowBase::documentRestored();
}

void
MainWindow::toolNavigateSelected()
{
    SVDEBUG << "MainWindow::toolNavigateSelected" << endl;
    m_viewManager->setToolMode(ViewManager::NavigateMode);
    m_scoreWidget->setInteractionMode(ScoreWidget::InteractionMode::Navigate);
}

void
MainWindow::toolSelectSelected()
{
    SVDEBUG << "MainWindow::toolSelectSelected" << endl;
    m_viewManager->setToolMode(ViewManager::SelectMode);
}

void
MainWindow::toolEditSelected()
{
    SVDEBUG << "MainWindow::toolEditSelected" << endl;
    m_viewManager->setToolMode(ViewManager::EditMode);
    m_scoreWidget->setInteractionMode(ScoreWidget::InteractionMode::Edit);
}

void
MainWindow::toolDrawSelected()
{
    m_viewManager->setToolMode(ViewManager::DrawMode);
}

void
MainWindow::toolEraseSelected()
{
    m_viewManager->setToolMode(ViewManager::EraseMode);
}

void
MainWindow::toolMeasureSelected()
{
    m_viewManager->setToolMode(ViewManager::MeasureMode);
}

void
MainWindow::importAudio()
{
    QString path = getOpenFileName(FileFinder::AudioFile);

    if (path != "") {
        if (openAudio(path, ReplaceSession) == FileOpenFailed) {
            emit hideSplash();
            QMessageBox::critical(this, tr("Failed to open file"),
                                  tr("<b>File open failed</b><p>Audio file \"%1\" could not be opened").arg(path));
        }
    }
}

void
MainWindow::importMoreAudio()
{
    QString path = getOpenFileName(FileFinder::AudioFile);

    if (path == "") {
        return;
    }

    // Add the new audio in a new pane in penultimate position
    
    int paneCountBefore = m_paneStack->getPaneCount();
    int addAtIndex = paneCountBefore /* - 1 */;
    
    AddPaneCommand *command = new AddPaneCommand(this, addAtIndex);
    CommandHistory::getInstance()->addCommand(command);

    Pane *pane = command->getPane();
    
    if (openAudio(path, ReplaceCurrentPane) == FileOpenFailed) {
        emit hideSplash();
        RemovePaneCommand *rcommand = new RemovePaneCommand(this, pane);
        CommandHistory::getInstance()->addCommand(rcommand);
        QMessageBox::critical(this, tr("Failed to open file"),
                              tr("<b>File open failed</b><p>Audio file \"%1\" could not be opened").arg(path));
        return;
    }

    m_session.addFurtherAudioPane(pane);
    m_paneStack->sizePanesEqually();

    currentPaneChanged(pane);
    
    updateWindowTitle();
    updateMenuStates();
}

void
MainWindow::replaceMainAudio()
{
    QString path = getOpenFileName(FileFinder::AudioFile);

    if (path != "") {
        if (openAudio(path, ReplaceMainModel) == FileOpenFailed) {
            emit hideSplash();
            QMessageBox::critical(this, tr("Failed to open file"),
                                  tr("<b>File open failed</b><p>Audio file \"%1\" could not be opened").arg(path));
        }
    }
}

void
MainWindow::exportAudio()
{
    exportAudio(false);
}

void
MainWindow::exportAudioData()
{
    exportAudio(true);
}

void
MainWindow::exportAudio(bool asData)
{
    auto modelId = getMainModelId();
    if (modelId.isNone()) return;
    
    set<ModelId> otherModelIds;
    ModelId current = modelId;
    
    if (m_paneStack) {
        for (int i = 0; i < m_paneStack->getPaneCount(); ++i) {
            Pane *pane = m_paneStack->getPane(i);
            if (!pane) continue;
            for (int j = 0; j < pane->getLayerCount(); ++j) {
                Layer *layer = pane->getLayer(j);
                if (!layer) continue;
                ModelId m = layer->getModel();
                if (ModelById::isa<RangeSummarisableTimeValueModel>(m)) {
                    otherModelIds.insert(m);
                    if (pane == m_paneStack->getCurrentPane()) {
                        current = m;
                    }
                }
            }
        }
    }
    if (!otherModelIds.empty()) {
        map<QString, ModelId> m;
        QString unnamed = tr("<unnamed>");
        QString oname = unnamed;
        if (auto mp = ModelById::get(modelId)) {
            oname = mp->objectName();
        }
        m[tr("1. %2").arg(oname)] = modelId;
        int n = 2;
        int c = 0;
        for (auto otherModelId: otherModelIds) {
            if (otherModelId == modelId) continue;
            oname = unnamed;
            if (auto mp = ModelById::get(otherModelId)) {
                oname = mp->objectName();
            }
            m[tr("%1. %2").arg(n).arg(oname)] = otherModelId;
            ++n;
            if (otherModelId == current) c = n-1;
        }
        QStringList items;
        for (auto i: m) {
            items << i.first;
        }
        if (items.size() > 1) {
            bool ok = false;
            QString item = QInputDialog::getItem
                (this, tr("Select audio file to export"),
                 tr("Which audio file do you want to export from?"),
                 items, c, false, &ok);
            if (!ok || item.isEmpty()) return;
            if (m.find(item) == m.end()) {
                SVCERR << "WARNING: Model " << item
                       << " not found in list!" << endl;
            } else {
                modelId = m[item];
            }
        }
    }

    auto model = ModelById::getAs<DenseTimeValueModel>(modelId);
    if (!model) {
        SVCERR << "ERROR: Chosen model is not a DenseTimeValueModel!" << endl;
        return;
    }
    
    QString path;
    if (asData) {
        path = getSaveFileName(FileFinder::CSVFile);
    } else {
        path = getSaveFileName(FileFinder::AudioFile);
    }
    if (path == "") return;

    bool ok = false;
    QString error;

    MultiSelection ms = m_viewManager->getSelection();
    MultiSelection::SelectionList selections = m_viewManager->getSelections();

    bool multiple = false;

    MultiSelection *selectionToWrite = nullptr;

    if (selections.size() == 1) {

        QStringList items;
        items << tr("Export the selected region only")
              << tr("Export the whole audio file");
        
        bool ok = false;
        QString item = ListInputDialog::getItem
            (this, tr("Select region to export"),
             tr("Which region from the original audio file do you want to export?"),
             items, 0, &ok);
        
        if (!ok || item.isEmpty()) return;
        
        if (item == items[0]) selectionToWrite = &ms;

    } else if (selections.size() > 1) {

        if (!asData) { // Multi-file export not supported for data

            QStringList items;
            items << tr("Export the selected regions into a single file")
                  << tr("Export the selected regions into separate files")
                  << tr("Export the whole file");

            QString item = ListInputDialog::getItem
                (this, tr("Select region to export"),
                 tr("Multiple regions of the original audio file are selected.\nWhat do you want to export?"),
                 items, 0, &ok);
            
            if (!ok || item.isEmpty()) return;
            
            if (item == items[0]) {
                selectionToWrite = &ms;
            } else if (item == items[1]) {
                multiple = true;
            }

        } else { // asData
            selectionToWrite = &ms;
        }

        if (multiple) { // Can only happen when asData false

            int n = 1;
            QString base = path;
            base.replace(".wav", "");

            for (MultiSelection::SelectionList::iterator i = selections.begin();
                 i != selections.end(); ++i) {

                MultiSelection subms;
                subms.setSelection(*i);

                QString subpath = QString("%1.%2.wav").arg(base).arg(n);
                ++n;

                if (QFileInfo(subpath).exists()) {
                    error = tr("Fragment file %1 already exists, aborting").arg(subpath);
                    break;
                }

                WavFileWriter subwriter(subpath,
                                        model->getSampleRate(),
                                        model->getChannelCount(),
                                        WavFileWriter::WriteToTemporary);
                subwriter.writeModel(model.get(), &subms);
                ok = subwriter.isOK();

                if (!ok) {
                    error = subwriter.getError();
                    break;
                }
            }
        }
    }

    if (!multiple) {
        if (asData) {
            stop();
            ProgressDialog dialog {
                QObject::tr("Exporting audio data..."),
                true,
                0,
                this,
                Qt::ApplicationModal
            };
            CSVFileWriter writer(path, model.get(), &dialog,
                                 ((QFileInfo(path).suffix() == "csv") ?
                                  "," : "\t"));
            if (selectionToWrite) {
                writer.writeSelection(*selectionToWrite);
            } else {
                writer.write();
            }
            ok = writer.isOK();
            error = writer.getError();
        } else {
            WavFileWriter writer(path,
                                 model->getSampleRate(),
                                 model->getChannelCount(),
                                 WavFileWriter::WriteToTemporary);
            writer.writeModel(model.get(), selectionToWrite);
            ok = writer.isOK();
            error = writer.getError();
        }
    }

    if (ok) {
        if (multiple) {
            emit activity(tr("Export multiple audio files"));
        } else {
            emit activity(tr("Export audio to \"%1\"").arg(path));
            m_recentFiles.addFile(path);
        }
    } else {
        QMessageBox::critical(this, tr("Failed to write file"), error);
    }
}

void
MainWindow::convertAudio()
{
    QString path = getOpenFileName(FileFinder::CSVFile);
    if (path == "") return;

    sv_samplerate_t defaultRate = 44100;
    
    CSVFormat format(path);
    format.setModelType(CSVFormat::WaveFileModel);
    format.setTimingType(CSVFormat::ImplicitTiming);
    format.setTimeUnits(CSVFormat::TimeAudioFrames);
    format.setSampleRate(defaultRate); // as a default for the dialog

    {
        CSVAudioFormatDialog *dialog = new CSVAudioFormatDialog(this, format);
        if (dialog->exec() != QDialog::Accepted) {
            delete dialog;
            return;
        }
        format = dialog->getFormat();
        delete dialog;
    }
    
    FileOpenStatus status = FileOpenSucceeded;

    ProgressDialog *progress = new ProgressDialog
        (tr("Converting audio data..."), true, 0, this, Qt::ApplicationModal);
    
    WaveFileModel *model = qobject_cast<WaveFileModel *>
        (DataFileReaderFactory::loadCSV
         (path, format,
          getMainModel() ? getMainModel()->getSampleRate() : defaultRate,
          progress));

    if (progress->wasCancelled()) {

        delete model;
        status = FileOpenCancelled;

    } else if (!model || !model->isOK()) {

        delete model;
        status = FileOpenFailed;

    } else {

        auto modelId = ModelById::add(std::shared_ptr<Model>(model));
        
        status = addOpenedAudioModel(path,
                                     modelId,
                                     CreateAdditionalModel,
                                     getDefaultSessionTemplate(),
                                     false);
    }

    delete progress;
    
    if (status == FileOpenFailed) {
        emit hideSplash();
        QMessageBox::critical(this, tr("Failed to open file"),
                              tr("<b>File open failed</b><p>Audio data file %1 could not be opened.").arg(path));
    }
}

void
MainWindow::loadScoreAlignment()
{
    SVDEBUG << "MainWindow::loadScoreAlignment" << endl;
    
    QString filename = getOpenFileName(FileFinder::CSVFile);
    if (filename == "") {
        // cancelled
        return;
    }

    if (!m_session.importAlignmentFrom(filename)) {
        QMessageBox::warning(this,
                             tr("Failed to import alignment"),
                             tr("Failed to import alignment. See log file for more information."),
                             QMessageBox::Ok);
    }
}

void
MainWindow::saveScoreAlignment()
{
    SVDEBUG << "MainWindow::saveScoreAlignment" << endl;

    if (m_session.canReExportAlignment()) {
        if (!m_session.reExportAlignment()) {
            QMessageBox::warning(this,
                                 tr("Failed to export alignment"),
                                 tr("Failed to export alignment. See log file for more information."),
                                 QMessageBox::Ok);
        }
    } else {
        saveScoreAlignmentAs();
    }

    updateMenuStates();
}

void
MainWindow::saveScoreAlignmentAs()
{
    SVDEBUG << "MainWindow::saveScoreAlignmentAs" << endl;

    QString filename = getSaveFileName(FileFinder::CSVFile);
    if (filename == "") {
        // cancelled
        return;
    }

    if (!m_session.exportAlignmentTo(filename)) {
        QMessageBox::warning(this,
                             tr("Failed to export alignment"),
                             tr("Failed to export alignment. See log file for more information."),
                             QMessageBox::Ok);
    }

    updateMenuStates();
}

void
MainWindow::propagateAlignmentFromReference()
{
    ModelId audioModelId = m_session.getActiveAudioModel();
    if (audioModelId.isNone()) {
        SVDEBUG << "MainWindow::propagateAlignmentFromReference: No active audio" << endl;
        return;
    }

    ModelId mainModelId = getMainModelId();
    if (audioModelId == mainModelId) {
        SVDEBUG << "MainWindow::propagateAlignmentFromReference: Active audio *is* reference" << endl;
        return;
    }

    // We want to be able to handle partial copies. We could do this
    // by cueing from the selected part of the score, if there is one,
    // by mapping from that to the reference model, then propagating
    // the events within that mapped range. Or we could do it by
    // cueing from a selection within the reference audio, if there is
    // one. But the latter has the problem that the audio selection
    // exists in the target audio as well, so it isn't entirely
    // unambiguous that we mean to propagate from that range of the
    // reference rather than to that range of the target.
    //
    // And we can't really support both score and audio selections, as
    // we'd have no way to reconcile them if both had a current
    // selection. We could prioritise, but I think it's probably
    // simpler and clearer to always use the score selection, and if
    // there is none, to map everything.
    
    if (m_subsetOfScoreSelected) {
        SVDEBUG << "MainWindow::propagateAlignmentFromReference: Subset of score selected" << endl;
        Fraction start, end;
        ScoreWidget::EventLabel startLabel, endLabel;
        m_scoreWidget->getSelection(start, startLabel, end, endLabel);
        sv_frame_t startFrame, endFrame;
        auto onsetsLayer = m_session.getOnsetsLayerFromPane
            (m_session.getReferencePane());
        m_scoreBasedFrameAligner->mapFromScoreLabelAndProportion
            (onsetsLayer, QString::fromStdString(startLabel), 0.0, startFrame);
        m_scoreBasedFrameAligner->mapFromScoreLabelAndProportion
            (onsetsLayer, QString::fromStdString(endLabel), 0.0, endFrame);
        SVDEBUG << "MainWindow::propagateAlignmentFromReference: Mapped score labels start = " << startLabel << ", end = " << endLabel << " to frames start = " << startFrame << ", end = " << endFrame << endl;
        m_session.propagatePartialAlignmentFromMain(startFrame, endFrame);
    } else {
        SVDEBUG << "MainWindow::propagateAlignmentFromReference: No subset selected" << endl;
        m_session.propagateAlignmentFromMain();
    }
}

void
MainWindow::importLayer()
{
    Pane *pane = m_paneStack->getCurrentPane();
    
    if (!pane) {
        // shouldn't happen, as the menu action should have been disabled
        SVCERR << "WARNING: MainWindow::importLayer: no current pane" << endl;
        return;
    }

    if (!getMainModel()) {
        // shouldn't happen, as the menu action should have been disabled
        SVCERR << "WARNING: MainWindow::importLayer: No main model -- hence no default sample rate available" << endl;
        return;
    }

    QString path = getOpenFileName(FileFinder::LayerFile);

    if (path != "") {

        FileOpenStatus status = openLayer(path);
        
        if (status == FileOpenFailed) {
            emit hideSplash();
            QMessageBox::critical(this, tr("Failed to open file"),
                                  tr("<b>File open failed</b><p>Layer file %1 could not be opened.").arg(path));
            return;
        } else if (status == FileOpenWrongMode) {
            emit hideSplash();
            QMessageBox::critical(this, tr("Failed to open file"),
                                  tr("<b>Audio required</b><p>Unable to load layer data from \"%1\" without an audio file.<br>Please load at least one audio file before importing annotations.").arg(path));
        }
    }
}

void
MainWindow::exportLayer()
{
    Pane *pane = m_paneStack->getCurrentPane();
    if (!pane) return;

    Layer *layer = pane->getSelectedLayer();
    if (!layer) return;

    ModelId modelId = layer->getModel();
    if (modelId.isNone()) return;

    FileFinder::FileType type = FileFinder::LayerFileNoMidi;
    if (ModelById::isa<NoteModel>(modelId)) type = FileFinder::LayerFile;
    QString path = getSaveFileName(type);

    if (path == "") return;

    QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "") suffix = "csv";

    bool canWriteSelection =
        ! (suffix == "xml" || suffix == "svl" ||
           suffix == "n3" || suffix == "ttl");

    bool useCSVDialog =
        ! (suffix == "xml" || suffix == "svl" ||
           suffix == "mid" || suffix == "midi" ||
           suffix == "n3" || suffix == "ttl");

    if (!ModelById::isa<NoteModel>(modelId) &&
        (suffix == "mid" || suffix == "midi")) {
        QMessageBox::critical
            (this, tr("Failed to export layer"),
             tr("Only note layers may be exported to MIDI files."));
        return;
    }

    if (ModelById::isa<DenseTimeValueModel>(modelId) &&
        !useCSVDialog) {
        // This is the case for spectrograms
        QMessageBox::critical
            (this, tr("Failed to export layer"),
             tr("Cannot export this layer to this file type. Only delimited column formats such as CSV are supported."));
        return;
    }
    
    MultiSelection ms = m_viewManager->getSelection();
    bool haveSelection = !ms.getSelections().empty();
    
    MultiSelection *selectionToWrite = nullptr;
    LayerGeometryProvider *provider = pane;

    DataExportOptions options = DataExportDefaults;
    QString delimiter = ",";
    
    if (useCSVDialog) {

        CSVExportDialog::Configuration config;
        config.layerName = layer->getLayerPresentationName();
        config.fileExtension = suffix;
        config.isDense = false;
        if (auto m = ModelById::get(modelId)) {
            config.isDense = !m->isSparse();
        }
        config.haveView = true;
        config.haveSelection = canWriteSelection && haveSelection;

        CSVExportDialog dialog(config, this);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }

        if (dialog.shouldConstrainToSelection()) {
            selectionToWrite = &ms;
        }

        if (!dialog.shouldConstrainToViewHeight()) {
            provider = nullptr;
        }

        delimiter = dialog.getDelimiter();

        if (dialog.shouldIncludeHeader()) {
            options |= DataExportIncludeHeader;
        }

        if (dialog.shouldIncludeTimestamps()) {
            options |= DataExportAlwaysIncludeTimestamp;
        }

        if (dialog.shouldWriteTimeInFrames()) {
            options |= DataExportWriteTimeInFrames;
        }

    } else if (canWriteSelection && haveSelection) {

        QStringList items;
        items << tr("Export the content of the selected area")
              << tr("Export the whole layer");
        
        bool ok = false;
        QString item = ListInputDialog::getItem
            (this, tr("Select region to export"),
             tr("Which region of the layer do you want to export?"),
             items, 0, &ok);
        
        if (!ok || item.isEmpty()) {
            return;
        }
        
        if (item == items[0]) {
            selectionToWrite = &ms;
        }
    }
    
    QString error;

    bool result = false;

    if (suffix == "xml" || suffix == "svl") {
        result = exportLayerToSVL(layer, path, error);
    } else if (suffix == "mid" || suffix == "midi") {
        result = exportLayerToMIDI(layer, selectionToWrite, path, error);
    } else if (suffix == "ttl" || suffix == "n3") {
        result = exportLayerToRDF(layer, path, error);
    } else {
        result = exportLayerToCSV(layer, provider, selectionToWrite,
                                  delimiter, options, path, error);
    }
    
    if (!result) {
        QMessageBox::critical(this, tr("Failed to write file"), error);
    } else {
        m_recentFiles.addFile(path);
        emit activity(tr("Export layer to \"%1\"").arg(path));
    }
}

void
MainWindow::exportImage()
{
    Pane *pane = m_paneStack->getCurrentPane();
    if (!pane) return;
    
    QString path = getSaveFileName(FileFinder::ImageFile);
    if (path == "") return;
    if (QFileInfo(path).suffix() == "") path += ".png";
    
    bool haveSelection = m_viewManager && !m_viewManager->getSelections().empty();

    QSize total, visible, selected;
    total = pane->getRenderedImageSize();
    visible = pane->getRenderedPartImageSize(pane->getFirstVisibleFrame(),
                                             pane->getLastVisibleFrame());

    sv_frame_t sf0 = 0, sf1 = 0;
 
    if (haveSelection) {
        MultiSelection::SelectionList selections = m_viewManager->getSelections();
        sf0 = selections.begin()->getStartFrame();
        MultiSelection::SelectionList::iterator e = selections.end();
        --e;
        sf1 = e->getEndFrame();
        selected = pane->getRenderedPartImageSize(sf0, sf1);
    }

    QStringList items;
    items << tr("Export the whole pane (%1x%2 pixels)")
        .arg(total.width()).arg(total.height());
    items << tr("Export the visible area only (%1x%2 pixels)")
        .arg(visible.width()).arg(visible.height());
    if (haveSelection) {
        items << tr("Export the selection extent (%1x%2 pixels)")
            .arg(selected.width()).arg(selected.height());
    } else {
        items << tr("Export the selection extent");
    }

    QSettings settings;
    settings.beginGroup("MainWindow");
    int deflt = settings.value("lastimageexportregion", 0).toInt();
    if (deflt == 2 && !haveSelection) deflt = 1;
    if (deflt == 0 && total.width() > 32767) deflt = 1;

    ListInputDialog *lid = new ListInputDialog
        (this, tr("Select region to export"),
         tr("Which region of the current pane do you want to export as an image?"),
         items, deflt);

    if (!haveSelection) {
        lid->setItemAvailability(2, false);
    }
    if (total.width() > 32767) { // appears to be limit of a QImage
        lid->setItemAvailability(0, false);
        lid->setFootnote(tr("Note: the whole pane is too wide to be exported as a single image."));
    }

    bool ok = lid->exec();
    QString item = lid->getCurrentString();
    delete lid;
            
    if (!ok || item.isEmpty()) return;

    settings.setValue("lastimageexportregion", deflt);

    QImage *image = nullptr;
    
    if (item == items[0]) {
        image = pane->renderToNewImage();
    } else if (item == items[1]) {
        image = pane->renderPartToNewImage(pane->getFirstVisibleFrame(),
                                           pane->getLastVisibleFrame());
    } else if (haveSelection) {
        image = pane->renderPartToNewImage(sf0, sf1);
    }
    
    if (!image) return;
    
    if (!image->save(path, "PNG")) {
        QMessageBox::critical(this, tr("Failed to save image file"),
                              tr("Failed to save image file %1").arg(path));
    }
    
    delete image;
}

void
MainWindow::exportSVG()
{
    Pane *pane = m_paneStack->getCurrentPane();
    if (!pane) return;
    
    QString path = getSaveFileName(FileFinder::SVGFile);
    if (path == "") return;
    if (QFileInfo(path).suffix() == "") path += ".svg";

    bool haveSelection = m_viewManager && !m_viewManager->getSelections().empty();

    sv_frame_t sf0 = 0, sf1 = 0;
 
    if (haveSelection) {
        MultiSelection::SelectionList selections = m_viewManager->getSelections();
        sf0 = selections.begin()->getStartFrame();
        MultiSelection::SelectionList::iterator e = selections.end();
        --e;
        sf1 = e->getEndFrame();
    }

    QStringList items;
    items << tr("Export the whole pane");
    items << tr("Export the visible area only");
    items << tr("Export the selection extent");

    QSettings settings;
    settings.beginGroup("MainWindow");
    int deflt = settings.value("lastsvgexportregion", 0).toInt();
    if (deflt == 2 && !haveSelection) deflt = 1;

    ListInputDialog *lid = new ListInputDialog
        (this, tr("Select region to export"),
         tr("Which region of the current pane do you want to export as a scalable SVG image?"),
         items, deflt);

    if (!haveSelection) {
        lid->setItemAvailability(2, false);
    }

    bool ok = lid->exec();
    QString item = lid->getCurrentString();
    delete lid;
            
    if (!ok || item.isEmpty()) return;

    settings.setValue("lastsvgexportregion", deflt);

    bool result = false;
        
    if (item == items[0]) {
        result = pane->renderToSvgFile(path);
    } else if (item == items[1]) {
        result = pane->renderPartToSvgFile(path,
                                           pane->getFirstVisibleFrame(),
                                           pane->getLastVisibleFrame());
    } else if (haveSelection) {
        result = pane->renderPartToSvgFile(path, sf0, sf1);
    }
    
    if (!result) {
        QMessageBox::critical(this, tr("Failed to save SVG file"),
                              tr("Failed to save SVG file %1").arg(path));
    }
}

void
MainWindow::browseRecordedAudio()
{
    QString path = RecordDirectory::getRecordContainerDirectory();
    if (path == "") path = RecordDirectory::getRecordDirectory();
    if (path == "") return;

    openLocalFolder(path);
}

void
MainWindow::newSession()
{
    if (!checkSaveModified()) return;

    closeSession();
    stop();
    createDocument();
}

QString
MainWindow::getDefaultSessionTemplate() const
{
    return "";
}

void
MainWindow::documentReplaced()
{
    SVDEBUG << "MainWindow::documentReplaced" << endl;
    
    connect(m_document, SIGNAL(activity(QString)),
            m_activityLog, SLOT(activityHappened(QString)));

    Align::setAlignmentPreference(Align::MATCHAlignmentWithPitchCompare);
    m_document->setAutoAlignment(true); // for audio-to-audio alignment
    
    Pane *topPane = m_paneStack->addPane();
    topPane->setSelectionSnapToFeatures(false);
    
    connect(topPane, SIGNAL(contextHelpChanged(const QString &)),
            this, SLOT(contextHelpChanged(const QString &)));

    m_overview->registerView(topPane);

    if (!m_timeRulerLayer) {
        m_timeRulerLayer = m_document->createMainModelLayer
            (LayerFactory::TimeRuler);
    }

    m_document->addLayerToView(topPane, m_timeRulerLayer);

    SVDEBUG << "MainWindow::documentReplaced: Added views, now calling m_session.setDocument" << endl;
    
    m_session.setDocument(m_document, topPane, m_tempoCurveWidget,
                          m_overview, m_timeRulerLayer);

    CommandHistory::getInstance()->clear();
    CommandHistory::getInstance()->documentSaved();
    documentRestored();
    updateMenuStates();
}

void
MainWindow::closeSession()
{
    SVDEBUG << "MainWindow::closeSession" << endl;
    
    if (!checkSaveModified()) return;

    CommandHistory::getInstance()->clear();

    SVDEBUG << "MainWindow::closeSession: telling session about it" << endl;
    m_session.unsetDocument();

    while (m_paneStack->getPaneCount() > 0) {

        Pane *pane = m_paneStack->getPane(m_paneStack->getPaneCount() - 1);

        while (pane->getLayerCount() > 0) {
            Layer *layer = pane->getLayer(pane->getLayerCount() - 1);
            m_document->removeLayerFromView(pane, layer);
            // will be deleted with the document below
        }

        m_overview->unregisterView(pane);
        m_paneStack->deletePane(pane);
    }

    while (m_paneStack->getHiddenPaneCount() > 0) {

        Pane *pane = m_paneStack->getHiddenPane
            (m_paneStack->getHiddenPaneCount() - 1);

        while (pane->getLayerCount() > 0) {
            Layer *layer = pane->getLayer(pane->getLayerCount() - 1);
            m_document->removeLayerFromView(pane, layer);
            // will be deleted with the document below
        }

        m_overview->unregisterView(pane);
        m_paneStack->deletePane(pane);
    }

    delete m_layerTreeDialog.data();
    delete m_preferencesDialog.data();

    m_activityLog->hide();
    m_unitConverter->hide();
    m_keyReference->hide();

    delete m_document;
    m_document = nullptr;
    m_viewManager->clearSelections();
    m_timeRulerLayer = nullptr; // document owned this

    m_sessionFile = "";
    m_originalLocation = "";
    setWindowTitle(QApplication::applicationName());
    
    CommandHistory::getInstance()->clear();
    CommandHistory::getInstance()->documentSaved();
    documentRestored();
}

void
MainWindow::openSomething()
{
    QString orig = m_audioFile;
    if (orig == "") orig = ".";
    else orig = QFileInfo(orig).absoluteDir().canonicalPath();

    QString path = getOpenFileName(FileFinder::AnyFile);

    if (path.isEmpty()) return;

    FileOpenStatus status = openPath(path, ReplaceSession);

    if (status == FileOpenFailed) {
        emit hideSplash();
        QMessageBox::critical(this, tr("Failed to open file"),
                              tr("<b>File open failed</b><p>File \"%1\" could not be opened").arg(path));
    } else if (status == FileOpenWrongMode) {
        emit hideSplash();
        QMessageBox::critical(this, tr("Failed to open file"),
                              tr("<b>Audio required</b><p>Unable to load layer data from \"%1\" without an audio file.<br>Please load at least one audio file before importing annotations.").arg(path));
    }
}

void
MainWindow::openLocation()
{
    QSettings settings;
    settings.beginGroup("MainWindow");
    QString lastLocation = settings.value("lastremote", "").toString();

    bool ok = false;
    QString text = QInputDialog::getText
        (this, tr("Open Location"),
         tr("Please enter the URL of the location to open:"),
         QLineEdit::Normal, lastLocation, &ok);

    if (!ok) return;

    settings.setValue("lastremote", text);

    if (text.isEmpty()) return;

    FileOpenStatus status = openPath(text, AskUser);

    if (status == FileOpenFailed) {
        emit hideSplash();
        QMessageBox::critical(this, tr("Failed to open location"),
                              tr("<b>Open failed</b><p>URL \"%1\" could not be opened").arg(text));
    } else if (status == FileOpenWrongMode) {
        emit hideSplash();
        QMessageBox::critical(this, tr("Failed to open location"),
                              tr("<b>Audio required</b><p>Unable to load layer data from \"%1\" without an audio file.<br>Please load at least one audio file before importing annotations.").arg(text));
    }
}

void
MainWindow::openRecentFile()
{
    QObject *obj = sender();
    QAction *action = dynamic_cast<QAction *>(obj);
    
    if (!action) {
        SVCERR << "WARNING: MainWindow::openRecentFile: sender is not an action"
             << endl;
        return;
    }

    QString path = action->objectName();

    if (path == "") {
        SVCERR << "WARNING: MainWindow::openRecentFile: action incorrectly named"
             << endl;
        return;
    }

    FileOpenStatus status = openPath(path, ReplaceSession);

    if (status == FileOpenFailed) {
        emit hideSplash();
        QMessageBox::critical(this, tr("Failed to open location"),
                              tr("<b>Open failed</b><p>File or URL \"%1\" could not be opened").arg(path));
    } else if (status == FileOpenWrongMode) {
        emit hideSplash();
        QMessageBox::critical(this, tr("Failed to open location"),
                              tr("<b>Audio required</b><p>Unable to load layer data from \"%1\" without an audio file.<br>Please load at least one audio file before importing annotations.").arg(path));
    }
}

void
MainWindow::applyTemplate()
{
    QObject *s = sender();
    QAction *action = qobject_cast<QAction *>(s);

    if (!action) {
        SVCERR << "WARNING: MainWindow::applyTemplate: sender is not an action"
                  << endl;
        return;
    }

    QString n = action->objectName();
    if (n == "") n = action->text();

    if (n == "") {
        SVCERR << "WARNING: MainWindow::applyTemplate: sender has no name"
                  << endl;
        return;
    }

    QString mainModelLocation;
    auto mm = getMainModel();
    if (mm) mainModelLocation = mm->getLocation();
    if (mainModelLocation != "") {
        openAudio(mainModelLocation, ReplaceSession, n);
    } else {
        openSessionTemplate(n);
    }
}

void
MainWindow::saveSessionAsTemplate()
{
    QDialog *d = new QDialog(this);
    d->setWindowTitle(tr("Enter template name"));

    QGridLayout *layout = new QGridLayout;
    d->setLayout(layout);

    layout->addWidget(new QLabel(tr("Please enter a name for the saved template:")),
                 0, 0);
    QLineEdit *lineEdit = new QLineEdit;
    layout->addWidget(lineEdit, 1, 0);
    QCheckBox *makeDefault = new QCheckBox(tr("Set as default template for future audio files"));
    layout->addWidget(makeDefault, 2, 0);
    
    QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok |
                                                QDialogButtonBox::Cancel);
    layout->addWidget(bb, 3, 0);
    connect(bb, SIGNAL(accepted()), d, SLOT(accept()));
    connect(bb, SIGNAL(accepted()), d, SLOT(accept()));
    connect(bb, SIGNAL(rejected()), d, SLOT(reject()));
    
    if (d->exec() == QDialog::Accepted) {

        QString name = lineEdit->text();
        name.replace(QRegularExpression("[^\\w\\s\\.\"'-]"), "_");

        ResourceFinder rf;
        QString dir = rf.getResourceSaveDir("templates");
        QString filename = QString("%1/%2.svt").arg(dir).arg(name);
        if (QFile(filename).exists()) {
            if (QMessageBox::warning(this,
                                     tr("Template file exists"),
                                     tr("<b>Template file exists</b><p>The template \"%1\" already exists.<br>Overwrite it?").arg(name),
                                     QMessageBox::Ok | QMessageBox::Cancel,
                                     QMessageBox::Cancel) != QMessageBox::Ok) {
                delete d;
                return;
            }
        }

        if (saveSessionTemplate(filename)) {
            if (makeDefault->isChecked()) {
                setDefaultSessionTemplate(name);
            }
        }
    }

    delete d;
}

void
MainWindow::manageSavedTemplates()
{
    ResourceFinder rf;
    openLocalFolder(rf.getResourceSaveDir("templates"));
}

void
MainWindow::paneAdded(Pane *pane)
{
    if (m_overview) {
        m_overview->registerView(pane);
    }
    if (pane) {
        connect(pane, &Pane::cancelButtonPressed,
                this, &MainWindow::paneCancelButtonPressed);
        pane->setPlaybackFrameAligner(m_scoreBasedFrameAligner);
        pane->setPlaybackFollow(PlaybackScrollPageWithCentre);
        pane->setSelectionSnapToFeatures(false);
    }
}    

void
MainWindow::paneHidden(Pane *pane)
{
    SVDEBUG << "MainWindow::paneHidden(" << pane << ")" << endl;
    if (m_overview) m_overview->unregisterView(pane);
}    

void
MainWindow::paneAboutToBeDeleted(Pane *)
{
    // If we delete panes using a command (as we generally do) then
    // this doesn't happen until the command is deleted, which is not
    // likely to be a useful moment.

    // In theory we can use paneHidden, but the problem with *that* is
    // that MainWindowBase::paneDeleteButtonClicked is called first
    // and removes the layers from the pane. So for anything that must
    // be called before the pane actually goes, we need to override
    // and use paneDeleteButtonClicked.
}

void
MainWindow::paneDeleteButtonClicked(Pane *pane)
{
    SVDEBUG << "MainWindow::paneDeleteButtonClicked(" << pane << ")" << endl;
    m_session.paneRemoved(pane);

    MainWindowBase::paneDeleteButtonClicked(pane);
}

void
MainWindow::paneCancelButtonPressed(Layer *layer)
{
    Pane *pane = qobject_cast<Pane *>(sender());
    bool found = false;
    if (pane && layer) {
        for (int i = 0; i < pane->getLayerCount(); ++i) {
            if (pane->getLayer(i) == layer) {
                found = true;
                break;
            }
        }
    }
    if (!found) {
        SVDEBUG << "MainWindow::paneCancelButtonPressed: Unknown layer in pane"
                << endl;
        return;
    }

    SVDEBUG << "MainWindow::paneCancelButtonPressed: Layer " << layer << endl;

    // We need to ensure that the transform that is populating this
    // layer's model is stopped - that is the main reason to use
    // Cancel after all. It would also be a good idea to remove the
    // incomplete layer from both the view and the undo/redo stack.

    // Deleting the target model will ensure that the transform gets
    // stopped, but removing the layer from the view is not enough to
    // delete the model, because a reference to the layer remains on
    // the undo/redo stack. If we also replace the model id with None
    // in the layer, that does the trick.
    
    m_document->setModel(layer, {});
    m_document->removeLayerFromView(pane, layer);

    // We still have a layer with no model on the undo/redo stack,
    // which is a pity. I'm not sure we can easily remove it, since
    // other commands may have been pushed on the stack since, so
    // let's just leave that for now.
    
    updateMenuStates();
}

void
MainWindow::paneDropAccepted(Pane *pane, QStringList uriList)
{
    if (pane) m_paneStack->setCurrentPane(pane);

    for (QStringList::iterator i = uriList.begin(); i != uriList.end(); ++i) {

        FileOpenStatus status;

        if (i == uriList.begin()) {
            status = openPath(*i, ReplaceCurrentPane);
        } else {
            status = openPath(*i, CreateAdditionalModel);
        }

        if (status == FileOpenFailed) {
            emit hideSplash();
            QMessageBox::critical(this, tr("Failed to open dropped URL"),
                                  tr("<b>Open failed</b><p>Dropped URL \"%1\" could not be opened").arg(*i));
            break;
        } else if (status == FileOpenWrongMode) {
            emit hideSplash();
            QMessageBox::critical(this, tr("Failed to open dropped URL"),
                                  tr("<b>Audio required</b><p>Unable to load layer data from \"%1\" without an audio file.<br>Please load at least one audio file before importing annotations.").arg(*i));
            break;
        } else if (status == FileOpenCancelled) {
            break;
        }
    }
}

void
MainWindow::paneDropAccepted(Pane *pane, QString text)
{
    if (pane) m_paneStack->setCurrentPane(pane);

    QUrl testUrl(text);
    if (testUrl.scheme() == "file" || 
        testUrl.scheme() == "http" || 
        testUrl.scheme() == "ftp") {
        QStringList list;
        list.push_back(text);
        paneDropAccepted(pane, list);
        return;
    }

    //!!! open as text -- but by importing as if a CSV, or just adding
    //to a text layer?
}

void
MainWindow::closeEvent(QCloseEvent *e)
{
    SVDEBUG << "MainWindow::closeEvent" << endl;

    if (m_openingAudioFile) {
        SVCERR << "Busy - ignoring close event" << endl;
        e->ignore();
        return;
    }

    if (!checkSaveModified()) {
        SVCERR << "Close refused by user - ignoring close event" << endl;
        e->ignore();
        return;
    }

    QSettings settings;
    settings.beginGroup("MainWindow");
    settings.setValue("maximised", isMaximized());
    if (!isMaximized()) {
        settings.setValue("size", size());
        settings.setValue("position", pos());
    }
    settings.endGroup();

    if (m_preferencesDialog &&
        m_preferencesDialog->isVisible()) {
        m_preferencesDialog->applicationClosing(true);
    }

    stop();
    closeSession();

    e->accept();

    return;
}

bool
MainWindow::commitData(bool mayAskUser)
{
    if (mayAskUser) {
        bool rv = checkSaveModified();
        if (rv) {
            if (m_preferencesDialog &&
                m_preferencesDialog->isVisible()) {
                m_preferencesDialog->applicationClosing(false);
            }
        }
        return rv;
    } else {
        if (m_preferencesDialog &&
            m_preferencesDialog->isVisible()) {
            m_preferencesDialog->applicationClosing(true);
        }
        if (!m_documentModified) return true;

        // If we can't check with the user first, then we can't save
        // to the original session file (even if we have it) -- have
        // to use a temporary file

        QString svDirBase = ".sv1";
        QString svDir = QDir::home().filePath(svDirBase);

        if (!QFileInfo(svDir).exists()) {
            if (!QDir::home().mkdir(svDirBase)) return false;
        } else {
            if (!QFileInfo(svDir).isDir()) return false;
        }
        
        // This name doesn't have to be unguessable
#ifndef _WIN32
        QString fname = QString("tmp-%1-%2.sv")
            .arg(QDateTime::currentDateTime().toString("yyyyMMddhhmmsszzz"))
            .arg(QProcess().processId());
#else
        QString fname = QString("tmp-%1.sv")
            .arg(QDateTime::currentDateTime().toString("yyyyMMddhhmmsszzz"));
#endif
        QString fpath = QDir(svDir).filePath(fname);
        if (saveSessionFile(fpath)) {
            m_recentFiles.addFile(fpath);
            emit activity(tr("Export image to \"%1\"").arg(fpath));
            return true;
        } else {
            return false;
        }
    }
}

bool
MainWindow::checkSaveModified()
{
    // Called before some destructive operation (e.g. new session,
    // exit program).  Return true if we can safely proceed, false to
    // cancel.

    if (!m_documentModified) return true;

    emit hideSplash();

    int button = 
        QMessageBox::warning(this,
                             tr("Session modified"),
                             tr("<b>Session modified</b><p>The current session has been modified.<br>Do you want to save it?"),
                             QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
                             QMessageBox::Yes);

    if (button == QMessageBox::Yes) {
        saveSession();
        if (m_documentModified) { // save failed -- don't proceed!
            return false;
        } else {
            return true; // saved, so it's safe to continue now
        }
    } else if (button == QMessageBox::No) {
        m_documentModified = false; // so we know to abandon it
        return true;
    }

    // else cancel
    return false;
}

bool
MainWindow::shouldCreateNewSessionForRDFAudio(bool *cancel)
{
    //!!! This is very similar to part of MainWindowBase::openAudio --
    //!!! make them a bit more uniform

    QSettings settings;
    settings.beginGroup("MainWindow");
    bool prevNewSession = settings.value("newsessionforrdfaudio", true).toBool();
    settings.endGroup();
    bool newSession = true;
            
    QStringList items;
    items << tr("Close the current session and create a new one")
          << tr("Add this data to the current session");

    bool ok = false;
    QString item = ListInputDialog::getItem
        (this, tr("Select target for import"),
         tr("<b>Select a target for import</b><p>This RDF document refers to one or more audio files.<br>You already have an audio waveform loaded.<br>What would you like to do with the new data?"),
         items, prevNewSession ? 0 : 1, &ok);
            
    if (!ok || item.isEmpty()) {
        *cancel = true;
        return false;
    }
            
    newSession = (item == items[0]);
    settings.beginGroup("MainWindow");
    settings.setValue("newsessionforrdfaudio", newSession);
    settings.endGroup();

    if (newSession) return true;
    else return false;
}

void
MainWindow::saveSession()
{
    if (m_sessionFile != "") {
        if (!saveSessionFile(m_sessionFile)) {
            QMessageBox::critical(this, tr("Failed to save file"),
                                  tr("<b>Save failed</b><p>Session file \"%1\" could not be saved.").arg(m_sessionFile));
        } else {
            CommandHistory::getInstance()->documentSaved();
            documentRestored();
        }
    } else {
        saveSessionAs();
    }
}

void
MainWindow::saveSessionAs()
{
    QString orig = m_audioFile;
    if (orig == "") orig = ".";
    else orig = QFileInfo(orig).absoluteDir().canonicalPath();

    QString path = getSaveFileName(FileFinder::SessionFile);

    if (path == "") return;

    if (!saveSessionFile(path)) {
        QMessageBox::critical(this, tr("Failed to save file"),
                              tr("<b>Save failed</b><p>Session file \"%1\" could not be saved.").arg(path));
    } else {
        setWindowTitle(tr("%1: %2")
                       .arg(QApplication::applicationName())
                       .arg(QFileInfo(path).fileName()));
        m_sessionFile = path;
        CommandHistory::getInstance()->documentSaved();
        documentRestored();
        m_recentFiles.addFile(path);
        emit activity(tr("Save session as \"%1\"").arg(path));
    }
}

void
MainWindow::preferenceChanged(PropertyContainer::PropertyName name)
{
    MainWindowBase::preferenceChanged(name);

    if (name == "Background Mode") {
        coloursChanged();
    }     
}

void
MainWindow::coloursChanged()
{
    QSettings settings;
    settings.beginGroup("Preferences");

    bool haveDarkBackground = (m_viewManager &&
                               m_viewManager->getGlobalDarkBackground());
    QColor highlight = QApplication::palette().color(QPalette::Highlight);
    ColourDatabase *cdb = ColourDatabase::getInstance();
    int nearestIndex = cdb->getNearbyColourIndex
        (highlight,
         haveDarkBackground ?
         ColourDatabase::WithDarkBackground :
         ColourDatabase::WithLightBackground);
    QString defaultColourName = cdb->getColourName(nearestIndex);
    settings.endGroup();

    SVDEBUG << "MainWindow::coloursChanged: haveDarkBackground = " << haveDarkBackground << ", highlight = " << highlight.name() << ", nearestIndex = " << nearestIndex << ", defaultColourName = " << defaultColourName << endl;
}

void
MainWindow::propertyStacksResized(int width)
{
//    SVDEBUG << "MainWindow::propertyStacksResized(" << width << ")" << endl;

    if (!m_playControlsSpacer) return;

    int spacerWidth = width - m_playControlsWidth - 4;
    
//    SVDEBUG << "resizing spacer from " << m_playControlsSpacer->width() << " to " << spacerWidth << endl;

    m_playControlsSpacer->setFixedSize(QSize(spacerWidth, 2));
}

void
MainWindow::addPane()
{
    QObject *s = sender();
    QAction *action = dynamic_cast<QAction *>(s);

    SVCERR << "addPane: sender is " << s << ", action is " << action << ", name " << action->text() << endl;
    
    if (!action) {
        SVCERR << "WARNING: MainWindow::addPane: sender is not an action"
                  << endl;
        return;
    }

    PaneActions::iterator i = m_paneActions.begin();
    while (i != m_paneActions.end()) {
        if (i->first == action) break;
        ++i;
    }

    if (i == m_paneActions.end()) {
        SVCERR << "WARNING: MainWindow::addPane: unknown action "
             << action->objectName() << endl;
        SVCERR << "known actions are:" << endl;
        for (PaneActions::const_iterator i = m_paneActions.begin();
             i != m_paneActions.end(); ++i) {
            SVCERR << i->first << ", name " << i->first->text() << endl;
        }
        return;
    }

    addPane(i->second, action->text());
}

void
MainWindow::addPane(const LayerConfiguration &configuration, QString text)
{
    CommandHistory::getInstance()->startCompoundOperation(text, true);

    AddPaneCommand *command = new AddPaneCommand(this);
    CommandHistory::getInstance()->addCommand(command);

    Pane *pane = command->getPane();

    if (configuration.layer == LayerFactory::Spectrum) {
        pane->setPlaybackFollow(PlaybackScrollContinuous);
        pane->setFollowGlobalZoom(false);
        pane->setZoomLevel(ZoomLevel(ZoomLevel::FramesPerPixel, 512));
    }

    if (configuration.layer != LayerFactory::TimeRuler &&
        configuration.layer != LayerFactory::Spectrum) {

        if (!m_timeRulerLayer) {
//            SVCERR << "no time ruler layer, creating one" << endl;
            m_timeRulerLayer = m_document->createMainModelLayer
                (LayerFactory::TimeRuler);
        }

//        SVDEBUG << "adding time ruler layer " << m_timeRulerLayer << endl;

        m_document->addLayerToView(pane, m_timeRulerLayer);
    }

    Layer *newLayer = m_document->createLayer(configuration.layer);

    ModelId suggestedModelId = configuration.sourceModel;
    ModelId modelId;

    if (!suggestedModelId.isNone()) {

        // check its validity
        vector<ModelId> inputModels = m_document->getTransformInputModels();
        for (auto im: inputModels) {
            if (im == suggestedModelId) {
                modelId = suggestedModelId;
            }
        }

        if (modelId.isNone()) {
            SVCERR << "WARNING: Model " << modelId
                 << " appears in pane action map, but is not reported "
                 << "by document as a valid transform source" << endl;
        }
    }

    if (modelId.isNone()) {
        modelId = m_document->getMainModel();
    }

    m_document->setModel(newLayer, modelId);

    m_document->setChannel(newLayer, configuration.channel);
    m_document->addLayerToView(pane, newLayer);

    m_paneStack->setCurrentPane(pane);
    m_paneStack->setCurrentLayer(pane, newLayer);

//    SVDEBUG << "MainWindow::addPane: global centre frame is "
//              << m_viewManager->getGlobalCentreFrame() << endl;
//    pane->setCentreFrame(m_viewManager->getGlobalCentreFrame());

    CommandHistory::getInstance()->endCompoundOperation();

    updateMenuStates();
}

void
MainWindow::addLayer()
{
    QObject *s = sender();
    QAction *action = dynamic_cast<QAction *>(s);
    
    if (!action) {
        SVCERR << "WARNING: MainWindow::addLayer: sender is not an action"
                  << endl;
        return;
    }

    Pane *pane = m_paneStack->getCurrentPane();
    
    if (!pane) {
        SVCERR << "WARNING: MainWindow::addLayer: no current pane" << endl;
        return;
    }

    ExistingLayerActions::iterator ei = m_existingLayerActions.begin();
    while (ei != m_existingLayerActions.end()) {
        if (ei->first == action) break;
        ++ei;
    }

    if (ei != m_existingLayerActions.end()) {
        Layer *newLayer = ei->second;
        m_document->addLayerToView(pane, newLayer);
        m_paneStack->setCurrentLayer(pane, newLayer);
        return;
    }

    ei = m_sliceActions.begin();
    while (ei != m_sliceActions.end()) {
        if (ei->first == action) break;
        ++ei;
    }

    if (ei != m_sliceActions.end()) {
        Layer *newLayer = m_document->createLayer(LayerFactory::Slice);
//        document->setModel(newLayer, ei->second->getModel());
        SliceableLayer *source = dynamic_cast<SliceableLayer *>(ei->second);
        SliceLayer *dest = dynamic_cast<SliceLayer *>(newLayer);
        if (source && dest) {
            //!!!???
            dest->setSliceableModel(source->getSliceableModel());
            connect(source, SIGNAL(sliceableModelReplaced(const Model *, const Model *)),
                    dest, SLOT(sliceableModelReplaced(const Model *, const Model *)));
            connect(m_document, SIGNAL(modelAboutToBeDeleted(Model *)),
                    dest, SLOT(modelAboutToBeDeleted(Model *)));
        }
        m_document->addLayerToView(pane, newLayer);
        m_paneStack->setCurrentLayer(pane, newLayer);
        return;
    }

    TransformActions::iterator i = m_transformActions.begin();
    while (i != m_transformActions.end()) {
        if (i->first == action) break;
        ++i;
    }

    if (i == m_transformActions.end()) {

        LayerActions::iterator i = m_layerActions.begin();
        while (i != m_layerActions.end()) {
            if (i->first == action) break;
            ++i;
        }
        
        if (i == m_layerActions.end()) {
            SVCERR << "WARNING: MainWindow::addLayer: unknown action "
                      << action->objectName() << endl;
            return;
        }

        LayerFactory::LayerType type = i->second.layer;
        
        LayerFactory::LayerTypeSet emptyTypes =
            LayerFactory::getInstance()->getValidEmptyLayerTypes();

        Layer *newLayer = nullptr;

        bool isNewEmptyLayer = false;
        
        if (emptyTypes.find(type) != emptyTypes.end()) {

            newLayer = m_document->createEmptyLayer(type);
            if (newLayer) {
                isNewEmptyLayer = true;
            }

        } else {

            ModelId modelId = i->second.sourceModel;

            if (modelId.isNone()) {
                if (type == LayerFactory::TimeRuler) {
                    newLayer = m_document->createMainModelLayer(type);
                } else {
                    // if model is unspecified and this is not a
                    // time-ruler layer, use any plausible model from
                    // the current pane -- this is the case for
                    // right-button menu layer additions
                    Pane::ModelSet ms = pane->getModels();
                    for (ModelId m: ms) {
                        if (ModelById::isa<RangeSummarisableTimeValueModel>(m)) {
                            modelId = m;
                        }
                    }
                    if (modelId.isNone()) {
                        modelId = getMainModelId();
                    }
                }
            }

            if (!modelId.isNone()) {
                newLayer = m_document->createLayer(type);
                if (m_document->isKnownModel(modelId)) {
                    m_document->setChannel(newLayer, i->second.channel);
                    m_document->setModel(newLayer, modelId);
                } else {
                    SVCERR << "WARNING: MainWindow::addLayer: unknown model "
                           << modelId << " in layer action map" << endl;
                }
            }
        }

        if (isNewEmptyLayer) {

            CoordinateScale scale = pane->getEffectiveVerticalExtents();
            if (scale.getUnit() != "") {
                // This is particularly relevant to new BoxLayers
                newLayer->adoptExtents(scale.getDisplayMinimum(),
                                       scale.getDisplayMaximum(),
                                       scale.getUnit());
            }
            
            for (auto &a : m_toolActions) {
                if (a.first == ViewManager::DrawMode) {
                    a.second->trigger();
                    break;
                }
            }
        }

        if (newLayer) {
            m_document->addLayerToView(pane, newLayer);
            m_paneStack->setCurrentLayer(pane, newLayer);
        }

        return;
    }

    //!!! want to do something like this, but it's not supported in
    //ModelTransformerFactory yet
    /*
    int channel = -1;
    // pick up the default channel from any existing layers on the same pane
    for (int j = 0; j < pane->getLayerCount(); ++j) {
        int c = LayerFactory::getInstance()->getChannel(pane->getLayer(j));
        if (c != -1) {
            channel = c;
            break;
        }
    }
    */

    // We always ask for configuration, even if the plugin isn't
    // supposed to be configurable, because we need to let the user
    // change the execution context (block size etc).

    QString transformId = i->second;

    addLayer(transformId);
}

void
MainWindow::addLayer(QString transformId)
{
    Pane *pane = m_paneStack->getCurrentPane();
    if (!pane) {
        SVCERR << "WARNING: MainWindow::addLayer: no current pane" << endl;
        return;
    }

    Transform transform;
    try {
        transform = TransformFactory::getInstance()->
            getDefaultTransformFor(transformId);
    } catch (std::exception &e) { // e.g. Piper server failure
        QMessageBox::critical
            (this, tr("Failed to query transform attributes"),
             tr("<b>Failed to query transform attributes</b><p>Plugin or server error: %1</p>")
             .arg(e.what()));
        return;
    }

    vector<ModelId> candidateInputModels =
        m_document->getTransformInputModels();

    ModelId defaultInputModelId;

    for (int j = 0; j < pane->getLayerCount(); ++j) {

        Layer *layer = pane->getLayer(j);
        if (!layer) continue;

        if (LayerFactory::getInstance()->getLayerType(layer) !=
            LayerFactory::Waveform &&
            !layer->isLayerOpaque()) {
            continue;
        }

        ModelId modelId = layer->getModel();
        if (modelId.isNone()) continue;

        for (ModelId candidateId: candidateInputModels) {
            if (candidateId == modelId) {
                defaultInputModelId = modelId;
                break;
            }
        }

        if (!defaultInputModelId.isNone()) break;
    }

    ModelId aggregate;
    
    if (candidateInputModels.size() > 1) {
        // Add an aggregate model as another option
        AggregateWaveModel::ChannelSpecList sl;
        for (ModelId mid: candidateInputModels) {
            if (ModelById::isa<RangeSummarisableTimeValueModel>(mid)) {
                sl.push_back(AggregateWaveModel::ModelChannelSpec(mid, -1));
            }
        }
        if (!sl.empty()) {
            auto aggregate = std::make_shared<AggregateWaveModel>(sl);
            aggregate->setObjectName(tr("Multiplex all of the above"));
            candidateInputModels.push_back(ModelById::add(aggregate));
        }
    }
    
    sv_frame_t startFrame = 0, duration = 0;
    sv_frame_t endFrame = 0;
    m_viewManager->getSelection().getExtents(startFrame, endFrame);
    if (endFrame > startFrame) duration = endFrame - startFrame;
    else startFrame = 0;

    TransformUserConfigurator configurator;

    ModelTransformer::Input input = ModelTransformerFactory::getInstance()->
        getConfigurationForTransform
        (transform,
         candidateInputModels,
         defaultInputModelId,
         m_playSource,
         startFrame,
         duration,
         &configurator);

    if (!aggregate.isNone()) {
        if (input.getModel() == aggregate) {
            if (auto aggregateModel = ModelById::get(aggregate)) {
                aggregateModel->setObjectName(tr("Multiplexed audio"));
            }
            m_document->addNonDerivedModel(aggregate);
        } else {
            ModelById::release(aggregate);
        }
    }
    
    if (input.getModel().isNone()) return;

//    SVDEBUG << "MainWindow::addLayer: Input model is " << input.getModel() << " \"" << input.getModel()->objectName() << "\"" << endl << "transform:" << endl << transform.toXmlString() << endl;

    try {
        Layer *newLayer = m_document->createDerivedLayer(transform, input);
        if (newLayer) {
            m_document->addLayerToView(pane, newLayer);
            m_document->setChannel(newLayer, input.getChannel());
            m_recentTransforms.add(transformId);
            m_paneStack->setCurrentLayer(pane, newLayer);
        }
    } catch (std::exception &e) { // e.g. Piper server failure
        QMessageBox::critical
            (this, tr("Transform failed"),
             tr("<b>Failed to run transform</b><p>Plugin or server error: %1</p>")
             .arg(e.what()));
        return;
    }
    
    updateMenuStates();
}

void
MainWindow::renameCurrentLayer()
{
    Pane *pane = m_paneStack->getCurrentPane();
    if (!pane) return;
    
    Layer *layer = pane->getSelectedLayer();
    if (!layer) return;
    
    bool ok = false;
    QString newName = QInputDialog::getText
        (this, tr("Rename Layer"),
         tr("New name for this layer:"),
         QLineEdit::Normal, layer->objectName(), &ok);
    if (!ok) return;
            
    bool existingNameSet = layer->isPresentationNameSet();
    QString existingName = layer->getLayerPresentationName();

    CommandHistory::getInstance()->addCommand
        (new GenericCommand
         (tr("Rename Layer"),
          [=]() {
              layer->setPresentationName(newName);
              setupExistingLayersMenus();
          },
          [=]() {
              layer->setPresentationName(existingNameSet ? existingName : "");
              setupExistingLayersMenus();
          }));
}

void
MainWindow::findTransform()
{
    TransformFinder *finder = new TransformFinder(this);
    if (!finder->exec()) {
        delete finder;
        return;
    }
    TransformId transform = finder->getTransform();
    delete finder;
    
    if (getMainModel() != nullptr && m_paneStack->getCurrentPane() != nullptr) {
        addLayer(transform);
    }
}

void
MainWindow::playSoloToggled()
{
    MainWindowBase::playSoloToggled();
    m_soloModified = true;
}

void
MainWindow::alignToggled()
{
    QAction *action = dynamic_cast<QAction *>(sender());
    
    if (!m_viewManager) return;

    if (action) {
        m_viewManager->setAlignMode(action->isChecked());
    } else {
        m_viewManager->setAlignMode(!m_viewManager->getAlignMode());
    }

    if (m_viewManager->getAlignMode()) {
        m_prevSolo = m_soloAction->isChecked();
        if (!m_soloAction->isChecked()) {
            m_soloAction->setChecked(true);
            MainWindowBase::playSoloToggled();
        }
        m_soloModified = false;
        emit canChangeSolo(false);
        m_document->alignModels();
        m_document->setAutoAlignment(true);
    } else {
        if (!m_soloModified) {
            if (m_soloAction->isChecked() != m_prevSolo) {
                m_soloAction->setChecked(m_prevSolo);
                MainWindowBase::playSoloToggled();
            }
        }
        emit canChangeSolo(true);
        m_document->setAutoAlignment(false);
    }

    for (int i = 0; i < m_paneStack->getPaneCount(); ++i) {

        Pane *pane = m_paneStack->getPane(i);
        if (!pane) continue;

        pane->update();
    }
}

void
MainWindow::playSpeedChanged(int position)
{
    PlaySpeedRangeMapper mapper;

    double percent = m_playSpeed->mappedValue();
    double factor = mapper.getFactorForValue(percent);

//    SVCERR << "play speed position = " << position << " (range 0-120) percent = " << percent << " factor = " << factor << endl;

    int centre = m_playSpeed->defaultValue();

    // Percentage is shown to 0dp if >100, to 1dp if <100; factor is
    // shown to 3sf

    constexpr size_t buflen = 30;
    char pcbuf[buflen];
    char facbuf[buflen];
    
    if (position == centre) {
        contextHelpChanged(tr("Playback speed: Normal"));
    } else if (position < centre) {
        snprintf(pcbuf, buflen, "%.1f", percent);
        snprintf(facbuf, buflen, "%.3g", 1.0 / factor);
        contextHelpChanged(tr("Playback speed: %1% (%2x slower)")
                           .arg(pcbuf)
                           .arg(facbuf));
    } else {
        snprintf(pcbuf, buflen, "%.0f", percent);
        snprintf(facbuf, buflen, "%.3g", factor);
        contextHelpChanged(tr("Playback speed: %1% (%2x faster)")
                           .arg(pcbuf)
                           .arg(facbuf));
    }

    m_playSource->setTimeStretch(1.0 / factor); // factor is a speedup

    updateMenuStates();
}

void
MainWindow::speedUpPlayback()
{
    int value = m_playSpeed->value();
    value = value + m_playSpeed->pageStep();
    if (value > m_playSpeed->maximum()) value = m_playSpeed->maximum();
    m_playSpeed->setValue(value);
}

void
MainWindow::slowDownPlayback()
{
    int value = m_playSpeed->value();
    value = value - m_playSpeed->pageStep();
    if (value < m_playSpeed->minimum()) value = m_playSpeed->minimum();
    m_playSpeed->setValue(value);
}

void
MainWindow::restoreNormalPlayback()
{
    m_playSpeed->setValue(m_playSpeed->defaultValue());
}

void
MainWindow::tempoCurveRequestedAudioModelChange(ModelId audioModel)
{
    for (int i = 0; i < m_paneStack->getPaneCount(); ++i) {
        Pane *pane = m_paneStack->getPane(i);
        if (!pane) continue;
        for (int j = 0; j < pane->getLayerCount(); ++j) {
            Layer *layer = pane->getLayer(j);
            if (!layer) continue;
            if (dynamic_cast<SpectrogramLayer *>(layer) &&
                layer->getModel() == audioModel) {
                m_paneStack->setCurrentPane(pane);
                return;
            }
        }
    }
}

void
MainWindow::currentPaneChanged(Pane *pane)
{
    // We don't call MainWindowBase::currentPaneChanged from this.
    // One of the big things it does is to update the solo model set
    // for playback in solo mode, but we want slightly different
    // behaviour from the SV default. While SV solos all models shown
    // in the selected pane, we want always to solo only the current
    // audio model, and any non-audio models derived from it.

    updateVisibleRangeDisplay(pane);
    
    if (!pane) return;
    
    QString scoreLabel;
    double proportion = 0;
    bool wasPlaying = false;
    sv_frame_t playingFrame = 0;
    if (m_playSource && m_playSource->isPlaying()) {
        playingFrame = m_playSource->getCurrentPlayingFrame();
        wasPlaying = true;
    } else {
        playingFrame = m_viewManager->getPlaybackFrame();
    }

    m_scoreBasedFrameAligner->mapToScoreLabelAndProportion
        (m_session.getOnsetsLayer(), playingFrame, scoreLabel, proportion);
    SVDEBUG << "currentPaneChanged: mapped current frame "
            << playingFrame << " to score label " << scoreLabel
            << " and proportion " << proportion << endl;
    if (scoreLabel != "" && wasPlaying) {
        m_playSource->stop();
    }

    for (int i = pane->getLayerCount(); i > 0; ) {
        --i;
        Layer *layer = pane->getLayer(i);
        ModelId modelId = layer->getModel();
        if (ModelById::isa<RangeSummarisableTimeValueModel>(modelId)) {
            auto type = LayerFactory::getInstance()->getLayerType(layer);
            if (type != LayerFactory::TimeRuler) {
                updateLayerShortcutsFor(modelId);
            }
        }
    }

    m_session.setActivePane(pane);

    if (m_viewManager->getPlaySoloMode()) {

        // We want to solo only the active audio model for the pane
        // (in case there is more than one audio model here), as well
        // as any non-audio models that are on top of it
        
        ModelId activeModel = m_session.getActiveAudioModel();
        std::set<ModelId> soloModels;
        soloModels.insert(activeModel);
    
        for (int i = pane->getLayerCount(); i > 0; ) {
            --i;
            Layer *layer = pane->getLayer(i);
            if (layer->isLayerDormant(pane)) {
                continue;
            }
            ModelId modelId = layer->getModel();
            if (modelId == activeModel) {
                break;
            }
            soloModels.insert(modelId);
            if (ModelById::isa<RangeSummarisableTimeValueModel>(modelId)) {
                break;
            }
        }

        m_viewManager->setPlaybackModel(activeModel);
        m_playSource->setSoloModelSet(soloModels);
        
    } else {
        m_viewManager->setPlaybackModel({});
        m_playSource->setSoloModelSet({});
    }
    
    if (scoreLabel != "") {
        m_scoreBasedFrameAligner->mapFromScoreLabelAndProportion
            (m_session.getOnsetsLayer(), scoreLabel, proportion, playingFrame);
        SVDEBUG << "currentPaneChanged: mapped score label " << scoreLabel
                << " and proportion " << proportion
                << " back to playback frame " << playingFrame << endl;
        if (wasPlaying) {
            m_playSource->play(playingFrame);
        } else {
            m_viewManager->setPlaybackFrame(playingFrame);
        }
    }

    updateWindowTitle();
    updateMenuStates();
}

void
MainWindow::updateVisibleRangeDisplay(Pane *p) const
{
    sv_samplerate_t sampleRate = 0;
    if (auto mm = getMainModel()) {
        sampleRate = mm->getSampleRate();
    } else {
        return;
    }
    if (!p) {
        return;
    }

    bool haveSelection = false;
    sv_frame_t startFrame = 0, endFrame = 0;

    if (m_viewManager && m_viewManager->haveInProgressSelection()) {

        bool exclusive = false;
        Selection s = m_viewManager->getInProgressSelection(exclusive);

        if (!s.isEmpty()) {
            haveSelection = true;
            startFrame = s.getStartFrame();
            endFrame = s.getEndFrame();
        }
    }

    if (!haveSelection) {
        startFrame = p->getFirstVisibleFrame();
        endFrame = p->getLastVisibleFrame();
    }

    RealTime start = RealTime::frame2RealTime(startFrame, sampleRate);
    RealTime end = RealTime::frame2RealTime(endFrame, sampleRate);
    RealTime duration = end - start;

    QString startStr, endStr, durationStr;
    startStr = start.toText(true).c_str();
    endStr = end.toText(true).c_str();
    durationStr = duration.toText(true).c_str();

    if (haveSelection) {
        m_myStatusMessage = tr("Selection: %1 to %2 (duration %3)")
            .arg(startStr).arg(endStr).arg(durationStr);
    } else {
        m_myStatusMessage = tr("Visible: %1 to %2 (duration %3)")
            .arg(startStr).arg(endStr).arg(durationStr);
    }

    if (getStatusLabel()->text() != m_myStatusMessage) {
        getStatusLabel()->setText(m_myStatusMessage);
    }

    updatePositionStatusDisplays();
}

void
MainWindow::updatePositionStatusDisplays() const
{
    if (!statusBar()->isVisible()) return;

    Pane *pane = nullptr;
    sv_frame_t frame = m_viewManager->getPlaybackFrame();

    if (m_paneStack) pane = m_paneStack->getCurrentPane();
    if (!pane) return;

    int layers = pane->getLayerCount();
    if (layers == 0) m_currentLabel->setText("");

    for (int i = layers-1; i >= 0; --i) {
        Layer *layer = pane->getLayer(i);
        if (!layer) continue;
        if (!layer->isLayerEditable()) continue;
        QString label = layer->getLabelAtOrPreceding
            (pane->alignFromReference(frame));
        label = label.split('\n')[0];
        m_currentLabel->setText(label);
        break;
    }
}

void
MainWindow::monitoringLevelsChanged(float left, float right)
{
    m_mainLevelPan->setMonitoringLevels(left, right);
}

void
MainWindow::sampleRateMismatch(sv_samplerate_t requested,
                               sv_samplerate_t actual,
                               bool willResample)
{
    if (!willResample) {
        emit hideSplash();
        QMessageBox::information
            (this, tr("Sample rate mismatch"),
             tr("<b>Wrong sample rate</b><p>The sample rate of this audio file (%1 Hz) does not match\nthe current playback rate (%2 Hz).<p>The file will play at the wrong speed and pitch.<p>Change the <i>Resample mismatching files on import</i> option under <i>File</i> -> <i>Preferences</i> if you want to alter this behaviour.")
             .arg(requested).arg(actual));
    }        

    updateDescriptionLabel();
}

void
MainWindow::audioOverloadPluginDisabled()
{
    QMessageBox::information
        (this, tr("Audio processing overload"),
         tr("<b>Overloaded</b><p>Audio effects plugin auditioning has been disabled due to a processing overload."));
}

void
MainWindow::betaReleaseWarning()
{
    QMessageBox::information
        (this, tr("Beta release"),
         tr("<b>This is a beta release of %1</b><p>Please see the \"What's New\" option in the Help menu for a list of changes since the last proper release.</p>").arg(QApplication::applicationName()));
}

void
MainWindow::pluginPopulationWarning(QString warning)
{
    emit hideSplash();
    QMessageBox box;
    box.setWindowTitle(tr("Problems loading plugins"));
    box.setText(tr("<b>Failed to load plugins</b>"));
    box.setInformativeText(warning);
    box.setIcon(QMessageBox::Warning);
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();
}

void
MainWindow::midiEventsAvailable()
{
    Pane *currentPane = nullptr;
    NoteLayer *currentNoteLayer = nullptr;
    TimeValueLayer *currentTimeValueLayer = nullptr;

    if (m_paneStack) {
        currentPane = m_paneStack->getCurrentPane();
    }

    if (currentPane) {
        currentNoteLayer = dynamic_cast<NoteLayer *>
            (currentPane->getSelectedLayer());
        currentTimeValueLayer = dynamic_cast<TimeValueLayer *>
            (currentPane->getSelectedLayer());
    } else {
        // discard these events
        while (m_midiInput->getEventsAvailable() > 0) {
            (void)m_midiInput->readEvent();
        }
        return;
    }

    // This is called through a serialised signal/slot invocation
    // (across threads).  It could happen quite some time after the
    // event was actually received, which is why event timestamping
    // happens in the MIDI input class and not here.

    while (m_midiInput->getEventsAvailable() > 0) {

        MIDIEvent ev(m_midiInput->readEvent());

        sv_frame_t frame = currentPane->alignFromReference(ev.getTime());

        bool noteOn = (ev.getMessageType() == MIDIConstants::MIDI_NOTE_ON &&
                       ev.getVelocity() > 0);

        bool noteOff = (ev.getMessageType() == MIDIConstants::MIDI_NOTE_OFF ||
                        (ev.getMessageType() == MIDIConstants::MIDI_NOTE_ON &&
                         ev.getVelocity() == 0));

        if (currentNoteLayer) {

            if (!m_playSource || !m_playSource->isPlaying()) continue;

            if (noteOn) {

                currentNoteLayer->addNoteOn(frame,
                                            ev.getPitch(),
                                            ev.getVelocity());

            } else if (noteOff) {

                currentNoteLayer->addNoteOff(frame,
                                             ev.getPitch());

            }

            continue;
        }

        if (currentTimeValueLayer) {

            if (!noteOn) continue;

            if (!m_playSource || !m_playSource->isPlaying()) continue;

            ModelId modelId = currentTimeValueLayer->getModel();
            if (ModelById::isa<SparseTimeValueModel>(modelId)) {
                Event point(frame, float(ev.getPitch() % 12), "");
                AddEventCommand *command = new AddEventCommand
                    (modelId.untyped, point, tr("Add Point"));
                CommandHistory::getInstance()->addCommand(command);
            }

            continue;
        }

        // This is reached only if !currentNoteLayer and
        // !currentTimeValueLayer, i.e. there is some other sort of
        // layer that may be insertable-into

        if (!noteOn) continue;
        insertInstantAt(ev.getTime());
    }
}    

void
MainWindow::playStatusChanged(bool )
{
    Pane *currentPane = nullptr;
    NoteLayer *currentNoteLayer = nullptr;

    if (m_paneStack) currentPane = m_paneStack->getCurrentPane();
    if (currentPane) {
        currentNoteLayer = dynamic_cast<NoteLayer *>(currentPane->getSelectedLayer());
    }

    if (currentNoteLayer) {
        currentNoteLayer->abandonNoteOns();
    }
}

void
MainWindow::layerRemoved(Layer *layer)
{
    Profiler profiler("MainWindow::layerRemoved");
    setupExistingLayersMenus();
    MainWindowBase::layerRemoved(layer);
}

void
MainWindow::layerInAView(Layer *layer, bool inAView)
{
    setupExistingLayersMenus();
    MainWindowBase::layerInAView(layer, inAView);
}

void
MainWindow::modelAdded(ModelId modelId)
{
    MainWindowBase::modelAdded(modelId);
    if (ModelById::isa<DenseTimeValueModel>(modelId)) {
        setupPaneAndLayerMenus();
    }
}

void
MainWindow::mainModelChanged(ModelId modelId)
{
    SVDEBUG << "MainWindow::mainModelChanged" << endl;
    
    MainWindowBase::mainModelChanged(modelId);

    if (m_playTarget || m_audioIO) {
        connect(m_mainLevelPan, SIGNAL(levelChanged(float)),
                this, SLOT(mainModelGainChanged(float)));
        connect(m_mainLevelPan, SIGNAL(panChanged(float)),
                this, SLOT(mainModelPanChanged(float)));
    }

    if (m_viewManager->getAlignMode() && !modelId.isNone()) {
        m_document->realignModels();
    }

    zoomToFit();
    rewindStart();

    SVDEBUG << "MainWindow::mainModelChanged: Now calling m_session.setMainModel" << endl;

    m_session.setMainModel(modelId);
    m_alignButton->setEnabled(!modelId.isNone());
}

void
MainWindow::updateAlignButtonText()
{
    bool subsetOfAudioSelected = !m_viewManager->getSelections().empty();
    QString label = tr("Align");
    if (m_chooseSmartCopyAction && m_chooseSmartCopyAction->isChecked()) {
        label = tr("Smart Copy from First Recording");
    } else if (m_subsetOfScoreSelected) {
        if (subsetOfAudioSelected) {
            label = tr("Align Selections of Score and Audio");
        } else {
            label = tr("Align Selection of Score with All of Audio");
        }
    } else {
        if (subsetOfAudioSelected) {
            label = tr("Align All of Score with Selection of Audio");
        } else {
            label = tr("Align Score with Audio");
        }
    }
    m_alignButton->setText(label);
}

void
MainWindow::mainModelGainChanged(float gain)
{
    if (m_playTarget) {
        m_playTarget->setOutputGain(gain);
    } else if (m_audioIO) {
        m_audioIO->setOutputGain(gain);
    }
}

void
MainWindow::mainModelPanChanged(float balance)
{
    // this is indeed stereo balance rather than pan
    if (m_playTarget) {
        m_playTarget->setOutputBalance(balance);
    } else if (m_audioIO) {
        m_audioIO->setOutputBalance(balance);
    }
}

void
MainWindow::setInstantsNumbering()
{
    QAction *a = dynamic_cast<QAction *>(sender());
    if (!a) return;

    int type = 0;
    for (auto &ai : m_numberingActions) {
        if (ai.first == a) type = ai.second;
    }
    
    if (m_labeller) m_labeller->setType(Labeller::ValueType(type));

    QSettings settings;
    settings.beginGroup("MainWindow");
    settings.setValue("labellertype", type);
    settings.endGroup();
}

void
MainWindow::setInstantsCounterCycle()
{
    QAction *a = dynamic_cast<QAction *>(sender());
    if (!a) return;
    
    int cycle = a->text().toInt();
    if (cycle == 0) return;

    if (m_labeller) m_labeller->setCounterCycleSize(cycle);
    
    QSettings settings;
    settings.beginGroup("MainWindow");
    settings.setValue("labellercycle", cycle);
    settings.endGroup();
}

void
MainWindow::setInstantsCounters()
{
    LabelCounterInputDialog dialog(m_labeller, this);
    dialog.setWindowTitle(tr("Reset Counters"));
    dialog.exec();
}

void
MainWindow::resetInstantsCounters()
{
    if (m_labeller) m_labeller->resetCounters();
}

void
MainWindow::subdivideInstants()
{
    QSettings settings;
    settings.beginGroup("MainWindow");
    int n = settings.value("subdivisions", 4).toInt();
    
    bool ok;

    n = QInputDialog::getInt(this,
                             tr("Subdivide instants"),
                             tr("Number of subdivisions:"),
                             n, 2, 96, 1, &ok);

    if (ok) {
        settings.setValue("subdivisions", n);
        subdivideInstantsBy(n);
    }

    settings.endGroup();
}

void
MainWindow::winnowInstants()
{
    QSettings settings;
    settings.beginGroup("MainWindow");
    int n = settings.value("winnow-subdivisions", 4).toInt();
    
    bool ok;

    n = QInputDialog::getInt(this,
                             tr("Winnow instants"),
                             tr("Remove all instants apart from multiples of:"),
                             n, 2, 96, 1, &ok);

    if (ok) {
        settings.setValue("winnow-subdivisions", n);
        winnowInstantsBy(n);
    }

    settings.endGroup();
}

void
MainWindow::modelGenerationFailed(QString transformName, QString message)
{
    emit hideSplash();

    QString quoted;
    if (transformName != "") {
        quoted = QString("\"%1\" ").arg(transformName);
    }
    
    if (message != "") {

        QMessageBox::warning
            (this,
             tr("Failed to generate layer"),
             tr("<b>Layer generation failed</b><p>Failed to generate derived layer.<p>The layer transform %1failed:<p>%2")
             .arg(quoted).arg(message),
             QMessageBox::Ok);
    } else {
        QMessageBox::warning
            (this,
             tr("Failed to generate layer"),
             tr("<b>Layer generation failed</b><p>Failed to generate a derived layer.<p>The layer transform %1failed.<p>No error information is available.")
             .arg(quoted),
             QMessageBox::Ok);
    }
}

void
MainWindow::modelGenerationWarning(QString /* transformName */, QString message)
{
    emit hideSplash();

    QMessageBox::warning
        (this, tr("Warning"), message, QMessageBox::Ok);
}

void
MainWindow::modelRegenerationFailed(QString layerName,
                                    QString transformName, QString message)
{
    emit hideSplash();

    if (message != "") {

        QMessageBox::warning
            (this,
             tr("Failed to regenerate layer"),
             tr("<b>Layer generation failed</b><p>Failed to regenerate derived layer \"%1\" using new data model as input.<p>The layer transform \"%2\" failed:<p>%3")
             .arg(layerName).arg(transformName).arg(message),
             QMessageBox::Ok);
    } else {
        QMessageBox::warning
            (this,
             tr("Failed to regenerate layer"),
             tr("<b>Layer generation failed</b><p>Failed to regenerate derived layer \"%1\" using new data model as input.<p>The layer transform \"%2\" failed.<p>No error information is available.")
             .arg(layerName).arg(transformName),
             QMessageBox::Ok);
    }
}

void
MainWindow::modelRegenerationWarning(QString layerName,
                                     QString /* transformName */,
                                     QString message)
{
    emit hideSplash();

    QMessageBox::warning
        (this, tr("Warning"), tr("<b>Warning when regenerating layer</b><p>When regenerating the derived layer \"%1\" using new data model as input:<p>%2").arg(layerName).arg(message), QMessageBox::Ok);
}

void
MainWindow::alignmentFailed(ModelId, QString message)
{
    QMessageBox::warning
        (this,
         tr("Failed to calculate alignment"),
         tr("<b>Alignment calculation failed</b><p>Failed to calculate an audio alignment:<p>%1")
         .arg(message),
         QMessageBox::Ok);
}

void
MainWindow::paneRightButtonMenuRequested(Pane *pane, QPoint position)
{
    m_paneStack->setCurrentPane(pane);
    m_rightButtonMenu->popup(position);
}

void
MainWindow::panePropertiesRightButtonMenuRequested(Pane *pane, QPoint position)
{
    if (m_lastRightButtonPropertyMenu) {
        delete m_lastRightButtonPropertyMenu;
    }
    
    QMenu *m = new QMenu;
    IconLoader il;

    MenuTitle::addTitle(m, tr("Pane"));

    m_paneStack->setCurrentLayer(pane, nullptr);

    m->addAction(il.load("editdelete"), tr("&Delete Pane"),
                 this, SLOT(deleteCurrentPane()));

    m->popup(position);
    m_lastRightButtonPropertyMenu = m;
}

void
MainWindow::layerPropertiesRightButtonMenuRequested(Pane *pane, Layer *layer, QPoint position)
{
    if (m_lastRightButtonPropertyMenu) {
        delete m_lastRightButtonPropertyMenu;
    }
    
    QMenu *m = new QMenu;
    IconLoader il;

    MenuTitle::addTitle(m, layer->getLayerPresentationName());

    m_paneStack->setCurrentLayer(pane, layer);

    m->addAction(tr("&Rename Layer..."),
                 this, SLOT(renameCurrentLayer()));

    m->addAction(tr("Edit Layer Data"),
                 this, SLOT(editCurrentLayer()))
        ->setEnabled(layer->isLayerEditable());

    m->addAction(il.load("editdelete"), tr("&Delete Layer"),
                 this, SLOT(deleteCurrentLayer()));

    m->popup(position);
    m_lastRightButtonPropertyMenu = m;
}

void
MainWindow::showLayerTree()
{
    if (!m_layerTreeDialog.isNull()) {
        m_layerTreeDialog->show();
        m_layerTreeDialog->raise();
        return;
    }

    m_layerTreeDialog = new LayerTreeDialog(m_paneStack, this);
    m_layerTreeDialog->setAttribute(Qt::WA_DeleteOnClose); // see below
    m_layerTreeDialog->show();
}

void
MainWindow::showActivityLog()
{
    m_activityLog->show();
    m_activityLog->raise();
    m_activityLog->scrollToEnd();
}

void
MainWindow::showUnitConverter()
{
    m_unitConverter->show();
    m_unitConverter->raise();
}

void
MainWindow::preferences()
{
    bool goToTemplateTab =
        (sender() && sender()->objectName() == "set_default_template");

    if (!m_preferencesDialog.isNull()) {
        m_preferencesDialog->show();
        m_preferencesDialog->raise();
        if (goToTemplateTab) {
            m_preferencesDialog->switchToTab(PreferencesDialog::TemplateTab);
        }
        return;
    }

    m_preferencesDialog = new PreferencesDialog(this);

    connect(m_preferencesDialog, SIGNAL(audioDeviceChanged()),
            this, SLOT(recreateAudioIO()));
    connect(m_preferencesDialog, SIGNAL(coloursChanged()),
            this, SLOT(coloursChanged()));
    
    // DeleteOnClose is safe here, because m_preferencesDialog is a
    // QPointer that will be zeroed when the dialog is deleted.  We
    // use it in preference to leaving the dialog lying around because
    // if you Cancel the dialog, it resets the preferences state
    // without resetting its own widgets, so its state will be
    // incorrect when next shown unless we construct it afresh
    m_preferencesDialog->setAttribute(Qt::WA_DeleteOnClose);

    m_preferencesDialog->show();
    if (goToTemplateTab) {
        m_preferencesDialog->switchToTab(PreferencesDialog::TemplateTab);
    }
}

void
MainWindow::mouseEnteredWidget()
{
    QWidget *w = dynamic_cast<QWidget *>(sender());
    if (!w) return;

    QString mainText, editText;

    if (w == m_mainLevelPan) {
        mainText = tr("Adjust the master playback level and pan");
        editText = tr("click then drag to adjust, ctrl+click to reset");
    } else if (w == m_playSpeed) {
        mainText = tr("Adjust the master playback speed");
        editText = tr("drag up/down to adjust, ctrl+click to reset");
    }

    if (mainText != "") {
        contextHelpChanged(tr("%1: %2").arg(mainText).arg(editText));
    }
}

void
MainWindow::mouseLeftWidget()
{
    contextHelpChanged("");
}

void
MainWindow::website()
{
    openHelpUrl(tr("http://www.sonicvisualiser.org/"));
}

void
MainWindow::help()
{
    openHelpUrl(tr("http://www.sonicvisualiser.org/doc/reference/%1/en/").arg(SV_VERSION));
}

void
MainWindow::whatsNew()
{
    QFile changelog(":CHANGELOG");
    changelog.open(QFile::ReadOnly);
    QByteArray content = changelog.readAll();
    QString text = QString::fromUtf8(content);

    QDialog *d = new QDialog(this);
    d->setWindowTitle(tr("What's New"));
        
    QGridLayout *layout = new QGridLayout;
    d->setLayout(layout);

    int row = 0;
    
    QLabel *iconLabel = new QLabel;
    iconLabel->setPixmap(QApplication::windowIcon().pixmap(64, 64));
    layout->addWidget(iconLabel, row, 0);
    
    layout->addWidget
        (new QLabel(tr("<h3>What's New in %1</h3>")
                    .arg(QApplication::applicationName())),
         row++, 1);
    layout->setColumnStretch(2, 10);

    QTextEdit *textEdit = new QTextEdit;
    layout->addWidget(textEdit, row++, 1, 1, 2);

    if (m_newerVersionIs != "") {
        layout->addWidget(new QLabel(tr("<b>Note:</b> A newer version of Sonic Visualiser is available.<br>(Version %1 is available; you are using version %2)").arg(m_newerVersionIs).arg(SV_VERSION)), row++, 1, 1, 2);
    }
    
    QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok);
    layout->addWidget(bb, row++, 0, 1, 3);
    connect(bb, SIGNAL(accepted()), d, SLOT(accept()));

    // Remove spurious linefeeds from DOS line endings
    text.replace('\r', "");

    // Un-wrap indented paragraphs (assume they are always preceded by
    // an empty line, so don't get merged into prior para)
    text.replace(QRegularExpression("(.)\n +(.)"), "\\1 \\2");

    // Rest of para following a " - " at start becomes bulleted entry
    text.replace(QRegularExpression("\n - ([^\n]+)"), "\n<li>\\1</li>");

    // Line-ending ":" introduces the bulleted list
    text.replace(QRegularExpression(": *\n"), ":\n<ul>\n");

    // Blank line (after unwrapping) ends the bulleted list
    text.replace(QRegularExpression("</li>\n\\s*\n"), "</li>\n</ul>\n\n");

    // Text leading up to that line-ending ":" becomes bold heading
    text.replace(QRegularExpression("\n(\\w[^:\n]+:)"), "\n<p><b>\\1</b></p>");
    
    textEdit->setHtml(text);
    textEdit->setReadOnly(true);

    d->setMinimumSize(m_viewManager->scalePixelSize(520),
                      m_viewManager->scalePixelSize(450));
    
    d->exec();

    delete d;
}

QString
MainWindow::getReleaseText() const
{
    bool debug = false;
    QString version = "(unknown version)";

#ifdef BUILD_DEBUG
    debug = true;
#endif // BUILD_DEBUG

    version = tr("Release %1").arg(SV_VERSION);

    QString archtag;
#ifdef Q_OS_MAC
#if (defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64))
    archtag = " (arm64)";
#elif (defined(__x86_64__) || defined(__i386__) || defined(_M_IX86) || defined(_M_X64))
    archtag = " (x86_64)";
#else
    archtag = " (unknown arch)"
#endif
#endif
    
    return tr("%1 : %2 configuration, %3-bit build%4")
        .arg(version)
        .arg(debug ? tr("Debug") : tr("Release"))
        .arg(sizeof(void *) * 8)
        .arg(archtag);
}

void
MainWindow::introduction()
{
#ifdef Q_OS_WIN32
    QString introText =
        "<h3>How to use Performance Precision</h3>"
        "<p><i>You can open this instruction page at any time from the Help menu.</i><p>"
        "<p>This is a software tool that assists in analyzing recorded performances together with their scores.</p>"
        "<p>The controls you'll need for loading a score and loading a recording are located at the top-left corner of the application."
        "<ol><li>First, you'll need to load an MEI score using the musical-note tool button.</li>"
        "<li>Then, you can load a performance (audio) recording of that score using the tool button next to it.</li>"
        "<li>Underneath the score area, you can find controls for synchronizing the score with the audio.</li></ol>"
        "<p>If you don't have your own MEI scores or recordings yet, you can use our samples located in the folder called <code>PerformancePrecision</code> within your Documents folder:"
        "<ul><li>Beethoven Sonata Op. 110 Movement I</li>"
        "<li>J. S. Bach Fugue in C Major, BWV 846</li>"
        "<li>Mozart Sonata No. 18 Movement II</li>"
        "<li>Schubert Impromptu Op. 90 No. 1</li><br></ul>";
#else // !Q_OS_WIN32
    QString introText =
        "<h3>How to use Performance Precision</h3>"
        "<p><i>You can open this instruction page at any time from the Help menu.</i><p>"
        "<p>This is a software tool that assists in analyzing recorded performances together with their scores.</p>"
        "<p>The controls you'll need for loading a score and loading a recording are located at the top-left corner of the application."
        "<br><img src=\":icons/scalable/blank.svg\" width=%2 height=%1>1. First, you'll need to load an MEI score using <img src=\":icons/scalable/chooseScore.svg\" width=%1 height=%1>."
        "<br><img src=\":icons/scalable/blank.svg\" width=%2 height=%1>2. Then, you can load a performance (audio) recording of that score using <img src=\":icons/scalable/fileopenaudio.svg\" width=%1 height=%1>."
        "<br><img src=\":icons/scalable/blank.svg\" width=%2 height=%1>3. Underneath the score area, you can find controls for synchronizing the score with the audio.</p>"
        "<p>If you don't have your own MEI scores or recordings yet, you can use our samples located in the folder called <code>PerformancePrecision</code> within your Documents folder:"
        "<br><img src=\":icons/scalable/blank.svg\" width=%2 height=%1>&bull; Beethoven Sonata Op. 110 Movement I"
        "<br><img src=\":icons/scalable/blank.svg\" width=%2 height=%1>&bull; J. S. Bach Fugue in C Major, BWV 846"
        "<br><img src=\":icons/scalable/blank.svg\" width=%2 height=%1>&bull; Mozart Sonata No. 18 Movement II"
        "<br><img src=\":icons/scalable/blank.svg\" width=%2 height=%1>&bull; Schubert Impromptu Op. 90 No. 1<br></p>";
#endif
    
    int fontSize = font().pixelSize();
    if (fontSize < 0) fontSize = font().pointSize();
    if (fontSize < 0) fontSize = 16;
    int iconSize = (fontSize * 3) / 2;
    int indent = fontSize * 2;
    introText = introText.arg(iconSize).arg(indent);

//   std::cout << "text is: " << introText << std::endl;
    
    QDialog *d = new QDialog(this);
    d->setWindowTitle(tr("Using %1").arg(QApplication::applicationName()));
    QGridLayout *layout = new QGridLayout;
    d->setLayout(layout);

    int row = 0;

    QLabel *iconLabel = new QLabel;
    iconLabel->setPixmap(QApplication::windowIcon().pixmap(64, 64));
    layout->addWidget(iconLabel, row, 0, Qt::AlignTop);

    QLabel *mainText = new QLabel();
    layout->addWidget(mainText, row, 1, 1, 2);

    layout->setRowStretch(row, 10);
    layout->setColumnStretch(1, 10);

    ++row;

    QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok);
    layout->addWidget(bb, row++, 0, 1, 3);
    connect(bb, SIGNAL(accepted()), d, SLOT(accept()));

    mainText->setWordWrap(true);
    mainText->setOpenExternalLinks(true);
    mainText->setText(introText);

    d->setMinimumSize(m_viewManager->scalePixelSize(420),
                      m_viewManager->scalePixelSize(200));
    
    d->exec();

    delete d;
}

void
MainWindow::about()
{
    QString aboutText;

    aboutText += tr("<h3>About Performance Precision</h3>");
    aboutText += tr("<p>Performance Precision is a program that assists analysis of recorded performances alongside their scores.<br><a href=\"https://github.com/yucongj/caamp/\">https://github.com/yucongj/caamp/</a></p><p>Performance Precision is based on Sonic Visualiser:<br><a href=\"https://www.sonicvisualiser.org/\">https://www.sonicvisualiser.org/</a></p>");
    aboutText += QString("<p><small>%1</small></p>").arg(getReleaseText());

    if (m_oscQueue && m_oscQueue->isOK()) {
        QString url = m_oscQueue->getOSCURL();
        if (url != "") {
            aboutText += tr("</small><p><small>The OSC URL for this instance is: \"%1\"").arg(url);
        }
    }

    aboutText += "</small><p><small>";

    aboutText += tr("With Qt v%1 &copy; The Qt Company").arg(QT_VERSION_STR);

    aboutText += "</small><small>";

#ifdef HAVE_JACK
#ifdef JACK_VERSION
    aboutText += tr("<br>With JACK audio output library v%1 &copy; Paul Davis and Jack O'Quin").arg(JACK_VERSION);
#else // !JACK_VERSION
    aboutText += tr("<br>With JACK audio output library &copy; Paul Davis and Jack O'Quin");
#endif // JACK_VERSION
#endif // HAVE_JACK
#ifdef HAVE_PORTAUDIO
    aboutText += tr("<br>With PortAudio audio output library &copy; Ross Bencina and Phil Burk");
#endif // HAVE_PORTAUDIO
#ifdef HAVE_LIBPULSE
#ifdef LIBPULSE_VERSION
    aboutText += tr("<br>With PulseAudio audio output library v%1 &copy; Lennart Poettering and Pierre Ossman").arg(LIBPULSE_VERSION);
#else // !LIBPULSE_VERSION
    aboutText += tr("<br>With PulseAudio audio output library &copy; Lennart Poettering and Pierre Ossman");
#endif // LIBPULSE_VERSION
#endif // HAVE_LIBPULSE
#ifdef HAVE_OGGZ
#ifdef OGGZ_VERSION
    aboutText += tr("<br>With Ogg file decoder (oggz v%1, fishsound v%2) &copy; CSIRO Australia").arg(OGGZ_VERSION).arg(FISHSOUND_VERSION);
#else // !OGGZ_VERSION
    aboutText += tr("<br>With Ogg file decoder &copy; CSIRO Australia");
#endif // OGGZ_VERSION
#endif // HAVE_OGGZ
#ifdef HAVE_OPUS
    aboutText += tr("<br>With Opus decoder &copy; Xiph.Org Foundation");
#endif // HAVE_OPUS
#ifdef HAVE_MAD
#ifdef MAD_VERSION
    aboutText += tr("<br>With MAD mp3 decoder v%1 &copy; Underbit Technologies Inc").arg(MAD_VERSION);
#else // !MAD_VERSION
    aboutText += tr("<br>With MAD mp3 decoder &copy; Underbit Technologies Inc");
#endif // MAD_VERSION
#endif // HAVE_MAD
#ifdef HAVE_SAMPLERATE
#ifdef SAMPLERATE_VERSION
    aboutText += tr("<br>With libsamplerate v%1 &copy; Erik de Castro Lopo").arg(SAMPLERATE_VERSION);
#else // !SAMPLERATE_VERSION
    aboutText += tr("<br>With libsamplerate &copy; Erik de Castro Lopo");
#endif // SAMPLERATE_VERSION
#endif // HAVE_SAMPLERATE
#ifdef HAVE_SNDFILE
#ifdef SNDFILE_VERSION
    aboutText += tr("<br>With libsndfile v%1 &copy; Erik de Castro Lopo").arg(SNDFILE_VERSION);
#else // !SNDFILE_VERSION
    aboutText += tr("<br>With libsndfile &copy; Erik de Castro Lopo");
#endif // SNDFILE_VERSION
#endif // HAVE_SNDFILE
#ifdef HAVE_FFTW3F
#ifdef FFTW3_VERSION
    aboutText += tr("<br>With FFTW3 v%1 &copy; Matteo Frigo and MIT").arg(FFTW3_VERSION);
#else // !FFTW3_VERSION
    aboutText += tr("<br>With FFTW3 &copy; Matteo Frigo and MIT");
#endif // FFTW3_VERSION
#endif // HAVE_FFTW3F
#ifdef HAVE_RUBBERBAND
#ifdef RUBBERBAND_VERSION
    aboutText += tr("<br>With Rubber Band Library v%1 &copy; Particular Programs Ltd").arg(RUBBERBAND_VERSION);
#else // !RUBBERBAND_VERSION
    aboutText += tr("<br>With Rubber Band Library &copy; Particular Programs Ltd");
#endif // RUBBERBAND_VERSION
#endif // HAVE_RUBBERBAND
    aboutText += tr("<br>With Vamp plugin support (API v%1, host SDK v%2) &copy; Chris Cannam and QMUL").arg(VAMP_API_VERSION).arg(VAMP_SDK_VERSION);
    aboutText += tr("<br>With Piper Vamp protocol bridge &copy; QMUL");
    aboutText += tr("<br>With LADSPA plugin support (API v%1) &copy; Richard Furse, Paul Davis, Stefan Westerfeld").arg(LADSPA_VERSION);
    aboutText += tr("<br>With DSSI plugin support (API v%1) &copy; Chris Cannam, Steve Harris, Sean Bolton").arg(DSSI_VERSION);
#ifdef REDLAND_VERSION
    aboutText += tr("<br>With Redland RDF datastore v%1 &copy; Dave Beckett and the University of Bristol").arg(REDLAND_VERSION);
#else // !REDLAND_VERSION
    aboutText += tr("<br>With Redland RDF datastore &copy; Dave Beckett and the University of Bristol");
#endif // REDLAND_VERSION
    aboutText += tr("<br>With Serd and Sord RDF parser and store &copy; David Robillard");
    aboutText += tr("<br>With Dataquay Qt/RDF library &copy; Particular Programs Ltd");
    aboutText += tr("<br>With Cap'n Proto serialisation &copy; Sandstorm Development Group");
    aboutText += tr("<br>With RtMidi &copy; Gary P. Scavone");

#ifdef HAVE_LIBLO
#ifdef LIBLO_VERSION
    aboutText += tr("<br>With liblo Lite OSC library v%1 &copy; Steve Harris").arg(LIBLO_VERSION);
#else // !LIBLO_VERSION
    aboutText += tr("<br>With liblo Lite OSC library &copy; Steve Harris");
#endif // LIBLO_VERSION

    aboutText += "</small></p>";
#endif // HAVE_LIBLO

    aboutText += "<p><small>";
    aboutText += tr("Russian UI translation contributed by Alexandre Prokoudine.");
    aboutText += "<br>";
    aboutText += tr("Czech UI translation contributed by Pavel Fric.");
    aboutText += "</small></p>";

    aboutText += 
        "<p><small>Performance Precision is Copyright &copy; 2005&ndash;2007 Chris Cannam; Copyright &copy; 2006&ndash;2024 Queen Mary, University of London; Copyright &copy; 2020-2024 Particular Programs Ltd; Copyright &copy; 2021-2024 Yucong Jiang.</small></p>";

    aboutText +=
        "<p><small>This program is free software; you can redistribute it and/or "
        "modify it under the terms of the GNU General Public License as "
        "published by the Free Software Foundation; either version 2 of the "
        "License, or (at your option) any later version.<br>See the file "
        "COPYING included with this distribution for more information.</small></p>";

    // use our own dialog so we can influence the size

    QDialog *d = new QDialog(this);

    d->setWindowTitle(tr("About %1").arg(QApplication::applicationName()));
        
    QGridLayout *layout = new QGridLayout;
    d->setLayout(layout);

    int row = 0;
    
    QLabel *iconLabel = new QLabel;
    iconLabel->setPixmap(QApplication::windowIcon().pixmap(64, 64));
    layout->addWidget(iconLabel, row, 0, Qt::AlignTop);

    QLabel *mainText = new QLabel();
    layout->addWidget(mainText, row, 1, 1, 2);

    layout->setRowStretch(row, 10);
    layout->setColumnStretch(1, 10);

    ++row;

    QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok);
    layout->addWidget(bb, row++, 0, 1, 3);
    connect(bb, SIGNAL(accepted()), d, SLOT(accept()));

//    mainText->setHtml(aboutText);
//    mainText->setReadOnly(true);
    mainText->setWordWrap(true);
    mainText->setOpenExternalLinks(true);
    mainText->setText(aboutText);

    d->setMinimumSize(m_viewManager->scalePixelSize(420),
                      m_viewManager->scalePixelSize(200));
    
    d->exec();

    delete d;
    /*
    QMessageBox about(QMessageBox::Information, 
                      tr("About Sonic Visualiser"),
                      aboutText,
                      QMessageBox::StandardButtons(QMessageBox::Ok),
                      this);

    QIcon icon = QApplication::windowIcon();
    QSize size = icon.actualSize(QSize(64, 64));
    about.setIconPixmap(icon.pixmap(size));

    about.setMinimumSize(m_viewManager->scalePixelSize(400),
                         m_viewManager->scalePixelSize(400));

    about.exec();
    */
}

void
MainWindow::keyReference()
{
    m_keyReference->show();
}

void
MainWindow::newerVersionAvailable(QString version)
{
    m_newerVersionIs = version;
    
    QSettings settings;
    settings.beginGroup("NewerVersionWarning");
    QString tag = QString("version-%1-available-show").arg(version);
    if (settings.value(tag, true).toBool()) {
        QString title(tr("Newer version available"));
        QString text(tr("<h3>Newer version available</h3><p>You are using version %1 of Sonic Visualiser, but version %2 is now available.</p><p>Please see the <a href=\"http://sonicvisualiser.org/\">Sonic Visualiser website</a> for more information.</p>").arg(SV_VERSION).arg(version));
        QMessageBox::information(this, title, text);
        settings.setValue(tag, false);
    }
    settings.endGroup();
}


