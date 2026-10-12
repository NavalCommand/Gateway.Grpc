#include <NavalCommand/Error/Api/MakeError.h>
#include <NavalCommand/Gateway/Grpc/Server.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <iomanip>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace NavalCommand::Gateway::Grpc
{
	namespace Wire = Api::Wire;
	namespace
	{
		constexpr std::size_t MAXIMUM_CLIENTS = 16;
		constexpr std::size_t MAXIMUM_REVISIONS_PER_CLIENT = 16;
		constexpr int MAXIMUM_MESSAGE_BYTES = 16 * 1024 * 1024;
		constexpr auto HEARTBEAT_INTERVAL = std::chrono::milliseconds(250);
		constexpr auto REVISION_LIFETIME = std::chrono::seconds(30);
		std::string NewIdentity()
		{
			// Private transport capabilities; never uses the simulator RNG.
			std::random_device random;
			std::ostringstream text;
			text << std::hex << std::setfill('0');
			for (int word = 0; word < 4; ++word)
			{
				text << std::setw(8) << random();
			}
			return text.str();
		}

		Wire::Result Failure(Error::Api::ErrorCondition condition)
		{
			Wire::Result result;
			*result.mutable_failure()->mutable_infrastructure() = Api::Encode(Error::Api::MakeError(condition));
			return result;
		}

		bool SameScope(const Wire::Scope& first, const Wire::Scope& second)
		{
			return first.kind_case() == second.kind_case() && first.faction() == second.faction();
		}

		bool SameQuery(const Wire::Query& first, const Wire::Query& second)
		{
			return first.method() == second.method() && first.id() == second.id() && first.second_id() == second.second_id();
		}
	}

	class Server::Service final : public Wire::Gateway::Service
	{
		struct RetainedRevision
		{
			std::string Client;
			std::shared_ptr<const Wire::Publication> Publication;
			int ViewIndex;
			std::chrono::steady_clock::time_point Expires;
		};

		Api::IProvider& m_Provider;
		std::mutex m_Mutex;
		std::set<std::string> m_Clients;
		std::unordered_map<std::string, RetainedRevision> m_Revisions;
		std::unordered_map<std::string, std::deque<std::string>> m_ClientRevisions;

	public:
		explicit Service(Api::IProvider& provider) : m_Provider{provider}
		{
		}

		grpc::Status Connect(grpc::ServerContext* context, const Wire::ConnectionRequest*, grpc::ServerWriter<Wire::ConnectionState>* writer) override
		{
			const auto client = NewIdentity();
			{
				std::lock_guard lock{m_Mutex};
				if (m_Clients.size() >= MAXIMUM_CLIENTS)
				{
					return {grpc::StatusCode::RESOURCE_EXHAUSTED, "client_capacity"};
				}
				m_Clients.insert(client);
			}
			Wire::ConnectionState state;
			state.set_client(client);
			while (!context->IsCancelled())
			{
				auto publication = m_Provider.Publication();
				state.set_session(publication ? publication->session() : "");
				if (!writer->Write(state))
				{
					break;
				}
				std::this_thread::sleep_for(HEARTBEAT_INTERVAL);
			}
			{
				std::lock_guard lock{m_Mutex};
				m_Clients.erase(client);
				for (const auto& token : m_ClientRevisions[client])
				{
					m_Revisions.erase(token);
				}
				m_ClientRevisions.erase(client);
			}
			m_Provider.Disconnected(client);
			return grpc::Status::OK;
		}

		grpc::Status Acquire(grpc::ServerContext*, const Wire::AcquireRequest* request, Wire::Revision* result) override
		{
			std::lock_guard lock{m_Mutex};
			if (!m_Clients.contains(request->client()))
			{
				return {grpc::StatusCode::UNAUTHENTICATED, "connection_required"};
			}
			auto publication = m_Provider.Publication();
			if (!publication || publication->session().empty())
			{
				return {grpc::StatusCode::UNAVAILABLE, "mission_unavailable"};
			}
			for (int index = 0; index < publication->views_size(); ++index)
			{
				if (!SameScope(request->scope(), publication->views(index).scope()))
				{
					continue;
				}
				const auto token = NewIdentity();
				auto& clientRevisions = m_ClientRevisions[request->client()];
				while (clientRevisions.size() >= MAXIMUM_REVISIONS_PER_CLIENT)
				{
					m_Revisions.erase(clientRevisions.front());
					clientRevisions.pop_front();
				}
				clientRevisions.push_back(token);
				m_Revisions.emplace(token, RetainedRevision{request->client(), publication, index, std::chrono::steady_clock::now() + REVISION_LIFETIME});
				result->set_token(token);
				result->set_session(publication->session());
				result->set_revision(publication->revision());
				result->set_captured_at_seconds(publication->captured_at_seconds());
				return grpc::Status::OK;
			}
			return {grpc::StatusCode::PERMISSION_DENIED, "scope_unavailable"};
		}

		grpc::Status Read(grpc::ServerContext*, const Wire::ReadRequest* request, Wire::Result* result) override
		{
			std::lock_guard lock{m_Mutex};
			auto revision = m_Revisions.find(request->token());
			auto current = m_Provider.Publication();
			if (!m_Clients.contains(request->client()) || revision == m_Revisions.end() || revision->second.Client != request->client())
			{
				*result = Failure(Error::Api::ErrorCondition::PermissionDenied);
				return grpc::Status::OK;
			}
			if (!current || current->session() != revision->second.Publication->session() || std::chrono::steady_clock::now() >= revision->second.Expires)
			{
				*result = Failure(Error::Api::ErrorCondition::NotReady);
				return grpc::Status::OK;
			}
			const auto& view = revision->second.Publication->views(revision->second.ViewIndex);
			for (const auto& entry : view.entries())
			{
				if (SameQuery(request->query(), entry.query()))
				{
					*result = entry.result();
					return grpc::Status::OK;
				}
			}
			// Identical denied lookup results regardless of hidden unit existence.
			*result = Failure(Error::Api::ErrorCondition::NotFound);
			return grpc::Status::OK;
		}

		grpc::Status Apply(grpc::ServerContext* context, const Wire::ApplyRequest* request, Wire::Result* result) override
		{
			// Serialize controls with disconnect removal. Disconnected() cannot
			// overtake an accepted request and then leave a late orphan claim.
			std::lock_guard lock{m_Mutex};
			auto publication = m_Provider.Publication();
			if (!m_Clients.contains(request->client()))
			{
				*result = Failure(Error::Api::ErrorCondition::PermissionDenied);
				return grpc::Status::OK;
			}
			if (!publication || request->control().session() != publication->session())
			{
				*result = Failure(Error::Api::ErrorCondition::NotReady);
				return grpc::Status::OK;
			}
			if (request->control().has_disconnected() || request->control().action_case() == Wire::Control::ACTION_NOT_SET)
			{
				*result = Failure(Error::Api::ErrorCondition::ContractError);
				return grpc::Status::OK;
			}
			auto control = request->control();
			control.set_client(request->client());
			const auto remaining = context->deadline() - std::chrono::system_clock::now();
			const auto boundedRemaining = std::min(remaining, std::chrono::system_clock::duration{std::chrono::seconds{1}});
			auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(boundedRemaining);
			auto applied = m_Provider.Apply(control, deadline);
			if (applied)
			{
				*result = *applied;
			}
			else
			{
				*result->mutable_failure()->mutable_infrastructure() = Api::Encode(applied.error());
			}
			return grpc::Status::OK;
		}
	};

	Server::Server(Api::IProvider& provider) : m_Service{std::make_unique<Service>(provider)}
	{
	}

	Server::~Server()
	{
		Stop();
	}

	Error::Api::Result<void> Server::Start(const std::string& endpoint)
	{
		constexpr std::string_view PREFIX = "127.0.0.1:";
		if (m_Server || !endpoint.starts_with(PREFIX))
		{
			return std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::ContractError));
		}
		unsigned port = 0;
		const auto text = std::string_view{endpoint}.substr(PREFIX.size());
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || port > 65535)
		{
			return std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::ContractError));
		}
		grpc::ServerBuilder builder;
		builder.SetMaxReceiveMessageSize(MAXIMUM_MESSAGE_BYTES);
		builder.SetMaxSendMessageSize(MAXIMUM_MESSAGE_BYTES);
		builder.RegisterService(m_Service.get());
		int selectedPort = 0;
		builder.AddListeningPort(endpoint, grpc::InsecureServerCredentials(), &selectedPort);
		m_Server = builder.BuildAndStart();
		if (!m_Server)
		{
			return std::unexpected(Error::Api::MakeError(Error::Api::ErrorCondition::CommunicationError));
		}
		m_Port = selectedPort;
		return {};
	}

	void Server::Stop()
	{
		if (m_Server)
		{
			m_Server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
			m_Server->Wait();
			m_Server.reset();
		}
	}

	int Server::Port() const
	{
		return m_Port;
	}
}
