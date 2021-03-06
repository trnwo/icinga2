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

#include "icinga/host.h"
#include "icinga/service.h"
#include "icinga/hostgroup.h"
#include "icinga/icingaapplication.h"
#include "base/dynamictype.h"
#include "base/objectlock.h"
#include "base/logger_fwd.h"
#include "base/timer.h"
#include "base/convert.h"
#include "base/scriptfunction.h"
#include "base/debug.h"
#include "config/configitembuilder.h"
#include "config/configcompilercontext.h"
#include <boost/tuple/tuple.hpp>
#include <boost/smart_ptr/make_shared.hpp>
#include <boost/foreach.hpp>

using namespace icinga;

REGISTER_SCRIPTFUNCTION(ValidateServiceDictionary, &Host::ValidateServiceDictionary);

REGISTER_TYPE(Host);

void Host::Start(void)
{
	DynamicObject::Start();

	ASSERT(!OwnsLock());

	Array::Ptr groups = GetGroups();

	if (groups) {
		BOOST_FOREACH(const String& name, groups) {
			HostGroup::Ptr hg = HostGroup::GetByName(name);

			if (hg)
				hg->AddMember(GetSelf());
		}
	}

	UpdateSlaveServices();
}

void Host::Stop(void)
{
	DynamicObject::Stop();

	Array::Ptr groups = GetGroups();

	if (groups) {
		BOOST_FOREACH(const String& name, groups) {
			HostGroup::Ptr hg = HostGroup::GetByName(name);

			if (hg)
				hg->RemoveMember(GetSelf());
		}
	}

	// TODO: unregister slave services/notifications?
}

String Host::GetDisplayName(void) const
{
	if (!m_DisplayName.IsEmpty())
		return m_DisplayName;
	else
		return GetName();
}

Array::Ptr Host::GetGroups(void) const
{
	return m_HostGroups;
}

Dictionary::Ptr Host::GetMacros(void) const
{
	return m_Macros;
}

Array::Ptr Host::GetHostDependencies(void) const
{
	return m_HostDependencies;;
}

Array::Ptr Host::GetServiceDependencies(void) const
{
	return m_ServiceDependencies;
}

String Host::GetHostCheck(void) const
{
	return m_HostCheck;
}

Dictionary::Ptr Host::GetNotificationDescriptions(void) const
{
	return m_NotificationDescriptions;
}

bool Host::IsReachable(void) const
{
	ASSERT(!OwnsLock());

	std::set<Service::Ptr> parentServices = GetParentServices();

	BOOST_FOREACH(const Service::Ptr& service, parentServices) {
		ObjectLock olock(service);

		/* ignore pending services */
		if (!service->GetLastCheckResult())
			continue;

		/* ignore soft states */
		if (service->GetStateType() == StateTypeSoft)
			continue;

		/* ignore services states OK and Warning */
		if (service->GetState() == StateOK ||
		    service->GetState() == StateWarning)
			continue;

		return false;
	}

	std::set<Host::Ptr> parentHosts = GetParentHosts();

	BOOST_FOREACH(const Host::Ptr& host, parentHosts) {
		Service::Ptr hc = host->GetHostCheckService();

		/* ignore hosts that don't have a hostcheck */
		if (!hc)
			continue;

		ObjectLock olock(hc);

		/* ignore soft states */
		if (hc->GetStateType() == StateTypeSoft)
			continue;

		/* ignore hosts that are up */
		if (hc->GetState() == StateOK)
			continue;

		return false;
	}

	return true;
}

void Host::UpdateSlaveServices(void)
{
	ASSERT(!OwnsLock());

	ConfigItem::Ptr item = ConfigItem::GetObject("Host", GetName());

	/* Don't create slave services unless we own this object */
	if (!item)
		return;

	if (!m_ServiceDescriptions)
		return;

	ObjectLock olock(m_ServiceDescriptions);
	String svcname;
	Value svcdesc;
	BOOST_FOREACH(boost::tie(svcname, svcdesc), m_ServiceDescriptions) {
		if (svcdesc.IsScalar())
			svcname = svcdesc;

		std::ostringstream namebuf;
		namebuf << GetName() << ":" << svcname;
		String name = namebuf.str();

		ConfigItemBuilder::Ptr builder = boost::make_shared<ConfigItemBuilder>(item->GetDebugInfo());
		builder->SetType("Service");
		builder->SetName(name);
		builder->AddExpression("host_name", OperatorSet, GetName());
		builder->AddExpression("display_name", OperatorSet, svcname);
		builder->AddExpression("short_name", OperatorSet, svcname);

		if (!svcdesc.IsObjectType<Dictionary>())
			BOOST_THROW_EXCEPTION(std::invalid_argument("Service description must be either a string or a dictionary."));

		Dictionary::Ptr service = svcdesc;

		Array::Ptr templates = service->Get("templates");

		if (templates) {
			ObjectLock olock(templates);

			BOOST_FOREACH(const Value& tmpl, templates) {
				builder->AddParent(tmpl);
			}
		}

		/* Clone attributes from the host object. */
		std::set<String, string_iless> keys;
		keys.insert("check_interval");
		keys.insert("retry_interval");
		keys.insert("servicegroups");
		keys.insert("checkers");
		keys.insert("notification_interval");
		keys.insert("notification_type_filter");
		keys.insert("notification_state_filter");
		keys.insert("check_period");
		keys.insert("servicedependencies");
		keys.insert("hostdependencies");
		keys.insert("export_macros");
		keys.insert("macros");
		keys.insert("custom");

		ExpressionList::Ptr host_exprl = boost::make_shared<ExpressionList>();
		item->GetLinkedExpressionList()->ExtractFiltered(keys, host_exprl);
		builder->AddExpressionList(host_exprl);

		/* Clone attributes from the service expression list. */
		std::vector<String> path;
		path.push_back("services");
		path.push_back(svcname);

		ExpressionList::Ptr svc_exprl = boost::make_shared<ExpressionList>();
		item->GetLinkedExpressionList()->ExtractPath(path, svc_exprl);
		builder->AddExpressionList(svc_exprl);

		ConfigItem::Ptr serviceItem = builder->Compile();
		DynamicObject::Ptr dobj = serviceItem->Commit();
		dobj->Start();
	}
}

std::set<Service::Ptr> Host::GetServices(void) const
{
	boost::mutex::scoped_lock lock(m_ServicesMutex);

	std::set<Service::Ptr> services;
	Service::WeakPtr wservice;
	BOOST_FOREACH(boost::tie(boost::tuples::ignore, wservice), m_Services) {
		Service::Ptr service = wservice.lock();

		if (!service)
			continue;

		services.insert(service);
	}

	return services;
}

void Host::AddService(const Service::Ptr& service)
{
	boost::mutex::scoped_lock lock(m_ServicesMutex);

	m_Services[service->GetShortName()] = service;
}

void Host::RemoveService(const Service::Ptr& service)
{
	boost::mutex::scoped_lock lock(m_ServicesMutex);

	m_Services.erase(service->GetShortName());
}

int Host::GetTotalServices(void) const
{
	return GetServices().size();
}

Value Host::ValidateServiceDictionary(const String& location, const Dictionary::Ptr& attrs)
{
	ObjectLock olock(attrs);

	String key;
	Value value;
	BOOST_FOREACH(boost::tie(key, value), attrs) {
		std::vector<String> templates;

		if (!value.IsObjectType<Dictionary>())
			BOOST_THROW_EXCEPTION(std::invalid_argument("Service description must be a dictionary."));

		Dictionary::Ptr serviceDesc = value;

		Array::Ptr templatesArray = serviceDesc->Get("templates");

		if (templatesArray) {
			ObjectLock tlock(templatesArray);

			BOOST_FOREACH(const Value& tmpl, templatesArray) {
				templates.push_back(tmpl);
			}
		}

		BOOST_FOREACH(const String& name, templates) {
			ConfigItem::Ptr item = ConfigItem::GetObject("Service", name);

			if (!item)
				ConfigCompilerContext::GetInstance()->AddError(false, "Validation failed for " +
					    location + ": Template '" + name + "' not found.");
		}
	}

	return Empty;
}

Service::Ptr Host::GetServiceByShortName(const Value& name) const
{
	if (name.IsScalar()) {
		{
			boost::mutex::scoped_lock lock(m_ServicesMutex);

			std::map<String, Service::Ptr>::const_iterator it = m_Services.find(name);

			if (it != m_Services.end())
				return it->second;
		}

		return Service::Ptr();
	} else if (name.IsObjectType<Dictionary>()) {
		Dictionary::Ptr dict = name;
		String short_name;

		return Service::GetByNamePair(dict->Get("host"), dict->Get("service"));
	} else {
		BOOST_THROW_EXCEPTION(std::invalid_argument("Host/Service name pair is invalid: " + name.Serialize()));
	}
}

std::set<Host::Ptr> Host::GetParentHosts(void) const
{
	std::set<Host::Ptr> parents;

	Array::Ptr dependencies = GetHostDependencies();

	if (dependencies) {
		ObjectLock olock(dependencies);

		BOOST_FOREACH(const Value& value, dependencies) {
			if (value == GetName())
				continue;

			Host::Ptr host = GetByName(value);

			if (!host)
				continue;

			parents.insert(host);
		}
	}

	return parents;
}

std::set<Host::Ptr> Host::GetChildHosts(void) const
{
	std::set<Host::Ptr> childs;

        BOOST_FOREACH(const Host::Ptr& host, DynamicType::GetObjects<Host>()) {
		Array::Ptr dependencies = host->GetHostDependencies();

		if (dependencies) {
			ObjectLock olock(dependencies);

			BOOST_FOREACH(const Value& value, dependencies) {
				if (value == GetName())
					childs.insert(host);
			}
		}
	}

	return childs;

}

Service::Ptr Host::GetHostCheckService(void) const
{
	String host_check = GetHostCheck();

	if (host_check.IsEmpty())
		return Service::Ptr();

	return GetServiceByShortName(host_check);
}

std::set<Service::Ptr> Host::GetParentServices(void) const
{
	std::set<Service::Ptr> parents;

	Array::Ptr dependencies = GetServiceDependencies();

	if (dependencies) {
		ObjectLock olock(dependencies);

		BOOST_FOREACH(const Value& value, dependencies) {
			parents.insert(GetServiceByShortName(value));
		}
	}

	return parents;
}

HostState Host::CalculateState(ServiceState state, bool reachable)
{
	if (!reachable)
		return HostUnreachable;

	switch (state) {
		case StateOK:
		case StateWarning:
			return HostUp;
		default:
			return HostDown;
	}
}

HostState Host::GetState(void) const
{
	ASSERT(!OwnsLock());

	if (!IsReachable())
		return HostUnreachable;

	Service::Ptr hc = GetHostCheckService();

	if (!hc)
		return HostUp;

	switch (hc->GetState()) {
		case StateOK:
		case StateWarning:
			return HostUp;
		default:
			return HostDown;
	}

}

HostState Host::GetLastState(void) const
{
	ASSERT(!OwnsLock());

	if (!IsReachable())
		return HostUnreachable;

	Service::Ptr hc = GetHostCheckService();

	if (!hc)
		return HostUp;

	switch (hc->GetLastState()) {
		case StateOK:
		case StateWarning:
			return HostUp;
		default:
			return HostDown;
	}
}

HostState Host::GetLastHardState(void) const
{
	ASSERT(!OwnsLock());

	if (!IsReachable())
		return HostUnreachable;

	Service::Ptr hc = GetHostCheckService();

	if (!hc)
		return HostUp;

	switch (hc->GetLastHardState()) {
		case StateOK:
		case StateWarning:
			return HostUp;
		default:
			return HostDown;
	}
}

double Host::GetLastStateUp(void) const
{
	ASSERT(!OwnsLock());

	Service::Ptr hc = GetHostCheckService();

	if (!hc)
		return 0;

	if (hc->GetLastStateOK() > hc->GetLastStateWarning())
		return hc->GetLastStateOK();
	else
		return hc->GetLastStateWarning();
}

double Host::GetLastStateDown(void) const
{
	ASSERT(!OwnsLock());

	Service::Ptr hc = GetHostCheckService();

	if (!hc)
		return 0;

	return hc->GetLastStateCritical();
}

double Host::GetLastStateUnreachable(void) const
{
	ASSERT(!OwnsLock());

	Service::Ptr hc = GetHostCheckService();

	if (!hc)
		return 0;

	return hc->GetLastStateUnreachable();
}

double Host::GetLastStateChange(void) const
{
	Service::Ptr hc = GetHostCheckService();

	if (!hc)
		return IcingaApplication::GetInstance()->GetStartTime();

	return hc->GetLastStateChange();
}


double Host::GetLastHardStateChange(void) const
{
	Service::Ptr hc = GetHostCheckService();

	if (!hc)
		return IcingaApplication::GetInstance()->GetStartTime();

	return hc->GetLastHardStateChange();
}

StateType Host::GetLastStateType(void) const
{
	Service::Ptr hc = GetHostCheckService();

	if (!hc)
		return StateTypeHard;

	return hc->GetLastStateType();
}

StateType Host::GetStateType(void) const
{
	Service::Ptr hc = GetHostCheckService();

	if (!hc)
		return StateTypeHard;

	return hc->GetStateType();
}

String Host::StateToString(HostState state)
{
	switch (state) {
		case HostUp:
			return "UP";
		case HostDown:
			return "DOWN";
		case HostUnreachable:
			return "UNREACHABLE";
		default:
			return "INVALID";
	}
}

bool Host::ResolveMacro(const String& macro, const Dictionary::Ptr&, String *result) const
{
	if (macro == "HOSTNAME") {
		*result = GetName();
		return true;
	}
	else if (macro == "HOSTDISPLAYNAME" || macro == "HOSTALIAS") {
		*result = GetDisplayName();
		return true;
	}

	Service::Ptr hc = GetHostCheckService();
	Dictionary::Ptr hccr;

	if (hc) {
		ServiceState state = hc->GetState();
		bool reachable = IsReachable();

		if (macro == "HOSTSTATE") {
			*result = Convert::ToString(CalculateState(state, reachable));
			return true;
		} else if (macro == "HOSTSTATEID") {
			*result = Convert::ToString(state);
			return true;
		} else if (macro == "HOSTSTATETYPE") {
			*result = Service::StateTypeToString(hc->GetStateType());
			return true;
		} else if (macro == "HOSTATTEMPT") {
			*result = Convert::ToString(hc->GetCurrentCheckAttempt());
			return true;
		} else if (macro == "MAXHOSTATTEMPT") {
			*result = Convert::ToString(hc->GetMaxCheckAttempts());
			return true;
		} else if (macro == "LASTHOSTSTATE") {
			*result = StateToString(GetLastState());
			return true;
		} else if (macro == "LASTHOSTSTATEID") {
			*result = Convert::ToString(GetLastState());
			return true;
		} else if (macro == "LASTHOSTSTATETYPE") {
			*result = Service::StateTypeToString(GetLastStateType());
			return true;
		} else if (macro == "LASTHOSTSTATECHANGE") {
			*result = Convert::ToString((long)hc->GetLastStateChange());
			return true;
		}

		hccr = hc->GetLastCheckResult();
	}

	if (hccr) {
		if (macro == "HOSTLATENCY") {
			*result = Convert::ToString(Service::CalculateLatency(hccr));
			return true;
		} else if (macro == "HOSTEXECUTIONTIME") {
			*result = Convert::ToString(Service::CalculateExecutionTime(hccr));
			return true;
		} else if (macro == "HOSTOUTPUT") {
			*result = hccr->Get("output");
			return true;
		} else if (macro == "HOSTPERFDATA") {
			*result = hccr->Get("performance_data_raw");
			return true;
		} else if (macro == "LASTHOSTCHECK") {
			*result = Convert::ToString((long)hccr->Get("schedule_start"));
			return true;
		}
	}

	Dictionary::Ptr macros = GetMacros();

	String name = macro;

	if (name == "HOSTADDRESS")
		name = "address";
	else if (macro == "HOSTADDRESS6")
		name = "address6";

	if (macros && macros->Contains(name)) {
		*result = macros->Get(name);
		return true;
	}

	if (macro == "HOSTADDRESS" || macro == "HOSTADDRESS6") {
		*result = GetName();
		return true;
	}

	return false;
}

void Host::InternalSerialize(const Dictionary::Ptr& bag, int attributeTypes) const
{
	DynamicObject::InternalSerialize(bag, attributeTypes);

	if (attributeTypes & Attribute_Config) {
		bag->Set("display_name", m_DisplayName);
		bag->Set("hostgroups", m_HostGroups);
		bag->Set("macros", m_Macros);
		bag->Set("hostdependencies", m_HostDependencies);
		bag->Set("servicedependencies", m_ServiceDependencies);
		bag->Set("hostcheck", m_HostCheck);
		bag->Set("services", m_ServiceDescriptions);
		bag->Set("notifications", m_NotificationDescriptions);
	}
}

void Host::InternalDeserialize(const Dictionary::Ptr& bag, int attributeTypes)
{
	DynamicObject::InternalDeserialize(bag, attributeTypes);

	if (attributeTypes & Attribute_Config) {
		m_DisplayName = bag->Get("display_name");
		m_HostGroups = bag->Get("hostgroups");
		m_Macros = bag->Get("macros");
		m_HostDependencies = bag->Get("hostdependencies");
		m_ServiceDependencies = bag->Get("servicedependencies");
		m_HostCheck = bag->Get("hostcheck");
		m_ServiceDescriptions = bag->Get("services");
		m_NotificationDescriptions = bag->Get("notifications");
	}
}
