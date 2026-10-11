#pragma once
#include <NavalCommand/Gateway/Api/IProvider.h>
#include <NavalCommand/Orders/Api/RouteMode.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <mutex>
#include <set>

namespace NavalCommand::Gateway::Grpc {
// Deliberately simulated execution, no game. Authority is independent of public identity.
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
	std::set<std::string> m_ControlledUnits;
	std::map<std::string, Orders::Api::Result> m_Ledger;

public:
	MockProvider() {
		Reset("mock-session");
	}

	void Reset(std::string session) {
		std::lock_guard lock(m_Mutex);
		m_Ledger.clear();
		m_ControlledUnits = {"unit-1"};
		m_Observation = {};
		auto &snapshot = m_Observation.Snapshot;
		snapshot.SessionId = std::move(session);
		snapshot.ObserverId = "mock-observer";
		snapshot.Ready = true;
		snapshot.Version = 1;
		using namespace Units::Api;
		using Uncertainty::Api::Knowledge;
		using Uncertainty::Api::ObservedCollection;
		UnitInstance unit;
		unit.Identity = UnitId{"unit-1"};
		unit.Display.Name = Knowledge<std::string>::Known("Mock vessel");
		unit.Kinematics.Position = Knowledge<PositionObservation>::Known({{{61.96}, {-10.37}, {}}});
		unit.Kinematics.Speed = Knowledge<Physics::Api::MetersPerSecond>::Known({0});
		unit.Observation.MeasuredAt = Knowledge<Physics::Api::Seconds>::Known({0});
		unit.Navigation = NavigationState{Knowledge<std::vector<RoutePoint>>::Known({})};
		snapshot.Instances.push_back(std::move(unit));
		UnitInstance contact;
		contact.Identity = UnitId{"contact-1"};
		contact.Display.TypeId = Knowledge<Catalog::Api::UnitTypeId>::Known({"observed-class"});
		contact.Kinematics.Position = Knowledge<PositionObservation>::Estimated({{{62}, {-11}, {}}}, "synthetic-contact-fixture");
		// A cache observation is not evidence of the last measurement time.
		snapshot.Instances.push_back(std::move(contact));
		m_Observation.OrderAvailability.push_back({{"unit-1"}, 1,
			{ObservedCollection<Orders::Api::Eligibility>::Coverage::Complete,
				{{Orders::Api::OrderKind::IdleWaypoint, Orders::Api::Availability::Executable, ""},
				 {Orders::Api::OrderKind::ReplaceRoute, Orders::Api::Availability::Executable, ""},
				 {Orders::Api::OrderKind::AppendRoute, Orders::Api::Availability::Executable, ""}}}});
	}

	std::expected<Api::ProviderObservation, Api::Error> View() override {
		std::lock_guard lock(m_Mutex);
		return m_Observation;
	}

	Orders::Api::Result Submit(const Orders::Api::Request &request) override {
		std::lock_guard lock(m_Mutex);
		using namespace Orders::Api;
		auto &snapshot = m_Observation.Snapshot;
		auto reject = [&](std::string reason) {
			return Result{request.SessionId, request.RequestId, Stage::Rejected, std::move(reason)};
		};
		if (request.SessionId != snapshot.SessionId) {
			return reject("stale_session");
		}
		if (auto retained = m_Ledger.find(request.RequestId); retained != m_Ledger.end()) {
			return retained->second;
		}
		if (request.RequestId.empty() || request.RequestId.size() > MAXIMUM_REQUEST_ID_CHARACTERS) {
			return reject("invalid_arguments");
		}
		if (m_Ledger.size() >= MAXIMUM_RETAINED_REQUESTS) {
			return reject("ledger_full");
		}
		Result result;
		const auto route = std::get_if<RouteCommand>(&request.Payload);
		const auto overlay = std::ranges::find_if(m_Observation.OrderAvailability, [&](const auto &availability) { return availability.Unit == request.Unit; });
		if (!m_ControlledUnits.contains(request.Unit.Value)) {
			result = reject("wrong_side_or_invalid_entity");
		} else if (!route) {
			result = reject("unsupported_order");
		} else if (!std::isfinite(route->Latitude.Value) || !std::isfinite(route->Longitude.Value) ||
			std::abs(route->Latitude.Value) > MAXIMUM_LATITUDE_DEGREES || std::abs(route->Longitude.Value) > MAXIMUM_LONGITUDE_DEGREES) {
			result = reject("invalid_arguments");
		} else if (route->Mode != RouteMode::IdleWaypoint && route->Mode != RouteMode::ReplaceRoute && route->Mode != RouteMode::AppendWaypoint) {
			result = reject("unsupported_route_mode");
		} else if (request.BasedOnVersion != snapshot.Version) {
			result = reject("stale_observation");
		} else if (overlay == m_Observation.OrderAvailability.end() ||
			overlay->Orders.GetCoverage() == Uncertainty::Api::ObservedCollection<Eligibility>::Coverage::Unknown) {
			result = reject("order_availability_unknown");
		} else {
			const auto kind = route->Mode == RouteMode::ReplaceRoute ? OrderKind::ReplaceRoute :
				route->Mode == RouteMode::AppendWaypoint ? OrderKind::AppendRoute : OrderKind::IdleWaypoint;
			const auto eligibility = std::ranges::find_if(overlay->Orders.Items(), [&](const auto &entry) { return entry.Kind == kind; });
			if (eligibility == overlay->Orders.Items().end()) {
				result = reject(overlay->Orders.GetCoverage() == Uncertainty::Api::ObservedCollection<Eligibility>::Coverage::Complete ?
					"unsupported_order" : "order_availability_unknown");
			} else if (eligibility->Availability != Availability::Executable) {
				result = reject(eligibility->Reason.empty() ? "order_availability_unknown" : eligibility->Reason);
			} else {
				auto &unit = snapshot.Instances[0];
				auto &plannedRoute = unit.Navigation->PlannedRoute.Value();
				if (route->Mode == RouteMode::AppendWaypoint && plannedRoute.size() >= MAXIMUM_ROUTE_POINTS) {
					result = reject("route_capacity");
				} else {
					unit.Kinematics.Position.Value().Coordinates.Latitude.Value += SYNTHETIC_LATITUDE_CHANGE_DEGREES;
					result = {request.SessionId, request.RequestId, Stage::MovementObserved, "mock_displacement", {1}, Physics::Api::Meters{SYNTHETIC_DISPLACEMENT_METERS}};
					result.Mode = route->Mode;
					result.RouteBefore = plannedRoute;
					if (route->Mode == RouteMode::ReplaceRoute) {
						plannedRoute.clear();
					}
					plannedRoute.push_back({route->Latitude, route->Longitude, "NC-" + request.RequestId});
					result.RouteAfter = plannedRoute;
					overlay->Orders.At(0).Availability = Availability::Blocked;
					overlay->Orders.At(0).Reason = "native_task_selection_active";
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
			return {sessionId, requestId, Orders::Api::Stage::Rejected, "stale_session"};
		}
		if (auto retained = m_Ledger.find(requestId); retained != m_Ledger.end()) {
			const auto &route = m_Observation.Snapshot.Instances[0].Navigation->PlannedRoute.Value();
			if (retained->second.Stage == Orders::Api::Stage::RouteMutated && !route.empty() && route.front().Label == "NC-" + requestId) {
				retained->second.Stage = Orders::Api::Stage::MovementObserved;
				retained->second.Reason = "mock_route_head_displacement";
				retained->second.Displacement = Physics::Api::Meters{SYNTHETIC_DISPLACEMENT_METERS};
			}
			return retained->second;
		}
		return {sessionId, requestId, Orders::Api::Stage::Unknown, "unknown_or_expired_request"};
	}

	void Block() {
		std::lock_guard lock(m_Mutex);
		for (std::size_t index = 0; index < m_Observation.OrderAvailability[0].Orders.Items().size(); ++index) {
			auto &eligibility = m_Observation.OrderAvailability[0].Orders.At(index);
			eligibility.Availability = Orders::Api::Availability::Blocked;
			eligibility.Reason = "protected_native_action";
		}
	}

	void RevokeControl() {
		std::lock_guard lock(m_Mutex);
		m_ControlledUnits.clear();
	}

	void ForgetAvailability() {
		std::lock_guard lock(m_Mutex);
		m_Observation.OrderAvailability.clear();
	}
};
}
