#pragma once
#include <NavalCommand/Gateway/Api/IProvider.h>
#include <cmath>
#include <cstddef>
#include <map>
#include <mutex>
namespace NavalCommand::Gateway::Grpc {
// Deliberately simulated execution, no game. The same domain provider contract.
class MockProvider final : public Api::IProvider {
	static constexpr std::size_t MAXIMUM_RETAINED_REQUESTS = 128;
	static constexpr std::size_t MAXIMUM_REQUEST_ID_CHARACTERS = 128;
	static constexpr double MAXIMUM_LATITUDE_DEGREES = 90;
	static constexpr double MAXIMUM_LONGITUDE_DEGREES = 180;
	// The mock reports a fixed synthetic movement, not a measured geographic distance.
	static constexpr double SYNTHETIC_LATITUDE_CHANGE_DEGREES = 0.001;
	static constexpr double SYNTHETIC_DISPLACEMENT_METERS = 111.0;
	std::mutex m_Mutex;
	World::Api::View m_View;
	std::map<std::string, Orders::Api::Result> m_Ledger;

public:
	MockProvider() {
		Reset("mock-session");
	}
	void Reset(std::string session) {
		std::lock_guard lock(m_Mutex);
		m_Ledger.clear();
		m_View = {};
		m_View.sessionId = session;
		m_View.observerId = "mock-observer";
		m_View.ready = true;
		m_View.version = 1;
		m_View.units.push_back({"unit-1",
								"Mock vessel",
								{61.96, -10.37, {}},
								0,
								0,
								{{"set_waypoint", Orders::Api::Availability::Executable, "", "append; current altitude; telegraph 2; no formation"}}});
	}
	std::expected<World::Api::View, Api::Error> View() override {
		std::lock_guard lock(m_Mutex);
		return m_View;
	}
	Orders::Api::Result Submit(const Orders::Api::Request &request) override {
		std::lock_guard lock(m_Mutex);
		using OrderStage = Orders::Api::Stage;
		auto reject = [&](std::string reason) {
			return Orders::Api::Result{request.sessionId, request.requestId, OrderStage::Rejected, reason};
		};
		if (request.sessionId != m_View.sessionId) {
			return reject("stale_session");
		}
		if (auto retainedResult = m_Ledger.find(request.requestId); retainedResult != m_Ledger.end()) {
			return retainedResult->second;
		}
		Orders::Api::Result result;
		if (request.requestId.empty() || request.requestId.size() > MAXIMUM_REQUEST_ID_CHARACTERS) {
			result = reject("invalid_arguments");
		} else if (m_Ledger.size() >= MAXIMUM_RETAINED_REQUESTS) {
			return reject("ledger_full");
		} else if (request.entityId != "unit-1") {
			result = reject("wrong_side_or_invalid_entity");
		} else if (!request.waypoint || !std::isfinite(request.waypoint->latitudeDeg) || !std::isfinite(request.waypoint->longitudeDeg) ||
				   std::abs(request.waypoint->latitudeDeg) > MAXIMUM_LATITUDE_DEGREES || std::abs(request.waypoint->longitudeDeg) > MAXIMUM_LONGITUDE_DEGREES) {
			result = reject("invalid_arguments");
		} else if (request.basedOnVersion != m_View.version) {
			result = reject("stale_observation");
		} else if (m_View.units[0].orders[0].availability != Orders::Api::Availability::Executable) {
			result = reject(m_View.units[0].orders[0].reason);
		} else {
			m_View.units[0].position.latitudeDeg += SYNTHETIC_LATITUDE_CHANGE_DEGREES;
			result = {request.sessionId, request.requestId, OrderStage::MovementObserved, "mock_displacement", 1, SYNTHETIC_DISPLACEMENT_METERS};
		}
		m_Ledger[request.requestId] = result;
		return result;
	}
	Orders::Api::Result Status(const std::string &sessionId, const std::string &requestId) override {
		std::lock_guard lock(m_Mutex);
		if (sessionId != m_View.sessionId) {
			return {sessionId, requestId, Orders::Api::Stage::Rejected, "stale_session"};
		}
		if (auto retainedResult = m_Ledger.find(requestId); retainedResult != m_Ledger.end()) {
			return retainedResult->second;
		}
		return {sessionId, requestId, Orders::Api::Stage::Unknown, "unknown_or_expired_request"};
	}
	void Block() {
		std::lock_guard lock(m_Mutex);
		m_View.units[0].orders[0].availability = Orders::Api::Availability::Blocked;
		m_View.units[0].orders[0].reason = "protected_native_action";
	}
};
} // namespace NavalCommand::Gateway::Grpc
