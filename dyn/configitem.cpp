/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012 Icinga Development Team (http://www.icinga.org/)        *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "i2-dyn.h"

using namespace icinga;

ConfigItem::ConfigItem(string type, string name, DebugInfo debuginfo)
	: m_Type(type), m_Name(name), m_DebugInfo(debuginfo)
{
}

string ConfigItem::GetType(void) const
{
	return m_Type;
}

string ConfigItem::GetName(void) const
{
	return m_Name;
}

ExpressionList::Ptr ConfigItem::GetExpressionList(void) const
{
	return m_ExpressionList;
}

void ConfigItem::SetExpressionList(const ExpressionList::Ptr& exprl)
{
	m_ExpressionList = exprl;
}

vector<string> ConfigItem::GetParents(void) const
{
	return m_Parents;
}

void ConfigItem::AddParent(string parent)
{
	m_Parents.push_back(parent);
}

Dictionary::Ptr ConfigItem::CalculateProperties(void) const
{
	Dictionary::Ptr result = make_shared<Dictionary>();

	vector<string>::const_iterator it;
	for (it = m_Parents.begin(); it != m_Parents.end(); it++) {
		ConfigItem::Ptr parent = ConfigItem::GetObject(GetType(), *it);

		if (!parent) {
			stringstream message;
			message << "Parent object '" << *it << "' does not exist (" << m_DebugInfo << ")";
			throw domain_error(message.str());
		}

		parent->GetExpressionList()->Execute(result);
	}

	m_ExpressionList->Execute(result);

	return result;
}

ObjectSet<ConfigItem::Ptr>::Ptr ConfigItem::GetAllObjects(void)
{
	static ObjectSet<ConfigItem::Ptr>::Ptr allObjects;

        if (!allObjects) {
                allObjects = make_shared<ObjectSet<ConfigItem::Ptr> >();
                allObjects->Start();
        }

        return allObjects;
}

bool ConfigItem::GetTypeAndName(const ConfigItem::Ptr& object, pair<string, string> *key)
{
	*key = make_pair(object->GetType(), object->GetName());

	return true;
}

ConfigItem::TNMap::Ptr ConfigItem::GetObjectsByTypeAndName(void)
{
	static ConfigItem::TNMap::Ptr tnmap;

	if (!tnmap) {
		tnmap = make_shared<ConfigItem::TNMap>(GetAllObjects(), &ConfigItem::GetTypeAndName);
		tnmap->Start();
	}

	return tnmap;
}

void ConfigItem::Commit(void)
{
	DynamicObject::Ptr dobj = m_DynamicObject.lock();

	if (!dobj) {
		dobj = DynamicObject::GetObject(GetType(), GetName());

		if (!dobj)
			dobj = make_shared<DynamicObject>();

		m_DynamicObject = dobj;
	}

	dobj->SetConfig(CalculateProperties());
	dobj->Commit();

	ConfigItem::Ptr ci = GetObject(GetType(), GetName());
	ConfigItem::Ptr self = static_pointer_cast<ConfigItem>(shared_from_this());
	if (ci && ci != self) {
		ci->m_DynamicObject.reset();
		GetAllObjects()->RemoveObject(ci);
	}
	GetAllObjects()->CheckObject(self);
}

void ConfigItem::Unregister(void)
{
	// TODO: unregister associated DynamicObject

	ConfigItem::Ptr self = static_pointer_cast<ConfigItem>(shared_from_this());
	GetAllObjects()->RemoveObject(self);
}

ConfigItem::Ptr ConfigItem::GetObject(string type, string name)
{
	ConfigItem::TNMap::Range range;
	range = GetObjectsByTypeAndName()->GetRange(make_pair(type, name));

	assert(distance(range.first, range.second) <= 1);

	if (range.first == range.second)
		return ConfigItem::Ptr();
	else
		return range.first->second;
}