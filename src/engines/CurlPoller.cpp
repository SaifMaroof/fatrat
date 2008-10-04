/*
FatRat download manager
http://fatrat.dolezel.info

Copyright (C) 2006-2008 Lubos Dolezel <lubos a dolezel.info>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
version 2 as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/


#include "CurlPoller.h"
#include <QtDebug>

CurlPoller* CurlPoller::m_instance = 0;

static const int TRANSFER_TIMEOUT = 20;

CurlPoller::CurlPoller()
	: m_bAbort(false), m_usersLock(QMutex::Recursive)
{
	if(m_instance)
		abort();
	
	curl_global_init(CURL_GLOBAL_SSL);
	m_curlm = curl_multi_init();
	m_poller = Poller::createInstance(this);
	
	curl_multi_setopt(m_curlm, CURLMOPT_SOCKETFUNCTION, socket_callback);
	curl_multi_setopt(m_curlm, CURLMOPT_SOCKETDATA, this);
	
	m_instance = this;
	start();
}

CurlPoller::~CurlPoller()
{
	m_bAbort = true;
	
	if(isRunning())
		wait();
	
	m_instance = 0;
	curl_multi_cleanup(m_curlm);
	curl_global_cleanup();
}

bool operator<(const timeval& t1, const timeval& t2)
{
	if(t1.tv_sec < t2.tv_sec)
		return true;
	else if(t1.tv_sec > t2.tv_sec)
		return false;
	else
		return t1.tv_usec < t2.tv_usec;
}

void CurlPoller::run()
{
	long timeout = 0, curl_timeout;
	
	curl_multi_setopt(m_curlm, CURLMOPT_TIMERFUNCTION, timer_callback);
	curl_multi_setopt(m_curlm, CURLMOPT_TIMERDATA, &curl_timeout);
	
	while(!m_bAbort)
	{
		QList<Poller::Event> events;
		int dummy;
		timeval tvNow;
		QList<CurlUser*> timedOut;
		
		events = m_poller->wait(timeout);
		
		m_usersLock.lock();
		if(events.isEmpty())
		{
			//qDebug() << "No events";
			curl_multi_socket_action(m_curlm, CURL_SOCKET_TIMEOUT, 0, &dummy);
		}
		
		for(int i=0;i<events.size();i++)
		{
			int mask = 0;
			
			if(events[i].flags & Poller::PollerIn)
				mask |= CURL_CSELECT_IN;
			if(events[i].flags & Poller::PollerOut)
				mask |= CURL_CSELECT_OUT;
			if(events[i].flags & (Poller::PollerError | Poller::PollerHup))
				mask |= CURL_CSELECT_ERR;
			
			//qDebug() << "Events:" << mask;
			curl_multi_socket_action(m_curlm, events[i].socket, mask, &dummy);
		}
		
		gettimeofday(&tvNow, 0);
		
		if(curl_timeout <= 0 || curl_timeout > 500)
			timeout = 500;
		else
			timeout = curl_timeout;
		
		for(QHash<int,QPair<int, CurlUser*> >::iterator it = m_sockets.begin(); it != m_sockets.end(); it++)
		{
			int mask = 0;
			CurlUser* user = it.value().second;
			timeval lastOp = user->lastOperation();
			
			int seconds = tvNow.tv_sec - lastOp.tv_sec;
			
			if(seconds > TRANSFER_TIMEOUT)
				timedOut << user;
			else if(seconds > 1)
			{
				CurlUser::read_function(0, 0, 0, user);
				CurlUser::write_function(0, 0, 0, user);
			}
			
			if(user->hasNextReadTime())
			{
				if(user->nextReadTime() < tvNow)
					mask |= CURL_CSELECT_IN;
			}
			if(user->hasNextWriteTime())
			{
				if(user->nextWriteTime() < tvNow)
					mask |= CURL_CSELECT_OUT;
			}
			
			if(mask)
				curl_multi_socket_action(m_curlm, it.key(), mask, &dummy);
		}
		for(QHash<int,QPair<int, CurlUser*> >::iterator it = m_sockets.begin(); it != m_sockets.end(); it++)
		{
			int msec = -1;
			CurlUser* user = it.value().second;
			
			if(user->hasNextReadTime())
			{
				timeval tv = user->nextReadTime();
				msec = (tv.tv_sec-tvNow.tv_sec)*1000 + (tv.tv_usec-tvNow.tv_usec)/1000;
			}
			if(user->hasNextWriteTime())
			{
				int mmsec;
				timeval tv = user->nextWriteTime();
				mmsec = (tv.tv_sec-tvNow.tv_sec)*1000 + (tv.tv_usec-tvNow.tv_usec)/1000;
				
				if(mmsec < msec || msec < 0)
					msec = mmsec;
			}
			
			if(msec > 0)
			{
				if(msec < timeout)
					timeout = msec;
			}
			else
			{
				int& flags = it.value().first;
				if(user->performsLimiting())
					flags |= Poller::PollerOneShot;
				else if(flags & Poller::PollerOneShot)
					flags ^= Poller::PollerOneShot;
				m_poller->addSocket(it.key(), flags);
			}
		}
		
		while(CURLMsg* msg = curl_multi_info_read(m_curlm, &dummy))
		{
			if(msg->msg != CURLMSG_DONE)
				continue;
			
			CurlUser* user = m_users[msg->easy_handle];
			
			if(user)
				user->transferDone(msg->data.result);
		}
		
		for(int i = 0; i < m_socketsToRemove.size(); i++)
			m_sockets.remove(m_socketsToRemove[i]);
		m_socketsToRemove.clear();
		
		for(sockets_hash::iterator it = m_socketsToAdd.begin(); it != m_socketsToAdd.end(); it++)
			m_sockets[it.key()] = it.value();
		m_socketsToAdd.clear();
		
		foreach(CurlUser* user, timedOut)
			user->transferDone(CURLE_OPERATION_TIMEDOUT);
		
		m_usersLock.unlock();
	}
}

void CurlPoller::epollEnable(int socket, int events)
{
	m_poller->addSocket(socket, events);
}

int CurlPoller::timer_callback(CURLM* multi, long newtimeout, long* timeout)
{
	*timeout = newtimeout;
	return 0;
}

int CurlPoller::socket_callback(CURL* easy, curl_socket_t s, int action, CurlPoller* This, void* socketp)
{
	int flags = Poller::PollerOneShot | Poller::PollerError | Poller::PollerHup;
	
	if(action == CURL_POLL_IN || action == CURL_POLL_INOUT)
		flags |= Poller::PollerIn;
	if(action == CURL_POLL_OUT || action == CURL_POLL_INOUT)
		flags |= Poller::PollerOut;
	
	if(action == CURL_POLL_REMOVE)
	{
		qDebug() << "CurlPoller::socket_callback - remove";
		
		This->m_socketsToRemove << s;
		return This->m_poller->removeSocket(s);
	}
	else
	{
		qDebug() << "CurlPoller::socket_callback - add/mod";
		
		This->m_socketsToAdd[s] = QPair<int,CurlUser*>(flags, This->m_users[easy]); 
		
		return This->m_poller->addSocket(s, flags);
	}
}

void CurlPoller::addTransfer(CurlUser* obj)
{
	QMutexLocker locker(&m_usersLock);
	
	qDebug() << "CurlPoller::addTransfer" << obj;
	
	obj->resetStatistics();
	CURL* handle = obj->curlHandle();
	m_users[handle] = obj;
	curl_multi_add_handle(m_curlm, handle);
}

void CurlPoller::removeTransfer(CurlUser* obj)
{
	QMutexLocker locker(&m_usersLock);
	
	qDebug() << "CurlPoller::removeTransfer" << obj;
	
	CURL* handle = obj->curlHandle();
	if(handle != 0)
	{
		curl_multi_remove_handle(m_curlm, handle);
		m_users.remove(handle);
	}
}
