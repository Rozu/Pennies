// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2013 Pennies developers and contributors
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "irc.h"
#include "db.h"
#include "net.h"
#include "init.h"
#include "strlcpy.h"
#include "addrman.h"
#include "ui_interface.h"

#ifdef WIN32
#include <string.h>
#endif

#ifdef USE_UPNP
#include <miniupnpc/miniwget.h>
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

using namespace std;
using namespace boost;


// WM - static const int MAX_OUTBOUND_CONNECTIONS = 8;
#define DEFAULT_MAX_CONNECTIONS         125    // WM - Default value for -maxconnections= parameter.
#define MIN_CONNECTIONS                 8      // WM - Lowest value we allow for -maxconnections= (never ever set less than 2!).
#define MAX_CONNECTIONS                 1000   // WM - Max allowed value for -maxconnections= parameter.  Getting kinda excessive, eh?

#define DEFAULT_OUTBOUND_CONNECTIONS    8      // WM - Reasonable default of 8 outbound connections for -maxoutbound= parameter.
#define MIN_OUTBOUND_CONNECTIONS        4      // WM - Lowest we allow for -maxoutbound= parameter shall be 4 connections (never ever set below 2).
#define MAX_OUTBOUND_CONNECTIONS        100    // WM - This no longer means what it used to.  Outbound conn count now runtime configurable.


void ThreadMessageHandler2(void* parg);
void ThreadSocketHandler2(void* parg);
void ThreadOpenConnections2(void* parg);
void ThreadOpenAddedConnections2(void* parg);
#ifdef USE_UPNP
void ThreadMapPort2(void* parg);
#endif
void ThreadDNSAddressSeed2(void* parg);
bool OpenNetworkConnection(const CAddress& addrConnect, CSemaphoreGrant *grantOutbound = NULL, const char *strDest = NULL, bool fOneShot = false);


struct LocalServiceInfo {
    int nScore;
    int nPort;
};

//
// Global state variables
//
bool fClient = false;
bool fDiscover = true;
bool fUseUPnP = false;
uint64 nLocalServices = (fClient ? 0 : NODE_NETWORK);
static CCriticalSection cs_mapLocalHost;
static map<CNetAddr, LocalServiceInfo> mapLocalHost;
static bool vfReachable[NET_MAX] = {};
static bool vfLimited[NET_MAX] = {};
static CNode* pnodeLocalHost = NULL;
CNode* pnodeSync = NULL;
//CNode* pnodeLastSync = NULL;

CAddress addrSeenByPeer(CService("0.0.0.0", 0), nLocalServices);
uint64 nLocalHostNonce = 0;
array<int, THREAD_MAX> vnThreadsRunning;
static std::vector<SOCKET> vhListenSocket;
CAddrMan addrman;

vector<CNode*> vNodes;
CCriticalSection cs_vNodes;
map<CInv, CDataStream> mapRelay;
deque<pair<int64, CInv> > vRelayExpiration;
CCriticalSection cs_mapRelay;
map<CInv, int64> mapAlreadyAskedFor;

static deque<string> vOneShots;
CCriticalSection cs_vOneShots;

set<CNetAddr> setservAddNodeAddresses;
CCriticalSection cs_setservAddNodeAddresses;

static CSemaphore *semOutbound = NULL;

extern std::map<int, uint256> mapHardenSyncPoints;
extern set<pair<COutPoint, unsigned int> > setStakeSeenOrphan;



void AddOneShot(string strDest)
{
    LOCK(cs_vOneShots);
    vOneShots.push_back(strDest);
}

unsigned short GetListenPort()
{
    return (unsigned short)(GetArg("-port", GetDefaultPort()));
}



//
// int GetMaxConnections( void )
//
//    WM - Function to determine maximum allowed in+out connections.
//
//    Parameters: None
//    Returns: Maximum connections allowed (int)
//

int GetMaxConnections()
{
    int count;

    // Config'eth away..
    count = GetArg( "-maxconnections", DEFAULT_MAX_CONNECTIONS );
    
    // Ensure some level of sanity amount the max connection count.
    count = max( count, MIN_CONNECTIONS );
    count = min( count, MAX_CONNECTIONS );
    
    //printf( "GetMaxConnections() = %d\n", count );

    return count;
}



//
// int GetMaxOutboundConnections( void )
//
//    WM - Function to determine maximum allowed outbound connections.
//
//    Parameters: None
//    Returns: Maximum outbound connections allowed (int)
//

int GetMaxOutboundConnections()
{
    int count;

    // What sayeth the config parameters?
    count = GetArg( "-maxoutbound", DEFAULT_OUTBOUND_CONNECTIONS );
    
    // Did someone set it too low or too high?  Shame, shame..
    count = max( count, MIN_OUTBOUND_CONNECTIONS );
    count = min( count, MAX_OUTBOUND_CONNECTIONS );
    count = min( count, GetMaxConnections() );

    //printf( "GetMaxOutboundConnections() = %d\n", count );
    
    return count;
}



void CNode::PushGetBlocks(CBlockIndex* pindexBegin, uint256 hashEnd)
{
	if(IsInitialBlockDownload())
	{
		if(fDebug)
			printf("ignore PushGetBlocks when concurrentsync\n");
		return;
	}
	
    // Filter out duplicate requests
    if (pindexBegin == pindexLastGetBlocksBegin && hashEnd == hashLastGetBlocksEnd){
		printf("duplicate PushGetBlocks request, ip:%s, hashEnd:%s\n", addr.ToString().c_str(), hashEnd.ToString().c_str());
        return;
    }
    pindexLastGetBlocksBegin = pindexBegin;
    hashLastGetBlocksEnd = hashEnd;

    PushMessage("getblocks", CBlockLocator(pindexBegin), hashEnd);
}

// find 'best' local address for a particular peer
bool GetLocal(CService& addr, const CNetAddr *paddrPeer)
{
    if (fNoListen)
        return false;

    int nBestScore = -1;
    int nBestReachability = -1;
    {
        LOCK(cs_mapLocalHost);
        for (map<CNetAddr, LocalServiceInfo>::iterator it = mapLocalHost.begin(); it != mapLocalHost.end(); it++)
        {
            int nScore = (*it).second.nScore;
            int nReachability = (*it).first.GetReachabilityFrom(paddrPeer);
            if (nReachability > nBestReachability || (nReachability == nBestReachability && nScore > nBestScore))
            {
                addr = CService((*it).first, (*it).second.nPort);
                nBestReachability = nReachability;
                nBestScore = nScore;
            }
        }
    }
    return nBestScore >= 0;
}

// get best local address for a particular peer as a CAddress
CAddress GetLocalAddress(const CNetAddr *paddrPeer)
{
    CAddress ret(CService("0.0.0.0",0),0);
    CService addr;
    if (GetLocal(addr, paddrPeer))
    {
        ret = CAddress(addr);
        ret.nServices = nLocalServices;
        ret.nTime = GetAdjustedTime();
    }
    return ret;
}

bool RecvLine(SOCKET hSocket, string& strLine)
{
    strLine = "";
    loop
    {
        char c;
        int nBytes = recv(hSocket, &c, 1, 0);
        if (nBytes > 0)
        {
            if (c == '\n')
                continue;
            if (c == '\r')
                return true;
            strLine += c;
            if (strLine.size() >= 9000)
                return true;
        }
        else if (nBytes <= 0)
        {
            if (fShutdown)
                return false;
            if (nBytes < 0)
            {
                int nErr = WSAGetLastError();
                if (nErr == WSAEMSGSIZE)
                    continue;
                if (nErr == WSAEWOULDBLOCK || nErr == WSAEINTR || nErr == WSAEINPROGRESS)
                {
                    Sleep(10);
                    continue;
                }
            }
            if (!strLine.empty())
                return true;
            if (nBytes == 0)
            {
                // socket closed
                printf("socket closed\n");
                return false;
            }
            else
            {
                // socket error
                int nErr = WSAGetLastError();
                printf("recv failed: %d\n", nErr);
                return false;
            }
        }
    }
}

// used when scores of local addresses may have changed
// pushes better local address to peers
void static AdvertizeLocal()
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        if (pnode->fSuccessfullyConnected)
        {
            CAddress addrLocal = GetLocalAddress(&pnode->addr);
            if (addrLocal.IsRoutable() && (CService)addrLocal != (CService)pnode->addrLocal)
            {
                pnode->PushAddress(addrLocal);
                pnode->addrLocal = addrLocal;
            }
        }
    }
}

void SetReachable(enum Network net, bool fFlag)
{
    LOCK(cs_mapLocalHost);
    vfReachable[net] = fFlag;
    if (net == NET_IPV6 && fFlag)
        vfReachable[NET_IPV4] = true;
}

// learn a new local address
bool AddLocal(const CService& addr, int nScore)
{
    if (!addr.IsRoutable())
        return false;

    if (!fDiscover && nScore < LOCAL_MANUAL)
        return false;

    if (IsLimited(addr))
        return false;

    printf("AddLocal(%s,%i)\n", addr.ToString().c_str(), nScore);

    {
        LOCK(cs_mapLocalHost);
        bool fAlready = mapLocalHost.count(addr) > 0;
        LocalServiceInfo &info = mapLocalHost[addr];
        if (!fAlready || nScore >= info.nScore) {
            info.nScore = nScore + (fAlready ? 1 : 0);
            info.nPort = addr.GetPort();
        }
        SetReachable(addr.GetNetwork());
    }

    AdvertizeLocal();

    return true;
}

bool AddLocal(const CNetAddr &addr, int nScore)
{
    return AddLocal(CService(addr, GetListenPort()), nScore);
}

/** Make a particular network entirely off-limits (no automatic connects to it) */
void SetLimited(enum Network net, bool fLimited)
{
    if (net == NET_UNROUTABLE)
        return;
    LOCK(cs_mapLocalHost);
    vfLimited[net] = fLimited;
}

bool IsLimited(enum Network net)
{
    LOCK(cs_mapLocalHost);
    return vfLimited[net];
}

bool IsLimited(const CNetAddr &addr)
{
    return IsLimited(addr.GetNetwork());
}

/** vote for a local address */
bool SeenLocal(const CService& addr)
{
    {
        LOCK(cs_mapLocalHost);
        if (mapLocalHost.count(addr) == 0)
            return false;
        mapLocalHost[addr].nScore++;
    }

    AdvertizeLocal();

    return true;
}

/** check whether a given address is potentially local */
bool IsLocal(const CService& addr)
{
    LOCK(cs_mapLocalHost);
    return mapLocalHost.count(addr) > 0;
}

/** check whether a given address is in a network we can probably connect to */
bool IsReachable(const CNetAddr& addr)
{
    LOCK(cs_mapLocalHost);
    enum Network net = addr.GetNetwork();
    return vfReachable[net] && !vfLimited[net];
}

bool GetMyExternalIP2(const CService& addrConnect, const char* pszGet, const char* pszKeyword, CNetAddr& ipRet)
{
    SOCKET hSocket;
    if (!ConnectSocket(addrConnect, hSocket))
        return error("GetMyExternalIP() : connection to %s failed", addrConnect.ToString().c_str());

    send(hSocket, pszGet, strlen(pszGet), MSG_NOSIGNAL);

    string strLine;
    while (RecvLine(hSocket, strLine))
    {
        if (strLine.empty()) // HTTP response is separated from headers by blank line
        {
            loop
            {
                if (!RecvLine(hSocket, strLine))
                {
                    closesocket(hSocket);
                    return false;
                }
                if (pszKeyword == NULL)
                    break;
                if (strLine.find(pszKeyword) != string::npos)
                {
                    strLine = strLine.substr(strLine.find(pszKeyword) + strlen(pszKeyword));
                    break;
                }
            }
            closesocket(hSocket);
            if (strLine.find("<") != string::npos)
                strLine = strLine.substr(0, strLine.find("<"));
            strLine = strLine.substr(strspn(strLine.c_str(), " \t\n\r"));
            while (strLine.size() > 0 && isspace(strLine[strLine.size()-1]))
                strLine.resize(strLine.size()-1);
            CService addr(strLine,0,true);
            printf("GetMyExternalIP() received [%s] %s\n", strLine.c_str(), addr.ToString().c_str());
            if (!addr.IsValid() || !addr.IsRoutable())
                return false;
            ipRet.SetIP(addr);
            return true;
        }
    }
    closesocket(hSocket);
    return error("GetMyExternalIP() : connection closed");
}

// We now get our external IP from the IRC server first and only use this as a backup
bool GetMyExternalIP(CNetAddr& ipRet)
{
    CService addrConnect;
    const char* pszGet;
    const char* pszKeyword;

    for (int nLookup = 0; nLookup <= 1; nLookup++)
    for (int nHost = 1; nHost <= 2; nHost++)
    {
        // We should be phasing out our use of sites like these.  If we need
        // replacements, we should ask for volunteers to put this simple
        // php file on their web server that prints the client IP:
        //  <?php echo $_SERVER["REMOTE_ADDR"]; ?>
        if (nHost == 1)
        {
            addrConnect = CService("91.198.22.70",80); // checkip.dyndns.org

            if (nLookup == 1)
            {
                CService addrIP("checkip.dyndns.org", 80, true);
                if (addrIP.IsValid())
                    addrConnect = addrIP;
            }

            pszGet = "GET / HTTP/1.1\r\n"
                     "Host: checkip.dyndns.org\r\n"
                     "User-Agent: Mozilla/4.0 (compatible; MSIE 7.0; Windows NT 5.1)\r\n"
                     "Connection: close\r\n"
                     "\r\n";

            pszKeyword = "Address:";
        }
        else if (nHost == 2)
        {
            addrConnect = CService("74.208.43.192", 80); // www.showmyip.com

            if (nLookup == 1)
            {
                CService addrIP("www.showmyip.com", 80, true);
                if (addrIP.IsValid())
                    addrConnect = addrIP;
            }

            pszGet = "GET /simple/ HTTP/1.1\r\n"
                     "Host: www.showmyip.com\r\n"
                     "User-Agent: Mozilla/4.0 (compatible; MSIE 7.0; Windows NT 5.1)\r\n"
                     "Connection: close\r\n"
                     "\r\n";

            pszKeyword = NULL; // Returns just IP address
        }

        if (GetMyExternalIP2(addrConnect, pszGet, pszKeyword, ipRet))
            return true;
    }

    return false;
}

void ThreadGetMyExternalIP(void* parg)
{
    // Make this thread recognisable as the external IP detection thread
    RenameThread("bitcoin-ext-ip");

    CNetAddr addrLocalHost;
    if (GetMyExternalIP(addrLocalHost))
    {
        printf("GetMyExternalIP() returned %s\n", addrLocalHost.ToStringIP().c_str());
        AddLocal(addrLocalHost, LOCAL_HTTP);
    }
}





void AddressCurrentlyConnected(const CService& addr)
{
    addrman.Connected(addr);
}







CNode* FindNode(const CNetAddr& ip)
{
    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
            if ((CNetAddr)pnode->addr == ip)
                return (pnode);
    }
    return NULL;
}

CNode* FindNode(std::string addrName)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
        if (pnode->addrName == addrName)
            return (pnode);
    return NULL;
}

CNode* FindNode(const CService& addr)
{
    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
            if ((CService)pnode->addr == addr)
                return (pnode);
    }
    return NULL;
}

CNode* ConnectNode(CAddress addrConnect, const char *pszDest, int64 nTimeout)
{
    if (pszDest == NULL) {
        if (IsLocal(addrConnect))
            return NULL;

        // Look for an existing connection
        CNode* pnode = FindNode((CService)addrConnect);
        if (pnode)
        {
            if (nTimeout != 0)
                pnode->AddRef(nTimeout);
            else
                pnode->AddRef();
            return pnode;
        }
    }


    /// debug print
    printf("trying connection %s lastseen=%.1fhrs\n",
        pszDest ? pszDest : addrConnect.ToString().c_str(),
        pszDest ? 0 : (double)(GetAdjustedTime() - addrConnect.nTime)/3600.0);

    // Connect
    SOCKET hSocket;
    if (pszDest ? ConnectSocketByName(addrConnect, hSocket, pszDest, GetDefaultPort()) : ConnectSocket(addrConnect, hSocket))
    {
        addrman.Attempt(addrConnect);

        /// debug print
        printf("connected %s\n", pszDest ? pszDest : addrConnect.ToString().c_str());

        // Set to non-blocking
#ifdef WIN32
        u_long nOne = 1;
        if (ioctlsocket(hSocket, FIONBIO, &nOne) == SOCKET_ERROR)
            printf("ConnectSocket() : ioctlsocket non-blocking setting failed, error %d\n", WSAGetLastError());
#else
        if (fcntl(hSocket, F_SETFL, O_NONBLOCK) == SOCKET_ERROR)
            printf("ConnectSocket() : fcntl non-blocking setting failed, error %d\n", errno);
#endif

        // Add node
        CNode* pnode = new CNode(hSocket, addrConnect, pszDest ? pszDest : "", false);
        if (nTimeout != 0)
            pnode->AddRef(nTimeout);
        else
            pnode->AddRef();

        {
            LOCK(cs_vNodes);
            vNodes.push_back(pnode);			
			printf("peerinfo, connected node:%s\n", pnode->addr.ToString().c_str());
        }

        pnode->nTimeConnected = GetTime();
        return pnode;
    }
    else
    {
        return NULL;
    }
}

bool CNode::OpenSocket()
{
		CService addrConnect;
		if (ConnectSocketByName(addrConnect, hSocket, addr.ToString().c_str(), GetDefaultPort()))
		{
			addrman.Attempt(addrConnect);
	
			/// debug print
			printf("opensocket, connected %s\n", addr.ToString().c_str());
	
			// Set to non-blocking
#ifdef WIN32
			u_long nOne = 1;
			if (ioctlsocket(hSocket, FIONBIO, &nOne) == SOCKET_ERROR)
				printf("opensocket, ConnectSocket() : ioctlsocket non-blocking setting failed, error %d\n", WSAGetLastError());
#else
			if (fcntl(hSocket, F_SETFL, O_NONBLOCK) == SOCKET_ERROR)
				printf("opensocket, ConnectSocket() : fcntl non-blocking setting failed, error %d\n", errno);
#endif
	
			nTimeConnected = GetTime();
			return true;
		}
		else
		{
			return false;
		}

}

void CNode::CloseSocketDisconnect()
{
    fDisconnect = true;
    if (hSocket != INVALID_SOCKET)
    {
        printf("disconnecting node %s\n", addrName.c_str());
        closesocket(hSocket);
        hSocket = INVALID_SOCKET;
        //vRecv.clear();
    }


	
    // in case this fails, we'll empty the recv buffer when the CNode is deleted
    TRY_LOCK(cs_vRecv, lockRecv);
    if (lockRecv)
        vRecv.clear();


	//do not set NULL here, the thread "bitcoin-msghand" will crash when visiting pnodeSync
    /*//// if this was the sync node, we'll need a new one
    if (this == pnodeSync)
        pnodeSync = NULL;
        */
}

bool CNode::DisconnectWhenReset()
{
    if (hSocket != INVALID_SOCKET)
    {
        printf("reset, disconnecting node %s\n", addrName.c_str());
        closesocket(hSocket);
        hSocket = INVALID_SOCKET;
        //vRecv.clear();
    }


	
    // in case this fails, we'll empty the recv buffer when the CNode is deleted
   	{
	    TRY_LOCK(cs_vRecv, lockRecv);
	    if (lockRecv)
	    {
	        vRecv.clear();
	    }
    }

	{
	    TRY_LOCK(cs_vSend, lockSend);
	    if (lockSend)
	    {
	        vSend.clear();
	    }
	}

	nReset = RESET_WAITING_FOR_CLEAR_MSG;

	return true;

}


bool CNode::ConnectWhenReset()
{
	bool ret =  OpenSocket();
	nReset = RESET_IDLE;

	PushVersion();

	printf("reset finished, ip:%s\n", addrName.c_str());
	return ret;
}

void CNode::Cleanup()
{
}


void CNode::PushVersion()
{
    /// when NTP implemented, change to just nTime = GetAdjustedTime()
    int64 nTime = (fInbound ? GetAdjustedTime() : GetTime());
    CAddress addrYou = (addr.IsRoutable() && !IsProxy(addr) ? addr : CAddress(CService("0.0.0.0",0)));
    CAddress addrMe = GetLocalAddress(&addr);
    RAND_bytes((unsigned char*)&nLocalHostNonce, sizeof(nLocalHostNonce));
    printf("send version message: version %d, blocks=%d, us=%s, them=%s, peer=%s\n", PROTOCOL_VERSION, nBestHeight, addrMe.ToString().c_str(), addrYou.ToString().c_str(), addr.ToString().c_str());
    PushMessage("version", PROTOCOL_VERSION, nLocalServices, nTime, addrYou, addrMe,
                nLocalHostNonce, FormatSubVersion(CLIENT_NAME, CLIENT_VERSION, std::vector<string>()), nBestHeight);
}





std::map<CNetAddr, int64> CNode::setBanned;
CCriticalSection CNode::cs_setBanned;

void CNode::ClearBanned()
{
    setBanned.clear();
}

bool CNode::IsBanned(CNetAddr ip)
{
    bool fResult = false;
    {
        LOCK(cs_setBanned);
        std::map<CNetAddr, int64>::iterator i = setBanned.find(ip);
        if (i != setBanned.end())
        {
            int64 t = (*i).second;
            if (GetTime() < t)
                fResult = true;
        }
    }
    return fResult;
}

bool CNode::Misbehaving(int howmuch)
{
    if (addr.IsLocal())
    {
        printf("Warning: Local node %s misbehaving (delta: %d)!\n", addrName.c_str(), howmuch);
        return false;
    }

    nMisbehavior += howmuch;
    if (nMisbehavior >= GetArg("-banscore", 100))
    {
        int64 banTime = GetTime()+GetArg("-bantime", 60*60*24);  // Default 24-hour ban
        printf("Misbehaving: %s (%d -> %d) DISCONNECTING\n", addr.ToString().c_str(), nMisbehavior-howmuch, nMisbehavior);
        {
            LOCK(cs_setBanned);
            if (setBanned[addr] < banTime)
                setBanned[addr] = banTime;
        }
        CloseSocketDisconnect();
        return true;
    } else
        printf("Misbehaving: %s (%d -> %d)\n", addr.ToString().c_str(), nMisbehavior-howmuch, nMisbehavior);
    return false;
}

#undef X
#define X(name) stats.name = name
void CNode::copyStats(CNodeStats &stats)
{
    X(nServices);
    X(nLastSend);
    X(nLastRecv);
    X(nTimeConnected);
    X(addrName);
    X(nVersion);
    X(strSubVer);
    X(fInbound);
    X(nReleaseTime);
    X(nStartingHeight);
    X(nMisbehavior);
}
#undef X










void ThreadSocketHandler(void* parg)
{
    // Make this thread recognisable as the networking thread
    RenameThread("bitcoin-net");

    try
    {
        vnThreadsRunning[THREAD_SOCKETHANDLER]++;
        ThreadSocketHandler2(parg);
        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
        PrintException(&e, "ThreadSocketHandler()");
    } catch (...) {
        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
        throw; // support pthread_cancel()
    }
    printf("ThreadSocketHandler exited\n");
}

void ThreadSocketHandler2(void* parg)
{
    printf("ThreadSocketHandler started\n");
    list<CNode*> vNodesDisconnected;
    unsigned int nPrevNodeCount = 0;

    loop
    {
        //
        // Disconnect nodes
        //
        {
            LOCK(cs_vNodes);
            // Disconnect unused nodes
            vector<CNode*> vNodesCopy = vNodes;
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
            {
                if (pnode->fDisconnect ||
                    (pnode->GetRefCount() <= 0 && pnode->vRecv.empty() && pnode->vSend.empty()))
                {
                	/*if(!pnode->fDisconnect && pnode->nSpeed > 0 && pnodeSync)
                	{
                		//it means this node is waiting for next download, don't disconnect it
                		continue;
                	}

					if(!pnode->fDisconnect && !pnode->bUsed && pnodeSync)
					{
						//don't disconnect unused node when downloading
						continue;
					}*/
                	printf("peerinfo, remove node:%s, fDisconnect:%d, refcount:%d, recv:%"PRIszu", send:%"PRIszu", speed:%d\n", 
						pnode->addr.ToString().c_str(),
						pnode->fDisconnect,
						pnode->GetRefCount(),
						pnode->vRecv.size(),
						pnode->vSend.size(),
						pnode->nSpeed);
                    // remove from vNodes
                    vNodes.erase(remove(vNodes.begin(), vNodes.end(), pnode), vNodes.end());

                    // release outbound grant (if any)
                    pnode->grantOutbound.Release();

                    // close socket and cleanup
                    pnode->CloseSocketDisconnect();
                    pnode->Cleanup();

                    // hold in disconnected pool until all refs are released
                    pnode->nReleaseTime = max(pnode->nReleaseTime, GetTime() + 15 * 60);
                    if (pnode->fNetworkNode || pnode->fInbound)
                        pnode->Release();
                    vNodesDisconnected.push_back(pnode);
                }
            }

            // Delete disconnected nodes
            list<CNode*> vNodesDisconnectedCopy = vNodesDisconnected;
            BOOST_FOREACH(CNode* pnode, vNodesDisconnectedCopy)
            {
                // wait until threads are done using it
                if (pnode->GetRefCount() <= 0)
                {
                    bool fDelete = false;
                    {
                        TRY_LOCK(pnode->cs_vSend, lockSend);
                        if (lockSend)
                        {
                            TRY_LOCK(pnode->cs_vRecv, lockRecv);
                            if (lockRecv)
                            {
                                TRY_LOCK(pnode->cs_mapRequests, lockReq);
                                if (lockReq)
                                {
                                    TRY_LOCK(pnode->cs_inventory, lockInv);
                                    if (lockInv)
                                        fDelete = true;
                                }
                            }
                        }
                    }
                    if (fDelete)
                    {
                        vNodesDisconnected.remove(pnode);
                        delete pnode;
                    }
                }
            }
        }
        if (vNodes.size() != nPrevNodeCount)
        {
            nPrevNodeCount = vNodes.size();
            uiInterface.NotifyNumConnectionsChanged(vNodes.size());
        }

		/*//
		//reset node, intent to clear sync context such as setInventoryKown on the remote node
		//
		
        {
            LOCK(cs_vNodes);
            vector<CNode*> vNodesCopy = vNodes;
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
            {
            	if(RESET_WAITING_FOR_DISCONNECT == pnode->nReset)
            	{
                    pnode->DisconnectWhenReset();
            	}
				
            	if(RESET_WAITING_FOR_CONNECTED == pnode->nReset)
            	{
                    pnode->ConnectWhenReset();
            	}
			}
		}*/

        //
        // Find which sockets have data to receive
        //
        struct timeval timeout;
        timeout.tv_sec  = 0;
        timeout.tv_usec = 50000; // frequency to poll pnode->vSend

        fd_set fdsetRecv;
        fd_set fdsetSend;
        fd_set fdsetError;
        FD_ZERO(&fdsetRecv);
        FD_ZERO(&fdsetSend);
        FD_ZERO(&fdsetError);
        SOCKET hSocketMax = 0;
        bool have_fds = false;

        BOOST_FOREACH(SOCKET hListenSocket, vhListenSocket) {
            FD_SET(hListenSocket, &fdsetRecv);
            hSocketMax = max(hSocketMax, hListenSocket);
            have_fds = true;
        }
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes)
            {
                if (pnode->hSocket == INVALID_SOCKET)
                    continue;
                FD_SET(pnode->hSocket, &fdsetRecv);
                FD_SET(pnode->hSocket, &fdsetError);
                hSocketMax = max(hSocketMax, pnode->hSocket);
                have_fds = true;
                {
                    TRY_LOCK(pnode->cs_vSend, lockSend);
                    if (lockSend && !pnode->vSend.empty())
                        FD_SET(pnode->hSocket, &fdsetSend);
                }
            }
        }

        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
        int nSelect = select(have_fds ? hSocketMax + 1 : 0,
                             &fdsetRecv, &fdsetSend, &fdsetError, &timeout);
        vnThreadsRunning[THREAD_SOCKETHANDLER]++;
        if (fShutdown)
            return;
        if (nSelect == SOCKET_ERROR)
        {
            if (have_fds)
            {
                int nErr = WSAGetLastError();
                printf("socket select error %d\n", nErr);
                for (unsigned int i = 0; i <= hSocketMax; i++)
                    FD_SET(i, &fdsetRecv);
            }
            FD_ZERO(&fdsetSend);
            FD_ZERO(&fdsetError);
            Sleep(timeout.tv_usec/1000);
        }


        //
        // Accept new connections
        //
        BOOST_FOREACH(SOCKET hListenSocket, vhListenSocket)
        if (hListenSocket != INVALID_SOCKET && FD_ISSET(hListenSocket, &fdsetRecv))
        {
#ifdef USE_IPV6
            struct sockaddr_storage sockaddr;
#else
            struct sockaddr sockaddr;
#endif
            socklen_t len = sizeof(sockaddr);
            SOCKET hSocket = accept(hListenSocket, (struct sockaddr*)&sockaddr, &len);
            CAddress addr;
            int nInbound = 0;

            if (hSocket != INVALID_SOCKET)
                if (!addr.SetSockAddr((const struct sockaddr*)&sockaddr))
                    printf("Warning: Unknown socket family\n");

            {
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                    if (pnode->fInbound)
                        nInbound++;
            }

            if (hSocket == INVALID_SOCKET)
            {
                int nErr = WSAGetLastError();
                if (nErr != WSAEWOULDBLOCK)
                    printf("socket error accept failed: %d\n", nErr);
            }
// WM            else if (nInbound >= GetArg("-maxconnections", DEFAULT_MAX_CONNECTIONS ) - /* WM - MAX_OUTBOUND_CONNECTIONS */ GetMaxOutboundConnections() )
            else if ( nInbound >= GetMaxConnections() - GetMaxOutboundConnections() )
            {
                {
                    LOCK(cs_setservAddNodeAddresses);
                    if (!setservAddNodeAddresses.count(addr))
                        closesocket(hSocket);
                }
            }
            else if (CNode::IsBanned(addr))
            {
                printf("connection from %s dropped (banned)\n", addr.ToString().c_str());
                closesocket(hSocket);
            }
            else
            {
                printf("peerinfo, accepted node:%s\n", addr.ToString().c_str());
                CNode* pnode = new CNode(hSocket, addr, "", true);
                pnode->AddRef();
                {
                    LOCK(cs_vNodes);
                    vNodes.push_back(pnode);
                }
            }
        }


        //
        // Service each socket
        //
        vector<CNode*> vNodesCopy;
        {
            LOCK(cs_vNodes);
            vNodesCopy = vNodes;
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->AddRef();
        }
        BOOST_FOREACH(CNode* pnode, vNodesCopy)
        {
            if (fShutdown)
                return;

            //
            // Receive
            //
            if (pnode->hSocket == INVALID_SOCKET)
                continue;
            if (FD_ISSET(pnode->hSocket, &fdsetRecv) || FD_ISSET(pnode->hSocket, &fdsetError))
            {
                TRY_LOCK(pnode->cs_vRecv, lockRecv);
                if (lockRecv)
                {
                    CDataStream& vRecv = pnode->vRecv;
                    unsigned int nPos = vRecv.size();

                    if (nPos > ReceiveBufferSize()) {
                        if (!pnode->fDisconnect)
                            printf("socket recv flood control disconnect (%"PRIszu" bytes), ip:%s\n", vRecv.size(), pnode->addr.ToString().c_str());
                        pnode->CloseSocketDisconnect();
                    }
                    else {
                        // typical socket buffer is 8K-64K
                        char pchBuf[0x10000];
                        int nBytes = recv(pnode->hSocket, pchBuf, sizeof(pchBuf), MSG_DONTWAIT);
                        if (nBytes > 0)
                        {
                            vRecv.resize(nPos + nBytes);
                            memcpy(&vRecv[nPos], pchBuf, nBytes);
                            pnode->nLastRecv = GetTime();
                        }
                        else if (nBytes == 0)
                        {
                            // socket closed gracefully
                            if (!pnode->fDisconnect)
                                printf("socket:%d closed, ip:%s\n", pnode->hSocket,  pnode->addr.ToString().c_str());
                            pnode->CloseSocketDisconnect();
                        }
                        else if (nBytes < 0)
                        {
                            // error
                            int nErr = WSAGetLastError();
                            if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
                            {
                                if (!pnode->fDisconnect)
                                    printf("socket recv error %d, ip:%s\n", nErr, pnode->addr.ToString().c_str());
                                pnode->CloseSocketDisconnect();
                            }
                        }
                    }
                }
            }

            //
            // Send
            //
            if (pnode->hSocket == INVALID_SOCKET)
                continue;
            if (FD_ISSET(pnode->hSocket, &fdsetSend))
            {
                TRY_LOCK(pnode->cs_vSend, lockSend);
                if (lockSend)
                {
                    CDataStream& vSend = pnode->vSend;
                    if (!vSend.empty())
                    {
                        int nBytes = send(pnode->hSocket, &vSend[0], vSend.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
                        if (nBytes > 0)
                        {
                            vSend.erase(vSend.begin(), vSend.begin() + nBytes);
                            pnode->nLastSend = GetTime();
                        }
                        else if (nBytes < 0)
                        {
                            // error
                            int nErr = WSAGetLastError();
                            if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
                            {
                                printf("socket send error %d, ip:%s\n", nErr, pnode->addr.ToString().c_str());
                                pnode->CloseSocketDisconnect();
                            }
                        }
                    }
                }
            }


			/*//heartbeat
			if(pnodeSync)
			{
				bool bSendPing = false;
				if(!pnode->fDisconnect && pnode->nSpeed > 0)
				{
					//it means this node is waiting for next download, don't disconnect it
					bSendPing = true;
				}
				
				if(!pnode->fDisconnect && !pnode->bUsed)
				{
					//don't disconnect unused node when downloading
					bSendPing = true;
				}

				if(bSendPing)
				{
					uint64 nonce = 0;
					if(GetTime() - pnode->nLastSend > 10)
					{
						//printf("peerinfo, test, send ping to:%s\n", pnode->addr.ToString().c_str());
						pnode->PushMessage("ping", nonce);
						pnode->nLastSend = GetTime();
					}
				}
			}*/

            //
            // Inactivity checking
            //
            if (pnode->vSend.empty())
                pnode->nLastSendEmpty = GetTime();
            if (GetTime() - pnode->nTimeConnected > 60)
            {
                if (pnode->nLastRecv == 0 || pnode->nLastSend == 0)
                {
                    printf("socket no message in first 60 seconds, %d %d, ip:%s\n",
						pnode->nLastRecv != 0, pnode->nLastSend != 0,
						pnode->addr.ToString().c_str());
                    pnode->fDisconnect = true;
                }
                else if (GetTime() - pnode->nLastSend > 90*60 && GetTime() - pnode->nLastSendEmpty > 90*60)
                {
                    printf("socket not sending, ip:%s\n", pnode->addr.ToString().c_str());
                    pnode->fDisconnect = true;
                }
                else if (GetTime() - pnode->nLastRecv > 90*60)
                {
                    printf("socket inactivity timeout, ip:%s\n", pnode->addr.ToString().c_str());
                    pnode->fDisconnect = true;
                }
            }
        }
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->Release();
        }

        Sleep(10);
    }
}









#ifdef USE_UPNP
void ThreadMapPort(void* parg)
{
    // Make this thread recognisable as the UPnP thread
    RenameThread("bitcoin-UPnP");

    try
    {
        vnThreadsRunning[THREAD_UPNP]++;
        ThreadMapPort2(parg);
        vnThreadsRunning[THREAD_UPNP]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_UPNP]--;
        PrintException(&e, "ThreadMapPort()");
    } catch (...) {
        vnThreadsRunning[THREAD_UPNP]--;
        PrintException(NULL, "ThreadMapPort()");
    }
    printf("ThreadMapPort exited\n");
}

void ThreadMapPort2(void* parg)
{
    printf("ThreadMapPort started\n");

    std::string port = strprintf("%u", GetListenPort());
    const char * multicastif = 0;
    const char * minissdpdpath = 0;
    struct UPNPDev * devlist = 0;
    char lanaddr[64];

#ifndef UPNPDISCOVER_SUCCESS
    /* miniupnpc 1.5 */
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0);
#else
    /* miniupnpc 1.6 */
    int error = 0;
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0, 0, &error);
#endif

    struct UPNPUrls urls;
    struct IGDdatas data;
    int r;

    r = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr));
    if (r == 1)
    {
        if (fDiscover) {
            char externalIPAddress[40];
            r = UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, externalIPAddress);
            if(r != UPNPCOMMAND_SUCCESS)
                printf("UPnP: GetExternalIPAddress() returned %d\n", r);
            else
            {
                if(externalIPAddress[0])
                {
                    printf("UPnP: ExternalIPAddress = %s\n", externalIPAddress);
                    AddLocal(CNetAddr(externalIPAddress), LOCAL_UPNP);
                }
                else
                    printf("UPnP: GetExternalIPAddress failed.\n");
            }
        }

        string strDesc = "Pennies " + FormatFullVersion();
#ifndef UPNPDISCOVER_SUCCESS
        /* miniupnpc 1.5 */
        r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                            port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0);
#else
        /* miniupnpc 1.6 */
        r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                            port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0, "0");
#endif

        if(r!=UPNPCOMMAND_SUCCESS)
            printf("AddPortMapping(%s, %s, %s) failed with code %d (%s)\n",
                port.c_str(), port.c_str(), lanaddr, r, strupnperror(r));
        else
            printf("UPnP Port Mapping successful.\n");
        int i = 1;
        loop {
            if (fShutdown || !fUseUPnP)
            {
                r = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port.c_str(), "TCP", 0);
                printf("UPNP_DeletePortMapping() returned : %d\n", r);
                freeUPNPDevlist(devlist); devlist = 0;
                FreeUPNPUrls(&urls);
                return;
            }
            if (i % 600 == 0) // Refresh every 20 minutes
            {
#ifndef UPNPDISCOVER_SUCCESS
                /* miniupnpc 1.5 */
                r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                    port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0);
#else
                /* miniupnpc 1.6 */
                r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                    port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0, "0");
#endif

                if(r!=UPNPCOMMAND_SUCCESS)
                    printf("AddPortMapping(%s, %s, %s) failed with code %d (%s)\n",
                        port.c_str(), port.c_str(), lanaddr, r, strupnperror(r));
                else
                    printf("UPnP Port Mapping successful.\n");;
            }
            Sleep(2000);
            i++;
        }
    } else {
        printf("No valid UPnP IGDs found\n");
        freeUPNPDevlist(devlist); devlist = 0;
        if (r != 0)
            FreeUPNPUrls(&urls);
        loop {
            if (fShutdown || !fUseUPnP)
                return;
            Sleep(2000);
        }
    }
}

void MapPort()
{
    if (fUseUPnP && vnThreadsRunning[THREAD_UPNP] < 1)
    {
        if (!NewThread(ThreadMapPort, NULL))
            printf("Error: ThreadMapPort(ThreadMapPort) failed\n");
    }
}
#else
void MapPort()
{
    // Intentionally left blank.
    // Intentionally left slightly less blank than the previous line.
}
#endif









// DNS seeds
// Each pair gives a source name and a seed name.
// The first name is used as information source for addrman.
// The second name should resolve to a list of seed addresses.
static const char *strDNSSeed[][9] = {
    {"seed1.cent-pennies.com", "seed2.cent-pennies.com", "seed3.cent-pennies.com", 
	 "seed1.cent-pennies.info", "seed2.cent-pennies.info", "seed3.cent-pennies.info",
	 "seed1.cent-pennies.org", "seed2.cent-pennies.org", "seed3.cent-pennies.org"},    
};

void ThreadDNSAddressSeed(void* parg)
{
    // Make this thread recognisable as the DNS seeding thread
    RenameThread("bitcoin-dnsseed");

    try
    {
        vnThreadsRunning[THREAD_DNSSEED]++;
        ThreadDNSAddressSeed2(parg);
        vnThreadsRunning[THREAD_DNSSEED]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_DNSSEED]--;
        PrintException(&e, "ThreadDNSAddressSeed()");
    } catch (...) {
        vnThreadsRunning[THREAD_DNSSEED]--;
        throw; // support pthread_cancel()
    }
    printf("ThreadDNSAddressSeed exited\n");
}

void ThreadDNSAddressSeed2(void* parg)
{
    printf("ThreadDNSAddressSeed started\n");
    int found = 0;


    if( !fTestNet )
    {
        printf("Loading addresses from DNS seeds (could take a while)\n");

        for( int seed_idx = 0; seed_idx < (int) ARRAYLEN( strDNSSeed ); seed_idx++ ) {
            if (HaveNameProxy()) {
                AddOneShot(strDNSSeed[seed_idx][1]);
            } else {
                vector<CNetAddr> vaddr;
                vector<CAddress> vAdd;
                if (LookupHost(strDNSSeed[seed_idx][1], vaddr))
                {
                    BOOST_FOREACH(CNetAddr& ip, vaddr)
                    {
                        int nOneDay = 24*3600;
                        CAddress addr = CAddress(CService(ip, GetDefaultPort()));
                        addr.nTime = GetTime() - 3*nOneDay - GetRand(4*nOneDay); // use a random age between 3 and 7 days old
                        vAdd.push_back(addr);
                        found++;
                    }
                }
                addrman.Add(vAdd, CNetAddr(strDNSSeed[seed_idx][0], true));
            }
        }
    }
    

    printf("%d addresses found from DNS seeds\n", found);
}












unsigned int pnSeed[] =
{
    //0x90EF78BC, 0x33F1C851, 0x36F1C851, 0xC6F5C851,
	//92.5.98.210
	0xD26205DC,
	//192.95.29.176
	0xB01D5FC0,
	//84.200.27.244
	0xF41BC854,
	//107.170.63.233
	0xE93FAA6B,
    //115.28.242.21
    0x15F21C73,
    //108.216.65.238
    0xEE41D86C,
};

void DumpAddresses()
{
    int64 nStart = GetTimeMillis();

    CAddrDB adb;
    adb.Write(addrman);

    printf("Flushed %d addresses to peers.dat  %"PRI64d"ms\n",
           addrman.size(), GetTimeMillis() - nStart);
}

void ThreadDumpAddress2(void* parg)
{
    vnThreadsRunning[THREAD_DUMPADDRESS]++;
    while (!fShutdown)
    {
        DumpAddresses();
        vnThreadsRunning[THREAD_DUMPADDRESS]--;
        Sleep(100000);
        vnThreadsRunning[THREAD_DUMPADDRESS]++;
    }
    vnThreadsRunning[THREAD_DUMPADDRESS]--;
}

void ThreadDumpAddress(void* parg)
{
    // Make this thread recognisable as the address dumping thread
    RenameThread("bitcoin-adrdump");

    try
    {
        ThreadDumpAddress2(parg);
    }
    catch (std::exception& e) {
        PrintException(&e, "ThreadDumpAddress()");
    }
    printf("ThreadDumpAddress exited\n");
}

void ThreadOpenConnections(void* parg)
{
    // Make this thread recognisable as the connection opening thread
    RenameThread("bitcoin-opencon");

    try
    {
        vnThreadsRunning[THREAD_OPENCONNECTIONS]++;
        ThreadOpenConnections2(parg);
        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
        PrintException(&e, "ThreadOpenConnections()");
    } catch (...) {
        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
        PrintException(NULL, "ThreadOpenConnections()");
    }
    printf("ThreadOpenConnections exited\n");
}

void static ProcessOneShot()
{
    string strDest;
    {
        LOCK(cs_vOneShots);
        if (vOneShots.empty())
            return;
        strDest = vOneShots.front();
        vOneShots.pop_front();
    }
    CAddress addr;
    CSemaphoreGrant grant(*semOutbound, true);
    if (grant) {
        if (!OpenNetworkConnection(addr, &grant, strDest.c_str(), true))
            AddOneShot(strDest);
    }
}

// ppcoin: stake minter thread
void static ThreadStakeMinter(void* parg)
{
    printf("ThreadStakeMinter started\n");
    CWallet* pwallet = (CWallet*)parg;
    try
    {
        vnThreadsRunning[THREAD_MINTER]++;
        BitcoinMiner(pwallet, true);
        vnThreadsRunning[THREAD_MINTER]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_MINTER]--;
        PrintException(&e, "ThreadStakeMinter()");
    } catch (...) {
        vnThreadsRunning[THREAD_MINTER]--;
        PrintException(NULL, "ThreadStakeMinter()");
    }
    printf("ThreadStakeMinter exiting, %d threads remaining\n", vnThreadsRunning[THREAD_MINTER]);
}

void ThreadOpenConnections2(void* parg)
{
    printf("ThreadOpenConnections started\n");

    // Connect to specific addresses
    if (mapArgs.count("-connect") && mapMultiArgs["-connect"].size() > 0)
    {
        for (int64 nLoop = 0;; nLoop++)
        {
            ProcessOneShot();
            BOOST_FOREACH(string strAddr, mapMultiArgs["-connect"])
            {
                CAddress addr;
                OpenNetworkConnection(addr, NULL, strAddr.c_str());
                for (int i = 0; i < 10 && i < nLoop; i++)
                {
                    Sleep(500);
                    if (fShutdown)
                        return;
                }
            }
            Sleep(500);
        }
    }

    // Initiate network connections
    int64 nStart = GetTime();
    loop
    {
        ProcessOneShot();

        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
        Sleep(500);
        vnThreadsRunning[THREAD_OPENCONNECTIONS]++;
        if (fShutdown)
            return;


        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
        CSemaphoreGrant grant(*semOutbound);
        vnThreadsRunning[THREAD_OPENCONNECTIONS]++;
        if (fShutdown)
            return;

        // Add seed nodes if IRC isn't working
        if (addrman.size()==0 && (GetTime() - nStart > 60) && !fTestNet)
        {
            std::vector<CAddress> vAdd;
            for (unsigned int i = 0; i < ARRAYLEN(pnSeed); i++)
            {
                // It'll only connect to one or two seed nodes because once it connects,
                // it'll get a pile of addresses with newer timestamps.
                // Seed nodes are given a random 'last seen time' of between one and two
                // weeks ago.
                const int64 nOneWeek = 7*24*60*60;
                struct in_addr ip;
                memcpy(&ip, &pnSeed[i], sizeof(ip));
                CAddress addr(CService(ip, GetDefaultPort()));
                addr.nTime = GetTime()-GetRand(nOneWeek)-nOneWeek;
                vAdd.push_back(addr);
            }
            addrman.Add(vAdd, CNetAddr("127.0.0.1"));
        }

        //
        // Choose an address to connect to based on most recently seen
        //
        CAddress addrConnect;

        // Only connect out to one peer per network group (/16 for IPv4).
        // Do this here so we don't have to critsect vNodes inside mapAddresses critsect.
        int nOutbound = 0;
        set<vector<unsigned char> > setConnected;
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes) {
                if (!pnode->fInbound) {
                    setConnected.insert(pnode->addr.GetGroup());
                    nOutbound++;
                }
            }
        }

        int64 nANow = GetAdjustedTime();

        int nTries = 0;
        loop
        {
            // use an nUnkBias between 10 (no outgoing connections) and 90 (8 outgoing connections)
            CAddress addr = addrman.Select(10 + min(nOutbound,8)*10);

            // if we selected an invalid address, restart
            if (!addr.IsValid() || setConnected.count(addr.GetGroup()) || IsLocal(addr))
                break;

            // If we didn't find an appropriate destination after trying 100 addresses fetched from addrman,
            // stop this loop, and let the outer loop run again (which sleeps, adds seed nodes, recalculates
            // already-connected network ranges, ...) before trying new addrman addresses.
            nTries++;
            if (nTries > 100)
                break;

            if (IsLimited(addr))
                continue;

            // only consider very recently tried nodes after 30 failed attempts
            if (nANow - addr.nLastTry < 600 && nTries < 30)
                continue;

            // do not allow non-default ports, unless after 50 invalid addresses selected already
            if (addr.GetPort() != GetDefaultPort() && nTries < 50)
                continue;

            addrConnect = addr;
            break;
        }

        if (addrConnect.IsValid())
            OpenNetworkConnection(addrConnect, &grant);
    }
}

void ThreadOpenAddedConnections(void* parg)
{
    // Make this thread recognisable as the connection opening thread
    RenameThread("bitcoin-opencon");

    try
    {
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]++;
        ThreadOpenAddedConnections2(parg);
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
        PrintException(&e, "ThreadOpenAddedConnections()");
    } catch (...) {
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
        PrintException(NULL, "ThreadOpenAddedConnections()");
    }
    printf("ThreadOpenAddedConnections exited\n");
}

void ThreadOpenAddedConnections2(void* parg)
{
    printf("ThreadOpenAddedConnections started\n");

    if (mapArgs.count("-addnode") == 0)
        return;

    if (HaveNameProxy()) {
        while(!fShutdown) {
            BOOST_FOREACH(string& strAddNode, mapMultiArgs["-addnode"]) {
                CAddress addr;
                CSemaphoreGrant grant(*semOutbound);
                OpenNetworkConnection(addr, &grant, strAddNode.c_str());
                Sleep(500);
            }
            vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
            Sleep(120000); // Retry every 2 minutes
            vnThreadsRunning[THREAD_ADDEDCONNECTIONS]++;
        }
        return;
    }

    vector<vector<CService> > vservAddressesToAdd(0);
    BOOST_FOREACH(string& strAddNode, mapMultiArgs["-addnode"])
    {
        vector<CService> vservNode(0);
        if(Lookup(strAddNode.c_str(), vservNode, GetDefaultPort(), fNameLookup, 0))
        {
            vservAddressesToAdd.push_back(vservNode);
            {
                LOCK(cs_setservAddNodeAddresses);
                BOOST_FOREACH(CService& serv, vservNode)
                    setservAddNodeAddresses.insert(serv);
            }
        }
    }
    loop
    {
        vector<vector<CService> > vservConnectAddresses = vservAddressesToAdd;
        // Attempt to connect to each IP for each addnode entry until at least one is successful per addnode entry
        // (keeping in mind that addnode entries can have many IPs if fNameLookup)
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes)
                for (vector<vector<CService> >::iterator it = vservConnectAddresses.begin(); it != vservConnectAddresses.end(); it++)
                    BOOST_FOREACH(CService& addrNode, *(it))
                        if (pnode->addr == addrNode)
                        {
                            it = vservConnectAddresses.erase(it);
                            it--;
                            break;
                        }
        }
        BOOST_FOREACH(vector<CService>& vserv, vservConnectAddresses)
        {
            CSemaphoreGrant grant(*semOutbound);
            OpenNetworkConnection(CAddress(*(vserv.begin())), &grant);
            Sleep(500);
            if (fShutdown)
                return;
        }
        if (fShutdown)
            return;
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
        Sleep(120000); // Retry every 2 minutes
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]++;
        if (fShutdown)
            return;
    }
}

// if successful, this moves the passed grant to the constructed node
bool OpenNetworkConnection(const CAddress& addrConnect, CSemaphoreGrant *grantOutbound, const char *strDest, bool fOneShot)
{
    //
    // Initiate outbound network connection
    //
    if (fShutdown)
    {
    	printf("shutdown when OpenNetWorkConnection.\n");
        return false;
    }	
    if (!strDest)
        if (IsLocal(addrConnect) ||
            FindNode((CNetAddr)addrConnect) || CNode::IsBanned(addrConnect) ||
            FindNode(addrConnect.ToStringIPPort().c_str()))
    {
    		printf("fail to OpenNetWorkConnection, addr is null.\n");
            return false;
    }
    if (strDest && FindNode(strDest))
    {
    	printf("fail to OpenNetWorkConnection, addr:%s is already connected.\n", strDest);
        return false;
    }

    vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
    CNode* pnode = ConnectNode(addrConnect, strDest);
    vnThreadsRunning[THREAD_OPENCONNECTIONS]++;
    if (fShutdown)
    {
    	printf("shutdown when OpenNetWorkConnection, after connectnode.\n");
        return false;
    }
    if (!pnode)
    {
		printf("fail to connectnode:%s in OpenNetWorkConnection.\n", strDest ? strDest : addrConnect.ToString().c_str());
        return false;
    }
    if (grantOutbound)
        grantOutbound->MoveTo(pnode->grantOutbound);
    pnode->fNetworkNode = true;
    if (fOneShot)
        pnode->fOneShot = true;

    return true;
}



CNode * getNodeSync(){
	return pnodeSync;
}

// for now, use a very simple selection metric: the node from which we received
// most recently
double static NodeSyncScore(const CNode *pnode) {
    //return -pnode->nLastRecv;
    return pnode->nSpeed;
}

void static StartSync(const list<CNode*> &vNodes) {
    CNode *pnodeNewSync = NULL;
    double dBestScore = 0;

    // Iterate over all nodes
    BOOST_FOREACH(CNode* pnode, vNodes) {
        // check preconditions for allowing a sync
        if (!pnode->fClient && !pnode->fOneShot &&
            !pnode->fDisconnect && pnode->fSuccessfullyConnected &&
            (pnode->nStartingHeight > (nBestHeight - 144)) &&
            (pnode->nVersion < NOBLKS_VERSION_START || pnode->nVersion >= NOBLKS_VERSION_END)) {

			//first select unused node, maybe it's quicker to download
			if(!pnode->bUsed)
			{
				pnodeNewSync = pnode;
				break;
			}
            // if ok, compare node's score with the best so far
            double dScore = NodeSyncScore(pnode);
            if (pnodeNewSync == NULL || dScore > dBestScore) {
                pnodeNewSync = pnode;
                dBestScore = dScore;
            }
        }
    }
    // if a new sync candidate was found, start sync!
    if (pnodeNewSync) {
        pnodeNewSync->fStartSync = true;
        pnodeSync = pnodeNewSync;
		pnodeNewSync->nSyncTime = GetTime();
		pnodeNewSync->nSyncHeight = nBestHeight;
		printf("set syncnode:%s, lastspeed:%d, synctime:%"PRI64d", used:%d\n", 
			pnodeSync->addr.ToString().c_str(), pnodeSync->nSpeed, pnodeSync->nSyncTime, 
			pnodeSync->bUsed);
		pnodeNewSync->bUsed = true;
    }
}

//util
bool compare_speed(CNode* pNode1, CNode* pNode2)
{
	if(!pNode1->bUsed)
		return true;

	if(!pNode2->bUsed)
		return false;
	
	return pNode1->nSpeed > pNode2->nSpeed;
}

bool compare_headerspeed(CNode* pNode1, CNode* pNode2)
{
	if(!pNode1->bHeaderUsed)
		return true;

	if(!pNode2->bHeaderUsed)
		return false;
	
	return pNode1->nHeaderSpeed > pNode2->nHeaderSpeed;
}

#define JUMP_TO_NEXTSLOT  \
	bJumpNext = true;	  \
	nSlot++;			  \
	if(nSlot >= nMaxSlot) \
	{					  \
		break;			  \
	}					  \
						  \
	continue;			  

void syncHeaders(int nMaxSlot, list<CNode*> vNodesToSync)
{
	bool bJumpNext = false;
	static int64 nHeaderPollTime = 0;
	int64 nNow = GetAdjustedTime();

	if((nNow - nHeaderPollTime) < nHeaderConcurrentPollTime && nHeaderPollTime != 0)
	{
		return;
	}

	//printf("concurrent sync start..., lastchecktime:%"PRI64d", now:%"PRI64d"\n", nPollTime, nNow);

	nHeaderPollTime = nNow;


	//stage 2: sorted by speed
	vNodesToSync.sort(compare_headerspeed);


	//printf("concurrent sync start..., lastchecktime:%"PRI64d", now:%"PRI64d"\n", nPollTime, nNow);

	//restrict the concurrent
	int nNodeSize = 0;


	int nSlot = 0;
    BOOST_FOREACH(CNode* pnode, vNodesToSync) {
		//stage 3:download headers if no finished
		loop
		{
			bJumpNext = false;
			SyncPoint* pSyncHeadersPoint = vSyncHeadersPoints[nSlot];
			//check finished
			if(pSyncHeadersPoint->startHeight >= pnode->nStartingHeight)
			{
				JUMP_TO_NEXTSLOT
			}
			//checkdownloaded
			if(pSyncHeadersPoint->startHeight < pSyncHeadersPoint->endHeight || 0 == pSyncHeadersPoint->endHeight)
			{
				loop
				{
					int nHeight = pSyncHeadersPoint->startHeight + 1;
					map<int, uint256>::const_iterator begin = mapSyncHeight2Hash.find(nHeight);
					if(begin == mapSyncHeight2Hash.end())
					{
						//next height not found
						break;
					}
					else
					{
						pSyncHeadersPoint->startHeight++;
						if(pSyncHeadersPoint->startHeight >= pSyncHeadersPoint->endHeight 
							&& 0 != pSyncHeadersPoint->endHeight)
						{
							break;
						}
					}
				}
			}
			
			//if current slot finished, jump to next slot
			if(pSyncHeadersPoint->startHeight == pSyncHeadersPoint->endHeight)
			{
				JUMP_TO_NEXTSLOT
			}


			if(pSyncHeadersPoint->startHeight < pSyncHeadersPoint->endHeight || 0 == pSyncHeadersPoint->endHeight)
			{
				map<int, uint256>::const_iterator begin = mapSyncHeight2Hash.find(pSyncHeadersPoint->startHeight);
	        	if (begin == mapSyncHeight2Hash.end())
	        	{
	        		//assert
	        		printf("slot:%d, ip:%s, getheaders interrupt at %d, error(that is impossible).\n", nSlot, pnode->addr.ToString().c_str(), 
	        			pSyncHeadersPoint->startHeight);
	        		continue;
	        	}
				uint256 hashBegin = begin->second;


				uint256 hashEnd = uint256(0);

				if(0 != pSyncHeadersPoint->endHeight)
				{
					map<int, uint256>::const_iterator end = mapSyncHeight2Hash.find(pSyncHeadersPoint->endHeight);
		        	if (end == mapSyncHeight2Hash.end())
		        	{
		        		//assert
		        	}
					hashEnd = end->second;
				}

				bool bSend = true;
				int64 nNow = GetAdjustedTime();
				if(hashBegin == pnode->getHeadersHashBegin && hashEnd == pnode->getHeadersHashEnd)
				{
					
					if((nNow - pnode->nSendGetHeadersTime) < nConcurrentRetry)
					{
						bSend = false;
					}
				}

				if(bSend)
				{
					printf("slot:%d, ip:%s, send getheaders, start:%d, end:%d, hashbegin:%s, hashend:%s, speed:%d\n",
						nSlot, pnode->addr.ToString().c_str(), 
						pSyncHeadersPoint->startHeight,
						pSyncHeadersPoint->endHeight,
						hashBegin.ToString().c_str(), hashEnd.ToString().c_str(),
						pnode->nSpeed);
					std::vector<uint256> vhash;
					vhash.push_back(hashBegin);
					pnode->PushMessage("getheaders", CBlockLocator(vhash), hashEnd);
					pnode->nSendGetHeadersTime = nNow;
					pnode->getHeadersHashBegin = hashBegin;
					pnode->getHeadersHashEnd = hashEnd;
					pnode->bHeaderUsed = true;
				}
			}
			
			break;
		}

		if(bJumpNext)
		{
			if(nSlot >= nMaxSlot)
				break;

			continue;
		}

		nNodeSize++;
		if(nNodeSize >= nHeaderConcurrent)
			break;
		
		nSlot++;
		if(nSlot >= nMaxSlot)
			break;

    }

}

void syncBlocks(int nMaxSlot, list<CNode*> vNodesToSync)
{
	bool bJumpNext = false;
	int nMaxBlocksOnce = 1000;
	static int64 nPollTime = 0;
	int64 nNow = GetAdjustedTime();


	if((nNow - nPollTime) < nConcurrentPollTime && nPollTime != 0)
	{
		return;
	}

	nPollTime = nNow;

	//stage 2: sorted by speed
	vNodesToSync.sort(compare_speed);

	int nSlot = 0;
	int nNodeSize = 0;
	//two loop, node loop, slot loop, each node process one slot
	//three cases:
	//1. nodes > slots
	//2. nodes == slots
	//3. nodes < slots
	BOOST_FOREACH(CNode* pnode, vNodesToSync) {
		//stage 4:download blocks
		//need to gethash
		loop
		{
			bJumpNext = false;
			SyncPoint* pSyncBlocksPoint = vSyncBlocksPoints[nSlot];
			
			//stage 4.1:check downloaded
			//four pointer: start, end, startwithhash, endwithhash
			//askfor:startwithhash-endwithhash
			bool fHashNotExist = false;
			bool fNeedToAskFor = false;
			bool fFinished = false;
			loop
			{
				if(pSyncBlocksPoint->startHeight >= pSyncBlocksPoint->endHeight && (0 != pSyncBlocksPoint->endHeight))
				{
					fFinished = true;
					//if(fDebug)
					//	printf("slot:%d, ip:%s, finished(start>=endheight), startheight:%d, endheight:%d, total:%d\n", 
					//			nSlot, pnode->addr.ToString().c_str(),  pSyncBlocksPoint->startHeight,
					//			pSyncBlocksPoint->endHeight, pnode->nStartingHeight);
					break;//finished
				}

				if(pSyncBlocksPoint->startHeight >= pnode->nStartingHeight)
				{
					fFinished = true;
					if(fDebug)
						printf("slot:%d, ip:%s, finished(start>=total), startheight:%d, endheight:%d, total:%d\n", 
								nSlot, pnode->addr.ToString().c_str(),  pSyncBlocksPoint->startHeight,
								pSyncBlocksPoint->endHeight, pnode->nStartingHeight);
					break;
				}
				
				//check header ready, if not ready, ignore it until headers downloaded			
				map<int, uint256>::const_iterator begin = mapSyncHeight2Hash.find(pSyncBlocksPoint->startHeight);
				if(begin == mapSyncHeight2Hash.end())
				{
					fHashNotExist = true;
					//if(fDebug)
					//	printf("slot:%d, ip:%s, hash not exist, startheight:%d, endheight:%d, total:%d\n", 
					//			nSlot, pnode->addr.ToString().c_str(),  pSyncBlocksPoint->startHeight,
					//			pSyncBlocksPoint->endHeight, pnode->nStartingHeight);
					break;//not ready
				}

				uint256 uHash = begin->second;

				if(mapBlockIndex.count(uHash)
					|| mapOrphanBlocks.count(uHash))
				{
					pSyncBlocksPoint->startHeight++; //downloaded
				}
				else
				{
					//not yet downloaded, ask for
					//now startwithhash = startHeight
					fNeedToAskFor = true;
					//if(fDebug)
					//	printf("slot:%d, ip:%s, need to askfor, startheight:%d, endheight:%d, total:%d\n", 
					//			nSlot, pnode->addr.ToString().c_str(),  pSyncBlocksPoint->startHeight,
					//			pSyncBlocksPoint->endHeight, pnode->nStartingHeight);
					break;
				}

			}

			//now two case:
			//1. hash correspond to startHeight does not exist in mapHeight2Hash;
			//2. hash exists in map, but block does not exist 

			//stage 4.2:check finished
			if(fHashNotExist)
			{
				JUMP_TO_NEXTSLOT
			}
			
			//nStartingHeight is the max height of the node
			if(fFinished)
			{
				JUMP_TO_NEXTSLOT
			}

			//now need to ask for
			if(!fNeedToAskFor)
			{
				printf("assert failed! impossible branch when ready to askfor block, height:%d\n", pSyncBlocksPoint->startHeight);
				JUMP_TO_NEXTSLOT
			}
			
			//stage 4.3:check ask for
			//bool fSearchEndWithHash = false;
			//bool fMaxStop = false;
			int nHeight = pSyncBlocksPoint->startHeight;
			vector<uint256> vAskforHash;
			int nCount = 0;
			loop
			{
				if(nHeight > pSyncBlocksPoint->endHeight && 0 != pSyncBlocksPoint->endHeight)
				{
					//fMaxStop = true;
					break;
				}
				
				if(nHeight > pnode->nStartingHeight)
				{
					//fMaxStop = true;
					break;
				}
				//check header ready, if not ready, ignore it until headers downloaded			
				map<int, uint256>::const_iterator begin = mapSyncHeight2Hash.find(nHeight);
				if(begin == mapSyncHeight2Hash.end())
				{
					//fSearchEndWithHash = true;
					break;//not ready
				}

				uint256 uHash = begin->second;

				if(mapBlockIndex.count(uHash)
					|| mapOrphanBlocks.count(uHash))
				{
					//pSyncBlocksPoint->startHeight++; //downloaded
					//printf("assert failed!impossible branch, logic error when processConcurrentSync, height:%d.\n", nHeight);
					//break;
					nHeight++;
					continue; //ignore it, retry to ask for next block
				}
				else
				{
					//not yet downloaded, ask for
					//pnode->AskFor(CInv(MSG_BLOCK, uHash));
					vAskforHash.push_back(uHash);
					nCount++;
					if(nCount >= nMaxBlocksOnce)
					{
						//fMaxStop = true;
						break;
					}
				}
				nHeight++;
			}

			//now four cases:
			//1. fSearchEndWithHash, nHeight = startHeight, impossible branch, because this case has been rejected by 4.2
			//2. fSearchEndWithHash, nHeight > startHeight, need to ask for
			//3. fMaxStop, nHeight = startHeight, only one (nHeight - startHeight + 1) needed to askfor
			//4. fMaxStop, nHeight > startHeight, nHeight - startHeight + 1 needed to ask for

			//now, startHeight to nHeight is intended to ask for
			//stage4.4:check header ready, if startHeight is not ready, it means all block headers not ready,
			//need to wait for header downloaded, so ignore it 			
			//
			
			if(nCount > 0)
			{

				uint256 hashBegin = vAskforHash[0];


				uint256 hashEnd = vAskforHash[nCount - 1];

				bool bSend = true;
				int64 nNow = GetAdjustedTime();
				if(nNow - pnode->nSendGetDataTime < nConcurrentPollTime)
				{
					bSend = false;
				}
				
				if(hashBegin == pnode->getDataHashBegin && hashEnd == pnode->getDataHashEnd)
				{
					
					if((nNow - pnode->nSendGetDataTime) < nConcurrentRetry)
					{
						bSend = false;
					}
				}

				if(bSend)
				{
					printf("slot:%d, ip:%s, send getblocks, start:%d, end:%d, count:%d, hashbegin:%s, hashend:%s\n",
						nSlot, pnode->addr.ToString().c_str(), 
						pSyncBlocksPoint->startHeight,
						nHeight,
						nCount,
						hashBegin.ToString().c_str(), hashEnd.ToString().c_str());
					//std::vector<uint256> vhash;
					//vhash.push_back(hashBegin);
					//pnode->PushMessage("getheaders", CBlockLocator(vhash), hashEnd);
					for(int i = 0; i < nCount; i++)
					{
						uint256 uHash = vAskforHash[i];
						pnode->AskFor(CInv(MSG_BLOCK, uHash));
					}
					pnode->nSendGetDataTime = nNow;
					pnode->getDataHashBegin = hashBegin;
					pnode->getDataHashEnd = hashEnd;
					pnode->bUsed = true;
				}
			}
			
			break;
		}

		if(bJumpNext)
		{
			if(nSlot >= nMaxSlot)
				break;

			continue;
		}
		
		nNodeSize++;
		if(nNodeSize >= nConcurrent)
			break;

		nSlot++;
		if(nSlot >= nMaxSlot)
			break;


    }

}

void processConcurrentSync()
{

	static int64 nCheckIPTime = 0;
	int64 nNow = GetAdjustedTime();
	/*static int64 nHeaderPollTime = 0;
	static int64 nPollTime = 0;
	

	if((nNow - nHeaderPollTime) < nHeaderConcurrentPollTime && nHeaderPollTime != 0)
	{
		return;
	}

	//printf("concurrent sync start..., lastchecktime:%"PRI64d", now:%"PRI64d"\n", nPollTime, nNow);

	nHeaderPollTime = nNow;*/

	if(nCheckIPTime == 0)
	{
		nCheckIPTime = nNow;
	}
	
	//initial context
	static int nMaxSlot = 0;
	if(0 == vSyncHeadersPoints.size())
	{
		nMaxSlot = 0;
		SyncPoint* pPrevSyncHeaderPoint = NULL;
		SyncPoint* pPrevSyncBlockPoint = NULL;
		//int nSize = mapHardenSyncPoints.size();
		map<int,uint256>::iterator it;
        for(it=mapHardenSyncPoints.begin();it!=mapHardenSyncPoints.end();++it)
		{
			int nStart = it->first;
			//int nEnd = mapHardenSyncPoints.get(i+1).first;
			//uint
			if(NULL != pPrevSyncHeaderPoint)
			{
				pPrevSyncHeaderPoint->endHeight = nStart - 1;
				pPrevSyncBlockPoint->endHeight = nStart - 1;
			}
			SyncPoint* pSyncHeaderPoint = new SyncPoint(nStart, 0);
			SyncPoint* pSyncBlockPoint = new SyncPoint(nStart, 0);
			vSyncHeadersPoints.push_back(pSyncHeaderPoint);
			vSyncBlocksPoints.push_back(pSyncBlockPoint);
			mapSyncHeight2Hash.insert(std::pair<int, uint256>(it->first, it->second));
			mapSyncHash2Height.insert(std::pair<uint256, int>(it->second, it->first));
			nMaxSlot++;
			pPrevSyncHeaderPoint = pSyncHeaderPoint;
			pPrevSyncBlockPoint  = pSyncBlockPoint;
		}
	}
	//dynamical const
	int nMaxBlockCount = 400000;
	vector<CNode*> vNodesCopy;
	
	LOCK(cs_vNodes);
	vNodesCopy = vNodes;
	//stage 1:check best connections:unused or best speed
	list<CNode*> vNodesToSync;
    BOOST_FOREACH(CNode* pnode, vNodesCopy) {
        // check preconditions for allowing a sync
        if (!pnode->fClient && //!pnode->fOneShot &&
            !pnode->fDisconnect && pnode->fSuccessfullyConnected &&
            (pnode->nStartingHeight > (nMaxBlockCount - 144)) &&
            (pnode->nVersion < NOBLKS_VERSION_START || pnode->nVersion >= NOBLKS_VERSION_END)) {

			if(pnode->nVersion >= 70002)//getheaders
			{
				if((nNow - pnode->nCheckSpeedTime) > 60)
				{
					pnode->nCheckSpeedTime = nNow;
					pnode->nSpeed = ((pnode->nDownloaded / 60) + pnode->nSpeed) / 2;
					pnode->nHeaderSpeed = ((pnode->nHeaderDownloaded / 60) + pnode->nHeaderSpeed) / 2;
					printf("concurrent sync, ip:%s, speed:%d, downloaded:%d\n", pnode->addr.ToString().c_str(), pnode->nSpeed,
						pnode->nDownloaded);
					pnode->nDownloaded = 0;
					pnode->nHeaderDownloaded = 0;
				}
				vNodesToSync.push_back(pnode);
			}
        }
    }



	//return;
	syncHeaders(nMaxSlot, vNodesToSync);
	syncBlocks(nMaxSlot, vNodesToSync);
	
	if((nNow - nCheckIPTime) > 60)
	{

	//check chain
	if(NULL != pindexBest)
	{
		int nNextHeight = pindexBest->nHeight + 1;
		map<int, uint256>::const_iterator begin = mapSyncHeight2Hash.find(nNextHeight);
		while(begin != mapSyncHeight2Hash.end())
		{
			uint256 uHash = begin->second;

				if(mapBlockIndex.count(uHash))
				{
					CTxDB txdb;
            		CBlockIndex* pindexCheckpoint = mapBlockIndex[uHash];
                	CBlock block;
                	if (!block.ReadFromDisk(pindexCheckpoint))
                    {
                    	printf("add block: ReadFromDisk failed for hash %s, height:%d\n", uHash.ToString().c_str(), nNextHeight);
						break;
					}
               		if (!block.SetBestChain(txdb, pindexCheckpoint))
                	{
						printf("add block: set bestchain failed for hash %s, height:%d\n", uHash.ToString().c_str(), nNextHeight);
						break;
                	}
            		txdb.Close();
					nNextHeight++;
					begin = mapSyncHeight2Hash.find(nNextHeight);
					continue;
				}
				
				if(mapOrphanBlocks.count(uHash))
				{
					 CBlock* pblockOrphan = mapOrphanBlocks[uHash];
					 printf("add block, hash:%s, height:%d\n", uHash.ToString().c_str(), nNextHeight);
					 if(!pblockOrphan->AcceptBlock())
					 {
					 	break;
					 }
					 mapOrphanBlocks.erase(pblockOrphan->GetHash());
					 setStakeSeenOrphan.erase(pblockOrphan->GetProofOfStake());
					 delete pblockOrphan;
				}

			nNextHeight++;
			begin = mapSyncHeight2Hash.find(nNextHeight);
		}
	}

	

		int i = 0;
		printf("concurrent sync begin-------------------------------------------------------\n");//for grep
		BOOST_FOREACH(CNode* pnode, vNodesToSync) {
			printf("concurrent sync, ip index:%d, ip:%s, speed:%d, bused:%d\n", i, pnode->addr.ToString().c_str(),
				pnode->nSpeed, pnode->bUsed);
			i++;
		}
		nCheckIPTime = nNow;


		printf("concurrent sync end+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n");//for grep

		for(i = 0; i < nMaxSlot; i++)
		{
			SyncPoint* pSyncHeadersPoint = vSyncHeadersPoints[i];
			printf("concurrent sync, header index:%d, start:%d, end:%d\n", i, pSyncHeadersPoint->startHeight,
				pSyncHeadersPoint->endHeight);
		}

		printf("concurrent sync end**********************************************************\n");//for grep

		for(i = 0; i < nMaxSlot; i++)
		{
			SyncPoint* pSyncHeadersPoint = vSyncBlocksPoints[i];
			printf("concurrent sync, block index:%d, start:%d, end:%d\n", i, pSyncHeadersPoint->startHeight,
				pSyncHeadersPoint->endHeight);
		}

		printf("concurrent sync end-------------------------------------------------------\n");//for grep
	}



	
	//printf("concurrent sync end..., lastchecktime:%"PRI64d", now:%"PRI64d"\n", nPollTime, nNow);
	
}

void processSync()
{
	vector<CNode*> vNodesCopy;
	
	LOCK(cs_vNodes);
	vNodesCopy = vNodes;
	//clear msg for resetting node
	BOOST_FOREACH(CNode* pnode, vNodesCopy)
	{
		//clear msg which is reseting node
		//
		if(RESET_WAITING_FOR_CLEAR_MSG == pnode->nReset)
		{
			printf("reset: clear msg, node:%s\n", pnode->addrName.c_str());
			{
				TRY_LOCK(pnode->cs_inventory, lockInventory);
				if (lockInventory)
				{
					pnode->vInventoryToSend.clear();
					pnode->setInventoryKnown.clear();
					pnode->mapAskFor.clear();
					pnode->hashContinue = 0;
					pnode->pindexLastGetBlocksBegin = 0;
					pnode->hashLastGetBlocksEnd = 0;
				}
			}
		
			pnode->nReset = RESET_WAITING_FOR_CONNECTED;
		}
	}

	//select valid node to sync	
    // Iterate over all nodes
    
	list<CNode*> vNodesToSync;
    BOOST_FOREACH(CNode* pnode, vNodesCopy) {
        // check preconditions for allowing a sync
        if (!pnode->fClient && !pnode->fOneShot &&
            !pnode->fDisconnect && pnode->fSuccessfullyConnected &&
            (pnode->nStartingHeight > (nBestHeight - 144)) &&
            (pnode->nVersion < NOBLKS_VERSION_START || pnode->nVersion >= NOBLKS_VERSION_END)) {

			vNodesToSync.push_back(pnode);
        }
    }
	
	//stage 0: there is no connected node, just return
	if(vNodesToSync.size() <= 0)
	{
		return;
	}
	
	//stage 1:there is connected nodes and nodesync is null, select one to be nodesync
	if(NULL == pnodeSync)
	{
		printf("sync stage 1: nodesync is null, re-select\n");
		StartSync(vNodesToSync);
		return;
	}

	//stage 2:check valid
	//nodesync exists, but state is incorrect, re-select
	if(pnodeSync->fDisconnect)
	{
		printf("sync stage 2: check, ip:%s disconnected\n", pnodeSync->addr.ToString().c_str());
		pnodeSync = NULL;
		StartSync(vNodesToSync);
		return;
	}

	
	//check whether there are some logical error, if pnodesync is connected, the state of nReset must be RESET_IDLE
	if(!pnodeSync->fDisconnect && RESET_IDLE != pnodeSync->nReset)
	{
		printf("sync stage 2: check state error, nodesync :%s disconnected, but the state is:%d\n", 
			pnodeSync->addr.ToString().c_str(), pnodeSync->nReset);
		pnodeSync = NULL;
		StartSync(vNodesToSync);
		return;
	}

	//pnodesync exists, but is not in connected node
	bool fSyncConnected = false;
	BOOST_FOREACH(CNode* pnode, vNodesCopy)
	{
		if(pnode == pnodeSync)
		{
			fSyncConnected = true;
		}
	}

	if(!fSyncConnected)
	{
		printf("sync stage 2: check ip:%s, nodesync exists, but disconnected\n", pnodeSync->addr.ToString().c_str());
		pnodeSync = NULL;
		StartSync(vNodesToSync);
		return;
	}


	//stage  3:check speed
	//check speed
	if( pnodeSync->nSyncLastCheckTime > 0)
	{
		int uDifTime = GetTime() - pnodeSync->nSyncLastCheckTime;
		int uDifHeight = nBestHeight - pnodeSync->nSyncLastHeight;
		if(uDifTime > nSyncTimer)//it's time to check
		{
			//update speed
			pnodeSync->nSpeed = (pnodeSync->nSpeed + (uDifHeight / uDifTime)) / 2;

			//stage 3.2.1
			if(pnodeSync->nSpeed <= 0)
			{
				pnodeSync->fDisconnect = true;
				printf("sync stage 3.2.1.1:timeout, disconnect node:%s\n", pnodeSync->addr.ToString().c_str());
				pnodeSync = NULL;
				StartSync(vNodesToSync);
				return;
			}
			
			if(uDifHeight <= nSyncThreshold)//at least sync 3000 blocks in one minute
			{
			
					//check other node
					int nMaxSpeed = 0;
					bool fHasBetter = false;
					CNode* pNodeWithMaxSpeed = NULL;
					
					list<CNode*> vNodesToSyncCopy = vNodesToSync;
					BOOST_FOREACH(CNode* pnode, vNodesToSyncCopy)
					{
					
						if(!pnode->bUsed)
						{
							printf("sync stage 3.2.2.1: there is unused ip %s to select\n", pnode->addr.ToString().c_str());
							fHasBetter = true;
							break;
						}
						
						if(pnode->nSpeed > nMaxSpeed)
						{
							pNodeWithMaxSpeed = pnode;
							nMaxSpeed = pnode->nSpeed;
						}
					
					}//FOREACH
			
					int nCurrentSpeed = pnodeSync->nSpeed;
					if(nCurrentSpeed < nMaxSpeed)
					{
						if(pNodeWithMaxSpeed)
						{
							printf("sync stage 3.2.2.2:there is better bandwidth ip:%s, speed:%d\n", 
								pNodeWithMaxSpeed->addr.ToString().c_str(), nMaxSpeed);
							fHasBetter = true;
						}
					}

					if(fHasBetter)
					{
							
							//disconnect pending node, reconnect other node to resume download
							printf("sync stage 3.2.2.3:sync timeout, reset syncnode, last syncnode:%s, diftime:%d, last sync count:%d, syncthreshold:%d, speed:%d, maxspeed:%d\n", 
								pnodeSync->addr.ToString().c_str(), uDifTime, uDifHeight, nSyncThreshold, pnodeSync->nSpeed, nMaxSpeed);

							pnodeSync->fStartSync = false;
							pnodeSync->nSyncTime = 0;
							pnodeSync->nSyncHeight = 0;
							pnodeSync->nSyncLastCheckTime = 0;
							pnodeSync->nSyncLastHeight = 0;
							
							//SaveToSyncMap(pnodeSync);
							//pnodeLastSync = pnodeSync;
							//reset
							pnodeSync->nReset = RESET_WAITING_FOR_DISCONNECT;

							pnodeSync = NULL;
							StartSync(vNodesToSync);
							return;
					}
					else
					{
						//stage 3.2.2.4 no better node, continue to sync
					}

			}
			else
			{
						//stage 3.2.1.3 goode node, continue to sync
			}

			
			printf("sync stage 3.2.2.4:sync timeout, continue to sync, last syncnode:%s, diftime:%d, last sync count:%d, syncthreshold:%d, speed:%d\n", 
				pnodeSync->addr.ToString().c_str(), uDifTime, uDifHeight, nSyncThreshold, pnodeSync->nSpeed);
			//record current height to check whether node is in download pending later
			pnodeSync->nSyncHeight = nBestHeight;
			pnodeSync->nSyncLastCheckTime = GetTime();//pnodeSync->nSyncTime;
			pnodeSync->nSyncLastHeight = nBestHeight;							
			
	    }
		else
		{
					 //no need to check when time elapse below 60 seconds
					 //stage 3.2.2
		}				
	}
	else
	{
			  	   //nSyncLastCheckTime = 0 means this is the first check, save nSyncTime and nSyncHeight, prepared to be compared in future
				   pnodeSync->nSyncLastCheckTime = GetTime();//pnodeSync->nSyncTime;
				   pnodeSync->nSyncLastHeight = nBestHeight;//pnodeSync->nSyncHeight;
				   printf("ip:%s,sync stage 3.1:start, time:%"PRI64d", height:%d\n", 
				   	    pnodeSync->addr.ToString().c_str(),
				   		pnodeSync->nSyncLastCheckTime, pnodeSync->nSyncLastHeight);
	}			
		

	
}

void ThreadMessageHandler(void* parg)
{
    // Make this thread recognisable as the message handling thread
    RenameThread("bitcoin-msghand");

    try
    {
        vnThreadsRunning[THREAD_MESSAGEHANDLER]++;
        ThreadMessageHandler2(parg);
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
        PrintException(&e, "ThreadMessageHandler()");
    } catch (...) {
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
        PrintException(NULL, "ThreadMessageHandler()");
    }
    printf("ThreadMessageHandler exited\n");
}

void ThreadMessageHandler2(void* parg)
{
    printf("ThreadMessageHandler started\n");
    SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL);
    while (!fShutdown)
    {
        vector<CNode*> vNodesCopy;
        {
            LOCK(cs_vNodes);
            vNodesCopy = vNodes;
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->AddRef();
        }

		//processSync();
		if(IsInitialBlockDownload())
		{
			processConcurrentSync();
		}

        // Poll the connected nodes for messages
        CNode* pnodeTrickle = NULL;
        if (!vNodesCopy.empty())
            pnodeTrickle = vNodesCopy[GetRand(vNodesCopy.size())];
        BOOST_FOREACH(CNode* pnode, vNodesCopy)
        {
            // Receive messages
            {
                TRY_LOCK(pnode->cs_vRecv, lockRecv);
				
				//int64 nBegin = GetAdjustedTime();
                if (lockRecv)
                    ProcessMessages(pnode);
				//int64 nEnd = GetAdjustedTime();
				
				//if((nEnd - nBegin) > 3)
				//	printf("process message timeout %"PRI64d", %"PRI64d"\n", nBegin, nEnd);
            }
            if (fShutdown)
                return;

            // Send messages
            {
                TRY_LOCK(pnode->cs_vSend, lockSend);
                if (lockSend)
                    SendMessages(pnode, pnode == pnodeTrickle);
            }
            if (fShutdown)
                return;
        }

        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->Release();
        }

        // Wait and allow messages to bunch up.
        // Reduce vnThreadsRunning so StopNode has permission to exit while
        // we're sleeping, but we must always check fShutdown after doing this.
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
        Sleep(100);
        if (fRequestShutdown)
            StartShutdown();
        vnThreadsRunning[THREAD_MESSAGEHANDLER]++;
        if (fShutdown)
            return;
    }
}






bool BindListenPort(const CService &addrBind, string& strError)
{
    strError = "";
    int nOne = 1;

#ifdef WIN32
    // Initialize Windows Sockets
    WSADATA wsadata;
    int ret = WSAStartup(MAKEWORD(2,2), &wsadata);
    if (ret != NO_ERROR)
    {
        strError = strprintf("Error: TCP/IP socket library failed to start (WSAStartup returned error %d)", ret);
        printf("%s\n", strError.c_str());
        return false;
    }
#endif

    // Create socket for listening for incoming connections
#ifdef USE_IPV6
    struct sockaddr_storage sockaddr;
#else
    struct sockaddr sockaddr;
#endif
    socklen_t len = sizeof(sockaddr);
    if (!addrBind.GetSockAddr((struct sockaddr*)&sockaddr, &len))
    {
        strError = strprintf("Error: bind address family for %s not supported", addrBind.ToString().c_str());
        printf("%s\n", strError.c_str());
        return false;
    }

    SOCKET hListenSocket = socket(((struct sockaddr*)&sockaddr)->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (hListenSocket == INVALID_SOCKET)
    {
        strError = strprintf("Error: Couldn't open socket for incoming connections (socket returned error %d)", WSAGetLastError());
        printf("%s\n", strError.c_str());
        return false;
    }

#ifdef SO_NOSIGPIPE
    // Different way of disabling SIGPIPE on BSD
    setsockopt(hListenSocket, SOL_SOCKET, SO_NOSIGPIPE, (void*)&nOne, sizeof(int));
#endif

#ifndef WIN32
    // Allow binding if the port is still in TIME_WAIT state after
    // the program was closed and restarted.  Not an issue on windows.
    setsockopt(hListenSocket, SOL_SOCKET, SO_REUSEADDR, (void*)&nOne, sizeof(int));
#endif


#ifdef WIN32
    // Set to non-blocking, incoming connections will also inherit this
    if (ioctlsocket(hListenSocket, FIONBIO, (u_long*)&nOne) == SOCKET_ERROR)
#else
    if (fcntl(hListenSocket, F_SETFL, O_NONBLOCK) == SOCKET_ERROR)
#endif
    {
        strError = strprintf("Error: Couldn't set properties on socket for incoming connections (error %d)", WSAGetLastError());
        printf("%s\n", strError.c_str());
        return false;
    }

#ifdef USE_IPV6
    // some systems don't have IPV6_V6ONLY but are always v6only; others do have the option
    // and enable it by default or not. Try to enable it, if possible.
    if (addrBind.IsIPv6()) {
#ifdef IPV6_V6ONLY
        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&nOne, sizeof(int));
#endif
#ifdef WIN32
        int nProtLevel = 10 /* PROTECTION_LEVEL_UNRESTRICTED */;
        int nParameterId = 23 /* IPV6_PROTECTION_LEVEl */;
        // this call is allowed to fail
        setsockopt(hListenSocket, IPPROTO_IPV6, nParameterId, (const char*)&nProtLevel, sizeof(int));
#endif
    }
#endif

    if (::bind(hListenSocket, (struct sockaddr*)&sockaddr, len) == SOCKET_ERROR)
    {
        int nErr = WSAGetLastError();
        if (nErr == WSAEADDRINUSE)
            strError = strprintf(_("Unable to bind to %s on this computer. Pennies is probably already running."), addrBind.ToString().c_str());
        else
            strError = strprintf(_("Unable to bind to %s on this computer (bind returned error %d, %s)"), addrBind.ToString().c_str(), nErr, strerror(nErr));
        printf("%s\n", strError.c_str());
        return false;
    }
    printf("Bound to %s\n", addrBind.ToString().c_str());

    // Listen for incoming connections
    if (listen(hListenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        strError = strprintf("Error: Listening for incoming connections failed (listen returned error %d)", WSAGetLastError());
        printf("%s\n", strError.c_str());
        return false;
    }

    vhListenSocket.push_back(hListenSocket);

    if (addrBind.IsRoutable() && fDiscover)
        AddLocal(addrBind, LOCAL_BIND);

    return true;
}

void static Discover()
{
    if (!fDiscover)
        return;

#ifdef WIN32
    // Get local host IP
    char pszHostName[1000] = "";
    if (gethostname(pszHostName, sizeof(pszHostName)) != SOCKET_ERROR)
    {
        vector<CNetAddr> vaddr;
        if (LookupHost(pszHostName, vaddr))
        {
            BOOST_FOREACH (const CNetAddr &addr, vaddr)
            {
                AddLocal(addr, LOCAL_IF);
            }
        }
    }
#else
    // Get local host ip
    struct ifaddrs* myaddrs;
    if (getifaddrs(&myaddrs) == 0)
    {
        for (struct ifaddrs* ifa = myaddrs; ifa != NULL; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr == NULL) continue;
            if ((ifa->ifa_flags & IFF_UP) == 0) continue;
            if (strcmp(ifa->ifa_name, "lo") == 0) continue;
            if (strcmp(ifa->ifa_name, "lo0") == 0) continue;
            if (ifa->ifa_addr->sa_family == AF_INET)
            {
                struct sockaddr_in* s4 = (struct sockaddr_in*)(ifa->ifa_addr);
                CNetAddr addr(s4->sin_addr);
                if (AddLocal(addr, LOCAL_IF))
                    printf("IPv4 %s: %s\n", ifa->ifa_name, addr.ToString().c_str());
            }
#ifdef USE_IPV6
            else if (ifa->ifa_addr->sa_family == AF_INET6)
            {
                struct sockaddr_in6* s6 = (struct sockaddr_in6*)(ifa->ifa_addr);
                CNetAddr addr(s6->sin6_addr);
                if (AddLocal(addr, LOCAL_IF))
                    printf("IPv6 %s: %s\n", ifa->ifa_name, addr.ToString().c_str());
            }
#endif
        }
        freeifaddrs(myaddrs);
    }
#endif

    // Don't use external IPv4 discovery, when -onlynet="IPv6"
    if (!IsLimited(NET_IPV4))
        NewThread(ThreadGetMyExternalIP, NULL);
}

void StartNode(void* parg)
{
    // Make this thread recognisable as the startup thread
    RenameThread("bitcoin-start");

    if (semOutbound == NULL) {
        // initialize semaphore
        int nMaxOutbound = min( GetMaxOutboundConnections(), GetMaxConnections() );
        semOutbound = new CSemaphore(nMaxOutbound);
    }

    if (pnodeLocalHost == NULL)
        pnodeLocalHost = new CNode(INVALID_SOCKET, CAddress(CService("127.0.0.1", 0), nLocalServices));

    Discover();

    //
    // Start threads
    //


    if (!GetBoolArg("-dnsseed", true))
        printf("DNS seeding disabled\n");
    else
        if (!NewThread(ThreadDNSAddressSeed, NULL))
            printf("Error: NewThread(ThreadDNSAddressSeed) failed\n");


    if (!GetBoolArg("-dnsseed", false))
        printf("DNS seeding disabled\n");
    if (GetBoolArg("-dnsseed", false))
        printf("DNS seeding NYI\n");

    // Map ports with UPnP
    if (fUseUPnP)
        MapPort();

    // Get addresses from IRC and advertise ours
    if (!NewThread(ThreadIRCSeed, NULL))
        printf("Error: NewThread(ThreadIRCSeed) failed\n");

    // Send and receive from sockets, accept connections
    if (!NewThread(ThreadSocketHandler, NULL))
        printf("Error: NewThread(ThreadSocketHandler) failed\n");

    // Initiate outbound connections from -addnode
    if (!NewThread(ThreadOpenAddedConnections, NULL))
        printf("Error: NewThread(ThreadOpenAddedConnections) failed\n");

    // Initiate outbound connections
    if (!NewThread(ThreadOpenConnections, NULL))
        printf("Error: NewThread(ThreadOpenConnections) failed\n");

    // Process messages
    if (!NewThread(ThreadMessageHandler, NULL))
        printf("Error: NewThread(ThreadMessageHandler) failed\n");

    // Dump network addresses
    if (!NewThread(ThreadDumpAddress, NULL))
        printf("Error; NewThread(ThreadDumpAddress) failed\n");

    // ppcoin: mint proof-of-stake blocks in the background
    if (!NewThread(ThreadStakeMinter, pwalletMain))
        printf("Error: NewThread(ThreadStakeMinter) failed\n");

    // Generate coins in the background
    GenerateBitcoins(GetBoolArg("-gen", false), pwalletMain);
}

bool StopNode()
{
    printf("StopNode()\n");
    fShutdown = true;
    nTransactionsUpdated++;
    int64 nStart = GetTime();
    if (semOutbound)
        for( int i = 0; i < GetMaxOutboundConnections(); i++ )
            semOutbound->post();
    do
    {
        int nThreadsRunning = 0;
        for (int n = 0; n < THREAD_MAX; n++)
            nThreadsRunning += vnThreadsRunning[n];
        if (nThreadsRunning == 0)
            break;
        if (GetTime() - nStart > 20)
            break;
        Sleep(20);
    } while(true);
    if (vnThreadsRunning[THREAD_SOCKETHANDLER] > 0) printf("ThreadSocketHandler still running\n");
    if (vnThreadsRunning[THREAD_OPENCONNECTIONS] > 0) printf("ThreadOpenConnections still running\n");
    if (vnThreadsRunning[THREAD_MESSAGEHANDLER] > 0) printf("ThreadMessageHandler still running\n");
    if (vnThreadsRunning[THREAD_MINER] > 0) printf("ThreadBitcoinMiner still running\n");
    if (vnThreadsRunning[THREAD_RPCLISTENER] > 0) printf("ThreadRPCListener still running\n");
    if (vnThreadsRunning[THREAD_RPCHANDLER] > 0) printf("ThreadsRPCServer still running\n");
#ifdef USE_UPNP
    if (vnThreadsRunning[THREAD_UPNP] > 0) printf("ThreadMapPort still running\n");
#endif
    if (vnThreadsRunning[THREAD_DNSSEED] > 0) printf("ThreadDNSAddressSeed still running\n");
    if (vnThreadsRunning[THREAD_ADDEDCONNECTIONS] > 0) printf("ThreadOpenAddedConnections still running\n");
    if (vnThreadsRunning[THREAD_DUMPADDRESS] > 0) printf("ThreadDumpAddresses still running\n");
    if (vnThreadsRunning[THREAD_MINTER] > 0) printf("ThreadStakeMinter still running\n");
    while (vnThreadsRunning[THREAD_MESSAGEHANDLER] > 0 || vnThreadsRunning[THREAD_RPCHANDLER] > 0)
        Sleep(20);
    Sleep(50);
    DumpAddresses();
    return true;
}

class CNetCleanup
{
public:
    CNetCleanup()
    {
    }
    ~CNetCleanup()
    {
        // Close sockets
        BOOST_FOREACH(CNode* pnode, vNodes)
            if (pnode->hSocket != INVALID_SOCKET)
                closesocket(pnode->hSocket);
        BOOST_FOREACH(SOCKET hListenSocket, vhListenSocket)
            if (hListenSocket != INVALID_SOCKET)
                if (closesocket(hListenSocket) == SOCKET_ERROR)
                    printf("closesocket(hListenSocket) failed with error %d\n", WSAGetLastError());

#ifdef WIN32
        // Shutdown Windows Sockets
        WSACleanup();
#endif
    }
}
instance_of_cnetcleanup;

void RelayTransaction(const CTransaction& tx, const uint256& hash)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss.reserve(10000);
    ss << tx;
    RelayTransaction(tx, hash, ss);
}

void RelayTransaction(const CTransaction& tx, const uint256& hash, const CDataStream& ss)
{
    CInv inv(MSG_TX, hash);
    {
        LOCK(cs_mapRelay);
        // Expire old relay messages
        while (!vRelayExpiration.empty() && vRelayExpiration.front().first < GetTime())
        {
            mapRelay.erase(vRelayExpiration.front().second);
            vRelayExpiration.pop_front();
        }

        // Save original serialized message so newer versions are preserved
        mapRelay.insert(std::make_pair(inv, ss));
        vRelayExpiration.push_back(std::make_pair(GetTime() + 15 * 60, inv));
    }
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        if(!pnode->fRelayTxes)
            continue;
        LOCK(pnode->cs_filter);
        if (pnode->pfilter)
        {
            if (pnode->pfilter->IsRelevantAndUpdate(tx, hash))
                pnode->PushInventory(inv);
        } else
            pnode->PushInventory(inv);
    }
}


