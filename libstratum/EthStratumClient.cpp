
#include "EthStratumClient.h"
#include <libdevcore/Log.h>
using boost::asio::ip::tcp;


EthStratumClient::EthStratumClient(GenericFarm<EthashProofOfWork> * f, MinerType m, string const & host, string const & port, string const & user, string const & pass, int const & retries, int const & worktimeout, bool const & precompute)
	: m_socket(m_io_service)
{
	m_minerType = m;
	m_primary.host = host;
	m_primary.port = port;
	m_primary.user = user;
	m_primary.pass = pass;

	p_active = &m_primary;

	m_authorized = false;
	m_connected = false;
	m_precompute = precompute;
	m_pending = 0;
	m_maxRetries = retries;
	m_worktimeout = worktimeout;

	p_farm = f;
	p_worktimer = nullptr;
	connect();
}

EthStratumClient::~EthStratumClient()
{

}

void EthStratumClient::setFailover(string const & host, string const & port)
{
	setFailover(host, port, p_active->user, p_active->pass);
}

void EthStratumClient::setFailover(string const & host, string const & port, string const & user, string const & pass)
{
	m_failover.host = host;
	m_failover.port = port;
	m_failover.user = user;
	m_failover.pass = pass;
}

void EthStratumClient::connect()
{
	
	tcp::resolver r(m_io_service);
	tcp::resolver::query q(p_active->host, p_active->port);
	
	r.async_resolve(q, boost::bind(&EthStratumClient::resolve_handler, this, 
		boost::asio::placeholders::error, boost::asio::placeholders::iterator));
	
	cnote << "Connecting to stratum server " << p_active->host + ":" + p_active->port;

	boost::thread t(boost::bind(&boost::asio::io_service::run, &m_io_service));
	
}

#define BOOST_ASIO_ENABLE_CANCELIO 

void EthStratumClient::reconnect()
{
	/*
	if (p_farm->isMining())
	{
		cnote << "Stopping farm";
		p_farm->stop();
	}
	*/
	if (p_worktimer) {
		p_worktimer->cancel();
		p_worktimer = nullptr;
	}

	m_io_service.reset();
	m_socket.close();
	m_authorized = false;
	m_connected = false;
	

	
	if (!m_failover.host.empty())
	{
		m_retries++;

		if (m_retries > m_maxRetries)
		{
			if (m_failover.host == "exit") {
				disconnect();
				return;
			}
			else if (p_active == &m_primary)
			{
				p_active = &m_failover;
			}
			else {
				p_active = &m_primary;
			}
			m_retries = 0;
		}
	}
	
	cnote << "Reconnecting in 3 seconds...";
	boost::asio::deadline_timer     timer(m_io_service, boost::posix_time::seconds(3));
	timer.wait();

	connect();
}

void EthStratumClient::disconnect()
{
	cnote << "Disconnecting";
	m_connected = false;
	m_running = false;
	if (p_farm->isMining())
	{
		cnote << "Stopping farm";
		p_farm->stop();
	}
	m_socket.close();
	m_io_service.stop();
}

void EthStratumClient::resolve_handler(const boost::system::error_code& ec, tcp::resolver::iterator i)
{
	if (!ec)
	{
		async_connect(m_socket, i, boost::bind(&EthStratumClient::connect_handler, this, 
			boost::asio::placeholders::error, boost::asio::placeholders::iterator));
	}
	else
	{
		cerr << "Could not resolve host " << p_active->host + ":" + p_active->port + ", " << ec.message();
		reconnect();
	}
}

void EthStratumClient::connect_handler(const boost::system::error_code& ec, tcp::resolver::iterator i)
{
	dev::setThreadName("stratum");
	
	if (!ec)
	{
		m_connected = true;
		cnote << "Connected to stratum server " << i->host_name() << ":" << p_active->port;
		if (!p_farm->isMining())
		{
			cnote << "Starting farm";
			if (m_minerType == MinerType::CPU)
				p_farm->start("cpu");
			else if (m_minerType == MinerType::CL)
				p_farm->start("opencl");
			else if (m_minerType == MinerType::CUDA)
				p_farm->start("cuda");
		}
		std::ostream os(&m_requestBuffer);
		os << "{\"id\": 1, \"method\": \"mining.subscribe\", \"params\": []}\n";

		
		async_write(m_socket, m_requestBuffer,
			boost::bind(&EthStratumClient::handleResponse, this,
									boost::asio::placeholders::error));
	}
	else
	{
		cwarn << "Could not connect to stratum server " << p_active->host << ":" << p_active->port << ", " << ec.message();
		reconnect();
	}

}

void EthStratumClient::readline() {
	x_pending.lock();
	if (m_pending == 0) {
		async_read_until(m_socket, m_responseBuffer, "\n",
			boost::bind(&EthStratumClient::readResponse, this,
			boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
	
		m_pending++;
		
	}
	x_pending.unlock();
}

void EthStratumClient::handleResponse(const boost::system::error_code& ec) {
	if (!ec)
	{
		readline();
	}
	else
	{
		dev::setThreadName("stratum");
		cwarn << "Handle response failed: " << ec.message();
	}
}

void EthStratumClient::readResponse(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
	dev::setThreadName("stratum");
	x_pending.lock();
	m_pending = m_pending > 0 ? m_pending - 1 : 0;
	x_pending.unlock();

	if (!ec && bytes_transferred)
	{
		std::istream is(&m_responseBuffer);
		std::string response;
		getline(is, response);

		if (response.front() == '{' && response.back() == '}') 
		{
			Json::Value responseObject;
			Json::Reader reader;
			if (reader.parse(response.c_str(), responseObject))
			{
				processReponse(responseObject);
				m_response = response;
			}
			else
			{
				cwarn << "Parse response failed: " << reader.getFormattedErrorMessages();
			}
		}
		else
		{
			cwarn << "Discarding incomplete response";
		}
		if (m_connected)
			readline();
	}
	else
	{
		cwarn << "Read response failed: " << ec.message();
		if (m_connected)
			reconnect();
	}
}

void EthStratumClient::processReponse(Json::Value& responseObject)
{
	Json::Value error = responseObject.get("error", new Json::Value);
	if (error.isArray())
	{
		string msg = error.get(1, "Unknown error").asString();
		cnote << msg;
	}
	std::ostream os(&m_requestBuffer);
	Json::Value params;
	int id = responseObject.get("id", Json::Value::null).asInt();
	switch (id)
	{
	case 1:
		cnote << "Subscribed to stratum server";

		os << "{\"id\": 2, \"method\": \"mining.authorize\", \"params\": [\"" << p_active->user << "\",\"" << p_active->pass << "\"]}\n";

		async_write(m_socket, m_requestBuffer,
			boost::bind(&EthStratumClient::handleResponse, this,
			boost::asio::placeholders::error));
		break;
	case 2:
		m_authorized = responseObject.get("result", Json::Value::null).asBool();
		if (!m_authorized)
		{
			cnote << "Worker not authorized:" << p_active->user;
			disconnect();
			return;
		}
		cnote << "Authorized worker " << p_active->user;
		break;
	case 4:
		if (responseObject.get("result", false).asBool()) {
			cnote << "B-) Submitted and accepted.";
			p_farm->acceptedSolution(m_stale);
		}
		else {
			cwarn << ":-( Not accepted.";
			p_farm->rejectedSolution(m_stale);
		}
		break;
	default:
		string method = responseObject.get("method", "").asString();
		if (method == "mining.notify")
		{
			params = responseObject.get("params", Json::Value::null);
			if (params.isArray())
			{
				string job = params.get((Json::Value::ArrayIndex)0, "").asString();
				string sHeaderHash = params.get((Json::Value::ArrayIndex)1, "").asString();
				string sSeedHash = params.get((Json::Value::ArrayIndex)2, "").asString();
				string sShareTarget = params.get((Json::Value::ArrayIndex)3, "").asString();
				//bool cleanJobs = params.get((Json::Value::ArrayIndex)4, "").asBool();
				
				// coinmine.pl fix
				int l = sShareTarget.length();
				if (l < 66)
					sShareTarget = "0x" + string(66 - l, '0') + sShareTarget.substr(2);
								

				if (sHeaderHash != "" && sSeedHash != "" && sShareTarget != "")
				{
					cnote << "Received new job #" + job.substr(0,8);
					//cnote << "Header hash: " + sHeaderHash;
					//cnote << "Seed hash: " + sSeedHash;
					//cnote << "Share target: " + sShareTarget;

					h256 seedHash = h256(sSeedHash);
					h256 headerHash = h256(sHeaderHash);
					EthashAux::FullType dag;


					if (seedHash != m_current.seedHash)
					{
						cnote << "Grabbing DAG for" << seedHash;
					}
					if (!(dag = EthashAux::full(seedHash, true, [&](unsigned _pc){ m_waitState = _pc < 100 ? MINER_WAIT_STATE_DAG : MINER_WAIT_STATE_WORK;  cnote << "Creating DAG. " << _pc << "% done..."; return 0; })))
					{
						BOOST_THROW_EXCEPTION(DAGCreationFailure());
					}
					if (m_precompute)
					{
						EthashAux::computeFull(sha3(seedHash), true);
					}
					if (headerHash != m_current.headerHash)
					{
						//x_current.lock();
						if (p_worktimer)
							p_worktimer->cancel();

						m_previous.headerHash = m_current.headerHash;
						m_previous.seedHash = m_current.seedHash;
						m_previous.boundary = m_current.boundary;
						m_previousJob = m_job;

						m_current.headerHash = h256(sHeaderHash);
						m_current.seedHash = seedHash;
						m_current.boundary = h256(sShareTarget);// , h256::AlignRight);
						m_job = job;

						p_farm->setWork(m_current);
						//x_current.unlock();
						p_worktimer = new boost::asio::deadline_timer(m_io_service, boost::posix_time::seconds(m_worktimeout));
						p_worktimer->async_wait(boost::bind(&EthStratumClient::work_timeout_handler, this, boost::asio::placeholders::error));

					}
				}
			}
		}
		else if (method == "mining.set_difficulty")
		{

		}
		else if (method == "client.get_version")
		{
			os << "{\"error\": null, \"id\" : " << id << ", \"result\" : \"" << ETH_PROJECT_VERSION << "\"}\n";
			async_write(m_socket, m_requestBuffer,
				boost::bind(&EthStratumClient::handleResponse, this,
				boost::asio::placeholders::error));
		}
		break;
	}

}

void EthStratumClient::work_timeout_handler(const boost::system::error_code& ec) {
	if (!ec) {
		cnote << "No new work received in" << m_worktimeout << "seconds.";
		reconnect();
	}
}

bool EthStratumClient::submit(EthashProofOfWork::Solution solution) {
	x_current.lock();
	EthashProofOfWork::WorkPackage tempWork(m_current);
	string temp_job = m_job;
	EthashProofOfWork::WorkPackage tempPreviousWork(m_previous);
	string temp_previous_job = m_previousJob;
	x_current.unlock();

	cnote << "Solution found; Submitting to" << p_active->host << "...";
	cnote << "  Nonce:" << "0x" + solution.nonce.hex();

	if (EthashAux::eval(tempWork.seedHash, tempWork.headerHash, solution.nonce).value < tempWork.boundary)
	{
		string json = "{\"id\": 4, \"method\": \"mining.submit\", \"params\": [\"" + p_active->user + "\",\"" + temp_job + "\",\"0x" + solution.nonce.hex() + "\",\"0x" + tempWork.headerHash.hex() + "\",\"0x" + solution.mixHash.hex() + "\"]}\n";
		std::ostream os(&m_requestBuffer);
		os << json;
		m_stale = false;
		async_write(m_socket, m_requestBuffer,
			boost::bind(&EthStratumClient::handleResponse, this,
			boost::asio::placeholders::error));
		return true;
	}
	else if (EthashAux::eval(tempPreviousWork.seedHash, tempPreviousWork.headerHash, solution.nonce).value < tempPreviousWork.boundary)
	{
		string json = "{\"id\": 4, \"method\": \"mining.submit\", \"params\": [\"" + p_active->user + "\",\"" + temp_previous_job + "\",\"0x" + solution.nonce.hex() + "\",\"0x" + tempPreviousWork.headerHash.hex() + "\",\"0x" + solution.mixHash.hex() + "\"]}\n";
		std::ostream os(&m_requestBuffer);
		os << json;
		m_stale = true;
		cwarn << "Submitting stale solution.";
		async_write(m_socket, m_requestBuffer,
			boost::bind(&EthStratumClient::handleResponse, this,
			boost::asio::placeholders::error));
		return true;
	}
	else {
		m_stale = false;
		cwarn << "FAILURE: GPU gave incorrect result!";
		p_farm->failedSolution();
	}

	return false;
}

