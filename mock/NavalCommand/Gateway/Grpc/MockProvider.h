#pragma once

#include <NavalCommand/Error/Api/MakeError.h>
#include <NavalCommand/Gateway/Api/IProvider.h>
#include <NavalCommand/Simulation/Api/Error.h>
#include <mutex>
#include <set>

namespace NavalCommand::Gateway::Grpc
{
	// Controlled synthetic test infrastructure. No assertion of native behavior.
	class MockProvider final : public Api::IProvider
	{
		mutable std::mutex m_Mutex;
		std::string m_Session{"mock-mission"};
		std::uint64_t m_Revision{1};
		bool m_PlayerPaused{};
		std::set<std::string> m_PauseOwners;
		double m_Compression{1};
		double m_Time{42};

		static Api::Wire::Entry& Add(Api::Wire::ScopedPublication& view, Api::Wire::Query::Method method, const std::string& id = {}, const std::string& second = {})
		{
			auto& entry = *view.add_entries();
			entry.mutable_query()->set_method(method);
			entry.mutable_query()->set_id(id);
			entry.mutable_query()->set_second_id(second);
			return entry;
		}

	public:
		std::shared_ptr<const Api::Wire::Publication> Publication() const override
		{
			namespace Wire = Api::Wire;
			std::lock_guard lock{m_Mutex};
			auto result = std::make_shared<Wire::Publication>();
			result->set_session(m_Session);
			result->set_revision(m_Revision);
			result->set_captured_at_seconds(m_Time);
			for (const std::string faction : {"", "blue", "red"})
			{
				auto& view = *result->add_views();
				if (faction.empty())
				{
					view.mutable_scope()->mutable_omniscient();
				}
				else
				{
					view.mutable_scope()->set_faction(faction);
				}
				auto& ids = *Add(view, Wire::Query::UNIT_IDS).mutable_result()->mutable_ids();
				for (const std::string unitFaction : {"blue", "red"})
				{
					if (!faction.empty() && faction != unitFaction)
					{
						continue;
					}
					const auto identity = "truth-" + unitFaction;
					ids.add_values(identity);
					Units::Api::Instance unit{};
					unit.Identity.Value = identity;
					unit.Faction.Value = unitFaction;
					unit.Name = "Synthetic " + unitFaction;
					unit.Type.Value = "frigate";
					unit.Kinematics.Position = Physics::Api::Position{{54}, {unitFaction == "blue" ? 8.0 : 8.3}, {0}};
					unit.Kinematics.Speed = Physics::Api::MetersPerSecond{4};
					unit.Kinematics.Heading = Physics::Api::Radians{1.5};
					*Add(view, Wire::Query::UNIT, identity).mutable_result()->mutable_unit() = Api::Encode(unit);
					Operations::Api::UnitState operations{};
					operations.CurrentActivity = Operations::Api::Idle{};
					operations.NavigationState.emplace();
					*Add(view, Wire::Query::OPERATIONS, identity).mutable_result()->mutable_operations() = Api::Encode(operations);
				}
				auto& observations = *Add(view, Wire::Query::OBSERVATION_IDS).mutable_result()->mutable_ids();
				if (!faction.empty())
				{
					observations.add_values("contact-1");
					Observations::Api::Instance observation{};
					observation.Identity.Value = "contact-1";
					observation.ObservationName = "Synthetic uncertain contact";
					observation.ObserverFaction.Value = faction;
					Observations::Api::PartiallyLocalizedPosition partial;
					partial.Latitude = Physics::Api::Degrees{54.1};
					partial.Longitude = Physics::Api::Degrees{faction == "blue" ? 8.4 : 8.1};
					partial.ErrorEllipse = Observations::Api::ObservedErrorEllipse{{1500}, {500}, {0.4}};
					observation.Kinematics.Position = partial;
					observation.Type = decltype(observation.Type)::Known(Units::Api::Classification::SurfaceVessel);
					*Add(view, Wire::Query::OBSERVATION, observation.Identity.Value).mutable_result()->mutable_observation() = Api::Encode(observation);
				}
				auto& factions = *Add(view, Wire::Query::FACTION_IDS).mutable_result()->mutable_ids();
				factions.add_values("blue");
				factions.add_values("red");
				for (const std::string first : {"blue", "red"})
				{
					for (const std::string second : {"blue", "red"})
					{
						Add(view, Wire::Query::FACTION_ALIGNMENT, first, second)
							.mutable_result()
							->set_alignment(static_cast<int>(first == second ? Factions::Api::Alignment::Friendly : Factions::Api::Alignment::Hostile));
					}
				}
				Add(view, Wire::Query::UNIT_TYPE_IDS).mutable_result()->mutable_ids()->add_values("frigate");
				Units::Api::Type type{{"frigate"}, Units::Api::Classification::SurfaceVessel, {}};
				*Add(view, Wire::Query::UNIT_TYPE, "frigate").mutable_result()->mutable_unit_type() = Api::Encode(type);
				Add(view, Wire::Query::EQUIPMENT_TYPE_IDS).mutable_result()->mutable_ids();
				Add(view, Wire::Query::RESOURCE_IDS).mutable_result()->mutable_ids();
				Add(view, Wire::Query::TIME).mutable_result()->set_time_seconds(m_Time);
				Add(view, Wire::Query::PAUSED).mutable_result()->set_boolean(m_PlayerPaused || !m_PauseOwners.empty());
				Add(view, Wire::Query::CAN_PAUSE).mutable_result()->set_boolean(true);
				Add(view, Wire::Query::CAN_UNPAUSE).mutable_result()->set_boolean(true);
				Add(view, Wire::Query::COMPRESSION).mutable_result()->set_compression(m_Compression);
			}
			return result;
		}

		Error::Api::Result<Api::Wire::Result> Apply(const Api::Wire::Control& control, std::chrono::steady_clock::time_point deadline) override
		{
			std::lock_guard lock{m_Mutex};
			if (control.session() != m_Session || std::chrono::steady_clock::now() >= deadline)
			{
				return std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::NotReady));
			}
			Api::Wire::Result result;
			if (control.has_pause())
			{
				if (control.pause())
				{
					m_PauseOwners.insert(control.client());
				}
				else
				{
					m_PauseOwners.erase(control.client());
				}
			}
			else if (control.has_compression())
			{
				// Deliberate mock factors, not discovered Sea Power capabilities.
				if (control.compression() != 1 && control.compression() != 2 && control.compression() != 4)
				{
					result.mutable_failure()->set_simulation(static_cast<int>(Simulation::Api::Error::InvalidTimeCompression));
					return result;
				}
				m_Compression = control.compression();
			}
			else
			{
				return std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::ContractError));
			}
			++m_Revision;
			result.mutable_success();
			return result;
		}

		void Disconnected(const std::string& client) override
		{
			std::lock_guard lock{m_Mutex};
			m_PauseOwners.erase(client);
			++m_Revision;
		}

		void PlayerPause(bool paused)
		{
			std::lock_guard lock{m_Mutex};
			m_PlayerPaused = paused;
			++m_Revision;
		}

		void PlayerCompression(double factor)
		{
			std::lock_guard lock{m_Mutex};
			m_Compression = factor;
			++m_Revision;
		}

		void Reload(std::string session)
		{
			std::lock_guard lock{m_Mutex};
			m_Session = std::move(session);
			m_PauseOwners.clear();
			m_PlayerPaused = false;
			m_Time = 0;
			++m_Revision;
		}
	};
}
