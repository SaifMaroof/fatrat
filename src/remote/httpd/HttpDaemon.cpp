#include "HttpDaemon.h"
#include "RuntimeException.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <boost/scoped_array.hpp>
#include <netdb.h>

HttpDaemon::HttpDaemon()
: m_server(0), m_poller(0), m_bUseV6(false), m_port(2233)
{
}

HttpDaemon::~HttpDaemon()
{
	stop();
}

void HttpDaemon::startV4()
{
	m_server = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_server < 0)
	        throwErrnoException();

	sockaddr_in sa;
	sa.sin_family = AF_INET;
	sa.sin_port = htons(m_port);

	if (m_strBindAddress.isEmpty())
        sa.sin_addr.s_addr = INADDR_ANY;
	else
	{
		addrinfo hints;
        addrinfo* res;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        QByteArray ba = m_strBindAddress.toUtf8();
        if (int e = getaddrinfo(ba.constData(), 0, &hints, &res))
            throw RuntimeException(gai_strerror(e));
        sa.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
	}

	if (::bind(m_server, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0)
		throwErrnoException();
}

void HttpDaemon::startV6()
{
	m_server = ::socket(AF_INET6, SOCK_STREAM, 0);
	if (m_server < 0)
			throwErrnoException();

	sockaddr_in6 sa;
	sa.sin6_family = AF_INET6;
	sa.sin6_port = htons(m_port);

	if (m_strBindAddress.isEmpty())
		sa.sin6_addr = in6addr_any;
	else
	{
		addrinfo hints;
		addrinfo* res;

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET6;
		hints.ai_socktype = SOCK_STREAM;

        QByteArray ba = m_strBindAddress.toUtf8();
		if (int e = getaddrinfo(ba.constData(), 0, &hints, &res))
			throw RuntimeException(gai_strerror(e));
        sa.sin6_addr = reinterpret_cast<sockaddr_in6*>(res->ai_addr)->sin6_addr;
	}

	if (::bind(m_server, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0)
		throwErrnoException();
}

void* HttpDaemon::pollThread(void* t)
{
	HttpDaemon* This = static_cast<HttpDaemon*>(t);

	// Add the listening socket to the fd set
	This->m_poller->addSocket(This->m_server, Poller::PollerIn | Poller::PollerError);
	
    while (This->m_server)
	{
        if (!This->pollCycle())
			break;
	}

	return 0;
}

bool HttpDaemon::pollCycle()
{
	Poller::Event ev[5];
	int nfds;

    nfds = m_poller->wait(500, ev, sizeof(ev)/sizeof(ev[0]));

	for (int i = 0; i < nfds; i++)
	{
        const Poller::Event& e = ev[i];
        if (e.socket == m_server)
		{
			if (e.flags & Poller::PollerError)
				return false;
			acceptClient();
 		}
        else if (m_clients.contains(e.socket))
		{
			if (e.flags & Poller::PollerError)
				closeClient(e.socket);
			else if (e.flags & Poller::PollerIn)
				readClient(e.socket);
			else if (e.flags & Poller::PollerOut)
				writeClient(e.socket);
		}
		else
		{
			// This should never happen
			m_poller->removeSocket(e.socket);
			::close(e.socket);
		}
	}
	return true;
}

void HttpDaemon::closeClient(int s)
{
	m_poller->removeSocket(s);
	::close(s);
    m_clients.remove(s);
}

bool HttpDaemon::tryProcessRequest(int s)
{
	// detect end-of-request (double \r\n)
	Client& c = m_clients[s];
	int pos = c.requestBuffer.indexOf("\r\n\r\n");

	if (pos < 0)
		return false;

	QByteArray req = c.requestBuffer.left(pos+4);
	QList<QByteArray> lines;
	HttpRequest request;

    c.requestBuffer = c.requestBuffer.mid(pos+4);
	lines = req.split('\n');

	// Parse request line
	QList<QByteArray> seg = lines[0].split(' ');

	if (lines.size() < 2 || seg.size() != 3)
	{
		// TODO: send bad request
		closeClient(s);
		return true;
	}

	request.method = seg[0];
	request.uri = seg[1]; // TODO: remove query string

	{
		int pos = request.uri.indexOf('?');
        QByteArray vars = request.uri.mid(pos+1).toLatin1();

		request.getVars = parseUE(vars);
		request.uri.truncate(pos);
	}

	for (int i=1; i<lines.size()-1; i++)
	{
        QByteArray line = lines[i].trimmed();
		QByteArray name, value;
		int pos = line.indexOf(": ");

		if (pos < 0)
			continue;

        request.headers[line.left(pos).toLower()] = line.mid(pos+2);
	}

	long long cl = request.headers["content-length"].toLongLong();
	if (!cl)
	{
		c.state = Client::Responding; 
	}
	else
	{
		if (request.method != "POST")
		{
			// TODO: send bad request
			closeClient(s);
            return true;
		}
		else if (request.headers["content-type"] == "application/x-www-form-urlencoded")
		{
			// handle locally
			c.state = Client::ReceivingUEBody;
		}
		else
		{
			// handled in the handler
			c.state = Client::ReceivingBody;
		}
	}

	// TODO: find the handler
	// no handler -> 404
	//
	// Client::Responding -> call the handler
	// other state -> process already received bytes

	return true;
}

QMap<QString,QString> HttpDaemon::parseUE(QByteArray ba)
{
	QList<QByteArray> segs = ba.split('&');
	QMap<QString, QString> rv;

	for (int i = 0; i < segs.size(); i++)
	{
		QString name, value;
		int pos = segs[i].indexOf('=');

		if (pos < 0)
			continue;

		name = QByteArray::fromPercentEncoding(segs[i].left(pos));
		value = QByteArray::fromPercentEncoding(segs[i].mid(pos+1));
		rv[name] = value;
	}

	return rv;
}

void HttpDaemon::readClient(int s)
{
	Client& c = m_clients[s];
	if (c.state == Client::Responding)
		return; // Shouldn't happen
	else if (c.state == Client::ReceivingBody)
	{
		if (!c.handler)
		{
			closeClient(s);
			return;
		}

		// TODO: call handler
	}
	else if (c.state == Client::ReceivingHeaders)
	{
        boost::scoped_array<char> buf(new char[4*1024]);
		int rd;

		while ((rd = readBytes(s, buf.get(), 4*1024)) > 0)
		{
			c.requestBuffer.append(buf.get(), rd);
			if (tryProcessRequest(s))
				break;
		}

		if (rd < 0)
		{
			// TODO: log read error
			closeClient(s);
			return;
		}
	}
}

void HttpDaemon::writeClient(int s)
{
}

void HttpDaemon::acceptClient()
{
	boost::shared_ptr<sockaddr> sa;
	socklen_t len;

    if (m_bUseV6)
	{
		len = sizeof(sockaddr_in6);
        sa = boost::shared_ptr<sockaddr>( reinterpret_cast<sockaddr*>(new sockaddr_in6) );
	}
	else
	{
		len = sizeof(sockaddr_in);
        sa = boost::shared_ptr<sockaddr>( reinterpret_cast<sockaddr*>(new sockaddr_in) );
	}

	int s = ::accept(m_server, sa.get(), &len);
	if (s > 0)
	{
		Client c;
		c.addr = sa;
		c.state = Client::ReceivingHeaders;
		m_clients[s] = c;

		// TODO: make unblocking

		m_poller->addSocket(s, Poller::PollerIn | Poller::PollerError);
	}
}

void HttpDaemon::start()
{
	if (m_bUseV6)
		startV6();
	else
		startV4();

	if (::listen(m_server, 5) < 0)
		throwErrnoException();

	m_poller = Poller::createInstance(this);
	pthread_create(&m_thread, 0, HttpDaemon::pollThread, this); 
}

void HttpDaemon::stop()
{
	if (m_server > 0)
	{
		::close(m_server);
		m_server = 0;
        pthread_join(m_thread, 0);
		delete m_poller;
		m_poller = 0;

		for (QMap<int,Client>::iterator it = m_clients.begin(); it != m_clients.end(); it++)
		{
            ::close(it.key());
		}
		m_clients.clear();
	}
}
