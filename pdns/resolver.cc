/*
 * This file is part of PowerDNS or dnsdist.
 * Copyright -- PowerDNS.COM B.V. and its contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * In addition, for the avoidance of any doubt, permission is granted to
 * link this program with OpenSSL and to (re)distribute the binaries
 * produced as the result of such linking.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "utility.hh"
#include "resolver.hh"
#include <pthread.h>
#include <semaphore.h>
#include <iostream>
#include <cerrno>
#include "misc.hh"
#include <algorithm>
#include <sstream>
#include "dnsrecords.hh"
#include <cstring>
#include <string>
#include <vector>
#include <boost/algorithm/string.hpp>
#include "dns.hh"
#include "qtype.hh"

#include "pdnsexception.hh"
#include "arguments.hh"
#include "base64.hh"
#include "dnswriter.hh"
#include "dnsparser.hh"
#include "query-local-address.hh"

#include "dns_random.hh"
#include <poll.h>
#include "gss_context.hh"
#include "namespaces.hh"

using pdns::resolver::parseResult;

int makeQuerySocket(const ComboAddress& local, bool udpOrTCP, bool nonLocalBind)
{
  ComboAddress ourLocal(local);

  int sock=socket(ourLocal.sin4.sin_family, udpOrTCP ? SOCK_DGRAM : SOCK_STREAM, 0);
  if(sock < 0) {
    if(errno == EAFNOSUPPORT && local.sin4.sin_family == AF_INET6) {
        return -1;
    }
    unixDie("Creating local resolver socket for address '"+ourLocal.toString()+"'");
  }

  setCloseOnExec(sock);

  if(nonLocalBind)
    Utility::setBindAny(local.sin4.sin_family, sock);

  if(udpOrTCP) {
    // udp, try hard to bind an unpredictable port
    int tries=10;
    while(--tries) {
      ourLocal.sin4.sin_port = htons(10000+(dns_random(10000)));

      if (::bind(sock, (struct sockaddr *)&ourLocal, ourLocal.getSocklen()) >= 0)
        break;
    }
    // cerr<<"bound udp port "<<ourLocal.sin4.sin_port<<", "<<tries<<" tries left"<<endl;

    if(!tries) {
      closesocket(sock);
      throw PDNSException("Resolver binding to local UDP socket on '"+ourLocal.toString()+"': "+stringerror());
    }
  }
  else {
    // tcp, let the kernel figure out the port
    ourLocal.sin4.sin_port = 0;
    if(::bind(sock, (struct sockaddr *)&ourLocal, ourLocal.getSocklen()) < 0) {
      closesocket(sock);
      throw PDNSException("Resolver binding to local TCP socket on '"+ourLocal.toString()+"': "+stringerror());
    }
  }
  return sock;
}

Resolver::Resolver()
{
  locals["default4"] = -1;
  locals["default6"] = -1;
  try {
    if (pdns::isQueryLocalAddressFamilyEnabled(AF_INET)) {
      locals["default4"] = makeQuerySocket(pdns::getQueryLocalAddress(AF_INET, 0), true, ::arg().mustDo("non-local-bind"));
    }
    if (pdns::isQueryLocalAddressFamilyEnabled(AF_INET6)) {
      locals["default6"] = makeQuerySocket(pdns::getQueryLocalAddress(AF_INET6, 0), true, ::arg().mustDo("non-local-bind"));
    }
  }
  catch(...) {
    if(locals["default4"]>=0)
      close(locals["default4"]);
    if(locals["default6"]>=0)
      close(locals["default6"]);
    throw;
  }
}

Resolver::~Resolver()
{
  for (auto& iter: locals) {
    if (iter.second >= 0)
      close(iter.second);
  }
}

uint16_t Resolver::sendResolve(const ComboAddress& remote, const ComboAddress& local,
                               const DNSName &domain, int type, int *localsock, bool dnssecOK,
                               const DNSName& tsigkeyname, const DNSName& tsigalgorithm,
                               const string& tsigsecret)
{
  uint16_t randomid;
  vector<uint8_t> packet;
  DNSPacketWriter pw(packet, domain, type);
  pw.getHeader()->id = randomid = dns_random_uint16();

  if(dnssecOK) {
    pw.addOpt(2800, 0, EDNSOpts::DNSSECOK);
    pw.commit();
  }

  if(!tsigkeyname.empty()) {
    // cerr<<"Adding TSIG to notification, key name: '"<<tsigkeyname<<"', algo: '"<<tsigalgorithm<<"', secret: "<<Base64Encode(tsigsecret)<<endl;
    TSIGRecordContent trc;
    if (tsigalgorithm == g_hmacmd5dnsname) {
      trc.d_algoName = g_hmacmd5dnsname_long;
    }
    else {
      trc.d_algoName = tsigalgorithm;
    }
    trc.d_time = time(nullptr);
    trc.d_fudge = 300;
    trc.d_origID=ntohs(randomid);
    trc.d_eRcode=0;
    addTSIG(pw, trc, tsigkeyname, tsigsecret, "", false);
  }

  int sock;

  // choose socket based on local
  if (local.sin4.sin_family == 0) {
    // up to us.
    if (!pdns::isQueryLocalAddressFamilyEnabled(remote.sin4.sin_family)) {
      string ipv = remote.sin4.sin_family == AF_INET ? "4" : "6";
      throw ResolverException("No IPv" + ipv + " socket available, is such an address configured in query-local-address?");
    }
    sock = remote.sin4.sin_family == AF_INET ? locals["default4"] : locals["default6"];
  } else {
    std::string lstr = local.toString();
    std::map<std::string, int>::iterator lptr;

    // reuse an existing local socket or make a new one
    if ((lptr = locals.find(lstr)) != locals.end()) {
      sock = lptr->second;
    } else {
      // try to make socket
      sock = makeQuerySocket(local, true);
      if (sock < 0)
        throw ResolverException("Unable to create local socket on '"+lstr+"'' to '"+remote.toLogString()+"': "+stringerror());
      setNonBlocking( sock );
      locals[lstr] = sock;
    }
  }

  if (localsock != nullptr) {
    *localsock = sock;
  }
  if(sendto(sock, &packet[0], packet.size(), 0, (struct sockaddr*)(&remote), remote.getSocklen()) < 0) {
    throw ResolverException("Unable to ask query of '"+remote.toLogString()+"': "+stringerror());
  }
  return randomid;
}

namespace pdns {
  namespace resolver {
    int parseResult(MOADNSParser& mdp, const DNSName& origQname, uint16_t /* origQtype */, uint16_t id, Resolver::res_t* result)
    {
      result->clear();

      if(mdp.d_header.rcode)
        return mdp.d_header.rcode;

      if(origQname.hasLabels()) {  // not AXFR
        if(mdp.d_header.id != id)
          throw ResolverException("Remote nameserver replied with wrong id");
        if(mdp.d_header.qdcount != 1)
          throw ResolverException("resolver: received answer with wrong number of questions ("+std::to_string(mdp.d_header.qdcount)+")");
        if(mdp.d_qname != origQname)
          throw ResolverException(string("resolver: received an answer to another question (")+mdp.d_qname.toLogString()+"!="+ origQname.toLogString()+".)");
      }

      vector<DNSResourceRecord> ret;
      DNSResourceRecord rr;
      result->reserve(mdp.d_answers.size());

      for (const auto& i: mdp.d_answers) {
        rr.qname = i.d_name;
        rr.qtype = i.d_type;
        rr.ttl = i.d_ttl;
        rr.content = i.getContent()->getZoneRepresentation(true);
        result->push_back(rr);
      }

      return 0;
    }

  } // namespace resolver
} // namespace pdns

bool Resolver::tryGetSOASerial(DNSName *domain, ComboAddress* remote, uint32_t *theirSerial, uint32_t *theirInception, uint32_t *theirExpire, uint16_t* id)
{
  auto fds = std::make_unique<struct pollfd[]>(locals.size());
  size_t i = 0, k;
  int sock;

  for (const auto& iter: locals) {
    fds[i].fd = iter.second;
    fds[i].events = POLLIN;
    ++i;
  }

  if (poll(fds.get(), i, 250) < 1) { // wait for 0.25s
    return false;
  }

  sock = -1;

  // determine who
  for(k=0;k<i;k++) {
    if ((fds[k].revents & POLLIN) == POLLIN) {
      sock = fds[k].fd;
      break;
    }
  }

  if (sock < 0) return false; // false alarm

  int err;
  remote->sin6.sin6_family = AF_INET6; // make sure getSocklen() below returns a large enough value
  socklen_t addrlen=remote->getSocklen();
  char buf[3000];
  err = recvfrom(sock, buf, sizeof(buf), 0,(struct sockaddr*)(remote), &addrlen);
  if(err < 0) {
    if(errno == EAGAIN)
      return false;

    throw ResolverException("recvfrom error waiting for answer: "+stringerror());
  }

  MOADNSParser mdp(false, (char*)buf, err);
  *id=mdp.d_header.id;
  *domain = mdp.d_qname;

  if(domain->empty())
    throw ResolverException("SOA query to '" + remote->toLogString() + "' produced response without domain name (RCode: " + RCode::to_s(mdp.d_header.rcode) + ")");

  if(mdp.d_answers.empty())
    throw ResolverException("Query to '" + remote->toLogString() + "' for SOA of '" + domain->toLogString() + "' produced no results (RCode: " + RCode::to_s(mdp.d_header.rcode) + ")");

  if(mdp.d_qtype != QType::SOA)
    throw ResolverException("Query to '" + remote->toLogString() + "' for SOA of '" + domain->toLogString() + "' returned wrong record type");

  if(mdp.d_header.rcode != 0)
    throw ResolverException("Query to '" + remote->toLogString() + "' for SOA of '" + domain->toLogString() + "' returned Rcode " + RCode::to_s(mdp.d_header.rcode));

  *theirInception = *theirExpire = 0;
  bool gotSOA=false;
  for(const MOADNSParser::answers_t::value_type& drc :  mdp.d_answers) {
    if(drc.d_type == QType::SOA && drc.d_name == *domain) {
      auto src = getRR<SOARecordContent>(drc);
      if (src) {
        *theirSerial = src->d_st.serial;
        gotSOA = true;
      }
    }
    if(drc.d_type == QType::RRSIG && drc.d_name == *domain) {
      auto rrc = getRR<RRSIGRecordContent>(drc);
      if(rrc && rrc->d_type == QType::SOA) {
        *theirInception= std::max(*theirInception, rrc->d_siginception);
        *theirExpire = std::max(*theirExpire, rrc->d_sigexpire);
      }
    }
  }
  if(!gotSOA)
    throw ResolverException("Query to '" + remote->toLogString() + "' for SOA of '" + domain->toLogString() + "' did not return a SOA");
  return true;
}

int Resolver::resolve(const ComboAddress& to, const DNSName &domain, int type, Resolver::res_t* res, const ComboAddress &local)
{
  try {
    int sock = -1;
    int id = sendResolve(to, local, domain, type, &sock);
    int err=waitForData(sock, 0, 3000000);

    if(!err) {
      throw ResolverException("Timeout waiting for answer");
    }
    if(err < 0)
      throw ResolverException("Error waiting for answer: "+stringerror());

    ComboAddress from;
    socklen_t addrlen = sizeof(from);
    char buffer[3000];
    int len;

    if((len=recvfrom(sock, buffer, sizeof(buffer), 0,(struct sockaddr*)(&from), &addrlen)) < 0)
      throw ResolverException("recvfrom error waiting for answer: "+stringerror());

    if (from != to) {
      throw ResolverException("Got answer from the wrong peer while resolving ('"+from.toLogString()+"' instead of '"+to.toLogString()+"', discarding");
    }

    MOADNSParser mdp(false, buffer, len);
    return parseResult(mdp, domain, type, id, res);
  }
  catch(ResolverException &re) {
    throw ResolverException(re.reason+" from "+to.toLogString());
  }
}

int Resolver::resolve(const ComboAddress& ipport, const DNSName &domain, int type, Resolver::res_t* res) {
  ComboAddress local;
  local.sin4.sin_family = 0;
  return resolve(ipport, domain, type, res, local);
}

void Resolver::getSoaSerial(const ComboAddress& ipport, const DNSName &domain, uint32_t *serial)
{
  vector<DNSResourceRecord> res;
  int ret = resolve(ipport, domain, QType::SOA, &res);

  if(ret || res.empty())
    throw ResolverException("Query to '" + ipport.toLogString() + "' for SOA of '" + domain.toLogString() + "' produced no answers");

  if(res[0].qtype.getCode() != QType::SOA)
    throw ResolverException("Query to '" + ipport.toLogString() + "' for SOA of '" + domain.toLogString() + "' produced a "+res[0].qtype.toString()+" record");

  vector<string>parts;
  stringtok(parts, res[0].content);
  if(parts.size()<3)
    throw ResolverException("Query to '" + ipport.toLogString() + "' for SOA of '" + domain.toLogString() + "' produced an unparseable response");

  try {
    *serial = pdns::checked_stoi<uint32_t>(parts[2]);
  }
  catch(const std::out_of_range& oor) {
    throw ResolverException("Query to '" + ipport.toLogString() + "' for SOA of '" + domain.toLogString() + "' produced an unparseable serial");
  }
}
