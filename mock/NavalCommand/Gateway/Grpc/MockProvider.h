#pragma once
#include <NavalCommand/Gateway/Api/IProvider.h>
#include <NavalCommand/Observations/Api/NavigationObservation.h>
#include <NavalCommand/Orders/Api/RouteMode.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <mutex>
#include <set>

namespace NavalCommand::Gateway::Grpc {
// Deliberately simulated execution, no game. Public handles are contacts, not truth IDs.
class MockProvider final : public Api::IProvider {
	static constexpr std::size_t MAXIMUM_RETAINED_REQUESTS = 128;
	static constexpr std::size_t MAXIMUM_REQUEST_ID_CHARACTERS = 128;
	static constexpr std::size_t MAXIMUM_ROUTE_POINTS = 32;
	static constexpr double MAXIMUM_LATITUDE_DEGREES = 90;
	static constexpr double MAXIMUM_LONGITUDE_DEGREES = 180;
	static constexpr double SYNTHETIC_LATITUDE_CHANGE_DEGREES = 0.001;
	static constexpr double SYNTHETIC_DISPLACEMENT_METERS = 111.0;
	std::mutex m_Mutex;
	Api::ProviderObservation m_Observation;
	std::set<std::string> m_ControlledObservations;
	std::map<std::string, Orders::Api::Result> m_Ledger;

	Orders::Api::Result Reject(const Orders::Api::Request &request, std::string reason) const {
		Orders::Api::Result result;
		result.SessionId = request.SessionId;
		result.RequestId = request.RequestId;
		result.Observer = m_Observation.Snapshot.Observer;
		result.Stage = Orders::Api::Stage::Rejected;
		result.Reason = std::move(reason);
		return result;
	}

public:
	MockProvider() {
		Reset("mock-session");
	}

	void Reset(std::string session) {
		std::lock_guard lock(m_Mutex);
		m_Ledger.clear();
		m_ControlledObservations = {"contact-alpha"};
		m_Observation = {};
		auto &snapshot = m_Observation.Snapshot;
		snapshot.SessionId = std::move(session);
		snapshot.Observer = {"mock-observer"};
		snapshot.Ready = true;
		snapshot.Version = 1;
		using namespace Observations::Api;
		UnitObservation own;
		own.Identity = {"contact-alpha"};
		own.Name = Uncertainty::Api::Exact<std::string>::Known("Mock Vessel");
		own.Type = Uncertainty::Api::Exact<Catalog::Api::UnitTypeId>::Known({"mock-vessel"});
		own.Kinematics.Position = PartiallyLocalizedPosition{
			Physics::Api::Degrees{61.96}, Physics::Api::Degrees{-10.37},
			ObservedErrorEllipse{Physics::Api::Meters{25}, Physics::Api::Meters{20}, Physics::Api::Radians{0}}};
		own.Kinematics.Speed = Uncertainty::Api::Exact<Physics::Api::MetersPerSecond>::Known({0});
		own.Navigation = NavigationObservation{Uncertainty::Api::Exact<std::vector<ObservedRoutePoint>>::Known({})};
		snapshot.Units.push_back(std::move(own));
		UnitObservation ambiguous;
		ambiguous.Identity = {"contact-beta"};
		ambiguous.Kinematics.Position = PartiallyLocalizedPosition{
			Physics::Api::Degrees{62}, {},
			ObservedErrorEllipse{Physics::Api::Meters{500}, Physics::Api::Meters{250}, Physics::Api::Radians{0}}};
		// This ambiguous fixture deliberately has no correspondence to any simulated UnitId.
		// The position is a partial localization; no contact name/type is reported.
		snapshot.Units.push_back(std::move(ambiguous));
		m_Observation.OrderAvailability.push_back({snapshot.Observer, {"contact-alpha"}, 1,
			{{Orders::Api::OrderKind::IdleWaypoint, Orders::Api::Availability::Executable, ""},
			 {Orders::Api::OrderKind::ReplaceRoute, Orders::Api::Availability::Executable, ""},
			 {Orders::Api::OrderKind::AppendRoute, Orders::Api::Availability::Executable, ""}}});
	}

	std::expected<Api::ProviderObservation, Api::Error> View() override {
		std::lock_guard lock(m_Mutex);
		return m_Observation;
	}

	Orders::Api::Result Submit(const Orders::Api::Request &request) override {
		std::lock_guard lock(m_Mutex);
		using namespace Orders::Api;
		auto &snapshot = m_Observation.Snapshot;
		if (request.SessionId != snapshot.SessionId) {
			return Reject(request, "stale_session");
		}
		if (request.Observer != snapshot.Observer) {
			return Reject(request, "wrong_observer_or_invalid_entity");
		}
		if (auto retained = m_Ledger.find(request.RequestId); retained != m_Ledger.end()) {
			return retained->second;
		}
		if (request.RequestId.empty() || request.RequestId.size() > MAXIMUM_REQUEST_ID_CHARACTERS) {
			return Reject(request, "invalid_arguments");
		}
		if (m_Ledger.size() >= MAXIMUM_RETAINED_REQUESTS) {
			return Reject(request, "ledger_full");
		}
		const auto route = std::get_if<RouteCommand>(&request.Payload);
		const auto unit = std::ranges::find_if(snapshot.Units, [&](const auto &observation) { return observation.Identity == request.Observation; });
		const auto overlay = std::ranges::find_if(m_Observation.OrderAvailability, [&](const auto &availability) {
			return availability.Observer == request.Observer && availability.Observation == request.Observation;
		});
		Result result;
		if (unit == snapshot.Units.end() || !m_ControlledObservations.contains(request.Observation.Value)) {
			result = Reject(request, "wrong_side_or_invalid_entity");
		} else if (!route) {
			result = Reject(request, "unsupported_order");
		} else if (!std::isfinite(route->Latitude.Value) || !std::isfinite(route->Longitude.Value) ||
			std::abs(route->Latitude.Value) > MAXIMUM_LATITUDE_DEGREES || std::abs(route->Longitude.Value) > MAXIMUM_LONGITUDE_DEGREES) {
			result = Reject(request, "invalid_arguments");
		} else if (route->Mode != RouteMode::IdleWaypoint && route->Mode != RouteMode::ReplaceRoute && route->Mode != RouteMode::AppendWaypoint) {
			result = Reject(request, "unsupported_route_mode");
		} else if (request.BasedOnVersion != snapshot.Version) {
			result = Reject(request, "stale_observation");
		} else if (overlay == m_Observation.OrderAvailability.end()) {
			result = Reject(request, "order_availability_unknown");
		} else {
			const auto kind = route->Mode == RouteMode::ReplaceRoute ? OrderKind::ReplaceRoute :
				route->Mode == RouteMode::AppendWaypoint ? OrderKind::AppendRoute : OrderKind::IdleWaypoint;
			const auto eligibility = std::ranges::find_if(overlay->Orders, [&](const auto &entry) { return entry.Kind == kind; });
			if (eligibility == overlay->Orders.end()) {
				result = Reject(request, "order_availability_unknown");
			} else if (eligibility->Availability == Availability::Unsupported) {
				result = Reject(request, eligibility->Reason.empty() ? "unsupported_order" : eligibility->Reason);
			} else if (eligibility->Availability != Availability::Executable) {
				result = Reject(request, eligibility->Reason.empty() ? "order_availability_unknown" : eligibility->Reason);
			} else {
				auto &plannedRoute = unit->Navigation->PlannedRoute.Value();
				if (route->Mode == RouteMode::AppendWaypoint && plannedRoute.size() >= MAXIMUM_ROUTE_POINTS) {
					result = Reject(request, "route_capacity");
				} else {
					result.SessionId = request.SessionId;
					result.RequestId = request.RequestId;
					result.Observer = request.Observer;
					result.Stage = Stage::MovementObserved;
					result.Reason = "mock_displacement";
					result.SampledAt = {1};
					result.Displacement = Physics::Api::Meters{SYNTHETIC_DISPLACEMENT_METERS};
					result.Mode = route->Mode;
					for (const auto &point : plannedRoute) {
						result.RouteBefore.push_back(point);
					}
					if (route->Mode == RouteMode::ReplaceRoute) {
						plannedRoute.clear();
					}
					plannedRoute.push_back({{route->Latitude, route->Longitude, {}}, "NC-" + request.RequestId});
					std::get<Observations::Api::PartiallyLocalizedPosition>(unit->Kinematics.Position).Latitude->Value += SYNTHETIC_LATITUDE_CHANGE_DEGREES;
					for (const auto &point : plannedRoute) {
						result.RouteAfter.push_back(point);
					}
					overlay->Orders[0].Availability = Availability::Blocked;
					overlay->Orders[0].Reason = "native_task_selection_active";
					if (route->Mode != RouteMode::IdleWaypoint) {
						result.Stage = Stage::RouteMutated;
						result.Reason = "mock_route_sequence";
						result.Displacement.reset();
					}
				}
			}
		}
		m_Ledger[request.RequestId] = result;
		return result;
	}

	Orders::Api::Result Status(const std::string &sessionId, const std::string &requestId) override {
		std::lock_guard lock(m_Mutex);
		if (sessionId != m_Observation.Snapshot.SessionId) {
			return {sessionId, requestId, m_Observation.Snapshot.Observer, Orders::Api::Stage::Rejected, "stale_session"};
		}
		if (auto retained = m_Ledger.find(requestId); retained != m_Ledger.end()) {
			const auto &route = m_Observation.Snapshot.Units[0].Navigation->PlannedRoute.Value();
			if (retained->second.Stage == Orders::Api::Stage::RouteMutated && !route.empty() && route.front().Label == "NC-" + requestId) {
				retained->second.Stage = Orders::Api::Stage::MovementObserved;
				retained->second.Reason = "mock_route_head_displacement";
				retained->second.Displacement = Physics::Api::Meters{SYNTHETIC_DISPLACEMENT_METERS};
			}
			return retained->second;
		}
		Orders::Api::Result result;
		result.SessionId = sessionId;
		result.RequestId = requestId;
		result.Observer = m_Observation.Snapshot.Observer;
		result.Stage = Orders::Api::Stage::Unknown;
		result.Reason = "unknown_or_expired_request";
		return result;
	}

	void Block() {
		std::lock_guard lock(m_Mutex);
		for (auto &eligibility : m_Observation.OrderAvailability[0].Orders) {
			eligibility.Availability = Orders::Api::Availability::Blocked;
			eligibility.Reason = "protected_native_action";
	}
		}

	void RevokeControl() {
		std::lock_guard lock(m_Mutex);
		m_ControlledObservations.clear();
	}

	void ForgetAvailability() {
		std::lock_guard lock(m_Mutex);
		m_Observation.OrderAvailability.clear();
	}
};
}
