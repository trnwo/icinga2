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

#include "base/convert.h"
#include "icinga/compatutility.h"
#include "icinga/checkcommand.h"
#include "icinga/eventcommand.h"
#include "base/objectlock.h"
#include "base/utility.h"
#include <boost/smart_ptr/make_shared.hpp>
#include <boost/foreach.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/join.hpp>

using namespace icinga;

Dictionary::Ptr CompatUtility::GetServiceStatusAttributes(const Service::Ptr& service, CompatObjectType type)
{
	Dictionary::Ptr attr = boost::make_shared<Dictionary>();

	ASSERT(service->OwnsLock());

	String raw_output;
	String output;
	String long_output;
	String perfdata;
	double schedule_end = -1;

	String check_period_str;
	TimePeriod::Ptr check_period = service->GetCheckPeriod();
	if (check_period)
		check_period_str = check_period->GetName();
	else
		check_period_str = "24x7";

	Dictionary::Ptr cr = service->GetLastCheckResult();

	if (cr) {
		raw_output = cr->Get("output");
		size_t line_end = raw_output.Find("\n");

		output = raw_output.SubStr(0, line_end);

		if (line_end > 0 && line_end != String::NPos) {
			long_output = raw_output.SubStr(line_end+1, raw_output.GetLength());
			long_output = EscapeString(long_output);
		}

		boost::algorithm::replace_all(output, "\n", "\\n");

		schedule_end = cr->Get("schedule_end");

		perfdata = cr->Get("performance_data_raw");
		boost::algorithm::replace_all(perfdata, "\n", "\\n");
	}

	int state = service->GetState();

	if (state > StateUnknown)
		state = StateUnknown;

	if (type == CompatTypeHost) {
		if (state == StateOK || state == StateWarning)
			state = 0; /* UP */
		else
			state = 1; /* DOWN */

		Host::Ptr host = service->GetHost();

		ASSERT(host);

		if (!host->IsReachable())
			state = 2; /* UNREACHABLE */

		attr->Set("last_time_up", host->GetLastStateUp());
		attr->Set("last_time_down", host->GetLastStateDown());
		attr->Set("last_time_unreachable", host->GetLastStateUnreachable());
	} else {
		attr->Set("last_time_ok", service->GetLastStateOK());
		attr->Set("last_time_warn", service->GetLastStateWarning());
		attr->Set("last_time_critical", service->GetLastStateCritical());
		attr->Set("last_time_unknown", service->GetLastStateUnknown());
	}

	double last_notification = 0;
	double next_notification = 0;
	int notification_number = 0;
	BOOST_FOREACH(const Notification::Ptr& notification, service->GetNotifications()) {
		if (notification->GetLastNotification() > last_notification)
			last_notification = notification->GetLastNotification();

		if (notification->GetNextNotification() < next_notification)
			next_notification = notification->GetNextNotification();

		if (notification->GetNotificationNumber() > notification_number)
			notification_number = notification->GetNotificationNumber();
	}

	CheckCommand::Ptr checkcommand = service->GetCheckCommand();
	if (checkcommand)
		attr->Set("check_command", "check_" + checkcommand->GetName());

	EventCommand::Ptr eventcommand = service->GetEventCommand();
	if (eventcommand)
		attr->Set("event_handler", "event_" + eventcommand->GetName());

	attr->Set("check_period", check_period_str);
	attr->Set("check_interval", service->GetCheckInterval() / 60.0);
	attr->Set("retry_interval", service->GetRetryInterval() / 60.0);
	attr->Set("has_been_checked", (service->GetLastCheckResult() ? 1 : 0));
	attr->Set("should_be_scheduled", 1);
	attr->Set("check_execution_time", Service::CalculateExecutionTime(cr));
	attr->Set("check_latency", Service::CalculateLatency(cr));
	attr->Set("current_state", state);
	attr->Set("state_type", service->GetStateType());
	attr->Set("plugin_output", output);
	attr->Set("long_plugin_output", long_output);
	attr->Set("performance_data", perfdata);
	attr->Set("last_check", schedule_end);
	attr->Set("next_check", service->GetNextCheck());
	attr->Set("current_attempt", service->GetCurrentCheckAttempt());
	attr->Set("max_attempts", service->GetMaxCheckAttempts());
	attr->Set("last_state_change", service->GetLastStateChange());
	attr->Set("last_hard_state_change", service->GetLastHardStateChange());
	attr->Set("last_update", time(NULL));
	attr->Set("process_performance_data", 1); /* always enabled */
	attr->Set("freshness_checks_enabled", 1); /* always enabled */
	attr->Set("notifications_enabled", (service->GetEnableNotifications() ? 1 : 0));
	attr->Set("event_handler_enabled", 1); /* always enabled */
	attr->Set("active_checks_enabled", (service->GetEnableActiveChecks() ? 1 : 0));
	attr->Set("passive_checks_enabled", (service->GetEnablePassiveChecks() ? 1 : 0));
	attr->Set("flap_detection_enabled", (service->GetEnableFlapping() ? 1 : 0));
	attr->Set("is_flapping", (service->IsFlapping() ? 1 : 0));
	attr->Set("percent_state_change", Convert::ToString(service->GetFlappingCurrent()));
	attr->Set("problem_has_been_acknowledged", (service->GetAcknowledgement() != AcknowledgementNone ? 1 : 0));
	attr->Set("acknowledgement_type", static_cast<int>(service->GetAcknowledgement()));
	attr->Set("acknowledgement_end_time", service->GetAcknowledgementExpiry());
	attr->Set("scheduled_downtime_depth", (service->IsInDowntime() ? 1 : 0));
	attr->Set("last_notification", last_notification);
	attr->Set("next_notification", next_notification);
	attr->Set("current_notification_number", notification_number);

	return attr;
}

Dictionary::Ptr CompatUtility::GetServiceConfigAttributes(const Service::Ptr& service, CompatObjectType type)
{
	Dictionary::Ptr attr = boost::make_shared<Dictionary>();

	ASSERT(service->OwnsLock());

	Host::Ptr host = service->GetHost();

	if (!host)
		return Dictionary::Ptr();

	String check_period_str;
	TimePeriod::Ptr check_period = service->GetCheckPeriod();
	if (check_period)
		check_period_str = check_period->GetName();
	else
		check_period_str = "24x7";

	double notification_interval = -1;
	String notification_period;
	unsigned long notification_type_filter;
	unsigned long notification_state_filter;

	BOOST_FOREACH(const Notification::Ptr& notification, service->GetNotifications()) {
		if (notification_interval == -1 || notification->GetNotificationInterval() < notification_interval)
			notification_interval = notification->GetNotificationInterval();

		if (notification->GetNotificationPeriod())
			notification_period = notification->GetNotificationPeriod()->GetName();

		if (notification->GetNotificationTypeFilter())
			notification_type_filter = notification->GetNotificationTypeFilter();

		if (notification->GetNotificationStateFilter())
			notification_state_filter = notification->GetNotificationStateFilter();

		Log(LogDebug, "compatutility", "notification_type_filter: " + Convert::ToString(notification_type_filter) + " notification_state_filter: " + Convert::ToString(notification_state_filter));
	}


	if (notification_interval == -1)
		notification_interval = 60;

	String check_command_str;
	String event_command_str;
	CheckCommand::Ptr checkcommand = service->GetCheckCommand();
	EventCommand::Ptr eventcommand = service->GetEventCommand();

	if (checkcommand)
		check_command_str = checkcommand->GetName();

	if (eventcommand)
		event_command_str = eventcommand->GetName();

	Dictionary::Ptr custom;
	Dictionary::Ptr macros;
	std::vector<String> notification_options;

	if (type == CompatTypeHost) {

		/* dicts */
		custom = host->GetCustom();
		macros = host->GetMacros();

		/* alias */
		if (!host->GetDisplayName().IsEmpty())
			attr->Set("alias", host->GetName());
		else
			attr->Set("alias", host->GetDisplayName());

		/* notification state filters */
		if (notification_state_filter & (1<<StateWarning) ||
				notification_state_filter & (1<<StateCritical)) {
			attr->Set("notify_on_down", 1);
			notification_options.push_back("d");
		}
		if (notification_state_filter & (1<<StateUncheckable)) {
			attr->Set("notify_on_unreachable", 1);
			notification_options.push_back("u");
		}
	}
	else {
		/* dicts */
		custom = service->GetCustom();
		macros = service->GetMacros();

		/* notification state filters */
		if (notification_state_filter & (1<<StateWarning)) {
			attr->Set("notify_on_warning", 1);
			notification_options.push_back("w");
		}
		if (notification_state_filter & (1<<StateUnknown)) {
			attr->Set("notify_on_unknown", 1);
			notification_options.push_back("u");
		}
		if (notification_state_filter & (1<<StateCritical)) {
			attr->Set("notify_on_critical", 1);
			notification_options.push_back("c");
		}
	}

	/* notification type filters */
	if (notification_type_filter & (1<<NotificationRecovery)) {
		attr->Set("notify_on_recovery", 1);
		notification_options.push_back("r");
	}
	if (notification_type_filter & (1<<NotificationFlappingStart) ||
			notification_type_filter & (1<<NotificationFlappingEnd)) {
		attr->Set("notify_on_flapping", 1);
		notification_options.push_back("f");
	}
	if (notification_type_filter & (1<<NotificationDowntimeStart) ||
			notification_type_filter & (1<<NotificationDowntimeEnd) ||
			notification_type_filter & (1<<NotificationDowntimeRemoved)) {
		attr->Set("notify_on_downtime", 1);
		notification_options.push_back("s");
	}

	attr->Set("check_period", check_period_str);
	attr->Set("check_interval", service->GetCheckInterval() / 60.0);
	attr->Set("retry_interval", service->GetRetryInterval() / 60.0);
	attr->Set("max_check_attempts", service->GetMaxCheckAttempts());
	attr->Set("active_checks_enabled", (service->GetEnableActiveChecks() ? 1 : 0));
	attr->Set("passive_checks_enabled", (service->GetEnablePassiveChecks() ? 1 : 0));
	attr->Set("flap_detection_enabled", (service->GetEnableFlapping() ? 1 : 0));
	attr->Set("low_flap_threshold", service->GetFlappingThreshold());
	attr->Set("high_flap_threshold", service->GetFlappingThreshold());
	attr->Set("notifications_enabled", (service->GetEnableNotifications() ? 1 : 0));
	attr->Set("eventhandler_enabled", 1); /* always 1 */
	attr->Set("is_volatile", (service->IsVolatile() ? 1 : 0));
	attr->Set("notifications_enabled", (service->GetEnableNotifications() ? 1 : 0));
	attr->Set("notification_options", boost::algorithm::join(notification_options, ","));
	attr->Set("notification_interval", notification_interval / 60.0);
	attr->Set("process_performance_data", 1); /* always 1 */
	attr->Set("notification_period", notification_period);
	attr->Set("check_freshness", 1); /* always 1 */
	attr->Set("check_command", check_command_str);
	attr->Set("event_handler", event_command_str);

	/* macros attr */
	if (macros) {
		if (type == CompatTypeHost) {
			attr->Set("address", macros->Get("address"));
			attr->Set("address6", macros->Get("address6"));
		}
	}

	/* custom attr */
	if (custom) {
		attr->Set("notes", custom->Get("notes"));
		attr->Set("notes_url", custom->Get("notes_url"));
		attr->Set("action_url", custom->Get("action_url"));
		attr->Set("icon_image", custom->Get("icon_image"));
		attr->Set("icon_image_alt", custom->Get("icon_image_alt"));

		if (type == CompatTypeHost) {
			attr->Set("statusmap_image", custom->Get("statusmap_image"));
			attr->Set("2d_coords", custom->Get("2d_coords"));

			if (!custom->Get("2d_coords").IsEmpty()) {
				std::vector<String> tokens;
				String coords = custom->Get("2d_coords");
				boost::algorithm::split(tokens, coords, boost::is_any_of(","));
				if (tokens.size() == 2) {
					attr->Set("have_2d_coords", 1);
					attr->Set("x_2d", tokens[0]);
					attr->Set("y_2d", tokens[1]);
				}
				else
					attr->Set("have_2d_coords", 0);
			}
			else
				attr->Set("have_2d_coords", 0);

			/* deprecated in 1.x, but empty */
			attr->Set("vrml_image", Empty);
			attr->Set("3d_coords", Empty);
			attr->Set("have_3d_coords", 0);
			attr->Set("x_3d", Empty);
			attr->Set("y_3d", Empty);
			attr->Set("z_3d", Empty);
		}
	}

	return attr;

}

String CompatUtility::EscapeString(const String& str)
{
	String result = str;
	boost::algorithm::replace_all(result, "\n", "\\n");
	return result;
}