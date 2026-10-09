#pragma once
#include <NavalCommand/Gateway/Api/IProvider.h>
#include <cmath>
#include <map>
#include <mutex>
namespace NavalCommand::Gateway::Grpc {
// Deliberately simulated execution, no game. The same domain provider contract.
class MockProvider final : public Api::IProvider {
	std::mutex mutex_;
	World::Api::View view_;
	std::map<std::string, Orders::Api::Result> ledger_;

  public:
	MockProvider() {
		Reset("mock-session");
	}
	void Reset(std::string session) {
		std::lock_guard l(mutex_);
		ledger_.clear();
		view_ = {};
		view_.sessionId = session;
		view_.observerId = "mock-observer";
		view_.ready = true;
		view_.version = 1;
		view_.units.push_back({"unit-1",
							   "Mock vessel",
							   {61.96, -10.37, {}},
							   0,
							   0,
							   {{"set_waypoint", Orders::Api::Availability::Executable, "", "append; current altitude; telegraph 2; no formation"}}});
	}
	std::expected<World::Api::View, Api::Error> View() override {
		std::lock_guard l(mutex_);
		return view_;
	}
	Orders::Api::Result Submit(const Orders::Api::Request &r) override {
		std::lock_guard l(mutex_);
		using S = Orders::Api::Stage;
		auto reject = [&](std::string why) {
			return Orders::Api::Result{r.sessionId, r.requestId, S::Rejected, why};
		};
		if (r.sessionId != view_.sessionId) {
			return reject("stale_session");
		}
		if (auto it = ledger_.find(r.requestId); it != ledger_.end()) {
			return it->second;
		}
		Orders::Api::Result result;
		if (r.requestId.empty() || r.requestId.size() > 128) {
			result = reject("invalid_arguments");
		} else if (ledger_.size() >= 128) {
			return reject("ledger_full");
		} else if (r.entityId != "unit-1") {
			result = reject("wrong_side_or_invalid_entity");
		} else if (!r.waypoint || !std::isfinite(r.waypoint->latitudeDeg) || !std::isfinite(r.waypoint->longitudeDeg) || std::abs(r.waypoint->latitudeDeg) > 90 ||
				   std::abs(r.waypoint->longitudeDeg) > 180) {
			result = reject("invalid_arguments");
		} else if (r.basedOnVersion != view_.version) {
			result = reject("stale_observation");
		} else if (view_.units[0].orders[0].availability != Orders::Api::Availability::Executable) {
			result = reject(view_.units[0].orders[0].reason);
		} else {
			view_.units[0].position.latitudeDeg += 0.001;
			result = {r.sessionId, r.requestId, S::MovementObserved, "mock_displacement", 1, 111.0};
		}
		ledger_[r.requestId] = result;
		return result;
	}
	Orders::Api::Result Status(const std::string &s, const std::string &id) override {
		std::lock_guard l(mutex_);
		if (s != view_.sessionId) {
			return {s, id, Orders::Api::Stage::Rejected, "stale_session"};
		}
		if (auto it = ledger_.find(id); it != ledger_.end()) {
			return it->second;
		}
		return {s, id, Orders::Api::Stage::Unknown, "unknown_or_expired_request"};
	}
	void Block() {
		std::lock_guard l(mutex_);
		view_.units[0].orders[0].availability = Orders::Api::Availability::Blocked;
		view_.units[0].orders[0].reason = "protected_native_action";
	}
};
} // namespace NavalCommand::Gateway::Grpc
