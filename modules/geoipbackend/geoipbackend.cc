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
#include <cstdint>
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "geoipbackend.hh"
#include "geoipinterface.hh"
#include "pdns/dns_random.hh"
#include <sstream>
#include <regex.h>
#include <glob.h>
#include <boost/algorithm/string/replace.hpp>
#include <boost/format.hpp>
#include <fstream>
#include <filesystem>
#include <utility>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <yaml-cpp/yaml.h>
#pragma GCC diagnostic pop

ReadWriteLock GeoIPBackend::s_state_lock;

struct GeoIPDNSResourceRecord : DNSResourceRecord
{
  int weight{};
  bool has_weight{};
};

struct GeoIPService
{
  NetmaskTree<vector<string>> masks;
  unsigned int netmask4;
  unsigned int netmask6;
};

struct GeoIPDomain
{
  domainid_t id{};
  ZoneName domain;
  int ttl{};
  map<DNSName, GeoIPService> services;
  map<DNSName, vector<GeoIPDNSResourceRecord>> records;
  vector<string> mapping_lookup_formats;
  map<std::string, std::string> custom_mapping;
};

static vector<GeoIPDomain> s_domains;
static int s_rc = 0; // refcount - always accessed under lock

const static std::array<string, 7> GeoIP_WEEKDAYS = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
const static std::array<string, 12> GeoIP_MONTHS = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};

/* So how does it work - we have static records and services. Static records "win".
   We also insert empty non terminals for records and services.

   If a service makes an internal reference to a domain also hosted within geoip, we give a direct
   answers, no CNAMEs involved.

   If the reference is external, we spoof up a CNAME, and good luck with that
*/

struct DirPtrDeleter
{
  /* using a deleter instead of decltype(&closedir) has two big advantages:
     - the deleter is included in the type and does not have to be passed
       when creating a new object (easier to use, less memory usage, in theory
       better inlining)
     - we avoid the annoying "ignoring attributes on template argument ‘int (*)(DIR*)’"
       warning from the compiler, which is there because closedir is tagged as __nonnull((1))
  */
  void operator()(DIR* dirPtr) const noexcept
  {
    closedir(dirPtr);
  }
};

using UniqueDirPtr = std::unique_ptr<DIR, DirPtrDeleter>;

GeoIPBackend::GeoIPBackend(const string& suffix)
{
  WriteLock writeLock(&s_state_lock);
  setArgPrefix("geoip" + suffix);
  if (!getArg("dnssec-keydir").empty()) {
    auto dirHandle = UniqueDirPtr(opendir(getArg("dnssec-keydir").c_str()));
    if (!dirHandle) {
      throw PDNSException("dnssec-keydir " + getArg("dnssec-keydir") + " does not exist");
    }
    d_dnssec = true;
  }
  if (s_rc == 0) { // first instance gets to open everything
    initialize();
  }
  s_rc++;
}

static vector<std::unique_ptr<GeoIPInterface>> s_geoip_files;

string getGeoForLua(const std::string& ip, int qaint);
static string queryGeoIP(const Netmask& addr, GeoIPInterface::GeoIPQueryAttribute attribute, GeoIPNetmask& gl);

// validateMappingLookupFormats validates any custom format provided by the
// user does not use the custom mapping placeholder again, else it would do an
// infinite recursion.
static bool validateMappingLookupFormats(const vector<string>& formats)
{
  string::size_type cur = 0;
  string::size_type last = 0;

  for (const auto& lookupFormat : formats) {
    last = 0;
    while ((cur = lookupFormat.find("%", last)) != string::npos) {
      if (lookupFormat.compare(cur, 3, "%mp") == 0) {
        return false;
      }

      if (lookupFormat.compare(cur, 2, "%%") == 0) { // Ensure escaped % is also accepted
        last = cur + 2;
        continue;
      }
      last = cur + 1; // move to next attribute
    }
  }
  return true;
}

static vector<GeoIPDNSResourceRecord> makeDNSResourceRecord(GeoIPDomain& dom, DNSName name)
{
  GeoIPDNSResourceRecord resourceRecord;
  resourceRecord.domain_id = dom.id;
  resourceRecord.ttl = dom.ttl;
  resourceRecord.qname = std::move(name);
  resourceRecord.qtype = QType(0); // empty non terminal
  resourceRecord.content = "";
  resourceRecord.auth = true;
  resourceRecord.weight = 100;
  resourceRecord.has_weight = false;
  vector<GeoIPDNSResourceRecord> rrs;
  rrs.push_back(resourceRecord);
  return rrs;
}

void GeoIPBackend::setupNetmasks(const YAML::Node& domain, GeoIPDomain& dom)
{
  for (auto service = domain["services"].begin(); service != domain["services"].end(); service++) {
    unsigned int netmask4 = 0;
    unsigned int netmask6 = 0;
    DNSName serviceName{service->first.as<string>()};
    NetmaskTree<vector<string>> netmaskTree;

    // if it's an another map, we need to iterate it again, otherwise we just add two root entries.
    if (service->second.IsMap()) {
      for (auto net = service->second.begin(); net != service->second.end(); net++) {
        vector<string> value;
        if (net->second.IsSequence()) {
          value = net->second.as<vector<string>>();
        }
        else {
          value.push_back(net->second.as<string>());
        }
        if (net->first.as<string>() == "default") {
          netmaskTree.insert(Netmask("0.0.0.0/0")).second.assign(value.begin(), value.end());
          netmaskTree.insert(Netmask("::/0")).second.swap(value);
        }
        else {
          Netmask netmask{net->first.as<string>()};
          netmaskTree.insert(netmask).second.swap(value);
          if (netmask.isIPv6() && netmask6 < netmask.getBits()) {
            netmask6 = netmask.getBits();
          }
          if (!netmask.isIPv6() && netmask4 < netmask.getBits()) {
            netmask4 = netmask.getBits();
          }
        }
      }
    }
    else {
      vector<string> value;
      if (service->second.IsSequence()) {
        value = service->second.as<vector<string>>();
      }
      else {
        value.push_back(service->second.as<string>());
      }
      netmaskTree.insert(Netmask("0.0.0.0/0")).second.assign(value.begin(), value.end());
      netmaskTree.insert(Netmask("::/0")).second.swap(value);
    }

    // Allow per domain override of mapping_lookup_formats and custom_mapping.
    // If not defined, the global values will be used.
    if (YAML::Node formats = domain["mapping_lookup_formats"]) {
      auto mapping_lookup_formats = formats.as<vector<string>>();
      if (!validateMappingLookupFormats(mapping_lookup_formats)) {
        throw PDNSException(string("%mp is not allowed in mapping lookup formats of domain ") + dom.domain.toLogString());
      }

      dom.mapping_lookup_formats = std::move(mapping_lookup_formats);
    }
    else {
      dom.mapping_lookup_formats = d_global_mapping_lookup_formats;
    }
    if (YAML::Node mapping = domain["custom_mapping"]) {
      dom.custom_mapping = mapping.as<map<std::string, std::string>>();
    }
    else {
      dom.custom_mapping = d_global_custom_mapping;
    }

    dom.services[serviceName].netmask4 = netmask4;
    dom.services[serviceName].netmask6 = netmask6;
    dom.services[serviceName].masks.swap(netmaskTree);
  }
}

bool GeoIPBackend::loadDomain(const YAML::Node& domain, domainid_t domainID, GeoIPDomain& dom)
{
  try {
    dom.id = domainID;
    dom.domain = ZoneName(domain["domain"].as<string>());
    dom.ttl = domain["ttl"].as<int>();

    for (auto recs = domain["records"].begin(); recs != domain["records"].end(); recs++) {
      ZoneName qname = ZoneName(recs->first.as<string>());
      vector<GeoIPDNSResourceRecord> rrs;

      for (auto item = recs->second.begin(); item != recs->second.end(); item++) {
        auto rec = item->begin();
        GeoIPDNSResourceRecord rr;
        rr.domain_id = dom.id;
        rr.ttl = dom.ttl;
        rr.qname = qname.operator const DNSName&();
        if (rec->first.IsNull()) {
          rr.qtype = QType(0);
        }
        else {
          string qtype = boost::to_upper_copy(rec->first.as<string>());
          rr.qtype = qtype;
        }
        rr.has_weight = false;
        rr.weight = 100;
        if (rec->second.IsNull()) {
          rr.content = "";
        }
        else if (rec->second.IsMap()) {
          for (YAML::const_iterator iter = rec->second.begin(); iter != rec->second.end(); iter++) {
            auto attr = iter->first.as<string>();
            if (attr == "content") {
              auto content = iter->second.as<string>();
              rr.content = std::move(content);
            }
            else if (attr == "weight") {
              rr.weight = iter->second.as<int>();
              if (rr.weight <= 0) {
                g_log << Logger::Error << "Weight must be positive for " << rr.qname << endl;
                throw PDNSException(string("Weight must be positive for ") + rr.qname.toLogString());
              }
              rr.has_weight = true;
            }
            else if (attr == "ttl") {
              rr.ttl = iter->second.as<int>();
            }
            else {
              g_log << Logger::Error << "Unsupported record attribute " << attr << " for " << rr.qname << endl;
              throw PDNSException(string("Unsupported record attribute ") + attr + string(" for ") + rr.qname.toLogString());
            }
          }
        }
        else {
          auto content = rec->second.as<string>();
          rr.content = std::move(content);
          rr.weight = 100;
        }
        rr.auth = true;
        rrs.push_back(rr);
      }
      std::swap(dom.records[qname.operator const DNSName&()], rrs);
    }

    setupNetmasks(domain, dom);

    // rectify the zone, first static records
    for (auto& item : dom.records) {
      // ensure we have parent in records
      DNSName name = item.first;
      while (name.chopOff() && name.isPartOf(dom.domain)) {
        if (dom.records.find(name) == dom.records.end() && (dom.services.count(name) == 0U)) { // don't ENT out a service!
          auto rrs = makeDNSResourceRecord(dom, name);
          std::swap(dom.records[name], rrs);
        }
      }
    }

    // then services
    for (auto& item : dom.services) {
      // ensure we have parent in records
      DNSName name = item.first;
      while (name.chopOff() && name.isPartOf(dom.domain)) {
        if (dom.records.find(name) == dom.records.end()) {
          auto rrs = makeDNSResourceRecord(dom, name);
          std::swap(dom.records[name], rrs);
        }
      }
    }

    // finally fix weights
    for (auto& item : dom.records) {
      map<uint16_t, float> weights;
      map<uint16_t, float> sums;
      map<uint16_t, GeoIPDNSResourceRecord*> lasts;
      bool has_weight = false;
      // first we look for used weight
      for (const auto& resourceRecord : item.second) {
        weights[resourceRecord.qtype.getCode()] += static_cast<float>(resourceRecord.weight);
        if (resourceRecord.has_weight) {
          has_weight = true;
        }
      }
      if (has_weight) {
        // put them back as probabilities and values..
        for (auto& resourceRecord : item.second) {
          uint16_t rr_type = resourceRecord.qtype.getCode();
          resourceRecord.weight = static_cast<int>((static_cast<float>(resourceRecord.weight) / weights[rr_type]) * 1000.0);
          sums[rr_type] += static_cast<float>(resourceRecord.weight);
          resourceRecord.has_weight = has_weight;
          lasts[rr_type] = &resourceRecord;
        }
        // remove rounding gap
        for (auto& x : lasts) {
          float sum = sums[x.first];
          if (sum < 1000) {
            x.second->weight += (1000 - sum);
          }
        }
      }
    }
  }
  catch (std::exception& ex) {
    g_log << Logger::Error << ex.what() << endl;
    return false;
  }
  catch (PDNSException& ex) {
    g_log << Logger::Error << ex.reason << endl;
    return false;
  }
  return true;
}

void GeoIPBackend::loadDomainsFromDirectory(const std::string& dir, vector<GeoIPDomain>& domains)
{
  vector<std::filesystem::path> paths;
  for (const std::filesystem::path& p : std::filesystem::directory_iterator(std::filesystem::path(dir))) {
    if (std::filesystem::is_regular_file(p) && p.has_extension() && (p.extension() == ".yaml" || p.extension() == ".yml")) {
      paths.push_back(p);
    }
  }
  std::sort(paths.begin(), paths.end());
  for (const auto& p : paths) {
    try {
      GeoIPDomain dom;
      const auto& zoneRoot = YAML::LoadFile(p.string());
      // expect zone key
      const auto& zone = zoneRoot["zone"];
      if (loadDomain(zone, domains.size(), dom)) {
        domains.push_back(dom);
      }
    }
    catch (std::exception& ex) {
      g_log << Logger::Warning << "Cannot load zone from " << p << ": " << ex.what() << endl;
    }
  }
}

void GeoIPBackend::initialize()
{
  YAML::Node config;
  vector<GeoIPDomain> tmp_domains;

  s_geoip_files.clear(); // reset pointers

  if (getArg("database-files").empty() == false) {
    vector<string> files;
    stringtok(files, getArg("database-files"), " ,\t\r\n");
    for (auto const& file : files) {
      s_geoip_files.push_back(GeoIPInterface::makeInterface(file));
    }
  }

  if (s_geoip_files.empty()) {
    g_log << Logger::Warning << "No GeoIP database files loaded!" << endl;
  }

  std::string zonesFile{getArg("zones-file")};
  if (!zonesFile.empty()) {
    try {
      config = YAML::LoadFile(zonesFile);
    }
    catch (YAML::Exception& ex) {
      std::string description{};
      if (!ex.mark.is_null()) {
        description = "Configuration error in " + zonesFile + ", line " + std::to_string(ex.mark.line + 1) + ", column " + std::to_string(ex.mark.column + 1);
      }
      else {
        description = "Cannot read config file " + zonesFile;
      }
      throw PDNSException(description + ": " + ex.msg);
    }
  }

  // Global lookup formats and mapping will be used
  // if none defined at the domain level.
  if (YAML::Node formats = config["mapping_lookup_formats"]) {
    d_global_mapping_lookup_formats = formats.as<vector<string>>();
    if (!validateMappingLookupFormats(d_global_mapping_lookup_formats)) {
      throw PDNSException(string("%mp is not allowed in mapping lookup"));
    }
  }
  if (YAML::Node mapping = config["custom_mapping"]) {
    d_global_custom_mapping = mapping.as<map<std::string, std::string>>();
  }

  for (YAML::const_iterator _domain = config["domains"].begin(); _domain != config["domains"].end(); _domain++) {
    GeoIPDomain dom;
    if (loadDomain(*_domain, tmp_domains.size(), dom)) {
      tmp_domains.push_back(std::move(dom));
    }
  }

  if (YAML::Node domain_dir = config["zones_dir"]) {
    loadDomainsFromDirectory(domain_dir.as<string>(), tmp_domains);
  }

  s_domains.clear();
  std::swap(s_domains, tmp_domains);

  extern std::function<std::string(const std::string& ip, int)> g_getGeo;
  g_getGeo = getGeoForLua;
}

GeoIPBackend::~GeoIPBackend()
{
  try {
    WriteLock writeLock(&s_state_lock);
    s_rc--;
    if (s_rc == 0) { // last instance gets to cleanup
      s_geoip_files.clear();
      s_domains.clear();
    }
  }
  catch (...) {
  }
}

bool GeoIPBackend::lookup_static(const GeoIPDomain& dom, const DNSName& search, const QType& qtype, const DNSName& qdomain, const Netmask& addr, GeoIPNetmask& gl)
{
  const auto& i = dom.records.find(search);
  map<uint16_t, int> cumul_probabilities;
  map<uint16_t, bool> weighted_match;
  int probability_rnd = 1 + (dns_random(1000)); // setting probability=0 means it never is used

  if (i != dom.records.end()) { // return static value
    for (const auto& rr : i->second) {
      if ((qtype != QType::ANY && rr.qtype != qtype) || weighted_match[rr.qtype.getCode()])
        continue;

      if (rr.has_weight) {
        gl.netmask = (addr.isIPv6() ? 128 : 32);
        int comp = cumul_probabilities[rr.qtype.getCode()];
        cumul_probabilities[rr.qtype.getCode()] += rr.weight;
        if (rr.weight == 0 || probability_rnd < comp || probability_rnd > (comp + rr.weight))
          continue;
      }
      const string& content = format2str(rr.content, addr, gl, dom);
      if (rr.qtype != QType::ENT && rr.qtype != QType::TXT && content.empty())
        continue;
      d_result.push_back(rr);
      d_result.back().content = content;
      d_result.back().qname = qdomain;
      // If we are weighted we only return one resource and we found a matching resource,
      // so no need to check the other ones.
      if (rr.has_weight)
        weighted_match[rr.qtype.getCode()] = true;
    }
    // ensure we get most strict netmask
    for (DNSResourceRecord& rr : d_result) {
      rr.scopeMask = gl.netmask;
    }
    return true; // no need to go further
  }

  return false;
};

void GeoIPBackend::lookup(const QType& qtype, const DNSName& qdomain, domainid_t zoneId, DNSPacket* pkt_p)
{
  ReadLock rl(&s_state_lock);
  const GeoIPDomain* dom;
  GeoIPNetmask gl;
  bool found = false;

  if (d_result.size() > 0)
    throw PDNSException("Cannot perform lookup while another is running");

  d_result.clear();

  if (zoneId >= 0 && zoneId < static_cast<domainid_t>(s_domains.size())) {
    dom = &(s_domains[zoneId]);
  }
  else {
    for (const GeoIPDomain& i : s_domains) { // this is arguably wrong, we should probably find the most specific match
      if (qdomain.isPartOf(i.domain)) {
        dom = &i;
        found = true;
        break;
      }
    }
    if (!found)
      return; // not found
  }

  Netmask addr{"0.0.0.0/0"};
  if (pkt_p != nullptr) {
    addr = Netmask(pkt_p->getRealRemote());
  }

  gl.netmask = 0;

  (void)this->lookup_static(*dom, qdomain, qtype, qdomain, addr, gl);

  const auto& target = (*dom).services.find(qdomain);
  if (target == (*dom).services.end())
    return; // no hit

  const NetmaskTree<vector<string>>::node_type* node = target->second.masks.lookup(addr);
  if (node == nullptr)
    return; // no hit, again.

  DNSName sformat;
  gl.netmask = node->first.getBits();
  // figure out smallest sensible netmask
  if (gl.netmask == 0) {
    GeoIPNetmask tmp_gl;
    tmp_gl.netmask = 0;
    // get netmask from geoip backend
    if (queryGeoIP(addr, GeoIPInterface::Name, tmp_gl) == "unknown") {
      if (addr.isIPv6())
        gl.netmask = target->second.netmask6;
      else
        gl.netmask = target->second.netmask4;
    }
  }
  else {
    if (addr.isIPv6())
      gl.netmask = target->second.netmask6;
    else
      gl.netmask = target->second.netmask4;
  }

  // note that this means the array format won't work with indirect
  for (auto it = node->second.begin(); it != node->second.end(); it++) {
    sformat = DNSName(format2str(*it, addr, gl, *dom));

    // see if the record can be found
    if (this->lookup_static((*dom), sformat, qtype, qdomain, addr, gl))
      return;
  }

  if (!d_result.empty()) {
    g_log << Logger::Error << "Cannot have static record and CNAME at the same time."
          << "Please fix your configuration for \"" << qdomain << "\", so that "
          << "it can be resolved by GeoIP backend directly." << std::endl;
    d_result.clear();
    return;
  }

  // we need this line since we otherwise claim to have NS records etc
  if (!(qtype == QType::ANY || qtype == QType::CNAME))
    return;

  DNSResourceRecord rr;
  rr.domain_id = dom->id;
  rr.qtype = QType::CNAME;
  rr.qname = qdomain;
  rr.content = sformat.toString();
  rr.auth = 1;
  rr.ttl = dom->ttl;
  rr.scopeMask = gl.netmask;
  d_result.push_back(rr);
}

bool GeoIPBackend::get(DNSResourceRecord& r)
{
  if (d_result.empty()) {
    return false;
  }

  r = d_result.back();
  d_result.pop_back();

  return true;
}

static string queryGeoIP(const Netmask& addr, GeoIPInterface::GeoIPQueryAttribute attribute, GeoIPNetmask& gl)
{
  string ret = "unknown";

  for (auto const& gi : s_geoip_files) {
    string val;
    const string ip = addr.toStringNoMask();
    bool found = false;

    switch (attribute) {
    case GeoIPInterface::ASn:
      if (addr.isIPv6())
        found = gi->queryASnumV6(val, gl, ip);
      else
        found = gi->queryASnum(val, gl, ip);
      break;
    case GeoIPInterface::Name:
      if (addr.isIPv6())
        found = gi->queryNameV6(val, gl, ip);
      else
        found = gi->queryName(val, gl, ip);
      break;
    case GeoIPInterface::Continent:
      if (addr.isIPv6())
        found = gi->queryContinentV6(val, gl, ip);
      else
        found = gi->queryContinent(val, gl, ip);
      break;
    case GeoIPInterface::Region:
      if (addr.isIPv6())
        found = gi->queryRegionV6(val, gl, ip);
      else
        found = gi->queryRegion(val, gl, ip);
      break;
    case GeoIPInterface::Country:
      if (addr.isIPv6())
        found = gi->queryCountryV6(val, gl, ip);
      else
        found = gi->queryCountry(val, gl, ip);
      break;
    case GeoIPInterface::Country2:
      if (addr.isIPv6())
        found = gi->queryCountry2V6(val, gl, ip);
      else
        found = gi->queryCountry2(val, gl, ip);
      break;
    case GeoIPInterface::City:
      if (addr.isIPv6())
        found = gi->queryCityV6(val, gl, ip);
      else
        found = gi->queryCity(val, gl, ip);
      break;
    case GeoIPInterface::Location:
      double lat = 0, lon = 0;
      std::optional<int> alt;
      std::optional<int> prec;
      if (addr.isIPv6())
        found = gi->queryLocationV6(gl, ip, lat, lon, alt, prec);
      else
        found = gi->queryLocation(gl, ip, lat, lon, alt, prec);
      val = std::to_string(lat) + " " + std::to_string(lon);
      break;
    }

    if (!found || val.empty() || val == "--") {
      continue; // try next database
    }
    ret = std::move(val);
    std::transform(ret.begin(), ret.end(), ret.begin(), ::tolower);
    break;
  }

  if (ret == "unknown")
    gl.netmask = (addr.isIPv6() ? 128 : 32); // prevent caching
  return ret;
}

string getGeoForLua(const std::string& ip, int qaint)
{
  GeoIPInterface::GeoIPQueryAttribute qa((GeoIPInterface::GeoIPQueryAttribute)qaint);
  try {
    const Netmask addr{ip};
    GeoIPNetmask gl;
    string res = queryGeoIP(addr, qa, gl);
    //    cout<<"Result for "<<ip<<" lookup: "<<res<<endl;
    if (qa == GeoIPInterface::ASn && boost::starts_with(res, "as"))
      return res.substr(2);
    return res;
  }
  catch (std::exception& e) {
    cout << "Error: " << e.what() << endl;
  }
  catch (PDNSException& e) {
    cout << "Error: " << e.reason << endl;
  }
  return "";
}

static bool queryGeoLocation(const Netmask& addr, GeoIPNetmask& gl, double& lat, double& lon,
                             std::optional<int>& alt, std::optional<int>& prec)
{
  for (auto const& gi : s_geoip_files) {
    string val;
    if (addr.isIPv6()) {
      if (gi->queryLocationV6(gl, addr.toStringNoMask(), lat, lon, alt, prec))
        return true;
    }
    else if (gi->queryLocation(gl, addr.toStringNoMask(), lat, lon, alt, prec))
      return true;
  }
  return false;
}

string GeoIPBackend::format2str(string sformat, const Netmask& addr, GeoIPNetmask& gl, const GeoIPDomain& dom)
{
  string::size_type cur, last;
  std::optional<int> alt;
  std::optional<int> prec;
  double lat, lon;
  time_t t = time(nullptr);
  GeoIPNetmask tmp_gl; // largest wins
  struct tm gtm;
  gmtime_r(&t, &gtm);
  last = 0;

  while ((cur = sformat.find('%', last)) != string::npos) {
    string rep;
    int nrep = 3;
    tmp_gl.netmask = 0;
    if (!sformat.compare(cur, 3, "%mp")) {
      rep = "unknown";
      for (const auto& lookupFormat : dom.mapping_lookup_formats) {
        auto it = dom.custom_mapping.find(format2str(lookupFormat, addr, gl, dom));
        if (it != dom.custom_mapping.end()) {
          rep = it->second;
          break;
        }
      }
    }
    else if (!sformat.compare(cur, 3, "%cn")) {
      rep = queryGeoIP(addr, GeoIPInterface::Continent, tmp_gl);
    }
    else if (!sformat.compare(cur, 3, "%co")) {
      rep = queryGeoIP(addr, GeoIPInterface::Country, tmp_gl);
    }
    else if (!sformat.compare(cur, 3, "%cc")) {
      rep = queryGeoIP(addr, GeoIPInterface::Country2, tmp_gl);
    }
    else if (!sformat.compare(cur, 3, "%af")) {
      rep = (addr.isIPv6() ? "v6" : "v4");
    }
    else if (!sformat.compare(cur, 3, "%as")) {
      rep = queryGeoIP(addr, GeoIPInterface::ASn, tmp_gl);
    }
    else if (!sformat.compare(cur, 3, "%re")) {
      rep = queryGeoIP(addr, GeoIPInterface::Region, tmp_gl);
    }
    else if (!sformat.compare(cur, 3, "%na")) {
      rep = queryGeoIP(addr, GeoIPInterface::Name, tmp_gl);
    }
    else if (!sformat.compare(cur, 3, "%ci")) {
      rep = queryGeoIP(addr, GeoIPInterface::City, tmp_gl);
    }
    else if (!sformat.compare(cur, 4, "%loc")) {
      char ns, ew;
      int d1, d2, m1, m2;
      double s1, s2;
      if (!queryGeoLocation(addr, gl, lat, lon, alt, prec)) {
        rep = "";
        tmp_gl.netmask = (addr.isIPv6() ? 128 : 32);
      }
      else {
        ns = (lat > 0) ? 'N' : 'S';
        ew = (lon > 0) ? 'E' : 'W';
        /* remove sign */
        lat = fabs(lat);
        lon = fabs(lon);
        d1 = static_cast<int>(lat);
        d2 = static_cast<int>(lon);
        m1 = static_cast<int>((lat - d1) * 60.0);
        m2 = static_cast<int>((lon - d2) * 60.0);
        s1 = static_cast<double>(lat - d1 - m1 / 60.0) * 3600.0;
        s2 = static_cast<double>(lon - d2 - m2 / 60.0) * 3600.0;
        rep = str(boost::format("%d %d %0.3f %c %d %d %0.3f %c") % d1 % m1 % s1 % ns % d2 % m2 % s2 % ew);
        if (alt)
          rep = rep + str(boost::format(" %d.00") % *alt);
        else
          rep = rep + string(" 0.00");
        if (prec)
          rep = rep + str(boost::format(" %dm") % *prec);
      }
      nrep = 4;
    }
    else if (!sformat.compare(cur, 4, "%lat")) {
      if (!queryGeoLocation(addr, gl, lat, lon, alt, prec)) {
        rep = "";
        tmp_gl.netmask = (addr.isIPv6() ? 128 : 32);
      }
      else {
        rep = str(boost::format("%lf") % lat);
      }
      nrep = 4;
    }
    else if (!sformat.compare(cur, 4, "%lon")) {
      if (!queryGeoLocation(addr, gl, lat, lon, alt, prec)) {
        rep = "";
        tmp_gl.netmask = (addr.isIPv6() ? 128 : 32);
      }
      else {
        rep = str(boost::format("%lf") % lon);
      }
      nrep = 4;
    }
    else if (!sformat.compare(cur, 3, "%hh")) {
      rep = boost::str(boost::format("%02d") % gtm.tm_hour);
      tmp_gl.netmask = (addr.isIPv6() ? 128 : 32);
    }
    else if (!sformat.compare(cur, 3, "%yy")) {
      rep = boost::str(boost::format("%02d") % (gtm.tm_year + 1900));
      tmp_gl.netmask = (addr.isIPv6() ? 128 : 32);
    }
    else if (!sformat.compare(cur, 3, "%dd")) {
      rep = boost::str(boost::format("%02d") % (gtm.tm_yday + 1));
      tmp_gl.netmask = (addr.isIPv6() ? 128 : 32);
    }
    else if (!sformat.compare(cur, 4, "%wds")) {
      nrep = 4;
      rep = GeoIP_WEEKDAYS.at(gtm.tm_wday);
      tmp_gl.netmask = (addr.isIPv6() ? 128 : 32);
    }
    else if (!sformat.compare(cur, 4, "%mos")) {
      nrep = 4;
      rep = GeoIP_MONTHS.at(gtm.tm_mon);
      tmp_gl.netmask = (addr.isIPv6() ? 128 : 32);
    }
    else if (!sformat.compare(cur, 3, "%wd")) {
      rep = boost::str(boost::format("%02d") % (gtm.tm_wday + 1));
      tmp_gl.netmask = (addr.isIPv6() ? 128 : 32);
    }
    else if (!sformat.compare(cur, 3, "%mo")) {
      rep = boost::str(boost::format("%02d") % (gtm.tm_mon + 1));
      tmp_gl.netmask = (addr.isIPv6() ? 128 : 32);
    }
    else if (!sformat.compare(cur, 4, "%ip6")) {
      nrep = 4;
      if (addr.isIPv6())
        rep = addr.toStringNoMask();
      else
        rep = "";
      tmp_gl.netmask = (addr.isIPv6() ? 128 : 32);
    }
    else if (!sformat.compare(cur, 4, "%ip4")) {
      nrep = 4;
      if (!addr.isIPv6())
        rep = addr.toStringNoMask();
      else
        rep = "";
      tmp_gl.netmask = (addr.isIPv6() ? 128 : 32);
    }
    else if (!sformat.compare(cur, 3, "%ip")) {
      rep = addr.toStringNoMask();
      tmp_gl.netmask = (addr.isIPv6() ? 128 : 32);
    }
    else if (!sformat.compare(cur, 2, "%%")) {
      last = cur + 2;
      continue;
    }
    else {
      last = cur + 1;
      continue;
    }
    if (tmp_gl.netmask > gl.netmask)
      gl.netmask = tmp_gl.netmask;
    sformat.replace(cur, nrep, rep);
    last = cur + rep.size(); // move to next attribute
  }
  return sformat;
}

void GeoIPBackend::reload()
{
  WriteLock wl(&s_state_lock);

  try {
    initialize();
  }
  catch (PDNSException& pex) {
    g_log << Logger::Error << "GeoIP backend reload failed: " << pex.reason << endl;
  }
  catch (std::exception& stex) {
    g_log << Logger::Error << "GeoIP backend reload failed: " << stex.what() << endl;
  }
  catch (...) {
    g_log << Logger::Error << "GeoIP backend reload failed" << endl;
  }
}

void GeoIPBackend::rediscover(string* /* status */)
{
  reload();
}

bool GeoIPBackend::getDomainInfo(const ZoneName& domain, DomainInfo& info, bool /* getSerial */)
{
  ReadLock rl(&s_state_lock);

  for (const GeoIPDomain& dom : s_domains) {
    if (dom.domain == domain) {
      SOAData sd;
      this->getSOA(dom.domain, dom.id, sd);
      info.id = dom.id;
      info.zone = dom.domain;
      info.serial = sd.serial;
      info.kind = DomainInfo::Native;
      info.backend = this;
      return true;
    }
  }
  return false;
}

void GeoIPBackend::getAllDomains(vector<DomainInfo>* domains, bool /* getSerial */, bool /* include_disabled */)
{
  ReadLock rl(&s_state_lock);

  DomainInfo di;
  for (const auto& dom : s_domains) {
    SOAData sd;
    this->getSOA(dom.domain, dom.id, sd);
    di.id = dom.id;
    di.zone = dom.domain;
    di.serial = sd.serial;
    di.kind = DomainInfo::Native;
    di.backend = this;
    domains->emplace_back(di);
  }
}

bool GeoIPBackend::getAllDomainMetadata(const ZoneName& name, std::map<std::string, std::vector<std::string>>& meta)
{
  if (!d_dnssec)
    return false;

  ReadLock rl(&s_state_lock);
  for (const GeoIPDomain& dom : s_domains) {
    if (dom.domain == name) {
      if (hasDNSSECkey(dom.domain)) {
        meta[string("NSEC3NARROW")].push_back("1");
        meta[string("NSEC3PARAM")].push_back("1 0 0 -");
      }
      return true;
    }
  }
  return false;
}

bool GeoIPBackend::getDomainMetadata(const ZoneName& name, const std::string& kind, std::vector<std::string>& meta)
{
  if (!d_dnssec)
    return false;

  ReadLock rl(&s_state_lock);
  for (const GeoIPDomain& dom : s_domains) {
    if (dom.domain == name) {
      if (hasDNSSECkey(dom.domain)) {
        if (kind == "NSEC3NARROW")
          meta.push_back(string("1"));
        if (kind == "NSEC3PARAM")
          meta.push_back(string("1 0 1 f95a"));
      }
      return true;
    }
  }
  return false;
}

bool GeoIPBackend::getDomainKeys(const ZoneName& name, std::vector<DNSBackend::KeyData>& keys)
{
  if (!d_dnssec)
    return false;
  ReadLock rl(&s_state_lock);
  for (const GeoIPDomain& dom : s_domains) {
    if (dom.domain == name) {
      regex_t reg;
      regmatch_t regm[5];
      regcomp(&reg, "(.*)[.]([0-9]+)[.]([0-9]+)[.]([01])[.]key$", REG_ICASE | REG_EXTENDED);
      ostringstream pathname;
      pathname << getArg("dnssec-keydir") << "/" << dom.domain.toStringNoDot() << "*.key";
      glob_t glob_result;
      if (glob(pathname.str().c_str(), GLOB_ERR, nullptr, &glob_result) == 0) {
        for (size_t i = 0; i < glob_result.gl_pathc; i++) {
          if (regexec(&reg, glob_result.gl_pathv[i], 5, regm, 0) == 0) {
            DNSBackend::KeyData kd;
            pdns::checked_stoi_into(kd.id, glob_result.gl_pathv[i] + regm[3].rm_so);
            kd.active = !strncmp(glob_result.gl_pathv[i] + regm[4].rm_so, "1", 1);
            kd.published = true;
            pdns::checked_stoi_into(kd.flags, glob_result.gl_pathv[i] + regm[2].rm_so);
            ifstream ifs(glob_result.gl_pathv[i]);
            ostringstream content;
            char buffer[1024];
            while (ifs.good()) {
              ifs.read(buffer, sizeof buffer);
              if (ifs.gcount() > 0) {
                content << string(buffer, ifs.gcount());
              }
            }
            ifs.close();
            kd.content = content.str();
            keys.emplace_back(std::move(kd));
          }
        }
      }
      regfree(&reg);
      globfree(&glob_result);
      return true;
    }
  }
  return false;
}

bool GeoIPBackend::removeDomainKey(const ZoneName& name, unsigned int keyId)
{
  if (!d_dnssec)
    return false;
  WriteLock rl(&s_state_lock);
  ostringstream path;

  for (const GeoIPDomain& dom : s_domains) {
    if (dom.domain == name) {
      regex_t reg;
      regmatch_t regm[5];
      regcomp(&reg, "(.*)[.]([0-9]+)[.]([0-9]+)[.]([01])[.]key$", REG_ICASE | REG_EXTENDED);
      ostringstream pathname;
      pathname << getArg("dnssec-keydir") << "/" << dom.domain.toStringNoDot() << "*.key";
      glob_t glob_result;
      if (glob(pathname.str().c_str(), GLOB_ERR, nullptr, &glob_result) == 0) {
        for (size_t i = 0; i < glob_result.gl_pathc; i++) {
          if (regexec(&reg, glob_result.gl_pathv[i], 5, regm, 0) == 0) {
            auto kid = pdns::checked_stoi<unsigned int>(glob_result.gl_pathv[i] + regm[3].rm_so);
            if (kid == keyId) {
              if (unlink(glob_result.gl_pathv[i])) {
                cerr << "Cannot delete key:" << strerror(errno) << endl;
              }
              break;
            }
          }
        }
      }
      regfree(&reg);
      globfree(&glob_result);
      return true;
    }
  }
  return false;
}

bool GeoIPBackend::addDomainKey(const ZoneName& name, const KeyData& key, int64_t& keyId)
{
  if (!d_dnssec)
    return false;
  WriteLock rl(&s_state_lock);
  unsigned int nextid = 1;

  for (const GeoIPDomain& dom : s_domains) {
    if (dom.domain == name) {
      regex_t reg;
      regmatch_t regm[5];
      regcomp(&reg, "(.*)[.]([0-9]+)[.]([0-9]+)[.]([01])[.]key$", REG_ICASE | REG_EXTENDED);
      ostringstream pathname;
      pathname << getArg("dnssec-keydir") << "/" << dom.domain.toStringNoDot() << "*.key";
      glob_t glob_result;
      if (glob(pathname.str().c_str(), GLOB_ERR, nullptr, &glob_result) == 0) {
        for (size_t i = 0; i < glob_result.gl_pathc; i++) {
          if (regexec(&reg, glob_result.gl_pathv[i], 5, regm, 0) == 0) {
            auto kid = pdns::checked_stoi<unsigned int>(glob_result.gl_pathv[i] + regm[3].rm_so);
            if (kid >= nextid)
              nextid = kid + 1;
          }
        }
      }
      regfree(&reg);
      globfree(&glob_result);
      pathname.str("");
      pathname << getArg("dnssec-keydir") << "/" << dom.domain.toStringNoDot() << "." << key.flags << "." << nextid << "." << (key.active ? "1" : "0") << ".key";
      ofstream ofs(pathname.str().c_str());
      ofs.write(key.content.c_str(), key.content.size());
      ofs.close();
      keyId = nextid;
      return true;
    }
  }
  return false;
}

bool GeoIPBackend::activateDomainKey(const ZoneName& name, unsigned int keyId)
{
  if (!d_dnssec)
    return false;
  WriteLock rl(&s_state_lock);
  for (const GeoIPDomain& dom : s_domains) {
    if (dom.domain == name) {
      regex_t reg;
      regmatch_t regm[5];
      regcomp(&reg, "(.*)[.]([0-9]+)[.]([0-9]+)[.]([01])[.]key$", REG_ICASE | REG_EXTENDED);
      ostringstream pathname;
      pathname << getArg("dnssec-keydir") << "/" << dom.domain.toStringNoDot() << "*.key";
      glob_t glob_result;
      if (glob(pathname.str().c_str(), GLOB_ERR, nullptr, &glob_result) == 0) {
        for (size_t i = 0; i < glob_result.gl_pathc; i++) {
          if (regexec(&reg, glob_result.gl_pathv[i], 5, regm, 0) == 0) {
            auto kid = pdns::checked_stoi<unsigned int>(glob_result.gl_pathv[i] + regm[3].rm_so);
            if (kid == keyId && strcmp(glob_result.gl_pathv[i] + regm[4].rm_so, "0") == 0) { // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
              ostringstream newpath;
              newpath << getArg("dnssec-keydir") << "/" << dom.domain.toStringNoDot() << "." << pdns::checked_stoi<unsigned int>(glob_result.gl_pathv[i] + regm[2].rm_so) << "." << kid << ".1.key";
              if (rename(glob_result.gl_pathv[i], newpath.str().c_str())) {
                cerr << "Cannot activate key: " << strerror(errno) << endl;
              }
            }
          }
        }
      }
      globfree(&glob_result);
      regfree(&reg);
      return true;
    }
  }
  return false;
}

bool GeoIPBackend::deactivateDomainKey(const ZoneName& name, unsigned int keyId)
{
  if (!d_dnssec)
    return false;
  WriteLock rl(&s_state_lock);
  for (const GeoIPDomain& dom : s_domains) {
    if (dom.domain == name) {
      regex_t reg;
      regmatch_t regm[5];
      regcomp(&reg, "(.*)[.]([0-9]+)[.]([0-9]+)[.]([01])[.]key$", REG_ICASE | REG_EXTENDED);
      ostringstream pathname;
      pathname << getArg("dnssec-keydir") << "/" << dom.domain.toStringNoDot() << "*.key";
      glob_t glob_result;
      if (glob(pathname.str().c_str(), GLOB_ERR, nullptr, &glob_result) == 0) {
        for (size_t i = 0; i < glob_result.gl_pathc; i++) {
          if (regexec(&reg, glob_result.gl_pathv[i], 5, regm, 0) == 0) {
            auto kid = pdns::checked_stoi<unsigned int>(glob_result.gl_pathv[i] + regm[3].rm_so);
            if (kid == keyId && strcmp(glob_result.gl_pathv[i] + regm[4].rm_so, "1") == 0) { // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
              ostringstream newpath;
              newpath << getArg("dnssec-keydir") << "/" << dom.domain.toStringNoDot() << "." << pdns::checked_stoi<unsigned int>(glob_result.gl_pathv[i] + regm[2].rm_so) << "." << kid << ".0.key";
              if (rename(glob_result.gl_pathv[i], newpath.str().c_str())) {
                cerr << "Cannot deactivate key: " << strerror(errno) << endl;
              }
            }
          }
        }
      }
      globfree(&glob_result);
      regfree(&reg);
      return true;
    }
  }
  return false;
}

bool GeoIPBackend::publishDomainKey(const ZoneName& /* name */, unsigned int /* id */)
{
  return false;
}

bool GeoIPBackend::unpublishDomainKey(const ZoneName& /* name */, unsigned int /* id */)
{
  return false;
}

bool GeoIPBackend::hasDNSSECkey(const ZoneName& name)
{
  ostringstream pathname;
  pathname << getArg("dnssec-keydir") << "/" << name.toStringNoDot() << "*.key";
  glob_t glob_result;
  if (glob(pathname.str().c_str(), GLOB_ERR, nullptr, &glob_result) == 0) {
    globfree(&glob_result);
    return true;
  }
  return false;
}

class GeoIPFactory : public BackendFactory
{
public:
  GeoIPFactory() :
    BackendFactory("geoip") {}

  void declareArguments(const string& suffix = "") override
  {
    declare(suffix, "zones-file", "YAML file to load zone(s) configuration", "");
    declare(suffix, "database-files", "File(s) to load geoip data from ([driver:]path[;opt=value]", "");
    declare(suffix, "dnssec-keydir", "Directory to hold dnssec keys (also turns DNSSEC on)", "");
  }

  DNSBackend* make(const string& suffix) override
  {
    return new GeoIPBackend(suffix);
  }
};

class GeoIPLoader
{
public:
  GeoIPLoader()
  {
    BackendMakers().report(std::make_unique<GeoIPFactory>());
    g_log << Logger::Info << "[geoipbackend] This is the geoip backend version " VERSION
#ifndef REPRODUCIBLE
          << " (" __DATE__ " " __TIME__ ")"
#endif
          << " reporting" << endl;
  }
};

static GeoIPLoader geoiploader;
