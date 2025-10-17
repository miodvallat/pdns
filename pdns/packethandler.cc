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
#include "packetcache.hh"
#include "utility.hh"
#include "base32.hh"
#include "base64.hh"
#include <string>
#include <sys/types.h>
#include <boost/algorithm/string.hpp>
#include "dnssecinfra.hh"
#include "dnsseckeeper.hh"
#include "dns.hh"
#include "dnsbackend.hh"
#include "ueberbackend.hh"
#include "dnspacket.hh"
#include "nameserver.hh"
#include "distributor.hh"
#include "logger.hh"
#include "arguments.hh"
#include "packethandler.hh"
#include "statbag.hh"
#include "resolver.hh"
#include "communicator.hh"
#include "dnsproxy.hh"
#include "version.hh"
#include "auth-main.hh"
#include "trusted-notification-proxy.hh"
#include "gss_context.hh"

#if 0
#undef DLOG
#define DLOG(x) x
#endif

AtomicCounter PacketHandler::s_count;
NetmaskGroup PacketHandler::s_allowNotifyFrom;
set<string> PacketHandler::s_forwardNotify;
bool PacketHandler::s_SVCAutohints{false};

extern string g_programname;

// See https://www.rfc-editor.org/rfc/rfc8078.txt and https://www.rfc-editor.org/errata/eid5049 for details
const std::shared_ptr<CDNSKEYRecordContent> PacketHandler::s_deleteCDNSKEYContent = std::make_shared<CDNSKEYRecordContent>("0 3 0 AA==");
const std::shared_ptr<CDSRecordContent> PacketHandler::s_deleteCDSContent = std::make_shared<CDSRecordContent>("0 0 0 00");

PacketHandler::PacketHandler(std::shared_ptr<Logr::Logger> slog) :
  B(g_programname), d_dk(&B)
{
  ++s_count;
  d_slog = slog;
  d_doDNAME=::arg().mustDo("dname-processing");
  d_doExpandALIAS = ::arg().mustDo("expand-alias");
  d_doResolveAcrossZones = ::arg().mustDo("resolve-across-zones");
  d_logDNSDetails= ::arg().mustDo("log-dns-details");
  string fname= ::arg()["lua-prequery-script"];

  if(fname.empty())
  {
    d_pdl = nullptr;
  }
  else
  {
    d_pdl = std::make_unique<AuthLua4>(::arg()["lua-global-include-dir"]);
    d_pdl->loadFile(fname);
  }
  fname = ::arg()["lua-dnsupdate-policy-script"];
  if (fname.empty())
  {
    d_update_policy_lua = nullptr;
  }
  else
  {
    try {
      d_update_policy_lua = std::make_unique<AuthLua4>();
      d_update_policy_lua->loadFile(fname);
    }
    catch (const std::runtime_error& e) {
      SLOG(g_log<<Logger::Warning<<"Failed to load update policy - disabling: "<<e.what()<<endl,
           d_slog->error(Logr::Warning, e.what(), "Failed to load Lua update policy, disabling"));
      d_update_policy_lua = nullptr;
    }
  }
}

UeberBackend *PacketHandler::getBackend()
{
  return &B;
}

PacketHandler::~PacketHandler()
{
  --s_count;
  DLOG(SLOG(g_log<<Logger::Error<<"PacketHandler destructor called - "<<s_count<<" left"<<endl,
            d_slog->info(Logr::Error, "PacketHandler destructor called", "instances left", Logging::Loggable(s_count))));
}

/**
 * This adds CDNSKEY records to the answer packet. Returns true if one was added.
 *
 * @param p          Pointer to the DNSPacket containing the original question
 * @param r          Pointer to the DNSPacket where the records should be inserted into
 * @param sd         SOAData of the zone from which CDNSKEY contents are taken (default: d_sd)
 * @return           bool that shows if any records were added
**/
bool PacketHandler::addCDNSKEY(DNSPacket& p, std::unique_ptr<DNSPacket>& r)
{
  return addCDNSKEY(p, r, d_sd);
}
bool PacketHandler::addCDNSKEY(DNSPacket& p, std::unique_ptr<DNSPacket>& r, SOAData &sd) // NOLINT(readability-identifier-length)
{
  string publishCDNSKEY;
  d_dk.getPublishCDNSKEY(sd.zonename,publishCDNSKEY);
  if (publishCDNSKEY.empty())
    return false;

  DNSZoneRecord rr;
  rr.dr.d_type=QType::CDNSKEY;
  rr.dr.d_ttl=sd.minimum;
  rr.dr.d_name=p.qdomain;
  rr.auth=true;

  if (publishCDNSKEY == "0") { // delete DS via CDNSKEY
    rr.dr.setContent(s_deleteCDNSKEYContent);
    r->addRecord(std::move(rr));
    return true;
  }

  bool haveOne=false;
  for (const auto& value : d_dk.getEntryPoints(sd.zonename)) {
    if (!value.second.published) {
      continue;
    }
    rr.dr.setContent(std::make_shared<DNSKEYRecordContent>(value.first.getDNSKEY()));
    r->addRecord(DNSZoneRecord(rr));
    haveOne=true;
  }

  if(::arg().mustDo("direct-dnskey")) {
    B.lookup(QType(QType::CDNSKEY), sd.qname(), sd.domain_id, &p);

    while(B.get(rr)) {
      rr.dr.d_ttl=sd.minimum;
      rr.dr.d_name=p.qdomain;
      r->addRecord(std::move(rr));
      haveOne=true;
    }
  }
  return haveOne;
}

/**
 * This adds DNSKEY records to the answer packet. Returns true if one was added.
 *
 * @param p          Pointer to the DNSPacket containing the original question
 * @param r          Pointer to the DNSPacket where the records should be inserted into
 * @return           bool that shows if any records were added
**/
bool PacketHandler::addDNSKEY(DNSPacket& p, std::unique_ptr<DNSPacket>& r)
{
  DNSZoneRecord rr;
  bool haveOne=false;

  for (const auto& value : d_dk.getKeys(r->qdomainzone)) {
    if (!value.second.published) {
      continue;
    }
    rr.dr.d_type=QType::DNSKEY;
    rr.dr.d_ttl=d_sd.minimum;
    rr.dr.d_name=p.qdomain;
    rr.dr.setContent(std::make_shared<DNSKEYRecordContent>(value.first.getDNSKEY()));
    rr.auth=true;
    r->addRecord(std::move(rr));
    haveOne=true;
  }

  if(::arg().mustDo("direct-dnskey")) {
    B.lookup(QType(QType::DNSKEY), p.qdomain, d_sd.domain_id, &p);

    while(B.get(rr)) {
      rr.dr.d_ttl=d_sd.minimum;
      r->addRecord(std::move(rr));
      haveOne=true;
    }
  }

  return haveOne;
}

/**
 * This adds CDS records to the answer packet r.
 *
 * @param p   Pointer to the DNSPacket containing the original question.
 * @param r   Pointer to the DNSPacket where the records should be inserted into.
 *            used to determine record TTL.
 * @param sd  SOAData of the zone from which CDS contents are taken (default: d_sd)
 * @return    bool that shows if any records were added.
**/
bool PacketHandler::addCDS(DNSPacket& p, std::unique_ptr<DNSPacket>& r) // NOLINT(readability-identifier-length)
{
  return addCDS(p, r, d_sd);
}
bool PacketHandler::addCDS(DNSPacket& p, std::unique_ptr<DNSPacket>& r, SOAData &sd) // NOLINT(readability-identifier-length)
{
  string publishCDS;
  d_dk.getPublishCDS(sd.zonename, publishCDS);
  if (publishCDS.empty())
    return false;

  vector<string> digestAlgos;
  stringtok(digestAlgos, publishCDS, ", ");

  DNSZoneRecord rr;
  rr.dr.d_type=QType::CDS;
  rr.dr.d_ttl=sd.minimum;
  rr.dr.d_name=p.qdomain;
  rr.auth=true;

  if(std::find(digestAlgos.begin(), digestAlgos.end(), "0") != digestAlgos.end()) { // delete DS via CDS
    rr.dr.setContent(s_deleteCDSContent);
    r->addRecord(std::move(rr));
    return true;
  }

  bool haveOne=false;

  for (const auto& value : d_dk.getEntryPoints(sd.zonename)) {
    if (!value.second.published) {
      continue;
    }
    for(auto const &digestAlgo : digestAlgos){
      rr.dr.setContent(std::make_shared<DSRecordContent>(makeDSFromDNSKey(sd.qname(), value.first.getDNSKEY(), pdns::checked_stoi<uint8_t>(digestAlgo))));
      r->addRecord(DNSZoneRecord(rr));
      haveOne=true;
    }
  }

  if(::arg().mustDo("direct-dnskey")) {
    B.lookup(QType(QType::CDS), sd.qname(), sd.domain_id, &p);

    while(B.get(rr)) {
      rr.dr.d_ttl=sd.minimum;
      rr.dr.d_name=p.qdomain;
      r->addRecord(std::move(rr));
      haveOne=true;
    }
  }

  return haveOne;
}

/** This adds NSEC3PARAM records. Returns true if one was added */
bool PacketHandler::addNSEC3PARAM(const DNSPacket& p, std::unique_ptr<DNSPacket>& r)
{
  DNSZoneRecord rr;

  NSEC3PARAMRecordContent ns3prc;
  if(d_dk.getNSEC3PARAM(r->qdomainzone, &ns3prc)) {
    rr.dr.d_type=QType::NSEC3PARAM;
    rr.dr.d_ttl=d_sd.minimum;
    rr.dr.d_name=p.qdomain;
    ns3prc.d_flags = 0; // the NSEC3PARAM 'flag' is defined to always be zero in RFC5155.
    rr.dr.setContent(std::make_shared<NSEC3PARAMRecordContent>(ns3prc));
    rr.auth = true;
    r->addRecord(std::move(rr));
    return true;
  }
  return false;
}


// This is our chaos class requests handler. Return 1 if content was added, 0 if it wasn't
int PacketHandler::doChaosRequest(const DNSPacket& p, std::unique_ptr<DNSPacket>& r, DNSName &target) const
{
  DNSZoneRecord rr;

  if(p.qtype.getCode()==QType::TXT) {
    static const DNSName versionbind("version.bind."), versionpdns("version.pdns."), idserver("id.server.");
    if (target==versionbind || target==versionpdns) {
      // modes: full, powerdns only, anonymous or custom
      const static string mode=::arg()["version-string"];
      string content;
      if(mode=="full")
        content=fullVersionString();
      else if(mode=="powerdns")
        content="Served by PowerDNS - https://www.powerdns.com/";
      else if(mode=="anonymous") {
        r->setRcode(RCode::ServFail);
        return 0;
      }
      else
        content=mode;
      rr.dr.setContent(DNSRecordContent::make(QType::TXT, 1, "\"" + content + "\""));
    }
    else if (target==idserver) {
      // modes: disabled, hostname or custom
      const static string id=::arg()["server-id"];

      if (id == "disabled") {
        r->setRcode(RCode::Refused);
        return 0;
      }
      string tid=id;
      if(!tid.empty() && tid[0]!='"') { // see #6010 however
        tid = "\"" + tid + "\"";
      }
      rr.dr.setContent(DNSRecordContent::make(QType::TXT, 1, tid));
    }
    else {
      r->setRcode(RCode::Refused);
      return 0;
    }

    rr.dr.d_ttl=5;
    rr.dr.d_name=target;
    rr.dr.d_type=QType::TXT;
    rr.dr.d_class=QClass::CHAOS;
    r->addRecord(std::move(rr));
    return 1;
  }

  r->setRcode(RCode::NotImp);
  return 0;
}

vector<DNSZoneRecord> PacketHandler::getBestReferralNS(DNSPacket& p, const DNSName &target)
{
  vector<DNSZoneRecord> ret;
  DNSZoneRecord rr;
  DNSName subdomain(target);
  do {
    if(subdomain == d_sd.qname()) { // stop at SOA
      break;
    }
    B.lookup(QType(QType::NS), subdomain, d_sd.domain_id, &p);
    while(B.get(rr)) {
      ret.push_back(rr); // this used to exclude auth NS records for some reason
    }
    if(!ret.empty()) {
      return ret;
    }
  } while( subdomain.chopOff() );   // 'www.powerdns.org' -> 'powerdns.org' -> 'org' -> ''
  return ret;
}

void PacketHandler::getBestDNAMESynth(DNSPacket& p, DNSName &target, vector<DNSZoneRecord> &ret)
{
  ret.clear();
  DNSZoneRecord rr;
  DNSName prefix;
  DNSName subdomain(target);
  do {
    DLOG(SLOG(g_log<<"Attempting DNAME lookup for "<<subdomain<<", d_sd.qname()="<<d_sd.qname()<<endl,
              d_slog->info("Attempting DNAME lookup", "subdomain", Logging::Loggable(subdomain), "name", Logging::Loggable(d_sd.qname()))));

    B.lookup(QType(QType::DNAME), subdomain, d_sd.domain_id, &p);
    while(B.get(rr)) {
      ret.push_back(rr);  // put in the original
      rr.dr.d_type = QType::CNAME;
      rr.dr.d_name = prefix + rr.dr.d_name;
      rr.dr.setContent(std::make_shared<CNAMERecordContent>(CNAMERecordContent(prefix + getRR<DNAMERecordContent>(rr.dr)->getTarget())));
      rr.auth = false; // don't sign CNAME
      target = getRR<CNAMERecordContent>(rr.dr)->getTarget();
      ret.push_back(rr);
    }
    if(!ret.empty()) {
      return;
    }
    if(subdomain.hasLabels()) {
      prefix.appendRawLabel(subdomain.getRawLabel(0)); // XXX DNSName pain this feels wrong
    }
    if(subdomain == d_sd.qname()) { // stop at SOA
      break;
    }

  } while( subdomain.chopOff() );   // 'www.powerdns.org' -> 'powerdns.org' -> 'org' -> ''
  return;
}


// Return best matching wildcard or next closer name
bool PacketHandler::getBestWildcard(DNSPacket& p, const DNSName &target, DNSName &wildcard, vector<DNSZoneRecord>* ret)
{
  ret->clear();
  DNSZoneRecord rr;
  DNSName subdomain(target);
  bool haveSomething=false;
  bool haveCNAME = false;

#ifdef HAVE_LUA_RECORDS
  bool doLua = doLuaRecords();
#endif

  wildcard=subdomain;
  while( subdomain.chopOff() && !haveSomething )  {
    if (subdomain.empty()) {
      B.lookup(QType(QType::ANY), g_wildcarddnsname, d_sd.domain_id, &p);
    } else {
      B.lookup(QType(QType::ANY), g_wildcarddnsname+subdomain, d_sd.domain_id, &p);
    }
    while(B.get(rr)) {
      if (haveCNAME) {
        // No need to look any further
        B.lookupEnd();
        break;
      }
#ifdef HAVE_LUA_RECORDS
      if (rr.dr.d_type == QType::LUA && !isPresigned()) {
        if(!doLua) {
          DLOG(SLOG(g_log<<"Have a wildcard Lua match, but not doing Lua record for this zone"<<endl,
                    d_slog->info("Have a wildcard Lua match, but not doing Lua record for this zone")));
          continue;
        }

        DLOG(SLOG(g_log<<"Have a wildcard Lua match"<<endl,
                  d_slog->info("Have a wildcard Lua match")));

        auto rec=getRR<LUARecordContent>(rr.dr);
        if (!rec) {
          continue;
        }
        if(rec->d_type == QType::CNAME || rec->d_type == p.qtype.getCode() || (p.qtype.getCode() == QType::ANY && rec->d_type != QType::RRSIG)) {
          //    noCache=true;
          DLOG(SLOG(g_log<<"Executing Lua: '"<<rec->getCode()<<"'"<<endl,
                    d_slog->info("Executing Lua", "code", Logging::Loggable(rec->getCode()))));
          try {
            auto recvec=luaSynth(rec->getCode(), target, rr, d_sd.qname(), p, rec->d_type, s_LUA);
            for (const auto& r : recvec) {
              rr.dr.d_type = rec->d_type; // might be CNAME
              rr.dr.setContent(r);
              rr.scopeMask = p.getRealRemote().getBits(); // this makes sure answer is a specific as your question
              if (rr.dr.d_type == QType::CNAME) {
                haveCNAME = true;
                *ret = {rr};
                break;
              }
              ret->push_back(rr);
            }
          }
          catch (std::exception &e) {
            B.lookupEnd();                 // don't leave DB handle in bad state

            throw;
          }
        }
      }
      else
#endif
      if(rr.dr.d_type != QType::ENT && (rr.dr.d_type == p.qtype.getCode() || rr.dr.d_type == QType::CNAME || (p.qtype.getCode() == QType::ANY && rr.dr.d_type != QType::RRSIG))) {
        if (rr.dr.d_type == QType::CNAME) {
          haveCNAME = true;
          ret->clear();
        }
        ret->push_back(rr);
      }

      // NOLINTNEXTLINE(readability-misleading-indentation): go home, clang-tidy, you're drunk
      wildcard=g_wildcarddnsname+subdomain;
      haveSomething=true;
    }

    if ( subdomain == d_sd.qname() || haveSomething ) { // stop at SOA or result
      break;
    }

    B.lookup(QType(QType::ANY), subdomain, d_sd.domain_id, &p);
    if (B.get(rr)) {
      DLOG(SLOG(g_log<<"No wildcard match, ancestor exists"<<endl,
                d_slog->info("No wildcard match, ancestor exists")));
      B.lookupEnd();
      break;
    }
    wildcard=subdomain;
  }

  return haveSomething;
}

DNSName PacketHandler::doAdditionalServiceProcessing(const DNSName &firstTarget, const uint16_t &qtype, std::unique_ptr<DNSPacket>& /* r */, vector<DNSZoneRecord>& extraRecords) {
  DNSName ret = firstTarget;
  size_t ctr = 5; // Max 5 SVCB Aliasforms per query
  bool done = false;
  while (!done && ctr > 0) {
    DNSZoneRecord rr;
    done = true;

    if(!ret.isPartOf(d_sd.qname())) {
      continue;
    }

    B.lookup(QType(qtype), ret, d_sd.domain_id);
    while (B.get(rr)) {
      rr.dr.d_place = DNSResourceRecord::ADDITIONAL;
      switch (qtype) {
        case QType::SVCB: /* fall-through */
        case QType::HTTPS: {
          auto rrc = getRR<SVCBBaseRecordContent>(rr.dr);
          extraRecords.push_back(std::move(rr));
          ret = rrc->getTarget().isRoot() ? ret : rrc->getTarget();
          if (rrc->getPriority() == 0) {
            done = false;
          }
          break;
        }
        default:
          B.lookupEnd();              // don't leave DB handle in bad state

          throw PDNSException("Unknown type (" + QType(qtype).toString() + ") for additional service processing");
      }
    }
    ctr--;
  }
  return ret;
}


// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void PacketHandler::doAdditionalProcessing(DNSPacket& p, std::unique_ptr<DNSPacket>& r)
{
  DNSName content;
  DNSZoneRecord dzr;
  std::unordered_set<DNSName> lookup;
  vector<DNSZoneRecord> extraRecords;
  const auto& rrs = r->getRRS();

  lookup.reserve(rrs.size());
  for(auto& rr : rrs) {
    if(rr.dr.d_place != DNSResourceRecord::ADDITIONAL) {
      content.clear();
      switch(rr.dr.d_type) {
        case QType::NS:
          content=getRR<NSRecordContent>(rr.dr)->getNS();
          break;
        case QType::MX:
          content=getRR<MXRecordContent>(rr.dr)->d_mxname;
          break;
        case QType::SRV:
          content=getRR<SRVRecordContent>(rr.dr)->d_target;
          break;
        case QType::SVCB: /* fall-through */
        case QType::HTTPS: {
          auto rrc = getRR<SVCBBaseRecordContent>(rr.dr);
          content = rrc->getTarget();
          if (content.isRoot()) {
            content = rr.dr.d_name;
          }
          if (rrc->getPriority() == 0) {
            content = doAdditionalServiceProcessing(content, rr.dr.d_type, r, extraRecords);
          }
          break;
        }
        case QType::NAPTR: {
          auto naptrContent = getRR<NAPTRRecordContent>(rr.dr);
          auto flags = naptrContent->getFlags();
          toLowerInPlace(flags);
          if (flags.find('a') != string::npos) {
            content = naptrContent->getReplacement();
            DLOG(SLOG(g_log<<Logger::Debug<<"adding NAPTR replacement 'a'="<<content<<endl,
                      d_slog->info(Logr::Debug, "adding NAPTR replacement", "a", Logging::Loggable(content))));
          }
          else if (flags.find('s') != string::npos) {
            content = naptrContent->getReplacement();
            DLOG(SLOG(g_log<<Logger::Debug<<"adding NAPTR replacement 's'="<<content<<endl,
                      d_slog->info(Logr::Debug, "adding NAPTR replacement", "s", Logging::Loggable(content))));
            B.lookup(QType(QType::SRV), content, d_sd.domain_id, &p);
            while(B.get(dzr)) {
              content=getRR<SRVRecordContent>(dzr.dr)->d_target;
              if(content.isPartOf(d_sd.qname())) {
                lookup.emplace(content);
              }
              dzr.dr.d_place=DNSResourceRecord::ADDITIONAL;
              extraRecords.emplace_back(std::move(dzr));
            }
            content.clear();
          }
          break;
        }
        default:
          continue;
      }
      if(!content.empty() && content.isPartOf(d_sd.qname())) {
        lookup.emplace(content);
      }
    }
  }

  for(auto& rr : extraRecords) {
    r->addRecord(std::move(rr));
  }
  extraRecords.clear();
  // TODO should we have a setting to do this?
  for (auto &rec : r->getServiceRecords()) {
    // Process auto hints
    auto rrc = getRR<SVCBBaseRecordContent>(rec->dr);
    DNSName target = rrc->getTarget().isRoot() ? rec->dr.d_name : rrc->getTarget();

    if (rrc->hasParam(SvcParam::ipv4hint) && rrc->autoHint(SvcParam::ipv4hint)) {
      auto newRRC = rrc->clone();
      if (!newRRC) {
        continue;
      }
      if (s_SVCAutohints) {
        auto hints = getIPAddressFor(target, QType::A);
        if (hints.size() == 0) {
          newRRC->removeParam(SvcParam::ipv4hint);
        } else {
          newRRC->setHints(SvcParam::ipv4hint, hints);
        }
      } else {
        newRRC->removeParam(SvcParam::ipv4hint);
      }
      rrc = newRRC;
      rec->dr.setContent(std::move(newRRC));
    }

    if (rrc->hasParam(SvcParam::ipv6hint) && rrc->autoHint(SvcParam::ipv6hint)) {
      auto newRRC = rrc->clone();
      if (!newRRC) {
        continue;
      }
      if (s_SVCAutohints) {
        auto hints = getIPAddressFor(target, QType::AAAA);
        if (hints.size() == 0) {
          newRRC->removeParam(SvcParam::ipv6hint);
        } else {
          newRRC->setHints(SvcParam::ipv6hint, hints);
        }
      } else {
        newRRC->removeParam(SvcParam::ipv6hint);
      }
      rec->dr.setContent(std::move(newRRC));
    }
  }

  for(const auto& name : lookup) {
    B.lookup(QType(QType::ANY), name, d_sd.domain_id, &p);
    while(B.get(dzr)) {
      if(dzr.dr.d_type == QType::A || dzr.dr.d_type == QType::AAAA) {
        dzr.dr.d_place=DNSResourceRecord::ADDITIONAL;
        r->addRecord(std::move(dzr));
      }
    }
  }
}

vector<ComboAddress> PacketHandler::getIPAddressFor(const DNSName &target, const uint16_t qtype) {
  vector<ComboAddress> ret;
  if (qtype != QType::A && qtype != QType::AAAA) {
    return ret;
  }
  B.lookup(qtype, target, d_sd.domain_id);
  DNSZoneRecord rr;
  while (B.get(rr)) {
    if (qtype == QType::AAAA) {
      auto aaaarrc = getRR<AAAARecordContent>(rr.dr);
      ret.push_back(aaaarrc->getCA());
    } else if (qtype == QType::A) {
      auto arrc = getRR<ARecordContent>(rr.dr);
      ret.push_back(arrc->getCA());
    }
  }
  return ret;
}

void PacketHandler::emitNSEC(std::unique_ptr<DNSPacket>& r, const DNSName& name, const DNSName& next, int mode)
{
  NSECRecordContent nrc;
  nrc.d_next = next;

  nrc.set(QType::NSEC);
  nrc.set(QType::RRSIG);
  if(d_sd.qname() == name) {
    nrc.set(QType::SOA); // 1dfd8ad SOA can live outside the records table
    if(!isPresigned()) {
      auto keyset = d_dk.getKeys(d_sd.zonename);
      for(const auto& value: keyset) {
        if (value.second.published) {
          nrc.set(QType::DNSKEY);
          string publishCDNSKEY;
          d_dk.getPublishCDNSKEY(d_sd.zonename, publishCDNSKEY);
          if (! publishCDNSKEY.empty())
            nrc.set(QType::CDNSKEY);
          string publishCDS;
          d_dk.getPublishCDS(d_sd.zonename, publishCDS);
          if (! publishCDS.empty())
            nrc.set(QType::CDS);
          break;
        }
      }
    }
  }

  DNSZoneRecord rr;
#ifdef HAVE_LUA_RECORDS
  bool first{true};
  bool doLua{false};
#endif

  B.lookup(QType(QType::ANY), name, d_sd.domain_id);
  while(B.get(rr)) {
#ifdef HAVE_LUA_RECORDS
    if (rr.dr.d_type == QType::LUA && first && !isPresigned()) {
      first = false;
      doLua = doLuaRecords();
    }

    if (rr.dr.d_type == QType::LUA && doLua) {
      nrc.set(getRR<LUARecordContent>(rr.dr)->d_type);
    }
    else
#endif
    if (d_doExpandALIAS && rr.dr.d_type == QType::ALIAS) {
      // Set the A and AAAA in the NSEC bitmap so aggressive NSEC
      // does not falsely deny the type for this name.
      // This does NOT add the ALIAS to the bitmap, as that record cannot
      // be requested.
      if (!isPresigned()) {
        nrc.set(QType::A);
        nrc.set(QType::AAAA);
      }
    }
    else if((rr.dr.d_type == QType::DNSKEY || rr.dr.d_type == QType::CDS || rr.dr.d_type == QType::CDNSKEY) && !isPresigned() && !::arg().mustDo("direct-dnskey")) {
      continue;
    }
    else if(rr.dr.d_type == QType::NS || rr.auth) {
      nrc.set(rr.dr.d_type);
    }
  }

  rr.dr.d_name = name;
  rr.dr.d_ttl = d_sd.getNegativeTTL();
  rr.dr.d_type = QType::NSEC;
  rr.dr.setContent(std::make_shared<NSECRecordContent>(std::move(nrc)));
  rr.dr.d_place = (mode == 5 ) ? DNSResourceRecord::ANSWER: DNSResourceRecord::AUTHORITY;
  rr.auth = true;

  r->addRecord(std::move(rr));
}

void PacketHandler::emitNSEC3(DNSPacket& p, std::unique_ptr<DNSPacket>& r, const NSEC3PARAMRecordContent& ns3prc, const DNSName& name, const string& namehash, const string& nexthash, int mode) // NOLINT(readability-identifier-length)
{
  NSEC3RecordContent n3rc;
  n3rc.d_algorithm = ns3prc.d_algorithm;
  n3rc.d_flags = ns3prc.d_flags;
  n3rc.d_iterations = ns3prc.d_iterations;
  n3rc.d_salt = ns3prc.d_salt;
  n3rc.d_nexthash = nexthash;

  DNSZoneRecord rr;

  if(!name.empty()) {
    if (d_sd.qname() == name) {
      n3rc.set(QType::SOA); // 1dfd8ad SOA can live outside the records table
      n3rc.set(QType::NSEC3PARAM);
      if(!isPresigned()) {
        auto keyset = d_dk.getKeys(d_sd.zonename);
        for(const auto& value: keyset) {
          if (value.second.published) {
            n3rc.set(QType::DNSKEY);
            string publishCDNSKEY;
            d_dk.getPublishCDNSKEY(d_sd.zonename, publishCDNSKEY);
            if (! publishCDNSKEY.empty())
              n3rc.set(QType::CDNSKEY);
            string publishCDS;
            d_dk.getPublishCDS(d_sd.zonename, publishCDS);
            if (! publishCDS.empty())
              n3rc.set(QType::CDS);
            break;
          }
        }
      }
    } else if(mode == 6) {
      if (p.qtype.getCode() != QType::CDS) {
        n3rc.set(QType::CDS);
      }
      if (p.qtype.getCode() != QType::CDNSKEY) {
        n3rc.set(QType::CDNSKEY);
      }
    }

#ifdef HAVE_LUA_RECORDS
    bool first{true};
    bool doLua{false};
#endif

    B.lookup(QType(QType::ANY), name, d_sd.domain_id);
    while(B.get(rr)) {
#ifdef HAVE_LUA_RECORDS
      if (rr.dr.d_type == QType::LUA && first && !isPresigned()) {
        first = false;
        doLua = doLuaRecords();
      }

      if (rr.dr.d_type == QType::LUA && doLua) {
        n3rc.set(getRR<LUARecordContent>(rr.dr)->d_type);
      }
      else
#endif
      if (d_doExpandALIAS && rr.dr.d_type == QType::ALIAS) {
        // Set the A and AAAA in the NSEC3 bitmap so aggressive NSEC
        // does not falsely deny the type for this name.
        // This does NOT add the ALIAS to the bitmap, as that record cannot
        // be requested.
        if (!isPresigned()) {
          n3rc.set(QType::A);
          n3rc.set(QType::AAAA);
        }
      }
      else if((rr.dr.d_type == QType::DNSKEY || rr.dr.d_type == QType::CDS || rr.dr.d_type == QType::CDNSKEY) && !isPresigned() && !::arg().mustDo("direct-dnskey")) {
        continue;
      }
      else if(rr.dr.d_type && (rr.dr.d_type == QType::NS || rr.auth)) {
          // skip empty non-terminals
          n3rc.set(rr.dr.d_type);
      }
    }
  }

  const auto numberOfTypesSet = n3rc.numberOfTypesSet();
  if (numberOfTypesSet != 0 && !(numberOfTypesSet == 1 && n3rc.isSet(QType::NS))) {
    n3rc.set(QType::RRSIG);
  }

  rr.dr.d_name = DNSName(toBase32Hex(namehash))+d_sd.qname();
  rr.dr.d_ttl = d_sd.getNegativeTTL();
  rr.dr.d_type=QType::NSEC3;
  rr.dr.setContent(std::make_shared<NSEC3RecordContent>(std::move(n3rc)));
  rr.dr.d_place = (mode == 5 ) ? DNSResourceRecord::ANSWER: DNSResourceRecord::AUTHORITY;
  rr.auth = true;

  r->addRecord(std::move(rr));
}

/*
   mode 0 = No Data Responses, QTYPE is not DS
   mode 1 = No Data Responses, QTYPE is DS
   mode 2 = Wildcard No Data Responses
   mode 3 = Wildcard Answer Responses
   mode 4 = Name Error Responses
   mode 5 = Direct NSEC request
   mode 6 = Authenticated DNSSEC bootstrapping (RFC 9615)
*/
void PacketHandler::addNSECX(DNSPacket& p, std::unique_ptr<DNSPacket>& r, const DNSName& target, const DNSName& wildcard, int mode)
{
  NSEC3PARAMRecordContent ns3rc;
  bool narrow = false;
  if(d_dk.getNSEC3PARAM(d_sd.zonename, &ns3rc, &narrow))  {
    if (mode != 5) // no direct NSEC3 queries, rfc5155 7.2.8
      addNSEC3(p, r, target, wildcard, ns3rc, narrow, mode);
  }
  else {
    addNSEC(p, r, target, wildcard, mode);
  }
}

bool PacketHandler::getNSEC3Hashes(bool narrow, const std::string& hashed, bool decrement, DNSName& unhashed, std::string& before, std::string& after, int mode)
{
  bool ret;
  if(narrow) { // nsec3-narrow
    ret=true;
    before=hashed;
    if(decrement) {
      decrementHash(before);
      unhashed.clear();
    }
    after=hashed;
    incrementHash(after);
  }
  else {
    DNSName hashedName = DNSName(toBase32Hex(hashed));
    DNSName beforeName, afterName;
    if (!decrement && mode >= 2)
      beforeName = hashedName;
    ret=d_sd.db->getBeforeAndAfterNamesAbsolute(d_sd.domain_id, hashedName, unhashed, beforeName, afterName);
    before=fromBase32Hex(beforeName.toString());
    after=fromBase32Hex(afterName.toString());
  }
  return ret;
}

void PacketHandler::addNSEC3(DNSPacket& p, std::unique_ptr<DNSPacket>& r, const DNSName& target, const DNSName& wildcard, const NSEC3PARAMRecordContent& ns3rc, bool narrow, int mode)
{
  DLOG(SLOG(g_log<<"addNSEC3() mode="<<mode<<" auth="<<d_sd.qname()<<" target="<<target<<" wildcard="<<wildcard<<endl,
            d_slog->info("addNSEC3()", "mode", Logging::Loggable(mode), "auth", Logging::Loggable(d_sd.qname()), "target", Logging::Loggable(target), "wildcard", Logging::Loggable(wildcard))));

  if (d_sd.db == nullptr) {
    if(!B.getSOAUncached(d_sd.zonename, d_sd)) {
      DLOG(SLOG(g_log<<"Could not get SOA for domain"<<endl,
                d_slog->info("Could not get SOA for domain")));
      return;
    }
  }

  if (!d_sd.db->doesDNSSEC() && !narrow) {
    // We are in a configuration where the zone is primarily served by a
    // non-DNSSEC-capable backend, but DNSSEC keys have been added to the
    // zone in a second, DNSSEC-capable backend, which caused d_dnssec to
    // be set to true. While it would be nice to support such a zone
    // configuration, we don't. Log a warning and skip DNSSEC processing.
    SLOG(g_log << Logger::Notice << "Backend for zone '" << d_sd.qname() << "' does not support DNSSEC operation, not adding NSEC3 hashes" << endl,
         d_slog->info(Logr::Notice, "Backend does not support DNSSEC operation, not adding NSEC3 hashes", "zone", Logging::Loggable(d_sd.qname())));
    return;
  }

  bool doNextcloser = false;
  string before, after, hashed;
  DNSName unhashed, closest;

  if (mode == 2 || mode == 3 || mode == 4) {
    closest=wildcard;
    closest.chopOff();
  } else
    closest=target;

  // add matching NSEC3 RR
  if (mode != 3) {
    unhashed=(mode == 0 || mode == 1 || mode == 5) ? target : closest;
    hashed=hashQNameWithSalt(ns3rc, unhashed);
    DLOG(SLOG(g_log<<"1 hash: "<<toBase32Hex(hashed)<<" "<<unhashed<<endl,
              d_slog->info("1 hash", "hash", Logging::Loggable(toBase32Hex(hashed)), "unhashed", Logging::Loggable(unhashed))));

    getNSEC3Hashes(narrow, hashed, false, unhashed, before, after, mode);

    if (((mode == 0 && ns3rc.d_flags) ||  mode == 1) && (hashed != before)) {
      DLOG(SLOG(g_log<<"No matching NSEC3, do closest (provable) encloser"<<endl,
                d_slog->info("No matching NSEC3, do closest (provable) encloser")));

      bool doBreak = false;
      DNSZoneRecord rr;
      while( closest.chopOff() && (closest != d_sd.qname()))  { // stop at SOA
        B.lookup(QType(QType::ANY), closest, d_sd.domain_id, &p);
        while (B.get(rr)) {
          if (rr.auth) {
            B.lookupEnd();
            doBreak = true;
            break;
          }
        }
        if(doBreak)
          break;
      }
      doNextcloser = true;
      unhashed=closest;
      hashed=hashQNameWithSalt(ns3rc, unhashed);
      DLOG(SLOG(g_log<<"1 hash: "<<toBase32Hex(hashed)<<" "<<unhashed<<endl,
                d_slog->info("1 hash", "hash", Logging::Loggable(toBase32Hex(hashed)), "unhashed", Logging::Loggable(unhashed))));

      getNSEC3Hashes(narrow, hashed, false, unhashed, before, after);
    }

    if (!after.empty()) {
      DLOG(SLOG(g_log<<"Done calling for matching, hashed: '"<<toBase32Hex(hashed)<<"' before='"<<toBase32Hex(before)<<"', after='"<<toBase32Hex(after)<<"'"<<endl,
                d_slog->info("Done calling for matching", "hash", Logging::Loggable(toBase32Hex(hashed)), "before", Logging::Loggable(toBase32Hex(before)), "after", Logging::Loggable(toBase32Hex(after)))));
      emitNSEC3(p, r, ns3rc, unhashed, before, after, mode);
    }
  }

  // add covering NSEC3 RR
  if ((mode >= 2 && mode <= 4) || doNextcloser) {
    DNSName next(target);
    do {
      unhashed=next;
    }
    while( next.chopOff() && !(next==closest));

    hashed=hashQNameWithSalt(ns3rc, unhashed);
    DLOG(SLOG(g_log<<"2 hash: "<<toBase32Hex(hashed)<<" "<<unhashed<<endl,
              d_slog->info("2 hash", "hash", Logging::Loggable(toBase32Hex(hashed)), "unhashed", Logging::Loggable(unhashed))));

    getNSEC3Hashes(narrow, hashed, true, unhashed, before, after);
    DLOG(SLOG(g_log<<"Done calling for covering, hashed: '"<<toBase32Hex(hashed)<<"' before='"<<toBase32Hex(before)<<"', after='"<<toBase32Hex(after)<<"'"<<endl,
              d_slog->info("Done calling for covering", "hash", Logging::Loggable(toBase32Hex(hashed)), "before", Logging::Loggable(toBase32Hex(before)), "after", Logging::Loggable(toBase32Hex(after)))));
    emitNSEC3(p, r, ns3rc, unhashed, before, after, mode);
  }

  // wildcard denial
  if (mode == 2 || mode == 4) {
    unhashed=g_wildcarddnsname+closest;

    hashed=hashQNameWithSalt(ns3rc, unhashed);
    DLOG(SLOG(g_log<<"3 hash: "<<toBase32Hex(hashed)<<" "<<unhashed<<endl,
              d_slog->info("3 hash", "hashed", Logging::Loggable(toBase32Hex(hashed)), "unhashed", Logging::Loggable(unhashed))));

    getNSEC3Hashes(narrow, hashed, (mode != 2), unhashed, before, after);
    DLOG(SLOG(g_log<<"Done calling for '*', hashed: '"<<toBase32Hex(hashed)<<"' before='"<<toBase32Hex(before)<<"', after='"<<toBase32Hex(after)<<"'"<<endl,
              d_slog->info("Done calling for '*'", "hash", Logging::Loggable(toBase32Hex(hashed)), "before", Logging::Loggable(toBase32Hex(before)), "after", Logging::Loggable(toBase32Hex(after)))));
    emitNSEC3(p, r, ns3rc, unhashed, before, after, mode);
  }
}

void PacketHandler::addNSEC(DNSPacket& /* p */, std::unique_ptr<DNSPacket>& r, const DNSName& target, const DNSName& wildcard, int mode)
{
  DLOG(SLOG(g_log<<"addNSEC() mode="<<mode<<" auth="<<d_sd.qname()<<" target="<<target<<" wildcard="<<wildcard<<endl,
            d_slog->info("addNSEC()", "mode", Logging::Loggable(mode), "auth", Logging::Loggable(d_sd.qname()), "target", Logging::Loggable(target), "wildcard", Logging::Loggable(wildcard))));

  if (d_sd.db == nullptr) {
    if(!B.getSOAUncached(d_sd.zonename, d_sd)) {
      DLOG(SLOG(g_log<<"Could not get SOA for domain"<<endl,
                d_slog->info("Could not get SOA for domain")));
      return;
    }
  }

  if (!d_sd.db->doesDNSSEC()) {
    // We are in a configuration where the zone is primarily served by a
    // non-DNSSEC-capable backend, but DNSSEC keys have been added to the
    // zone in a second, DNSSEC-capable backend, which caused d_dnssec to
    // be set to true. While it would be nice to support such a zone
    // configuration, we don't. Log a warning and skip DNSSEC processing.
    SLOG(g_log << Logger::Notice << "Backend for zone '" << d_sd.qname() << "' does not support DNSSEC operation, not adding NSEC records" << endl,
         d_slog->info(Logr::Notice, "Backend does not support DNSSEC operation, not adding NSEC records", "zone", Logging::Loggable(d_sd.qname())));
    return;
  }

  DNSName before,after;
  d_sd.db->getBeforeAndAfterNames(d_sd.domain_id, d_sd.zonename, target, before, after);
  if (mode != 5 || before == target)
    emitNSEC(r, before, after, mode);

  if (mode == 2 || mode == 4) {
    // wildcard NO-DATA or wildcard denial
    before.clear();
    DNSName closest(wildcard);
    if (mode == 4) {
      closest.chopOff();
      closest.prependRawLabel("*");
    }
    d_sd.db->getBeforeAndAfterNames(d_sd.domain_id, d_sd.zonename, closest, before, after);
    emitNSEC(r, before, after, mode);
  }
  return;
}

/* Semantics:

- only one backend owns the SOA of a zone
- only one AXFR per zone at a time - double startTransaction should fail
- backends implement transaction semantics

How BindBackend implements this:
   startTransaction makes a file
   feedRecord sends everything to that file
   commitTransaction moves that file atomically over the regular file, and triggers a reload
   abortTransaction removes the file

How SQL backends implement this:
   startTransaction starts a sql transaction, which also deletes all records if requested
   feedRecord is an insert statement
   commitTransaction commits the transaction
   abortTransaction aborts it

*/

int PacketHandler::tryAutoPrimary(const DNSPacket& p, const DNSName& tsigkeyname)
{
  if(p.d_tcp)
  {
    // do it right now if the client is TCP
    // rarely happens
    return tryAutoPrimarySynchronous(p, tsigkeyname);
  }
  else
  {
    // queue it if the client is on UDP
    Communicator.addTryAutoPrimaryRequest(p);
    return 0;
  }
}

int PacketHandler::tryAutoPrimarySynchronous(const DNSPacket& p, const DNSName& tsigkeyname)
{
  ComboAddress remote = p.getInnerRemote();
  if(p.hasEDNSSubnet() && pdns::isAddressTrustedNotificationProxy(remote)) {
    remote = p.getRealRemote().getNetwork();
  }
  else {
    remote = p.getInnerRemote();
  }
  remote.setPort(53);

  Resolver::res_t nsset;
  try {
    Resolver resolver;
    uint32_t theirserial;
    resolver.getSoaSerial(remote, p.qdomain, &theirserial);
    resolver.resolve(remote, p.qdomain, QType::NS, &nsset);
  }
  catch(ResolverException &re) {
    SLOG(g_log<<Logger::Error<<"Error resolving SOA or NS for "<<p.qdomain<<" at: "<< remote <<": "<<re.reason<<endl,
         d_slog->error(Logr::Error, re.reason, "Error resolving SOA or NS", "domain", Logging::Loggable(p.qdomain), "remote", Logging::Loggable(remote)));
    return RCode::ServFail;
  }

  // check if the returned records are NS records
  bool haveNS=false;
  for(const auto& ns: nsset) {
    if(ns.qtype==QType::NS)
      haveNS=true;
  }

  if(!haveNS) {
    SLOG(g_log << Logger::Error << "While checking for autoprimary, did not find NS for " << p.qdomain << " at: " << remote << endl,
         d_slog->error(Logr::Error, "While checking for autoprimary, did not find NS", "domain", Logging::Loggable(p.qdomain), "remote", Logging::Loggable(remote)));
    return RCode::ServFail;
  }

  string nameserver, account;
  DNSBackend *db;

  if (!::arg().mustDo("allow-unsigned-autoprimary") && tsigkeyname.empty()) {
    SLOG(g_log << Logger::Error << "Received unsigned NOTIFY for " << p.qdomain << " from potential autoprimary " << remote << ". Refusing." << endl,
         d_slog->error(Logr::Error, "Received unsigned NOTIFY", "domain", Logging::Loggable(p.qdomain), "potential autoprimary", Logging::Loggable(remote)));
    return RCode::Refused;
  }

  ZoneName zonename(p.qdomain);
  if (!B.autoPrimaryBackend(remote.toString(), zonename, nsset, &nameserver, &account, &db)) {
    if (g_slogStructured) {
      std::ostringstream oss;
      bool first{false};
      for (const auto& drr: nsset) {
        if (drr.qtype == QType::NS) {
          if (first) {
            oss << ",";
            first = false;
          }
          oss << drr.content;
        }
      }
      d_slog->error(Logr::Error, "Unable to find backend willing to host domain", "domain", Logging::Loggable(p.qdomain), "potential autoprimary", Logging::Loggable(remote), "remote nameservers", Logging::Loggable(oss.str()));
    }
    else {
      g_log << Logger::Error << "Unable to find backend willing to host " << p.qdomain << " for potential autoprimary " << remote << ". Remote nameservers: " << endl;
      for(const auto& rr: nsset) {
        if(rr.qtype==QType::NS) {
          g_log<<Logger::Error<<rr.content<<endl;
        }
      }
    }
    return RCode::Refused;
  }
  try {
    db->createSecondaryDomain(remote.toString(), zonename, nameserver, account);
    DomainInfo di;
    if (!db->getDomainInfo(zonename, di, false)) {
      SLOG(g_log << Logger::Error << "Failed to create " << zonename << " for potential autoprimary " << remote << endl,
           d_slog->error(Logr::Error, "Failed to create zone for potential autoprimary", "zone", Logging::Loggable(zonename), "potential autoprimary", Logging::Loggable(remote)));
      return RCode::ServFail;
    }
    g_zoneCache.add(zonename, di.id);
    if (tsigkeyname.empty() == false) {
      vector<string> meta;
      meta.push_back(tsigkeyname.toStringNoDot());
      db->setDomainMetadata(zonename, "AXFR-MASTER-TSIG", meta);
    }
  }
  catch(PDNSException& ae) {
    SLOG(g_log << Logger::Error << "Database error trying to create " << zonename << " for potential autoprimary " << remote << ": " << ae.reason << endl,
         d_slog->error(Logr::Error, ae.reason, "Database error trying to create zone for potential autoprimary", "zone", Logging::Loggable(zonename), "potential autoprimary", Logging::Loggable(remote)));
    return RCode::ServFail;
  }
  SLOG(g_log << Logger::Warning << "Created new secondary zone '" << zonename << "' from autoprimary " << remote << endl,
       d_slog->info(Logr::Warning, "Created new secondary zone from autoprimary", "zone", Logging::Loggable(zonename), "autoprimary", Logging::Loggable(remote)));
  return RCode::NoError;
}

int PacketHandler::processNotify(const DNSPacket& p)
{
  ZoneName zonename(p.qdomain);
  /* now what?
     was this notification from an approved address?
     was this notification approved by TSIG?
     We determine our internal SOA id (via UeberBackend)
     We determine the SOA at our (known) primary
     if primary is higher -> do stuff
  */

  SLOG(g_log<<Logger::Debug<<"Received NOTIFY for "<<zonename<<" from "<<p.getRemoteString()<<endl,
       d_slog->info(Logr::Debug, "Received NOTIFY", "zone", Logging::Loggable(zonename), "from", Logging::Loggable(p.getRemoteString())));

  if(!::arg().mustDo("secondary") && s_forwardNotify.empty()) {
    SLOG(g_log << Logger::Warning << "Received NOTIFY for " << zonename << " from " << p.getRemoteString() << " but secondary support is disabled in the configuration" << endl,
         d_slog->info(Logr::Warning, "Received NOTIFY but secondary support is currently disabled", "zone", Logging::Loggable(zonename), "from", Logging::Loggable(p.getRemoteString())));
    return RCode::Refused;
  }

  // Sender verification
  //
  if(!s_allowNotifyFrom.match(p.getInnerRemote()) || p.d_havetsig) {
    if (p.d_havetsig && p.getTSIGKeyname().empty() == false) {
        SLOG(g_log<<Logger::Notice<<"Received secure NOTIFY for "<<zonename<<" from "<<p.getRemoteString()<<", with TSIG key '"<<p.getTSIGKeyname()<<"'"<<endl,
             d_slog->info(Logr::Notice, "Received secure NOTIFY", "zone", Logging::Loggable(zonename), "from", Logging::Loggable(p.getRemoteString()), "tsig key", Logging::Loggable(p.getTSIGKeyname())));
    } else {
      SLOG(g_log<<Logger::Warning<<"Received NOTIFY for "<<zonename<<" from "<<p.getRemoteString()<<" but the remote is not providing a TSIG key or in allow-notify-from (Refused)"<<endl,
           d_slog->info(Logr::Warning, "Received NOTIFY but remote is either not providing a TSIG key or not listed in allow-notify-from (Refused)", "zone", Logging::Loggable(zonename), "from", Logging::Loggable(p.getRemoteString())));
      return RCode::Refused;
    }
  }

  if ((!::arg().mustDo("allow-unsigned-notify") && !p.d_havetsig) || p.d_havetsig) {
    if (!p.d_havetsig) {
      SLOG(g_log<<Logger::Warning<<"Received unsigned NOTIFY for "<<zonename<<" from "<<p.getRemoteString()<<" while a TSIG key was required (Refused)"<<endl,
           d_slog->info(Logr::Warning, "Received unsigned NOTIFY while a TSIG key was required (Refused)", "zone", Logging::Loggable(zonename), "from", Logging::Loggable(p.getRemoteString())));
      return RCode::Refused;
    }
    vector<string> meta;
    if (B.getDomainMetadata(zonename,"AXFR-MASTER-TSIG",meta) && !meta.empty()) {
      DNSName expected{meta[0]};
      if (p.getTSIGKeyname() != expected) {
        SLOG(g_log<<Logger::Warning<<"Received secure NOTIFY for "<<zonename<<" from "<<p.getRemoteString()<<": expected TSIG key '"<<expected<<"', got '"<<p.getTSIGKeyname()<<"' (Refused)"<<endl,
           d_slog->info(Logr::Warning, "Received secure NOTIFY with unexpected TSIG key (Refused)", "zone", Logging::Loggable(zonename), "from", Logging::Loggable(p.getRemoteString()), "expected", Logging::Loggable(expected), "key received", Logging::Loggable(p.getTSIGKeyname())));
        return RCode::Refused;
      }
    }
  }

  // Domain verification
  //
  DomainInfo di;
  if(!B.getDomainInfo(zonename, di, false) || di.backend == nullptr) {
    if(::arg().mustDo("autosecondary")) {
      SLOG(g_log << Logger::Warning << "Received NOTIFY for " << zonename << " from " << p.getRemoteString() << " for which we are not authoritative, trying autoprimary" << endl,
           d_slog->info(Logr::Warning, "Received NOTIFY for a zone for which we are not authoritative, trying autoprimary", "zone", Logging::Loggable(zonename), "from", Logging::Loggable(p.getRemoteString())));
      return tryAutoPrimary(p, p.getTSIGKeyname());
    }
    SLOG(g_log<<Logger::Notice<<"Received NOTIFY for "<<zonename<<" from "<<p.getRemoteString()<<" for which we are not authoritative (Refused)"<<endl,
         d_slog->info(Logr::Notice, "Received NOTIFY for a zone for which we are not authoritative (Refused)", "zone", Logging::Loggable(zonename), "from", Logging::Loggable(p.getRemoteString())));
    return RCode::Refused;
  }

  if(pdns::isAddressTrustedNotificationProxy(p.getInnerRemote())) {
    if (di.primaries.empty()) {
      SLOG(g_log << Logger::Warning << "Received NOTIFY for " << zonename << " from trusted-notification-proxy " << p.getRemoteString() << ", zone does not have any primaries defined (Refused)" << endl,
           d_slog->info(Logr::Warning, "Received NOTIFY from trusted-notification-proxy, but zone does not have any primaries defined (Refused)", "zone", Logging::Loggable(zonename), "from", Logging::Loggable(p.getRemoteString())));
      return RCode::Refused;
    }
    SLOG(g_log<<Logger::Notice<<"Received NOTIFY for "<<zonename<<" from trusted-notification-proxy "<<p.getRemoteString()<<endl,
         d_slog->info(Logr::Warning, "Received NOTIFY from trusted-notification-proxy", "zone", Logging::Loggable(zonename), "from", Logging::Loggable(p.getRemoteString())));
  }
  else if (::arg().mustDo("primary") && di.isPrimaryType()) {
    SLOG(g_log << Logger::Warning << "Received NOTIFY for " << zonename << " from " << p.getRemoteString() << " but we are primary (Refused)" << endl,
         d_slog->info(Logr::Warning, "Received NOTIFY but we are primary (Refused)", "zone", Logging::Loggable(zonename), "from", Logging::Loggable(p.getRemoteString())));
    return RCode::Refused;
  }
  else if (!di.isPrimary(p.getInnerRemote())) {
    SLOG(g_log << Logger::Warning << "Received NOTIFY for " << zonename << " from " << p.getRemoteString() << " which is not a primary (Refused)" << endl,
         d_slog->info(Logr::Warning, "Received NOTIFY but not a primary zone (Refused)", "zone", Logging::Loggable(zonename), "from", Logging::Loggable(p.getRemoteString())));
    return RCode::Refused;
  }

  if(!s_forwardNotify.empty()) {
    set<string> forwardNotify(s_forwardNotify);
    for(const auto & forward : forwardNotify) {
      SLOG(g_log<<Logger::Notice<<"Relaying notification of domain "<<zonename<<" from "<<p.getRemoteString()<<" to "<<forward<<endl,
         d_slog->info(Logr::Notice, "Relaying notification", "zone", Logging::Loggable(zonename), "from", Logging::Loggable(p.getRemoteString()), "to", Logging::Loggable(forward)));
      Communicator.notify(zonename,forward);
    }
  }

  if(::arg().mustDo("secondary")) {
    SLOG(g_log<<Logger::Notice<<"Received NOTIFY for "<<zonename<<" from "<<p.getRemoteString()<<" - queueing check"<<endl,
         d_slog->info(Logr::Notice, "Received NOTIFY, queueing check", "zone", Logging::Loggable(zonename), "from", Logging::Loggable(p.getRemoteString())));
    di.receivedNotify = true;
    Communicator.addSecondaryCheckRequest(di, p.getInnerRemote());
  }
  return 0;
}

static bool validDNSName(const DNSName& name)
{
  if (!g_8bitDNS) {
    return name.has8bitBytes() == false;
  }
  return true;
}

std::unique_ptr<DNSPacket> PacketHandler::question(DNSPacket& p)
{
  std::unique_ptr<DNSPacket> ret{nullptr};

  if(d_pdl)
  {
    ret=d_pdl->prequery(p);
    if(ret)
      return ret;
  }

  if(p.d.rd) {
    static AtomicCounter &rdqueries=*S.getPointer("rd-queries");
    rdqueries++;
  }

  return doQuestion(p);
}


void PacketHandler::makeNXDomain(DNSPacket& p, std::unique_ptr<DNSPacket>& r, const DNSName& target, const DNSName& wildcard)
{
  DNSZoneRecord rr;
  rr=makeEditedDNSZRFromSOAData(d_dk, d_sd, DNSResourceRecord::AUTHORITY);
  rr.dr.d_ttl=d_sd.getNegativeTTL();
  r->addRecord(std::move(rr));

  if(d_dnssec) {
    addNSECX(p, r, target, wildcard, 4);
  }

  r->setRcode(RCode::NXDomain);
}

void PacketHandler::makeNOError(DNSPacket& p, std::unique_ptr<DNSPacket>& r, const DNSName& target, const DNSName& wildcard, int mode)
{
  DNSZoneRecord rr;
  rr=makeEditedDNSZRFromSOAData(d_dk, d_sd, DNSResourceRecord::AUTHORITY);
  rr.dr.d_ttl=d_sd.getNegativeTTL();
  r->addRecord(std::move(rr));

  if(d_dnssec) {
    addNSECX(p, r, target, wildcard, mode);
  }

  S.inc("noerror-packets");
  S.ringAccount("noerror-queries", p.qdomain, p.qtype);
}


bool PacketHandler::addDSforNS(DNSPacket& p, std::unique_ptr<DNSPacket>& r, const DNSName& dsname)
{
  //cerr<<"Trying to find a DS for '"<<dsname<<"', domain_id = "<<d_sd.domain_id<<endl;
  B.lookup(QType(QType::DS), dsname, d_sd.domain_id, &p);
  DNSZoneRecord rr;
  bool gotOne=false;
  while(B.get(rr)) {
    gotOne=true;
    rr.dr.d_place = DNSResourceRecord::AUTHORITY;
    r->addRecord(std::move(rr));
  }
  return gotOne;
}

bool PacketHandler::tryReferral(DNSPacket& p, std::unique_ptr<DNSPacket>& r, const DNSName &target, bool retargeted)
{
  vector<DNSZoneRecord> rrset = getBestReferralNS(p, target);
  if(rrset.empty())
    return false;

  DNSName name = rrset.begin()->dr.d_name;
  for(auto& rr: rrset) {
    rr.dr.d_place=DNSResourceRecord::AUTHORITY;
    r->addRecord(std::move(rr));
  }
  if(!retargeted)
    r->setA(false);

  if(isSecuredZone() && !addDSforNS(p, r, name) && d_dnssec) {
    addNSECX(p, r, name, DNSName(), 1);
  }

  return true;
}

void PacketHandler::completeANYRecords(DNSPacket& p, std::unique_ptr<DNSPacket>& r, const DNSName &target)
{
  addNSECX(p, r, target, DNSName(), 5);
  if(d_sd.qname() == p.qdomain) {
    if(!isPresigned()) {
      addDNSKEY(p, r);
      addCDNSKEY(p, r);
      addCDS(p, r);
    }
    addNSEC3PARAM(p, r);
  }
}

bool PacketHandler::tryAuthSignal(DNSPacket& p, std::unique_ptr<DNSPacket>& r, DNSName &target) // NOLINT(readability-identifier-length)
{
  DLOG(SLOG(g_log<<Logger::Warning<<"Let's try authenticated DNSSEC bootstrapping (RFC 9615) ..."<<endl,
            d_slog->info(Logr::Warning, "Let's try authenticated DNSSEC bootstrapping (RFC 9615) ...")));
  if(!d_sd.zonename.operator const DNSName&().hasLabels() || !pdns_iequals(d_sd.zonename.operator const DNSName&().getRawLabel(0), "_signal") || !d_dk.isSignalingZone(d_sd.zonename)) {
    return false;
  }

  // Check that we're doing online signing in narrow mode (as we don't know next owner names)
  if(!isSecuredZone() || isPresigned()) {
    SLOG(g_log << Logger::Warning << "Signaling zone '" << d_sd.zonename << "' must be secured (but not presigned!); synthesis disabled (" << target << "/" << p.qtype << " from " << p.getRemoteString() << ")" << endl,
         d_slog->info(Logr::Warning, "Signaling zone must be secured (but not presigned!); synthesis disabled", "zone", Logging::Loggable(d_sd.zonename), "target", Logging::Loggable(p.qtype), "from", Logging::Loggable(p.getRemoteString())));
    return false;
  }
  bool narrow{false};
  if (!d_dk.getNSEC3PARAM(d_sd.zonename, nullptr, &narrow) || !narrow) {
    SLOG(g_log << Logger::Warning << "Signaling zone '" << d_sd.zonename << "' must use NSEC3 narrow; synthesis disabled (" << target << "/" << p.qtype << ")" << " from " << p.getRemoteString() << ")" << endl,
         d_slog->info(Logr::Warning, "Signaling zone must use NSEC3 narrow; synthesis disabled", "zone", Logging::Loggable(d_sd.zonename), "target", Logging::Loggable(p.qtype), "from", Logging::Loggable(p.getRemoteString())));
    return false;
  }

  // Check for prefix mismatch
  if(!target.hasLabels() || !pdns_iequals(target.getRawLabel(0), "_dsboot")) {
    makeNOError(p, r, target, DNSName(), 0); // could be ENT
    return true;
  }

  // Check for qtype match
  if(!(p.qtype.getCode() == QType::CDS || p.qtype.getCode() == QType::CDNSKEY)) {
    // We don't know at this point whether CDS/CDNSKEY exist, so let's add both to DoE type map
    makeNOError(p, r, target, DNSName(), 6);
    return true;
  }

  // Extract target zone name and fetch zone
  SOAData zone_sd;
  ZoneName zone = ZoneName(target.makeRelative(d_sd.zonename));
  zone.chopOff();
  // Zone must exist, but need not be secured (could be using front-sign setup)
  if(!B.getAuth(zone, p.qtype, &zone_sd, p.getRealRemote()) || zone_sd.zonename != zone) {
    return false; // Bootstrap target zone unknown, NXDOMAIN at _dsboot is OK
  }

  // Insert synthetic response
  bool haveOne = false;
  bool autoPublish = !d_dk.isPresigned(zone);
  std::string val;
  if(p.qtype.getCode() == QType::CDS) {
    d_dk.getPublishCDS(zone, val);
    autoPublish &= !val.empty();
    if(autoPublish) {
      haveOne = addCDS(p, r, zone_sd);
    }
  } else if(p.qtype.getCode() == QType::CDNSKEY) {
    d_dk.getPublishCDNSKEY(zone, val);
    autoPublish &= !val.empty();
    if(autoPublish) {
      haveOne = addCDNSKEY(p, r, zone_sd);
    }
  }
  if(!autoPublish) {
      DNSZoneRecord rec;
      B.lookup(p.qtype.getCode(), DNSName(zone), zone_sd.domain_id, &p);
      while(B.get(rec)) {
        rec.dr.d_name = p.qdomain;
        r->addRecord(std::move(rec));
        haveOne=true;
      }
    }
  if(!haveOne) {
    makeNOError(p, r, target, DNSName(), 6); // other type might exist
  }
  return true;
}

bool PacketHandler::tryDNAME(DNSPacket& p, std::unique_ptr<DNSPacket>& r, DNSName &target)
{
  if(!d_doDNAME)
    return false;
  DLOG(SLOG(g_log<<Logger::Warning<<"Let's try DNAME.."<<endl,
            d_slog->info(Logr::Warning, "Let's try DNAME...")));
  vector<DNSZoneRecord> rrset;
  try {
    getBestDNAMESynth(p, target, rrset);
    if(!rrset.empty()) {
      for (auto& record : rrset) {
        record.dr.d_place = DNSResourceRecord::ANSWER;
        r->addRecord(std::move(record));
      }
      return true;
    }
  } catch (const std::range_error &e) {
    // Add the DNAME regardless, but throw to let the caller know we could not
    // synthesize a CNAME
    if(!rrset.empty()) {
      for (auto& record : rrset) {
        record.dr.d_place = DNSResourceRecord::ANSWER;
        r->addRecord(std::move(record));
      }
    }
    throw e;
  }
  return false;
}
bool PacketHandler::tryWildcard(DNSPacket& p, std::unique_ptr<DNSPacket>& r, DNSName &target, DNSName &wildcard, bool& retargeted, bool& nodata)
{
  retargeted = nodata = false;
  DNSName bestmatch;

  vector<DNSZoneRecord> rrset;
  if(!getBestWildcard(p, target, wildcard, &rrset))
    return false;

  if(rrset.empty()) {
    DLOG(SLOG(g_log<<"Wildcard matched something, but not of the correct type"<<endl,
              d_slog->info("Wildcard matched something, but not of the correct type")));
    nodata=true;
  }
  else {
    bestmatch = target;
    for(auto& rr: rrset) {
      rr.wildcardname = rr.dr.d_name;
      rr.dr.d_name = bestmatch;

      if(rr.dr.d_type == QType::CNAME)  {
        retargeted=true;
        target=getRR<CNAMERecordContent>(rr.dr)->getTarget();
      }

      rr.dr.d_place=DNSResourceRecord::ANSWER;
      r->addRecord(std::move(rr));
    }
  }
  if(d_dnssec && !nodata) {
    addNSECX(p, r, bestmatch, wildcard, 3);
  }

  return true;
}

//! Called by the Distributor to ask a question. Returns 0 in case of an error
std::unique_ptr<DNSPacket> PacketHandler::doQuestion(DNSPacket& pkt)
{
  bool noCache=false;

  if(pkt.d.qr) { // QR bit from dns packet (thanks RA from N)
    if(d_logDNSDetails) {
      SLOG(g_log<<Logger::Error<<"Received an answer (non-query) packet from "<<pkt.getRemoteString()<<", dropping"<<endl,
           d_slog->error(Logr::Error, "Received an answer (non-query) packet, dropping", "from", Logging::Loggable(pkt.getRemoteString())));
    }
    S.inc("corrupt-packets");
    S.ringAccount("remotes-corrupt", pkt.getInnerRemote());
    return nullptr;
  }

  if(pkt.d.tc) { // truncated query. MOADNSParser would silently parse this packet in an incomplete way.
    if(d_logDNSDetails) {
      SLOG(g_log<<Logger::Error<<"Received truncated query packet from "<<pkt.getRemoteString()<<", dropping"<<endl,
           d_slog->error(Logr::Error, "Received truncated query packet, dropping", "from", Logging::Loggable(pkt.getRemoteString())));
    }
    S.inc("corrupt-packets");
    S.ringAccount("remotes-corrupt", pkt.getInnerRemote());
    return nullptr;
  }

  if (pkt.hasEDNS()) {
    if(pkt.getEDNSVersion() > 0) {
      auto resp = pkt.replyPacket();
      // PacketWriter::addOpt will take care of setting this correctly in the packet
      resp->setEDNSRcode(ERCode::BADVERS);
      return resp;
    }
    if (pkt.hasEDNSCookie()) {
      if (!pkt.hasWellFormedEDNSCookie()) {
        auto resp = pkt.replyPacket();
        resp->setRcode(RCode::FormErr);
        return resp;
      }
      if (!pkt.hasValidEDNSCookie() && !pkt.d_tcp) {
        auto resp = pkt.replyPacket();
        resp->setEDNSRcode(ERCode::BADCOOKIE);
        return resp;
      }
    }
  }

  if(pkt.d_havetsig) {
    DNSName tsigkeyname;
    string secret;
    TSIGRecordContent trc;
    if (!checkForCorrectTSIG(pkt, &tsigkeyname, &secret, &trc)) {
      auto resp=pkt.replyPacket();  // generate an empty reply packet
      if(d_logDNSDetails) {
        SLOG(g_log<<Logger::Error<<"Received a TSIG signed message with a non-validating key"<<endl,
             d_slog->error(Logr::Error, "Received a TSIG-signed message with a non-validating key"));
      }
      // RFC3007 describes that a non-secure message should be sending Refused for DNS Updates
      if (pkt.d.opcode == Opcode::Update) {
        resp->setRcode(RCode::Refused);
      }
      else {
        resp->setRcode(RCode::NotAuth);
      }
      return resp;
    }
    getTSIGHashEnum(trc.d_algoName, pkt.d_tsig_algo);
#ifdef ENABLE_GSS_TSIG
    if (g_doGssTSIG && pkt.d_tsig_algo == TSIG_GSS) {
      GssContext gssctx(tsigkeyname);
      if (!gssctx.getPeerPrincipal(pkt.d_peer_principal)) {
        SLOG(g_log<<Logger::Warning<<"Failed to extract peer principal from GSS context with keyname '"<<tsigkeyname<<"'"<<endl,
             d_slog->info(Logr::Warning, "Failed to extract peer principal from GSS context", "key", Logging::Loggable(tsigkeyname)));
      }
    }
#endif
    pkt.setTSIGDetails(trc, tsigkeyname, secret, trc.d_mac); // this will get copied by replyPacket()
    noCache=true;
  }

  if (pkt.qtype == QType::TKEY) {
    auto resp=pkt.replyPacket();  // generate an empty reply packet, possibly with TSIG details inside
    this->tkeyHandler(pkt, resp);
    return resp;
  }

  try {

    // XXX FIXME do this in DNSPacket::parse ?

    if(!validDNSName(pkt.qdomain)) {
      if(d_logDNSDetails) {
        SLOG(g_log<<Logger::Error<<"Received a malformed qdomain from "<<pkt.getRemoteString()<<", '"<<pkt.qdomain<<"': sending servfail"<<endl,
             d_slog->error(Logr::Error, "Received a malformed qdomain; sending servfail", "qdomain", Logging::Loggable(pkt.qdomain), "from", Logging::Loggable(pkt.getRemoteString())));
      }
      S.inc("corrupt-packets");
      S.ringAccount("remotes-corrupt", pkt.getInnerRemote());
      S.inc("servfail-packets");
      auto resp=pkt.replyPacket();  // generate an empty reply packet
      resp->setRcode(RCode::ServFail);
      return resp;
    }

    using opcodeHandler = std::unique_ptr<DNSPacket> (PacketHandler::*)(DNSPacket&, bool);
    const static std::array<opcodeHandler, 16> opcodeHandlers = {
      &PacketHandler::opcodeQuery,
      &PacketHandler::opcodeNotImplemented,
      &PacketHandler::opcodeNotImplemented,
      &PacketHandler::opcodeNotImplemented,
      &PacketHandler::opcodeNotify,
      &PacketHandler::opcodeUpdate,
      &PacketHandler::opcodeNotImplemented,
      &PacketHandler::opcodeNotImplemented,

      &PacketHandler::opcodeNotImplemented,
      &PacketHandler::opcodeNotImplemented,
      &PacketHandler::opcodeNotImplemented,
      &PacketHandler::opcodeNotImplemented,
      &PacketHandler::opcodeNotImplemented,
      &PacketHandler::opcodeNotImplemented,
      &PacketHandler::opcodeNotImplemented,
      &PacketHandler::opcodeNotImplemented
    };

    return (this->*(opcodeHandlers.at(pkt.d.opcode)))(pkt, noCache);
  }
  catch(const DBException &e) {
    SLOG(g_log<<Logger::Error<<"Backend reported condition which prevented lookup ("+e.reason+") sending out servfail"<<endl,
         d_slog->error(Logr::Error, e.reason, "Backend reported condition which prevented lookup; sending servfail"));
    auto resp=pkt.replyPacket(); // generate an empty reply packet
    resp->setRcode(RCode::ServFail);
    S.inc("servfail-packets");
    S.ringAccount("servfail-queries", pkt.qdomain, pkt.qtype);
    return resp;
  }
  catch(const PDNSException &e) {
    SLOG(g_log<<Logger::Error<<"Backend reported permanent error which prevented lookup ("+e.reason+"), aborting"<<endl,
         d_slog->error(Logr::Error, e.reason, "Backend reported permanent error which prevented lookup; aborting"));
    throw; // we WANT to die at this point
  }
  catch(const std::exception &e) {
    SLOG(g_log<<Logger::Error<<"Exception building answer packet for "<<pkt.qdomain<<"/"<<pkt.qtype.toString()<<" ("<<e.what()<<") sending out servfail"<<endl,
         d_slog->error(Logr::Error, e.what(), "Exception building answer packet; sending servfail", "query", Logging::Loggable(pkt.qdomain), "type", Logging::Loggable(pkt.qtype)));
    auto resp=pkt.replyPacket(); // generate an empty reply packet
    resp->setRcode(RCode::ServFail);
    S.inc("servfail-packets");
    S.ringAccount("servfail-queries", pkt.qdomain, pkt.qtype);
    return resp;
  }
}

bool PacketHandler::opcodeQueryInner(DNSPacket& pkt, queryState &state)
{
  state.r=pkt.replyPacket();  // generate an empty reply packet, possibly with TSIG details inside

#if 0
  SLOG(g_log<<Logger::Warning<<"Query for '"<<pkt.qdomain<<"' "<<pkt.qtype.toString()<<" from "<<pkt.getRemoteString()<< " (tcp="<<pkt.d_tcp<<")"<<endl,
       d_slog->info(Logr::Warning, "Query", "domain", Logging::Loggable(pkt.qdomain), "type", Logging::Loggable(pkt.qtype), "from", Logging::Loggable(pkt.getRemoteString()), "tcp", Logging::Loggable(pkt.d_tcp)));
#endif

  if(pkt.qtype.getCode()==QType::IXFR) {
    state.r->setRcode(RCode::Refused);
    return false;
  }

  state.target=pkt.qdomain;

  // catch chaos qclass requests
  if(pkt.qclass == QClass::CHAOS) {
    return doChaosRequest(pkt,state.r,state.target) != 0;
  }

  // we only know about qclass IN (and ANY), send Refused for everything else.
  if(pkt.qclass != QClass::IN && pkt.qclass!=QClass::ANY) {
    state.r->setRcode(RCode::Refused);
    return false;
  }

  // send TC for udp ANY query if any-to-tcp is enabled.
  if(pkt.qtype.getCode() == QType::ANY && !pkt.d_tcp && g_anyToTcp) {
    state.r->d.tc = 1;
    state.r->commitD();
    return false;
  }

  // for qclass ANY the response should never be authoritative unless the response covers all classes.
  if(pkt.qclass==QClass::ANY) {
    state.r->setA(false);
  }

  int retargetcount{0};
  while (true) {
    state.retargeted = false;
    bool result = opcodeQueryInner2(pkt, state, retargetcount != 0);
    if (!state.retargeted) {
      return result;
    }
    retargetcount++;
    if (retargetcount > 10) {
      SLOG(g_log<<Logger::Warning<<"Abort CNAME chain resolution after "<<--retargetcount<<" redirects, sending out servfail. Initial query: '"<<pkt.qdomain<<"'"<<endl,
           d_slog->info(Logr::Warning, "Aborting CNAME chain resolution; sending servfail", "query", Logging::Loggable(pkt.qdomain), "redirects", Logging::Loggable(--retargetcount)));
      state.r=pkt.replyPacket();
      state.r->setRcode(RCode::ServFail);
      return false;
    }
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): TODO continue splitting this into smaller pieces
bool PacketHandler::opcodeQueryInner2(DNSPacket& pkt, queryState &state, bool retargeted)
{
  DNSZoneRecord zrr;

  if (retargeted && !d_doResolveAcrossZones && !state.target.isPartOf(state.r->qdomainzone)) {
    // We are following a retarget outside the initial zone (and do not need to check getAuth to know this). Config asked us not to do that.
    // This is a performance optimization, the generic case is checked after getAuth below.
    return true;
  }

  // Reset possibly dangling data associated to d_sd.
  d_ispresigned.reset();
  d_issecuredzone.reset();

  if(!B.getAuth(ZoneName(state.target), pkt.qtype, &d_sd, pkt.getRealRemote(), true, &pkt)) {
    DLOG(SLOG(g_log<<Logger::Error<<"We have no authority over zone '"<<state.target<<"'"<<endl,
              d_slog->info(Logr::Error, "We have no authority over zone", "zone", Logging::Loggable(state.target))));
    if (!retargeted) {
      state.r->setA(false); // drop AA if we never had a SOA in the first place
      state.r->setRcode(RCode::Refused); // send REFUSED - but only on empty 'no idea'
    }
    return true;
  }
  DLOG(SLOG(g_log<<Logger::Error<<"We have authority, zone='"<<d_sd.qname()<<"', id="<<d_sd.domain_id<<", zonename="<<d_sd.zonename<<endl,
            d_slog->info(Logr::Error, "We have authority", "zone", Logging::Loggable(d_sd.qname()), "domain id", Logging::Loggable(d_sd.domain_id), "zonename", Logging::Loggable(d_sd.zonename))));

  if (!retargeted) {
    state.r->qdomainzone = d_sd.zonename;
  } else if (!d_doResolveAcrossZones && state.r->qdomainzone.operator const DNSName&() != d_sd.qname()) {
    // We are following a retarget outside the initial zone. Config asked us not to do that.
    return true;
  }

  state.authSet.insert(d_sd.zonename);
  d_dnssec=(pkt.d_dnssecOk && isSecuredZone());
  state.doSigs |= d_dnssec;

  if(d_sd.qname()==pkt.qdomain) {
    if(!isPresigned()) {
      switch (pkt.qtype.getCode()) {
      case QType::DNSKEY:
        if(addDNSKEY(pkt, state.r)) {
          return true;
        }
        break;
      case QType::CDNSKEY:
        if(addCDNSKEY(pkt,state.r)) {
          return true;
        }
        break;
      case QType::CDS:
        if(addCDS(pkt,state.r)) {
          return true;
        }
        break;
      }
    }
    if(pkt.qtype.getCode() == QType::NSEC3PARAM) {
      if(addNSEC3PARAM(pkt,state.r)) {
        return true;
      }
    }
  }

  if(pkt.qtype.getCode() == QType::SOA && d_sd.qname()==pkt.qdomain) {
    zrr=makeEditedDNSZRFromSOAData(d_dk, d_sd);
    state.r->addRecord(std::move(zrr));
    return true;
  }

  // this TRUMPS a cname!
  if(d_dnssec && pkt.qtype.getCode() == QType::NSEC && !d_dk.getNSEC3PARAM(d_sd.zonename, nullptr)) {
    addNSEC(pkt, state.r, state.target, DNSName(), 5);
    if (!state.r->isEmpty()) {
      return true;
    }
  }

  // this TRUMPS a cname!
  if(pkt.qtype.getCode() == QType::RRSIG) {
    SLOG(g_log<<Logger::Info<<"Direct RRSIG query for "<<state.target<<" from "<<pkt.getRemoteString()<<endl,
         d_slog->info(Logr::Info, "Direct RRSIG query", "query", Logging::Loggable(state.target), "from", Logging::Loggable(pkt.getRemoteString())));
    state.r->setRcode(RCode::Refused);
    return true;
  }

  DLOG(SLOG(g_log<<"Checking for referrals first, unless this is a DS query"<<endl,
            d_slog->info("Checking for referrals first, unless this is a DS query")));
  if(pkt.qtype.getCode() != QType::DS && tryReferral(pkt, state.r, state.target, retargeted)) {
    return true;
  }

  DLOG(SLOG(g_log<<"Got no referrals, trying ANY"<<endl,
            d_slog->info("Got no referrals, trying ANY")));

#ifdef HAVE_LUA_RECORDS
  bool doLua = doLuaRecords();
#endif

  // see what we get..
  B.lookup(QType(QType::ANY), state.target, d_sd.domain_id, &pkt);
  vector<DNSZoneRecord> rrset;
  DNSName haveAlias;
  uint8_t aliasScopeMask = 0;
  bool weDone{false};
  bool weRedirected{false};
  bool weHaveUnauth{false};

  while(B.get(zrr)) {
#ifdef HAVE_LUA_RECORDS
    if (zrr.dr.d_type == QType::LUA && !isPresigned()) {
      if(!doLua) {
        continue;
      }
      auto rec=getRR<LUARecordContent>(zrr.dr);
      if (!rec) {
        continue;
      }
      if(rec->d_type == QType::CNAME || rec->d_type == pkt.qtype.getCode() || (pkt.qtype.getCode() == QType::ANY && rec->d_type != QType::RRSIG)) {
        state.noCache=true;
        try {
          auto recvec=luaSynth(rec->getCode(), state.target, zrr, d_sd.qname(), pkt, rec->d_type, s_LUA);
          if(!recvec.empty()) {
            for (const auto& r_it : recvec) {
              zrr.dr.d_type = rec->d_type; // might be CNAME
              zrr.dr.setContent(r_it);
              zrr.scopeMask = pkt.getRealRemote().getBits(); // this makes sure answer is a specific as your question
              rrset.push_back(zrr);
            }
            if(rec->d_type == QType::CNAME && pkt.qtype.getCode() != QType::CNAME) {
              weRedirected = true;
            }
            else {
              weDone = true;
            }
          }
        }
        catch(std::exception &e) {
          B.lookupEnd();              // don't leave DB handle in bad state

          state.r=pkt.replyPacket();
          state.r->setRcode(RCode::ServFail);
          return false;
        }
      }
    }
#endif
    //cerr<<"got content: ["<<zrr.content<<"]"<<endl;
    if (!d_dnssec && pkt.qtype.getCode() == QType::ANY && (zrr.dr.d_type == QType:: DNSKEY || zrr.dr.d_type == QType::NSEC3PARAM)) {
      continue; // Don't send dnssec info.
    }
    if (zrr.dr.d_type == QType::RRSIG) { // RRSIGS are added later any way.
      continue; // TODO: this actually means addRRSig should check if the RRSig is already there
    }

    // cerr<<"Auth: "<<zrr.auth<<", "<<(zrr.dr.d_type == pkt.qtype)<<", "<<zrr.dr.d_type.toString()<<endl;
    if((pkt.qtype.getCode() == QType::ANY || zrr.dr.d_type == pkt.qtype.getCode()) && zrr.auth) {
      weDone=true;
    }
    // the line below fakes 'unauth NS' for delegations for non-DNSSEC backends.
    if((zrr.dr.d_type == pkt.qtype.getCode() && !zrr.auth) || (zrr.dr.d_type == QType::NS && (!zrr.auth || !(d_sd.qname()==zrr.dr.d_name)))) {
      weHaveUnauth=true;
    }

    if(zrr.dr.d_type == QType::CNAME && pkt.qtype.getCode() != QType::CNAME) {
      weRedirected=true;
    }

    if (DP && zrr.dr.d_type == QType::ALIAS && (pkt.qtype.getCode() == QType::A || pkt.qtype.getCode() == QType::AAAA || pkt.qtype.getCode() == QType::ANY) && !isPresigned()) {
      if (!d_doExpandALIAS) {
        SLOG(g_log<<Logger::Info<<"ALIAS record found for "<<state.target<<", but ALIAS expansion is disabled."<<endl,
             d_slog->info(Logr::Info, "ALIAS record found, but ALIAS expansion is disabled", "query", Logging::Loggable(state.target)));
        continue;
      }
      haveAlias=getRR<ALIASRecordContent>(zrr.dr)->getContent();
      aliasScopeMask=zrr.scopeMask;
    }

    // Filter out all SOA's and add them in later
    if(zrr.dr.d_type == QType::SOA) {
      continue;
    }

    rrset.push_back(zrr);
  }

  /* Add in SOA if required */
  if(state.target==d_sd.qname()) {
      zrr=makeEditedDNSZRFromSOAData(d_dk, d_sd);
      rrset.push_back(std::move(zrr));
  }

  DLOG(SLOG(g_log<<"After first ANY query for '"<<state.target<<"', id="<<d_sd.domain_id<<": weDone="<<weDone<<", weHaveUnauth="<<weHaveUnauth<<", weRedirected="<<weRedirected<<", haveAlias='"<<haveAlias<<"'"<<endl,
            d_slog->info("After first ANY query", "query", Logging::Loggable(state.target), "id", Logging::Loggable(d_sd.domain_id), "weDone", Logging::Loggable(weDone), "weHaveUnauth", Logging::Loggable(weHaveUnauth), "weRedirected", Logging::Loggable(weRedirected), "haveAlias", Logging::Loggable(haveAlias))));
  if(pkt.qtype.getCode() == QType::DS && weHaveUnauth &&  !weDone && !weRedirected) {
    DLOG(SLOG(g_log<<"Q for DS of a name for which we do have NS, but for which we don't have DS; need to provide an AUTH answer that shows we don't"<<endl,
              d_slog->info("Query for DS of a name for which we do have NS, but don't have any DS; need to provide an AUTH answer that shows this")));
    makeNOError(pkt, state.r, state.target, DNSName(), 1);
    return true;
  }

  if(!haveAlias.empty() && (!weDone || pkt.qtype.getCode() == QType::ANY)) {
    DLOG(SLOG(g_log<<Logger::Warning<<"Found nothing that matched for '"<<state.target<<"', but did get alias to '"<<haveAlias<<"', referring"<<endl,
              d_slog->info(Logr::Warning, "Found nothing that matched, but got alias, referring", "query", Logging::Loggable(state.target), "alias", Logging::Loggable(haveAlias))));
    DP->completePacket(state.r, haveAlias, state.target, aliasScopeMask);
    state.r = nullptr;
    return false;
  }

  // referral for DS query
  if(pkt.qtype.getCode() == QType::DS) {
    DLOG(SLOG(g_log<<"Qtype is DS"<<endl,
              d_slog->info("QType is DS")));
    bool doReferral = true;
    if(d_dk.doesDNSSEC()) {
      for(auto& loopRR: rrset) {
        // In a dnssec capable backend auth=true means, there is no delegation at
        // or above this qname in this zone (for DS queries). Without a delegation,
        // at or above this level, it is pointless to search for referrals.
        if(loopRR.auth) {
          doReferral = false;
          break;
        }
      }
    } else {
      for(auto& loopRR: rrset) {
        // In a non dnssec capable backend auth is always true, so our only option
        // is, always look for referrals. Unless there is a direct match for DS.
        if(loopRR.dr.d_type == QType::DS) {
          doReferral = false;
          break;
        }
      }
    }
    if(doReferral) {
      DLOG(SLOG(g_log<<"DS query found no direct result, trying referral now"<<endl,
                d_slog->info("DS query found no direct result, trying referral now")));
      if(tryReferral(pkt, state.r, state.target, retargeted)) {
        DLOG(SLOG(g_log<<"Got referral for DS query"<<endl,
                  d_slog->info("Got referral for DS query")));
        return true;
      }
    }
  }

  if(rrset.empty()) {
    DLOG(SLOG(g_log<<Logger::Warning<<"Found nothing in the by-name ANY, but let's try wildcards.."<<endl,
              d_slog->info(Logr::Warning, "Found nothing in the by-name ANY, trying wildcards")));
    bool wereRetargeted{false};
    bool nodata{false};
    DNSName wildcard;
    if(tryWildcard(pkt, state.r, state.target, wildcard, wereRetargeted, nodata)) {
      if(wereRetargeted) {
        if (!retargeted) {
          state.r->qdomainwild=std::move(wildcard);
        }
        state.retargeted = true;
        return true;
      }
      if(nodata) {
        makeNOError(pkt, state.r, state.target, wildcard, 2);
      }

      return true;
    }
    try {
      if (tryDNAME(pkt, state.r, state.target)) {
        state.retargeted = true;
        return true;
      }
    } catch (const std::range_error &e) {
      // We couldn't make a CNAME.....
      state.r->setRcode(RCode::YXDomain);
      return true;
    }
    if(tryAuthSignal(pkt, state.r, state.target)) {
      return true;
    }

    if (!(((pkt.qtype.getCode() == QType::CNAME) || (pkt.qtype.getCode() == QType::ANY)) && retargeted)) {
      makeNXDomain(pkt, state.r, state.target, wildcard);
    }

    return true;
  }

  if(weRedirected) {
    for(auto& loopRR: rrset) {
      if(loopRR.dr.d_type == QType::CNAME) {
        state.r->addRecord(DNSZoneRecord(loopRR));
        state.target = getRR<CNAMERecordContent>(loopRR.dr)->getTarget();
        state.retargeted = true;
        return true;
      }
    }
  }
  else if(weDone) {
    bool haveRecords = false;
    bool presigned = isPresigned();
    for(const auto& loopRR: rrset) {
      if (loopRR.dr.d_type == QType::ENT) {
        continue;
      }
      if (loopRR.dr.d_type == QType::ALIAS && d_doExpandALIAS && !presigned) {
        continue;
      }
#ifdef HAVE_LUA_RECORDS
      if (loopRR.dr.d_type == QType::LUA && !presigned) {
        continue;
      }
#endif
      if ((pkt.qtype.getCode() == QType::ANY || loopRR.dr.d_type == pkt.qtype.getCode()) && loopRR.auth) {
        state.r->addRecord(DNSZoneRecord(loopRR));
        haveRecords = true;
      }
    }

    if (haveRecords) {
      if(d_dnssec && pkt.qtype.getCode() == QType::ANY) {
        completeANYRecords(pkt, state.r, state.target);
      }
    }
    else {
      makeNOError(pkt, state.r, state.target, DNSName(), 0);
    }

    return true;
  }
  else if(weHaveUnauth) {
    DLOG(SLOG(g_log<<"Have unauth data, so need to hunt for best NS records"<<endl,
              d_slog->info("Have unauth data, searching for best NS record")));
    if (tryReferral(pkt, state.r, state.target, retargeted)) {
      return true;
    }
    // check whether this could be fixed easily
#if 0
    if (*(rrset.back().dr.d_name.getStorage().rbegin()) == '.') {
      SLOG(g_log<<Logger::Error<<"Should not get here ("<<pkt.qdomain<<"|"<<pkt.qtype.toString()<<"): you have a trailing dot, this could be the problem (or run 'pdnsutil zone rectify " <<d_sd.qname()<<"')"<<endl,
           d_slog->error(Logr::Error, "Should not get here, also unexpected trailing dot in rrset, consider running 'pdnsutil zone rectify' on the zone", "query", Logging::Loggable(pkt.qdomain), "type", Logging::Loggable(pkt.qtype), "zone", Logging::Loggable(d_sd.qname())));
    } else {
      SLOG(g_log<<Logger::Error<<"Should not get here ("<<pkt.qdomain<<"|"<<pkt.qtype.toString()<<"): please run 'pdnsutil zone rectify "<<d_sd.qname()<<"'"<<endl,
           d_slog->error(Logr::Error, "Should not get here, consider running 'pdnsutil zone rectify' on the zone", "query", Logging::Loggable(pkt.qdomain), "type", Logging::Loggable(pkt.qtype), "zone", Logging::Loggable(d_sd.qname())));

    }
#endif
  }
  else {
    DLOG(SLOG(g_log<<"Have some data, but not the right data"<<endl,
              d_slog->info("Have some data, but not the right data")));
    makeNOError(pkt, state.r, state.target, DNSName(), 0);
  }
  return true;
}

std::unique_ptr<DNSPacket> PacketHandler::opcodeQuery(DNSPacket& pkt, bool noCache)
{
  queryState state;
  state.noCache = noCache;

  if (opcodeQueryInner(pkt, state)) {
    doAdditionalProcessing(pkt, state.r);

    // now that all processing is done, span and view may have been set, so we copy them
    state.r->d_span = pkt.d_span;
    state.r->d_view = pkt.d_view;

    for(const auto& loopRR: state.r->getRRS()) {
      if (loopRR.scopeMask != 0) {
        state.noCache=true;
        break;
      }
    }
    if (state.doSigs) {
      addRRSigs(d_dk, B, state.authSet, state.r->getRRS(), &pkt);
    }

    if (PC.enabled() && !state.noCache && pkt.couldBeCached()) {
      PC.insert(pkt, *state.r, state.r->getMinTTL(), pkt.d_view); // in the packet cache
    }
  }

  return std::move(state.r);
}

std::unique_ptr<DNSPacket> PacketHandler::opcodeNotify(DNSPacket& pkt, bool /* noCache */)
{
  S.inc("incoming-notifications");
  int res=processNotify(pkt);
  if(res>=0) {
    auto resp=pkt.replyPacket();  // generate an empty reply packet
    resp->setRcode(res);
    resp->setOpcode(Opcode::Notify);
    return resp;
  }
  return nullptr;
}

std::unique_ptr<DNSPacket> PacketHandler::opcodeUpdate(DNSPacket& pkt, bool /* noCache */)
{
  if (g_views) {
    // Make this variant-aware without performing the complete UeberBackend::getAuth work
    g_zoneCache.setZoneVariant(pkt);
  }
  else {
    pkt.qdomainzone = ZoneName(pkt.qdomain);
  }

  S.inc("dnsupdate-queries");
  int res=processUpdate(pkt);
  if (res == RCode::Refused) {
    S.inc("dnsupdate-refused");
  }
  else if (res != RCode::ServFail) {
    S.inc("dnsupdate-answers");
  }
  auto resp=pkt.replyPacket();  // generate an empty reply packet
  resp->setRcode(res);
  resp->setOpcode(Opcode::Update);
  return resp;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): can't make it static as it is used through member method pointer in doQuestion()
std::unique_ptr<DNSPacket> PacketHandler::opcodeNotImplemented(DNSPacket& pkt, bool /* noCache */)
{
  SLOG(g_log<<Logger::Error<<"Received an unknown opcode "<<pkt.d.opcode<<" from "<<pkt.getRemoteString()<<" for "<<pkt.qdomain<<endl,
       d_slog->error(Logr::Error, "Received an unknown opcode", "opcode", Logging::Loggable(pkt.d.opcode), "from", Logging::Loggable(pkt.getRemoteString()), "query", Logging::Loggable(pkt.qdomain)));

  auto resp=pkt.replyPacket();  // generate an empty reply packet
  resp->setRcode(RCode::NotImp);
  return resp;
}

bool PacketHandler::checkForCorrectTSIG(const DNSPacket& packet, DNSName* tsigkeyname, string* secret, TSIGRecordContent* tsigContent)
{
  uint16_t tsigPos{0};

  if (!packet.getTSIGDetails(tsigContent, tsigkeyname, &tsigPos)) {
    return false;
  }

  TSIGTriplet tsigTriplet;
  tsigTriplet.name = *tsigkeyname;
  tsigTriplet.algo = tsigContent->d_algoName;
  if (tsigTriplet.algo == g_hmacmd5dnsname_long) {
    tsigTriplet.algo = g_hmacmd5dnsname;
  }

  if (tsigTriplet.algo != g_gsstsigdnsname) {
    string secret64;
    if (!B.getTSIGKey(*tsigkeyname, tsigTriplet.algo, secret64)) {
      SLOG(g_log << Logger::Error << "Packet for domain '" << packet.qdomain << "' denied: can't find TSIG key with name '" << *tsigkeyname << "' and algorithm '" << tsigTriplet.algo << "'" << endl,
           d_slog->error(Logr::Error, "Packet denied: can't find TSIG key with given name and algorithm", "query", Logging::Loggable(packet.qdomain), "key", Logging::Loggable(*tsigkeyname), "algorithm", Logging::Loggable(tsigTriplet.algo)));
      return false;
    }
    B64Decode(secret64, *secret);
    tsigTriplet.secret = *secret;
  }

  try {
    return packet.validateTSIG(tsigTriplet, *tsigContent, "", tsigContent->d_mac, false);
  }
  catch(const std::runtime_error& err) {
    SLOG(g_log<<Logger::Error<<"Packet for '"<<packet.qdomain<<"' denied: "<<err.what()<<endl,
         d_slog->error(Logr::Error, err.what(), "Packet denied", "query", Logging::Loggable(packet.qdomain)));
    return false;
  }
}

bool PacketHandler::doLuaRecords()
{
#ifdef HAVE_LUA_RECORDS
  if (g_doLuaRecord) {
    return true;
  }
  if (!d_doLua) {
    d_doLua = d_dk.isMetadataOne(d_sd.zonename, "ENABLE-LUA-RECORDS", true);
  }
  return *d_doLua;
#endif
  return false;
}

bool PacketHandler::isPresigned()
{
  if (!d_ispresigned) {
    d_ispresigned = d_dk.isPresigned(d_sd.zonename);
  }
  return *d_ispresigned;
}

bool PacketHandler::isSecuredZone()
{
  if (!d_issecuredzone) {
    d_issecuredzone = d_dk.isSecuredZone(d_sd.zonename);
  }
  return *d_issecuredzone;
}
