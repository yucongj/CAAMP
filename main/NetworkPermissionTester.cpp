/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Sonic Visualiser
    An audio file viewer and annotation editor.
    Centre for Digital Music, Queen Mary, University of London.
    
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.
*/

#include "NetworkPermissionTester.h"

#include "../version.h"

#include "base/Debug.h"

#include <QWidget>
#include <QString>
#include <QSettings>
#include <QCoreApplication>
#include <QDialog>
#include <QGridLayout>
#include <QLabel>
#include <QDialogButtonBox>
#include <QCheckBox>

bool
NetworkPermissionTester::havePermission()
{
    QSettings settings;
    settings.beginGroup("Preferences");
    
    QString tag = QString("network-permission-%1").arg(SV_VERSION);

    bool permish = false;

    if (settings.contains(tag)) {
        permish = settings.value(tag, false).toBool();
        SVDEBUG << "NetworkPermissionTester: Asked already, result was " << permish << endl;
    } else {
        SVDEBUG << "NetworkPermissionTester: Asking for permission" << endl;

        QString appname = QCoreApplication::applicationName();
        
        QDialog d;
        d.setWindowTitle(QCoreApplication::translate("NetworkPermissionTester", "Welcome to %1").arg(appname));

        QGridLayout *layout = new QGridLayout;
        d.setLayout(layout);

        QString preamble;
        preamble = QCoreApplication::translate
            ("NetworkPermissionTester",
             "<h2>Welcome to %1!</h2>"
             "<p>%1 is a program that assists analysis of recorded music performances alongside their scores.</p>"
             "<p>%1 is open source software under the GNU General Public License.</p>"
             "<p><hr></p>"
             "<p><b>Before we go on...</b></p>"
             "<p>%1 would like permission to use the network.</p>")
            .arg(appname);

        QString bullets;
        if (m_withOSC) {
            bullets = QCoreApplication::translate
                ("NetworkPermissionTester",
                 "<p>This is to:</p>"
                 "<ul><li> Find information about available and installed plugins;</li>"
                 "<li> Support the use of Open Sound Control; and</li>"
                 "<li> Tell you when updates are available.</li>"
                 "</ul>");
        } else {
            bullets = QCoreApplication::translate
                ("NetworkPermissionTester",
                 "<p>This is to:</p>"
                 "<ul><li> Find information about available and installed plugins; and</li>"
                 "<li> Tell you when updates are available.</li>"
                 "</ul>");
        }

        QString postamble;
        postamble = QCoreApplication::translate
            ("NetworkPermissionTester",
             "<p><b>No personal information will be sent, no tracking is carried out, and no information will be shared with anyone else.</b></p>"
             "<p>We recommend that you allow this, because it makes %1 more useful to you. But if you do not wish to allow it, please un-check the box below.<br></p>").arg(appname);
        
        QLabel *label = new QLabel;
        label->setWordWrap(true);
        label->setText(preamble + bullets + postamble);
        layout->addWidget(label, 0, 0);

        QCheckBox *cb = new QCheckBox(QCoreApplication::translate("NetworkPermissionTester", "Allow this"));
        cb->setChecked(true);
        layout->addWidget(cb, 1, 0);
        
        QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok);
        QObject::connect(bb, SIGNAL(accepted()), &d, SLOT(accept()));
        layout->addWidget(bb, 2, 0);
        
        d.exec();

        permish = cb->isChecked();
        settings.setValue(tag, permish);

        SVDEBUG << "NetworkPermissionTester: asked, answer was " << permish << endl;
    }

    settings.endGroup();

    return permish;
}

   

