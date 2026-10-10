#pragma once
#include <NavalCommand/Gateway/Api/IProvider.h>
#include <NavalCommand/Orders/Api/RouteMode.h>
#include <cmath>
#include <cstddef>
#include <map>
#include <mutex>

namespace NavalCommand::Gateway::Grpc
{
	// Deliberately simulated execution, no game. The same domain provider contract.
	class MockProvider final : public Api::IProvider
	{
		static constexpr std::size_t MAXIMUM_RETAINED_REQUESTS = 128;
		static constexpr std::size_t MAXIMUM_REQUEST_ID_CHARACTERS = 128;
		static constexpr std::size_t MAXIMUM_ROUTE_POINTS = 32;
		static constexpr double MAXIMUM_LATITUDE_DEGREES = 90;
		static constexpr double MAXIMUM_LONGITUDE_DEGREES = 180;
		// The mock reports a fixed synthetic movement, not a measured geographic distance.
		static constexpr double SYNTHETIC_LATITUDE_CHANGE_DEGREES = 0.001;
		static constexpr double SYNTHETIC_DISPLACEMENT_METERS = 111.0;
		std::mutex m_Mutex;
		World::Api::View m_View;
		std::map<std::string, Orders::Api::Result> m_Ledger;

	public:
		MockProvider()
		{
			Reset("mock-session");
		}

		void Reset(std::string session)
		{
			std::lock_guard lock(m_Mutex);
			m_Ledger.clear();
			m_View = {};
			m_View.sessionId = session;
			m_View.observerId = "mock-observer";
			m_View.ready = true;
			m_View.version = 1;
			m_View.units.push_back({
				"unit-1",
				"Mock vessel",
				{61.96, -10.37, {}},
				0,
				0,
				{{"set_waypoint", Orders::Api::Availability::Executable, "", "append; current altitude; telegraph 2; no formation"}}
			});
			m_View.units[0].routeInspected = true;
			m_View.units[0].orders.push_back({"replace_route", Orders::Api::Availability::Executable, "", "explicit replace"});
			m_View.units[0].orders.push_back({"append_waypoint", Orders::Api::Availability::Executable, "", "explicit append"});
		}

		std::expected<World::Api::View, Api::Error> View() override
		{
			std::lock_guard lock(m_Mutex);
			return m_View;
		}

		Orders::Api::Result Submit(const Orders::Api::Request& request) override
		{
			std::lock_guard lock(m_Mutex);
			using OrderStage = Orders::Api::Stage;
			auto reject = [&](std::string reason)
			{
				return Orders::Api::Result{request.sessionId, request.requestId, OrderStage::Rejected, reason};
			};
			if (request.sessionId != m_View.sessionId)
			{
				return reject("stale_session");
			}
			if (auto retainedResult = m_Ledger.find(request.requestId); retainedResult != m_Ledger.end())
			{
				return retainedResult->second;
			}
			Orders::Api::Result result;
			if (request.requestId.empty() || request.requestId.size() > MAXIMUM_REQUEST_ID_CHARACTERS)
			{
				result = reject("invalid_arguments");
			}
			else if (m_Ledger.size() >= MAXIMUM_RETAINED_REQUESTS)
			{
				return reject("ledger_full");
			}
			else if (request.entityId != "unit-1")
			{
				result = reject("wrong_side_or_invalid_entity");
			}
			else if (request.facilityLaunch)
			{
				result = reject("unsupported_order");
			}
			else if (!request.routeCommand || !std::isfinite(request.routeCommand->latitudeDeg) || !std::isfinite(request.routeCommand->longitudeDeg) ||
				std::abs(request.routeCommand->latitudeDeg) > MAXIMUM_LATITUDE_DEGREES || std::abs(request.routeCommand->longitudeDeg) > MAXIMUM_LONGITUDE_DEGREES)
			{
				result = reject("invalid_arguments");
			}
			else if (request.routeCommand->routeMode != Orders::Api::RouteMode::LegacyIdle && request.routeCommand->routeMode != Orders::Api::RouteMode::ReplaceRoute &&
				request.routeCommand->routeMode != Orders::Api::RouteMode::AppendWaypoint)
			{
				result = reject("unsupported_route_mode");
			}
			else if (request.basedOnVersion != m_View.version)
			{
				result = reject("stale_observation");
			}
			else if (m_View.units[0].orders[static_cast<std::size_t>(request.routeCommand->routeMode)].availability != Orders::Api::Availability::Executable)
			{
				result = reject(m_View.units[0].orders[static_cast<std::size_t>(request.routeCommand->routeMode)].reason);
			}
			else if (request.routeCommand->routeMode == Orders::Api::RouteMode::AppendWaypoint && m_View.units[0].plannedRoute.size() >= MAXIMUM_ROUTE_POINTS)
			{
				result = reject("route_capacity");
			}
			else
			{
				m_View.units[0].position.latitudeDeg += SYNTHETIC_LATITUDE_CHANGE_DEGREES;
				result = {request.sessionId, request.requestId, OrderStage::MovementObserved, "mock_displacement", 1, SYNTHETIC_DISPLACEMENT_METERS};
				result.routeMode = request.routeCommand->routeMode;
				result.routeBefore = m_View.units[0].plannedRoute;
				if (request.routeCommand->routeMode == Orders::Api::RouteMode::ReplaceRoute)
				{
					m_View.units[0].plannedRoute.clear();
				}
				m_View.units[0].plannedRoute.push_back({request.routeCommand->latitudeDeg, request.routeCommand->longitudeDeg, "NC-" + request.requestId});
				result.routeAfter = m_View.units[0].plannedRoute;
				m_View.units[0].orders[0].availability = Orders::Api::Availability::Blocked;
				m_View.units[0].orders[0].reason = "native_task_selection_active";
				if (request.routeCommand->routeMode != Orders::Api::RouteMode::LegacyIdle)
				{
					result.stage = OrderStage::RouteMutated;
					result.reason = "mock_route_sequence";
					result.displacementMeters.reset();
				}
			}
			m_Ledger[request.requestId] = result;
			return result;
		}

		Orders::Api::Result Status(const std::string& sessionId, const std::string& requestId) override
		{
			std::lock_guard lock(m_Mutex);
			if (sessionId != m_View.sessionId)
			{
				return {sessionId, requestId, Orders::Api::Stage::Rejected, "stale_session"};
			}
			if (auto retainedResult = m_Ledger.find(requestId); retainedResult != m_Ledger.end())
			{
				// Simulate progress only for the current destination. A queued append
				// must not inherit displacement from a preceding route head.
				if (retainedResult->second.stage == Orders::Api::Stage::RouteMutated && !m_View.units[0].plannedRoute.empty() &&
					m_View.units[0].plannedRoute.front().label == "NC-" + requestId)
				{
					retainedResult->second.stage = Orders::Api::Stage::MovementObserved;
					retainedResult->second.reason = "mock_route_head_displacement";
					retainedResult->second.displacementMeters = SYNTHETIC_DISPLACEMENT_METERS;
				}
				return retainedResult->second;
			}
			return {sessionId, requestId, Orders::Api::Stage::Unknown, "unknown_or_expired_request"};
		}

		void Block()
		{
			std::lock_guard lock(m_Mutex);
			for (auto& eligibility : m_View.units[0].orders)
			{
				eligibility.availability = Orders::Api::Availability::Blocked;
				eligibility.reason = "protected_native_action";
			}
		}
	};
}
