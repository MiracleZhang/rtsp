#include "TCPServer.h"
#include "VioletError.h"
#include "TCPTransfer.h"
#include "TCPLinkReceiver.h"
#include "VioletLog.h"
#include "VioletTime.h"
#include "OS_Define.h"
#include <time.h>

//TCP���ӳ�ʱʱ��
#define MAX_CONNETION_TIMEOUT_VAL  60

TCPServer::TCPServer()
:m_fdCount(0)
,m_nMaxConnectionCount(10)
,m_objListenTCPTransfer(NULL)
{
	LOCK_INIT(&m_objLock);
}

TCPServer::~TCPServer()
{
	stop();
	LOCK_UNINIT(&m_objLock);
}

void TCPServer::setBindAddr(const InetAddr& p_objInetAddr)
{
	m_objInetAddr = p_objInetAddr;
}

const InetAddr& TCPServer::getBindAddr()const
{
	return m_objInetAddr;
}

void TCPServer::setMaxConnectionCount(const int p_nMaxConnectionCount)
{
	m_nMaxConnectionCount = p_nMaxConnectionCount;
}

int TCPServer::getMaxConnectionCount()const
{
	return m_nMaxConnectionCount;
}

int TCPServer::start()
{
	if (m_bThreadRunFlag)
		return VIOLET_SUCCESS;

	//1.��������������
	m_objListenTCPTransfer = new TCPTransfer();
	struct epoll_event ev;
	int nRet = m_objListenTCPTransfer->open();
	if (VIOLET_SUCCESS != nRet)
		goto fail;
	
	//2.�󶨶˿�
	m_objListenTCPTransfer->setRecvBufSize(128*1024);
	m_objListenTCPTransfer->setSendBufSize(128*1024);
	nRet = m_objListenTCPTransfer->bind(m_objInetAddr);
	if (VIOLET_SUCCESS != nRet)
		goto fail;
	
	//3.��������
	nRet = m_objListenTCPTransfer->listen(m_nMaxConnectionCount);
	if (VIOLET_SUCCESS != nRet)
		goto fail;
	
	//4.����epoll
	m_nEpollFd = epoll_create(EPOLL_SIZE_MAX);
	if(m_nEpollFd == -1) 
	{
		nRet = VIOLET_ERROR_SOCKET_EPOLL_CREATE_FAILED;
		goto fail;
	}
	
	//5.��Ӽ����¼�
	m_fdCount = 1;
	ev.events = EPOLLIN;
	ev.data.fd = m_objListenTCPTransfer->getSocket();
	nRet = epoll_ctl(m_nEpollFd, EPOLL_CTL_ADD, ev.data.fd, &ev);
	if(nRet == -1) 
	{
		nRet = VIOLET_ERROR_SOCKET_EPOLL_CTRL_FAILRD;
		goto fail;
	}
	
	//6.���������߳�
	m_nServerThreadId = createThread(TCPServerThread, this, &m_bThreadRunFlag);
	if( CREATE_THREAD_FAIL == m_nServerThreadId )
	{
		nRet = VIOLET_ERROR_THREAD_CREATED_FAILED;
		goto fail;
	}
	
	return VIOLET_SUCCESS;
fail:
	if (m_objListenTCPTransfer != NULL)
	{
		m_objListenTCPTransfer->close();
		delete m_objListenTCPTransfer;
		m_objListenTCPTransfer = NULL;
	}
	return nRet;
}

void*  TCPServer::TCPServerThread(void *pParam)
{
	INFO("TCPServer", "ThreadId=%u", GET_CURRENT_THREADID);
	TCPServer *pTCPServer = static_cast<TCPServer*>(pParam);
	pTCPServer->run();
	return NULL;
}

void TCPServer::stop()
{
	//1.ֹͣ�����߳�
	if (m_nServerThreadId != 0)
		exitThread(&m_nServerThreadId, &m_bThreadRunFlag);
	m_bThreadRunFlag = false;

	//2.�رռ���
	m_objListenTCPTransfer->close();
	delete m_objListenTCPTransfer;
	m_objListenTCPTransfer = NULL;

	//3.����epoll�ļ�������
	::CLOSESOCKET(m_nEpollFd);

	//4.�������Ӷ����б�
	std::list<TCPLinkReceiver*>::iterator it = m_objTCPLinkReceiverList.begin();
	for (; it != m_objTCPLinkReceiverList.end(); ++it)
	{
		TCPLinkReceiver* pobjTCPLinkReceiver = *it;
		if (NULL != pobjTCPLinkReceiver)
			delete pobjTCPLinkReceiver;
	}
	m_objTCPLinkReceiverList.clear();
}

int TCPServer::run()
{
	int nInterval = 0;
	while (m_bThreadRunFlag)
	{
		//1.epoll���������ӣ�����ע���¼�ҵ������
		if (epoll() != VIOLET_SUCCESS)
			nInterval += 5000;
		else
			nInterval += 10;

		//2.����ʱ���ӣ�������epoll�ĳ�ʱʱ����Ϊ�ο������ԼΪ30s
		if (nInterval > 30000)
		{
			clearTimeOutConnection();
			nInterval= 0;
		}
		OS_Sleep(2);
	}
	return VIOLET_SUCCESS;
}

int TCPServer::clearTimeOutConnection()
{
	unsigned long dwNow = VIOLETTime::RealSeconds();
	std::list<TCPLinkReceiver*>::iterator it = m_objTCPLinkReceiverList.begin();
	for (; it != m_objTCPLinkReceiverList.end(); )
	{
		TCPLinkReceiver* pobjTCPLinkReceiver = *it;
		if (dwNow - pobjTCPLinkReceiver->getLastActiveTime() > MAX_CONNETION_TIMEOUT_VAL)
		{
			closeConnection(pobjTCPLinkReceiver);
			it = m_objTCPLinkReceiverList.erase(it);
			continue;
		}
		++it;
	}
	return VIOLET_SUCCESS;
}

int TCPServer::epoll()
{
	//1.epoll_wait
	int nfds = epoll_wait(m_nEpollFd, m_events, m_fdCount, 5000);
	if(nfds == -1)
	{
		ERROR("TCPServer", "epoll wait error:%s epool_fd:%d fd_count:%d", strerror(GET_LAST_ERROR), m_nEpollFd, m_fdCount);
		return VIOLET_ERROR_SOCKET_EPOLL_WAIT_FAILED;
	}
	else if(nfds == 0)
	{
		VDEBUG("TCPServer", "epoll wait timeout.");
		return VIOLET_ERROR_SOCKET_EPOLL_TIMEOUT;
	}

	//2.����ע���¼�
	for(int i = 0; i < nfds; i++)
	{
		//2.1 �����¼�������ζ�µ����ӵ���
		if(m_events[i].data.fd == m_objListenTCPTransfer->getSocket()) 
		{
			INFO("TCPServer", "server handle.");
			int nRet = handleAccept();
			if(nRet != 0) 
			{
				ERROR("TCPServer", "tcp new connection failure.");
				continue;
			}
		}
		else
		{
			//2.2 �ͻ������Ӳ����¼�
			int nRet = handleEvent((TCPLinkReceiver *)m_events[i].data.ptr, &m_events[i]);
			if (VIOLET_ERROR_SOCKET_SEND_FAILED == nRet || VIOLET_ERROR_SOCKET_RECV_FAILED == nRet)
			{
				closeConnection((TCPLinkReceiver *)m_events[i].data.ptr);
			}
		}
	}

	return VIOLET_SUCCESS;
}

int TCPServer::handleAccept()
{
	//1.accept
	ITransfer* pobjTransfer = NULL;
	int nRet = m_objListenTCPTransfer->accept(pobjTransfer);
	if(nRet != VIOLET_SUCCESS)
		return nRet;

	//2.����ת��Ϊ����
	TCPTransfer* pobjTCPTransfer = dynamic_cast<TCPTransfer*>(pobjTransfer);
	if (pobjTCPTransfer == NULL)
	{
		pobjTransfer->close();
		return VIOLET_ERROR_NULL_POINTER;
	}
	//3.���ü���ʱ��
	TCPLinkReceiver* pobjTCPLinkReceiver = creatTCPLinkReceiver();
	pobjTCPLinkReceiver->setTCPTransfer(pobjTCPTransfer);
	pobjTCPLinkReceiver->setLastActiveTime(VIOLETTime::RealSeconds());

	//4.��ʼ�������¼�����ǰ��ע��ɶ�
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLPRI;
	ev.data.ptr = (void*)pobjTCPLinkReceiver;

	//5.��Ӽ����¼�
	nRet = epoll_ctl(m_nEpollFd, EPOLL_CTL_ADD, pobjTCPTransfer->getSocket(), &ev);
	if(nRet == -1)
	{
		pobjTCPTransfer->close();
		delete pobjTCPLinkReceiver;
		delete pobjTCPTransfer;
		ERROR("VAPSessionManager", "epoll ctl error:%s epool:%d", strerror(errno), m_nEpollFd);
		return VIOLET_ERROR_SOCKET_EPOLL_CTRL_FAILRD;
	}
	//6.׷�ӵ������б�
	m_objTCPLinkReceiverList.push_back(pobjTCPLinkReceiver);
	m_fdCount++;
	return VIOLET_SUCCESS;
}

int TCPServer::addPassiveTCPTransfer(TCPTransfer* pobjTCPTransfer)
{
	if (pobjTCPTransfer == NULL)
		return VIOLET_ERROR_NULL_POINTER;

	//1.����TCP��·����
	TCPLinkReceiver* pobjTCPLinkReceiver = creatTCPLinkReceiver();
	pobjTCPLinkReceiver->setTCPTransfer(pobjTCPTransfer);
	pobjTCPLinkReceiver->setLastActiveTime(VIOLETTime::RealSeconds());

	//2.���������¼�
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLPRI;
	ev.data.ptr = (void*)pobjTCPLinkReceiver;

	//3.����¼�
	int nRet = epoll_ctl(m_nEpollFd, EPOLL_CTL_ADD, pobjTCPTransfer->getSocket(), &ev);
	if(nRet == -1)
	{
		pobjTCPTransfer->close();
		delete pobjTCPLinkReceiver;
		delete pobjTCPTransfer;
		ERROR("TCPServer", "epoll ctl error:%s epool:%d", strerror(errno), m_nEpollFd);
		return VIOLET_ERROR_SOCKET_EPOLL_CTRL_FAILRD;
	}
	//4.���뵽�����б�
	m_objTCPLinkReceiverList.push_back(pobjTCPLinkReceiver);
	m_fdCount++;
	INFO("TCPServer", "addPassiveTCPTransfer done");
	return VIOLET_SUCCESS;
}

int TCPServer::handleEvent(TCPLinkReceiver* p_pobjTCPLinkReceiver, struct epoll_event *ev)
{
	if (p_pobjTCPLinkReceiver == NULL)
		return VIOLET_ERROR_NULL_POINTER;
	//1.�¼����ദ��
	int nNetWorkEventType = 0;
	if (ev->events & EPOLLIN)
	{
		nNetWorkEventType = ITransfer::READ;
		p_pobjTCPLinkReceiver->setLastActiveTime(VIOLETTime::RealSeconds());
	}
	else if (ev->events & EPOLLOUT)
	{
		nNetWorkEventType = ITransfer::WRITE;
	}
	else if (ev->events & EPOLLPRI)
	{
		nNetWorkEventType = ITransfer::ERROR;
		ERROR("TCPServer", "handleEvent EPOLLPRI Event");
	}
	else if (ev->events & EPOLLERR)
	{
		nNetWorkEventType = ITransfer::ERROR;
		ERROR("TCPServer", "handleEvent EPOLLERR Event");
	}
	else if (ev->events & EPOLLHUP)
	{
		nNetWorkEventType = ITransfer::ERROR;
		ERROR("TCPServer", "handleEvent EPOLLHUP Event");
	}
	else 
	{
		ERROR("TCPServer", "handleEvent UnKnown Event");
		return VIOLET_ERROR_BAD_PARAM;
	}	

	//2.���崦����ʵ���ߴ���	
	return p_pobjTCPLinkReceiver->handleEvent(nNetWorkEventType);
}

int TCPServer::closeConnection(TCPLinkReceiver* p_pobjTCPLinkReceiver)
{
	if (p_pobjTCPLinkReceiver == NULL)
		return VIOLET_ERROR_NULL_POINTER;

	TCPTransfer* pobjTCPTransfer = p_pobjTCPLinkReceiver->getTCPTransfer();
	SOCKET sock = pobjTCPTransfer->getSocket();
	//1.�Ӽ����¼��б��Ƴ�
	int nRet = epoll_ctl(m_nEpollFd, EPOLL_CTL_DEL, sock, NULL);
	if(nRet == -1) 
	{
		ERROR("TCPServer", "epoll ctl sock:%d error:%s.", sock, strerror(errno));
	}
	else
	{
		m_fdCount--;
	}
	//2.�ر�����
	pobjTCPTransfer->close();
	ERROR("TCPServer", "TCPTransfer->close:%p.", pobjTCPTransfer);
	return VIOLET_SUCCESS;
}


