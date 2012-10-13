/****************************************************************************
**
** Copyright (C) 2012 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the Qt Build Suite.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#ifndef QBSINTERFACE_H
#define QBSINTERFACE_H

#include <language/language.h>
#include <buildgraph/buildgraph.h>
#include <tools/error.h>

#include "buildproject.h"

#include <QExplicitlySharedDataPointer>
#include <QFutureInterface>
#include <QSharedPointer>

namespace qbs {
class BuildProject;
class Loader;
class Settings;
}

namespace Qbs {

class SourceProjectPrivate;

class SourceProject
{
    Q_DECLARE_TR_FUNCTIONS(SourceProject)
    friend class BuildProject;
public:
    SourceProject(qbs::QbsEngine *engine);
    ~SourceProject();

    SourceProject(const SourceProject &other);
    SourceProject &operator=(const SourceProject &other);

    void setSettings(const qbs::Settings::Ptr &settings);

    // These may throw qbs::Error.
    void loadPlugins();
    void loadProject(const QString &projectFileName, const QList<QVariantMap> &buildConfigs);

    QVector<BuildProject> buildProjects() const;
    QList<qbs::BuildProject::Ptr> internalBuildProjects() const;

private:
    QExplicitlySharedDataPointer<SourceProjectPrivate> d;
};

} // namespace Qbs

#endif
