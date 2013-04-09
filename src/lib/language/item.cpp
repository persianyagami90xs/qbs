/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
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

#include "item.h"
#include "filecontext.h"
#include <tools/qbsassert.h>

namespace qbs {
namespace Internal {

Item::Item()
    : m_propertyObserver(0)
    , m_moduleInstance(false)
{
}

Item::~Item()
{
    if (m_propertyObserver)
        m_propertyObserver->onItemDestroyed(this);
}

ItemPtr Item::create()
{
    return ItemPtr(new Item);
}

ItemPtr Item::clone() const
{
    ItemPtr dup = create();
    dup->m_id = m_id;
    dup->m_typeName = m_typeName;
    dup->m_location = m_location;
    dup->m_prototype = m_prototype;
    dup->m_scope = m_scope;
    dup->m_outerItem = m_outerItem;
    dup->m_parent = m_parent;
    dup->m_children = m_children;
    dup->m_file = m_file;
    dup->m_properties = m_properties;
    dup->m_propertyDeclarations = m_propertyDeclarations;
    dup->m_functions = m_functions;
    dup->m_modules = m_modules;
    return dup;
}

bool Item::hasProperty(const QString &name) const
{
    for (const Item *item = this; item; item = item->m_prototype.data())
        if (item->m_properties.contains(name))
            return true;

    return false;
}

bool Item::hasOwnProperty(const QString &name) const
{
    return m_properties.contains(name);
}

ValuePtr Item::property(const QString &name) const
{
    ValuePtr value;
    for (const Item *item = this; item; item = item->m_prototype.data())
        if ((value = item->m_properties.value(name)))
            break;
    return value;
}

ItemValuePtr Item::itemProperty(const QString &name, bool create)
{
    ItemValuePtr result;
    ValuePtr v = property(name);
    if (v && v->type() == Value::ItemValueType) {
        result = v.staticCast<ItemValue>();
    } else if (create) {
        result = ItemValue::create(Item::create());
        setProperty(name, result);
    }
    return result;
}

JSSourceValuePtr Item::sourceProperty(const QString &name) const
{
    ValuePtr v = property(name);
    if (!v || v->type() != Value::JSSourceValueType)
        return JSSourceValuePtr();
    return v.staticCast<JSSourceValue>();
}

void Item::setPropertyObserver(ItemObserver *observer) const
{
    QBS_ASSERT(!observer || !m_propertyObserver, return);   // warn if accidentally overwritten
    m_propertyObserver = observer;
}

} // namespace Internal
} // namespace qbs
