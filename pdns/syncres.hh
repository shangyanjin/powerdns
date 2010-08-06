#ifndef PDNS_SYNCRES_HH
#define PDNS_SYNCRES_HH
#include <string>
#include "dns.hh"
#include "qtype.hh"
#include <vector>
#include <set>
#include <map>
#include <cmath>
#include <iostream>
#include <utility>
#include "misc.hh"
#include "lwres.hh"
#include <boost/utility.hpp>
#include "sstuff.hh"
#include "recursor_cache.hh"
#include "recpacketcache.hh"
#include <boost/tuple/tuple.hpp>
#include <boost/optional.hpp>
#include <boost/tuple/tuple_comparison.hpp>
#include "mtasker.hh"
#include "iputils.hh"

void primeHints(void);

struct NegCacheEntry
{
  string d_name;
  QType d_qtype;
  string d_qname;
  uint32_t d_ttd;
};


template<class Thing> class Throttle : public boost::noncopyable
{
public:
  Throttle()
  {
    d_limit=3;
    d_ttl=60;
    d_last_clean=time(0);
  }
  bool shouldThrottle(time_t now, const Thing& t)
  {
    if(now > d_last_clean + 300 ) {

      d_last_clean=now;
      for(typename cont_t::iterator i=d_cont.begin();i!=d_cont.end();) {
        if( i->second.ttd < now) {
          d_cont.erase(i++);
        }
        else
          ++i;
      }
    }

    typename cont_t::iterator i=d_cont.find(t);
    if(i==d_cont.end())
      return false;
    if(now > i->second.ttd || i->second.count-- < 0) {
      d_cont.erase(i);
    }

    return true; // still listed, still blocked
  }
  void throttle(time_t now, const Thing& t, unsigned int ttl=0, unsigned int tries=0) 
  {
    typename cont_t::iterator i=d_cont.find(t);
    entry e={ now+(ttl ? ttl : d_ttl), tries ? tries : d_limit};

    if(i==d_cont.end()) {
      d_cont[t]=e;
    } 
    else if(i->second.ttd > e.ttd || (i->second.count) < e.count) 
      d_cont[t]=e;
  }
  
  unsigned int size()
  {
    return (unsigned int)d_cont.size();
  }
private:
  int d_limit;
  int d_ttl;
  time_t d_last_clean;
  struct entry 
  {
    time_t ttd;
    int count;
  };
  typedef map<Thing,entry> cont_t;
  cont_t d_cont;
};


/** Class that implements a decaying EWMA.
    This class keeps an exponentially weighted moving average which, additionally, decays over time.
    The decaying is only done on get.
*/
class DecayingEwma
{
public:
  DecayingEwma() :  d_val(0.0) 
  {
    d_needinit=true;
    d_last.tv_sec = d_last.tv_usec = 0;
    d_lastget=d_last;
  }

  DecayingEwma(const DecayingEwma& orig) : d_last(orig.d_last),  d_lastget(orig.d_lastget), d_val(orig.d_val), d_needinit(orig.d_needinit)
  {
  }

  struct timeval getOrMakeTime(struct timeval* tv)
  {
    if(tv)
      return *tv;
    else {
      struct timeval ret;
      Utility::gettimeofday(&ret, 0);
      return ret;
    }
  }

  void submit(int val, struct timeval* tv)
  {
    struct timeval now=getOrMakeTime(tv);

    if(d_needinit) {
      d_last=now;
      d_needinit=false;
    }

    float diff= makeFloat(d_last - now);

    d_last=now;
    double factor=exp(diff)/2.0; // might be '0.5', or 0.0001
    d_val=(float)((1-factor)*val+ (float)factor*d_val); 
  }

  double get(struct timeval* tv)
  {
    struct timeval now=getOrMakeTime(tv);
    float diff=makeFloat(d_lastget-now);
    d_lastget=now;
    float factor=exp(diff/60.0f); // is 1.0 or less
    return d_val*=factor;
  }

  bool stale(time_t limit) const
  {
    return limit > d_lastget.tv_sec;
  }

private:
  struct timeval d_last;          // stores time
  struct timeval d_lastget;       // stores time
  float d_val;
  bool d_needinit;
};


class SyncRes : public boost::noncopyable
{
public:
  explicit SyncRes(const struct timeval& now);

  int beginResolve(const string &qname, const QType &qtype, uint16_t qclass, vector<DNSResourceRecord>&ret);
  void setId(int id)
  {
    if(s_log)
      d_prefix="["+itoa(id)+"] ";
  }
  static void setLog(bool log)
  {
    s_log=log;
  }
  void setCacheOnly(bool state=true)
  {
    d_cacheonly=state;
  }
  void setNoCache(bool state=true)
  {
    d_nocache=state;
  }

  void setDoEDNS0(bool state=true)
  {
    d_doEDNS0=state;
  }



  int asyncresolveWrapper(const ComboAddress& ip, const string& domain, int type, bool doTCP, bool sendRDQuery, struct timeval* now, LWResult* res);
  
  static void doEDNSDumpAndClose(int fd);

  static unsigned int s_queries;
  static unsigned int s_outgoingtimeouts;
  static unsigned int s_throttledqueries;
  static unsigned int s_dontqueries;
  static unsigned int s_outqueries;
  static unsigned int s_tcpoutqueries;
  static unsigned int s_nodelegated;
  static unsigned int s_unreachables;
  static bool s_doIPv6;
  unsigned int d_outqueries;
  unsigned int d_tcpoutqueries;
  unsigned int d_throttledqueries;
  unsigned int d_timeouts;
  unsigned int d_unreachables;

  //  typedef map<string,NegCacheEntry> negcache_t;

  typedef multi_index_container <
    NegCacheEntry,
    indexed_by <
       ordered_unique<
           composite_key<
                 NegCacheEntry,
                    member<NegCacheEntry, string, &NegCacheEntry::d_name>,
                    member<NegCacheEntry, QType, &NegCacheEntry::d_qtype>
           >,
           composite_key_compare<CIStringCompare, std::less<QType> >
       >,
       ordered_non_unique<
           member<NegCacheEntry, uint32_t, &NegCacheEntry::d_ttd>
       >
    >
  > negcache_t;
  
  //! This represents a number of decaying Ewmas, used to store performance per namerserver-name. 
  /** Modelled to work mostly like the underlying DecayingEwma. After you've called get,
      d_best is filled out with the best address for this collection */
  struct DecayingEwmaCollection
  {
    void submit(const ComboAddress& remote, int usecs, struct timeval* now) 
    {
      collection_t::iterator pos;
      for(pos=d_collection.begin(); pos != d_collection.end(); ++pos)
        if(pos->first==remote)
          break;
      if(pos!=d_collection.end()) {
        pos->second.submit(usecs, now);
      }
      else {
        DecayingEwma de;
        de.submit(usecs, now);
        d_collection.push_back(make_pair(remote, de));
      }
    }

    double get(struct timeval* now)
    {
      if(d_collection.empty())
        return 0;
      double ret=numeric_limits<double>::max();
      double tmp;
      for(collection_t::iterator pos=d_collection.begin(); pos != d_collection.end(); ++pos) {
        if((tmp=pos->second.get(now)) < ret) {
          ret=tmp;
          d_best=pos->first;
        }
      }
      
      return ret;
    }
    
    bool stale(time_t limit) const
    {
      for(collection_t::const_iterator pos=d_collection.begin(); pos != d_collection.end(); ++pos) 
        if(!pos->second.stale(limit))
          return false;
      return true;
    }

    typedef vector<pair<ComboAddress, DecayingEwma> > collection_t;
    collection_t d_collection;
    ComboAddress d_best;
  };

  typedef map<string, DecayingEwmaCollection, CIStringCompare> nsspeeds_t;
  

  struct EDNSStatus
  {
    EDNSStatus() : mode(UNKNOWN), modeSetAt(0), EDNSPingHitCount(0) {}
    enum EDNSMode { CONFIRMEDPINGER=-1, UNKNOWN=0, EDNSNOPING=1, EDNSPINGOK=2, EDNSIGNORANT=3, NOEDNS=4 } mode;
    time_t modeSetAt;
    int EDNSPingHitCount;
  };

  typedef map<ComboAddress, EDNSStatus> ednsstatus_t;

  

  static bool s_noEDNSPing;
  static bool s_noEDNS;

  struct AuthDomain
  {
    vector<ComboAddress> d_servers;
    bool d_rdForward;
    typedef multi_index_container <
      DNSResourceRecord,
      indexed_by < 
        ordered_non_unique< 
          composite_key< DNSResourceRecord,
        	         member<DNSResourceRecord, string, &DNSResourceRecord::qname>,
        	         member<DNSResourceRecord, QType, &DNSResourceRecord::qtype>
                       >,
          composite_key_compare<CIStringCompare, std::less<QType> >
        >
      >
    > records_t;
    records_t d_records;       
  };
  

  typedef map<string, AuthDomain, CIStringCompare> domainmap_t;
  

  typedef Throttle<tuple<ComboAddress,string,uint16_t> > throttle_t;
  
  struct timeval d_now;
  static unsigned int s_maxnegttl;
  static unsigned int s_maxcachettl;
  static unsigned int s_packetcachettl;
  static unsigned int s_packetcacheservfailttl;
  static bool s_nopacketcache;
  static string s_serverID;

  struct StaticStorage {
    negcache_t negcache;    
    nsspeeds_t nsSpeeds;
    ednsstatus_t ednsstatus;
    throttle_t throttle;
    domainmap_t* domainmap;
  };

private:
  struct GetBestNSAnswer;
  int doResolveAt(set<string, CIStringCompare> nameservers, string auth, bool flawedNSSet, const string &qname, const QType &qtype, vector<DNSResourceRecord>&ret,
        	  int depth, set<GetBestNSAnswer>&beenthere);
  int doResolve(const string &qname, const QType &qtype, vector<DNSResourceRecord>&ret, int depth, set<GetBestNSAnswer>& beenthere);
  bool doOOBResolve(const string &qname, const QType &qtype, vector<DNSResourceRecord>&ret, int depth, int &res);
  domainmap_t::const_iterator getBestAuthZone(string* qname);
  bool doCNAMECacheCheck(const string &qname, const QType &qtype, vector<DNSResourceRecord>&ret, int depth, int &res);
  bool doCacheCheck(const string &qname, const QType &qtype, vector<DNSResourceRecord>&ret, int depth, int &res);
  void getBestNSFromCache(const string &qname, set<DNSResourceRecord>&bestns, bool* flawedNSSet, int depth, set<GetBestNSAnswer>& beenthere);
  void addCruft(const string &qname, vector<DNSResourceRecord>& ret);
  string getBestNSNamesFromCache(const string &qname,set<string, CIStringCompare>& nsset, bool* flawedNSSet, int depth, set<GetBestNSAnswer>&beenthere);
  void addAuthorityRecords(const string& qname, vector<DNSResourceRecord>& ret, int depth);

  inline vector<string> shuffleInSpeedOrder(set<string, CIStringCompare> &nameservers, const string &prefix);
  bool moreSpecificThan(const string& a, const string &b);
  vector<ComboAddress> getAs(const string &qname, int depth, set<GetBestNSAnswer>& beenthere);

private:
  string d_prefix;
  static bool s_log;
  bool d_cacheonly;
  bool d_nocache;
  bool d_doEDNS0;

  struct GetBestNSAnswer
  {
    string qname;
    set<DNSResourceRecord> bestns;
    bool operator<(const GetBestNSAnswer &b) const
    {
      if(qname<b.qname)
        return true;
      if(qname==b.qname)
        return bestns<b.bestns;
      return false;
    }
  };

};
extern __thread SyncRes::StaticStorage* t_sstorage;

class Socket;
/* external functions, opaque to us */
int asendtcp(const string& data, Socket* sock);
int arecvtcp(string& data, int len, Socket* sock);


struct PacketID
{
  PacketID() : id(0), type(0), sock(0), inNeeded(0), outPos(0), nearMisses(0), fd(-1)
  {
    memset(&remote, 0, sizeof(remote));
  }

  uint16_t id;  // wait for a specific id/remote pair
  ComboAddress remote;  // this is the remote
  string domain;             // this is the question 
  uint16_t type;             // and this is its type

  Socket* sock;  // or wait for an event on a TCP fd
  int inNeeded; // if this is set, we'll read until inNeeded bytes are read
  string inMSG; // they'll go here

  string outMSG; // the outgoing message that needs to be sent
  string::size_type outPos;    // how far we are along in the outMSG

  mutable uint32_t nearMisses; // number of near misses - host correct, id wrong
  typedef set<uint16_t > chain_t;
  mutable chain_t chain;
  int fd;

  bool operator<(const PacketID& b) const
  {
    int ourSock= sock ? sock->getHandle() : 0;
    int bSock = b.sock ? b.sock->getHandle() : 0;
    if( tie(remote, ourSock, type) < tie(b.remote, bSock, b.type))
      return true;
    if( tie(remote, ourSock, type) > tie(b.remote, bSock, b.type))
      return false;

    if(pdns_ilexicographical_compare(domain, b.domain))
      return true;
    if(pdns_ilexicographical_compare(b.domain, domain))
      return false;

    return tie(fd, id) < tie(b.fd, b.id);
  }
};

struct PacketIDBirthdayCompare: public binary_function<PacketID, PacketID, bool>  
{
  bool operator()(const PacketID& a, const PacketID& b) const
  {
    int ourSock= a.sock ? a.sock->getHandle() : 0;
    int bSock = b.sock ? b.sock->getHandle() : 0;
    if( tie(a.remote, ourSock, a.type) < tie(b.remote, bSock, b.type))
      return true;
    if( tie(a.remote, ourSock, a.type) > tie(b.remote, bSock, b.type))
      return false;

    return pdns_ilexicographical_compare(a.domain, b.domain);
  }
};
extern __thread MemRecursorCache* t_RC;
extern __thread RecursorPacketCache* t_packetCache;
typedef MTasker<PacketID,string> MT_t;
extern __thread MT_t* MT;


struct RecursorStats
{
  uint64_t servFails;
  uint64_t nxDomains;
  uint64_t noErrors;
  uint64_t answers0_1, answers1_10, answers10_100, answers100_1000, answersSlow;
  uint64_t avgLatencyUsec;
  uint64_t qcounter;
  uint64_t tcpqcounter;
  uint64_t unauthorizedUDP;
  uint64_t unauthorizedTCP;
  uint64_t tcpClientOverflow;
  uint64_t clientParseError;
  uint64_t serverParseError;
  uint64_t unexpectedCount;
  uint64_t caseMismatchCount;
  uint64_t spoofCount;
  uint64_t resourceLimits;
  uint64_t overCapacityDrops;
  uint64_t ipv6queries;
  uint64_t chainResends;
  uint64_t nsSetInvalidations;
  uint64_t ednsPingMatches;
  uint64_t ednsPingMismatches;
  uint64_t noPingOutQueries, noEdnsOutQueries;
  uint64_t packetCacheHits;
  uint64_t noPacketError;
  time_t startupTime;
};

//! represents a running TCP/IP client session
class TCPConnection
{
public:
  int fd;
  enum stateenum {BYTE0, BYTE1, GETQUESTION, DONE} state;
  int qlen;
  int bytesread;
  ComboAddress remote;
  char data[65535];

  static void closeAndCleanup(int fd, const ComboAddress& remote);
  void closeAndCleanup();
  static unsigned int getCurrentConnections() { return s_currentConnections; }
  static void incCurrentConnections() { s_currentConnections++; }
  static void decCurrentConnections() { s_currentConnections--; }
private:
  static volatile unsigned int s_currentConnections; //!< total number of current TCP connections
};


struct RemoteKeeper
{
  typedef vector<ComboAddress> remotes_t;
  remotes_t remotes;
  int d_remotepos;
  void addRemote(const ComboAddress& remote)
  {
    if(!remotes.size())
      return;

    remotes[(d_remotepos++) % remotes.size()]=remote;
  }
};
extern __thread RemoteKeeper* t_remotes;
string doQueueReloadLuaScript(vector<string>::const_iterator begin, vector<string>::const_iterator end);
void parseACLs();
extern RecursorStats g_stats;
extern unsigned int g_numThreads;

template<typename Index>
std::pair<typename Index::iterator,bool>
replacing_insert(Index& i,const typename Index::value_type& x)
{
  std::pair<typename Index::iterator,bool> res=i.insert(x);
  if(!res.second)res.second=i.replace(res.first,x);
  return res;
}


std::string reloadAuthAndForwards();
ComboAddress parseIPAndPort(const std::string& input, uint16_t port);
ComboAddress getQueryLocalAddress(int family, uint16_t port);
typedef boost::function<void*(void)> pipefunc_t;
void broadcastFunction(const pipefunc_t& func, bool skipSelf = false);
void distributeAsyncFunction(const pipefunc_t& func);


template<class T> T broadcastAccFunction(const boost::function<T*()>& func, bool skipSelf=false);

SyncRes::domainmap_t* parseAuthAndForwards();

uint64_t* pleaseGetNsSpeedsSize();
uint64_t* pleaseGetCacheSize();
uint64_t* pleaseGetNegCacheSize();
uint64_t* pleaseGetCacheHits();
uint64_t* pleaseGetCacheMisses();
uint64_t* pleaseGetConcurrentQueries();
uint64_t* pleaseGetThrottleSize();
uint64_t* pleaseGetPacketCacheHits();
uint64_t* pleaseGetPacketCacheSize();

#endif
