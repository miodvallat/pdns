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
#include <limits>
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "remotebackend.hh"

// The following requirement guarantees UnknownDomainID will get output as "-1"
// in the Json data for compatibility.
static_assert(std::is_signed<domainid_t>::value);

// The following requirement is a consequency of JsonInt not able to store
// anything larger than "int".
static_assert(sizeof(domainid_t) <= sizeof(int));

static const char* kBackendId = "[RemoteBackend]";

/**
 * Forwarder for value. This is just in case
 * we need to do some treatment to the value before
 * sending it downwards.
 */
bool Connector::send(Json& value)
{
  return send_message(value) > 0;
}

/**
 * Helper for handling receiving of data.
 * Basically what happens here is that we check
 * that the receiving happened ok, and extract
 * result. Logging is performed here, too.
 */
bool Connector::recv(Json& value)
{
  if (recv_message(value) > 0) {
    bool retval = true;
    if (value["result"] == Json()) {
      throw PDNSException("No 'result' field in response from remote process");
    }
    if (value["result"].is_bool() && !boolFromJson(value, "result", false)) {
      retval = false;
    }
    for (const auto& message : value["log"].array_items()) {
      g_log << Logger::Info << "[remotebackend]: " << message.string_value() << std::endl;
    }
    return retval;
  }
  throw PDNSException("Unknown error while receiving data");
}

void RemoteBackend::makeErrorAndThrow(Json& value)
{
  std::string msg = "Remote process indicated a failure";
  for (const auto& message : value["log"].array_items()) {
    msg += " '" + message.string_value() + "'";
  }
  throw PDNSException(msg);
}

/**
 * Standard ctor and dtor
 */
RemoteBackend::RemoteBackend(const std::string& suffix)
{
  setArgPrefix("remote" + suffix);

  this->d_connstr = getArg("connection-string");
  this->d_dnssec = mustDo("dnssec");

  build();
}

RemoteBackend::~RemoteBackend() = default;

bool RemoteBackend::send(Json& value)
{
  try {
    if (!connector->send(value)) {
      // XXX does this work work even though we throw?
      this->connector.reset();
      build();
      throw DBException("Could not send a message to remote process");
    }
  }
  catch (const PDNSException& ex) {
    throw DBException("Exception caught when sending: " + ex.reason);
  }
  return true;
}

bool RemoteBackend::recv(Json& value)
{
  try {
    return connector->recv(value);
  }
  catch (const PDNSException& ex) {
    this->connector.reset();
    build();
    throw DBException("Exception caught when receiving: " + ex.reason);
  }
  catch (const std::exception& e) {
    this->connector.reset();
    build();
    throw DBException("Exception caught when receiving: " + std::string(e.what()));
  }
}

/**
 * Builds connector based on options
 * Currently supports unix,pipe and http
 */
int RemoteBackend::build()
{
  std::vector<std::string> parts;
  std::string type;
  std::string opts;
  std::map<std::string, std::string> options;

  // connstr is of format "type:options"
  size_t pos = 0;
  pos = d_connstr.find_first_of(':');
  if (pos == std::string::npos) {
    throw PDNSException("Invalid connection string: malformed");
  }

  type = d_connstr.substr(0, pos);
  opts = d_connstr.substr(pos + 1);

  // tokenize the string on comma
  stringtok(parts, opts, ",");

  // find out some options and parse them while we're at it
  for (const auto& opt : parts) {
    std::string key;
    std::string val;
    // make sure there is something else than air in the option...
    if (opt.find_first_not_of(" ") == std::string::npos) {
      continue;
    }

    // split it on '='. if not found, we treat it as "yes"
    pos = opt.find_first_of("=");

    if (pos == std::string::npos) {
      key = opt;
      val = "yes";
    }
    else {
      key = opt.substr(0, pos);
      val = opt.substr(pos + 1);
    }
    options[key] = std::move(val);
  }

  // connectors know what they are doing
  if (type == "unix") {
    this->connector = std::make_unique<UnixsocketConnector>(options);
  }
  else if (type == "http") {
    this->connector = std::make_unique<HTTPConnector>(options);
  }
  else if (type == "zeromq") {
#ifdef REMOTEBACKEND_ZEROMQ
    this->connector = std::make_unique<ZeroMQConnector>(options);
#else
    throw PDNSException("Invalid connection string: zeromq connector support not enabled. Recompile with --enable-remotebackend-zeromq");
#endif
  }
  else if (type == "pipe") {
    this->connector = std::make_unique<PipeConnector>(options);
  }
  else {
    throw PDNSException("Invalid connection string: unknown connector");
  }

  return -1;
}

/**
 * The functions here are just remote json stubs that send and receive the method call
 * data is mainly left alone, some defaults are assumed.
 */
void RemoteBackend::lookup(const QType& qtype, const DNSName& qdomain, domainid_t zoneId, DNSPacket* pkt_p)
{
  if (d_index != -1) {
    throw PDNSException("Attempt to lookup while one running");
  }

  string localIP = "0.0.0.0";
  string remoteIP = "0.0.0.0";
  string realRemote = "0.0.0.0/0";

  if (pkt_p != nullptr) {
    localIP = pkt_p->getLocal().toString();
    realRemote = pkt_p->getRealRemote().toString();
    remoteIP = pkt_p->getInnerRemote().toString();
  }

  Json query = Json::object{
    {"method", "lookup"},
    {"parameters", Json::object{{"qtype", qtype.toString()}, {"qname", qdomain.toString()}, {"remote", remoteIP}, {"local", localIP}, {"real-remote", realRemote}, {"zone-id", zoneId}}}};

  if (!this->send(query) || !this->recv(d_result)) {
    return;
  }

  // OK. we have result parameters in result. do not process empty result.
  if (!d_result["result"].is_array() || d_result["result"].array_items().empty()) {
    return;
  }

  d_index = 0;
}

// Similar to lookup above, but passes an extra include_disabled parameter.
void RemoteBackend::APILookup(const QType& qtype, const DNSName& qdomain, domainid_t zoneId, bool include_disabled)
{
  if (d_index != -1) {
    throw PDNSException("Attempt to lookup while one running");
  }

  string localIP = "0.0.0.0";
  string remoteIP = "0.0.0.0";
  string realRemote = "0.0.0.0/0";

  Json query = Json::object{
    {"method", "APILookup"},
    {"parameters", Json::object{{"qtype", qtype.toString()}, {"qname", qdomain.toString()}, {"remote", remoteIP}, {"local", localIP}, {"real-remote", realRemote}, {"zone-id", zoneId}, {"include-disabled", include_disabled}}}};

  if (!this->send(query) || !this->recv(d_result)) {
    return;
  }

  // OK. we have result parameters in result. do not process empty result.
  if (!d_result["result"].is_array() || d_result["result"].array_items().empty()) {
    return;
  }

  d_index = 0;
}

bool RemoteBackend::list(const ZoneName& target, domainid_t domain_id, bool include_disabled)
{
  if (d_index != -1) {
    throw PDNSException("Attempt to lookup while one running");
  }

  Json query = Json::object{
    {"method", "list"},
    {"parameters", Json::object{{"zonename", target.toString()}, {"domain_id", domain_id}, {"include_disabled", include_disabled}}}};

  if (!this->send(query) || !this->recv(d_result)) {
    return false;
  }
  if (!d_result["result"].is_array() || d_result["result"].array_items().empty()) {
    return false;
  }

  d_index = 0;
  return true;
}

bool RemoteBackend::get(DNSResourceRecord& rr)
{
  if (d_index == -1) {
    return false;
  }

  rr.qtype = stringFromJson(d_result["result"][d_index], "qtype");
  rr.qname = DNSName(stringFromJson(d_result["result"][d_index], "qname"));
  rr.qclass = QClass::IN;
  rr.content = stringFromJson(d_result["result"][d_index], "content");
  rr.ttl = d_result["result"][d_index]["ttl"].int_value();
  rr.domain_id = static_cast<domainid_t>(intFromJson(d_result["result"][d_index], "domain_id", UnknownDomainID));
  if (d_dnssec) {
    rr.auth = (intFromJson(d_result["result"][d_index], "auth", 1) != 0);
  }
  else {
    rr.auth = true;
  }
  rr.scopeMask = d_result["result"][d_index]["scopeMask"].int_value();
  d_index++;

  // id index is out of bounds, we know the results end here.
  if (d_index == static_cast<int>(d_result["result"].array_items().size())) {
    d_result = Json();
    d_index = -1;
  }
  return true;
}

// NOLINTNEXTLINE(readability-identifier-length)
bool RemoteBackend::getBeforeAndAfterNamesAbsolute(domainid_t id, const DNSName& qname, DNSName& unhashed, DNSName& before, DNSName& after)
{
  // no point doing dnssec if it's not supported
  if (!d_dnssec) {
    return false;
  }

  Json query = Json::object{
    {"method", "getBeforeAndAfterNamesAbsolute"},
    {"parameters", Json::object{{"id", Json(id)}, {"qname", qname.toString()}}}};
  Json answer;

  if (!this->send(query) || !this->recv(answer)) {
    return false;
  }

  unhashed = DNSName(stringFromJson(answer["result"], "unhashed"));
  before.clear();
  after.clear();
  if (answer["result"]["before"] != Json()) {
    before = DNSName(stringFromJson(answer["result"], "before"));
  }
  if (answer["result"]["after"] != Json()) {
    after = DNSName(stringFromJson(answer["result"], "after"));
  }

  return true;
}

bool RemoteBackend::getAllDomainMetadata(const ZoneName& name, std::map<std::string, std::vector<std::string>>& meta)
{
  Json query = Json::object{
    {"method", "getAllDomainMetadata"},
    {"parameters", Json::object{{"name", name.toString()}}}};

  if (!this->send(query)) {
    return false;
  }

  meta.clear();

  Json answer;
  // not mandatory to implement
  if (!this->recv(answer)) {
    return true;
  }

  for (const auto& pair : answer["result"].object_items()) {
    if (pair.second.is_array()) {
      for (const auto& val : pair.second.array_items()) {
        meta[pair.first].push_back(asString(val));
      }
    }
    else {
      meta[pair.first].push_back(asString(pair.second));
    }
  }

  return true;
}

bool RemoteBackend::getDomainMetadata(const ZoneName& name, const std::string& kind, std::vector<std::string>& meta)
{
  Json query = Json::object{
    {"method", "getDomainMetadata"},
    {"parameters", Json::object{{"name", name.toString()}, {"kind", kind}}}};

  if (!this->send(query)) {
    return false;
  }

  meta.clear();

  Json answer;
  // not mandatory to implement
  if (!this->recv(answer)) {
    return true;
  }

  if (answer["result"].is_array()) {
    for (const auto& row : answer["result"].array_items()) {
      meta.push_back(row.string_value());
    }
  }
  else if (answer["result"].is_string()) {
    meta.push_back(answer["result"].string_value());
  }

  return true;
}

bool RemoteBackend::setDomainMetadata(const ZoneName& name, const std::string& kind, const std::vector<std::string>& meta)
{
  Json query = Json::object{
    {"method", "setDomainMetadata"},
    {"parameters", Json::object{{"name", name.toString()}, {"kind", kind}, {"value", meta}}}};

  Json answer;
  if (!this->send(query) || !this->recv(answer)) {
    return false;
  }

  return boolFromJson(answer, "result", false);
}

bool RemoteBackend::getDomainKeys(const ZoneName& name, std::vector<DNSBackend::KeyData>& keys)
{
  // no point doing dnssec if it's not supported
  if (!d_dnssec) {
    return false;
  }

  Json query = Json::object{
    {"method", "getDomainKeys"},
    {"parameters", Json::object{{"name", name.toString()}}}};

  Json answer;
  if (!this->send(query) || !this->recv(answer)) {
    return false;
  }

  keys.clear();

  for (const auto& jsonKey : answer["result"].array_items()) {
    DNSBackend::KeyData key;
    key.id = intFromJson(jsonKey, "id");
    key.flags = intFromJson(jsonKey, "flags");
    key.active = asBool(jsonKey["active"]);
    key.published = boolFromJson(jsonKey, "published", true);
    key.content = stringFromJson(jsonKey, "content");
    keys.emplace_back(std::move(key));
  }

  return true;
}

bool RemoteBackend::removeDomainKey(const ZoneName& name, unsigned int keyId)
{
  // no point doing dnssec if it's not supported
  if (!d_dnssec) {
    return false;
  }

  Json query = Json::object{
    {"method", "removeDomainKey"},
    {"parameters", Json::object{{"name", name.toString()}, {"id", static_cast<int>(keyId)}}}};

  Json answer;
  return this->send(query) && this->recv(answer);
}

bool RemoteBackend::addDomainKey(const ZoneName& name, const KeyData& key, int64_t& keyId)
{
  // no point doing dnssec if it's not supported
  if (!d_dnssec) {
    return false;
  }

  Json query = Json::object{
    {"method", "addDomainKey"},
    {"parameters", Json::object{{"name", name.toString()}, {"key", Json::object{{"flags", static_cast<int>(key.flags)}, {"active", key.active}, {"published", key.published}, {"content", key.content}}}}}};

  Json answer;
  if (!this->send(query) || !this->recv(answer)) {
    return false;
  }

  keyId = answer["result"].int_value();
  return keyId >= 0;
}

bool RemoteBackend::activateDomainKey(const ZoneName& name, unsigned int keyId)
{
  // no point doing dnssec if it's not supported
  if (!d_dnssec) {
    return false;
  }

  Json query = Json::object{
    {"method", "activateDomainKey"},
    {"parameters", Json::object{{"name", name.toString()}, {"id", static_cast<int>(keyId)}}}};

  Json answer;
  return this->send(query) && this->recv(answer);
}

bool RemoteBackend::deactivateDomainKey(const ZoneName& name, unsigned int keyId)
{
  // no point doing dnssec if it's not supported
  if (!d_dnssec) {
    return false;
  }

  Json query = Json::object{
    {"method", "deactivateDomainKey"},
    {"parameters", Json::object{{"name", name.toString()}, {"id", static_cast<int>(keyId)}}}};

  Json answer;
  return this->send(query) && this->recv(answer);
}

bool RemoteBackend::publishDomainKey(const ZoneName& name, unsigned int keyId)
{
  // no point doing dnssec if it's not supported
  if (!d_dnssec) {
    return false;
  }

  Json query = Json::object{
    {"method", "publishDomainKey"},
    {"parameters", Json::object{{"name", name.toString()}, {"id", static_cast<int>(keyId)}}}};

  Json answer;
  return this->send(query) && this->recv(answer);
}

bool RemoteBackend::unpublishDomainKey(const ZoneName& name, unsigned int keyId)
{
  // no point doing dnssec if it's not supported
  if (!d_dnssec) {
    return false;
  }

  Json query = Json::object{
    {"method", "unpublishDomainKey"},
    {"parameters", Json::object{{"name", name.toString()}, {"id", static_cast<int>(keyId)}}}};

  Json answer;
  return this->send(query) && this->recv(answer);
}

unsigned int RemoteBackend::getCapabilities()
{
  unsigned int caps = CAP_DIRECT | CAP_LIST | CAP_SEARCH;
  if (d_dnssec) {
    caps |= CAP_DNSSEC;
  }
  return caps;
}

bool RemoteBackend::getTSIGKey(const DNSName& name, DNSName& algorithm, std::string& content)
{
  // no point doing dnssec if it's not supported
  if (!d_dnssec) {
    return false;
  }

  Json query = Json::object{
    {"method", "getTSIGKey"},
    {"parameters", Json::object{{"name", name.toString()}}}};

  Json answer;
  if (!this->send(query) || !this->recv(answer)) {
    return false;
  }

  algorithm = DNSName(stringFromJson(answer["result"], "algorithm"));
  content = stringFromJson(answer["result"], "content");

  return true;
}

bool RemoteBackend::setTSIGKey(const DNSName& name, const DNSName& algorithm, const std::string& content)
{
  // no point doing dnssec if it's not supported
  if (!d_dnssec) {
    return false;
  }

  Json query = Json::object{
    {"method", "setTSIGKey"},
    {"parameters", Json::object{{"name", name.toString()}, {"algorithm", algorithm.toString()}, {"content", content}}}};

  Json answer;
  return connector->send(query) && connector->recv(answer);
}

bool RemoteBackend::deleteTSIGKey(const DNSName& name)
{
  // no point doing dnssec if it's not supported
  if (!d_dnssec) {
    return false;
  }
  Json query = Json::object{
    {"method", "deleteTSIGKey"},
    {"parameters", Json::object{{"name", name.toString()}}}};

  Json answer;
  return connector->send(query) && connector->recv(answer);
}

bool RemoteBackend::getTSIGKeys(std::vector<struct TSIGKey>& keys)
{
  // no point doing dnssec if it's not supported
  if (!d_dnssec) {
    return false;
  }
  Json query = Json::object{
    {"method", "getTSIGKeys"},
    {"parameters", Json::object{}}};

  Json answer;
  if (!connector->send(query) || !connector->recv(answer)) {
    return false;
  }

  for (const auto& jsonKey : answer["result"].array_items()) {
    struct TSIGKey key;
    key.name = DNSName(stringFromJson(jsonKey, "name"));
    key.algorithm = DNSName(stringFromJson(jsonKey, "algorithm"));
    key.key = stringFromJson(jsonKey, "content");
    keys.push_back(key);
  }

  return true;
}

void RemoteBackend::parseDomainInfo(const Json& obj, DomainInfo& di)
{
  di.id = static_cast<domainid_t>(intFromJson(obj, "id", UnknownDomainID));
  di.zone = ZoneName(stringFromJson(obj, "zone"));
  for (const auto& primary : obj["masters"].array_items()) {
    di.primaries.emplace_back(primary.string_value(), 53);
  }

  di.notified_serial = static_cast<unsigned int>(doubleFromJson(obj, "notified_serial", 0));
  di.serial = static_cast<unsigned int>(obj["serial"].number_value());
  di.last_check = static_cast<time_t>(obj["last_check"].number_value());

  string kind;
  if (obj["kind"].is_string()) {
    kind = stringFromJson(obj, "kind");
  }
  if (kind == "master") {
    di.kind = DomainInfo::Primary;
  }
  else if (kind == "slave") {
    di.kind = DomainInfo::Secondary;
  }
  else {
    di.kind = DomainInfo::Native;
  }
  di.backend = this;
}

bool RemoteBackend::getDomainInfo(const ZoneName& domain, DomainInfo& info, bool /* getSerial */)
{
  if (domain.empty()) {
    return false;
  }

  Json query = Json::object{
    {"method", "getDomainInfo"},
    {"parameters", Json::object{{"name", domain.toString()}}}};

  Json answer;
  if (!this->send(query) || !this->recv(answer)) {
    return false;
  }

  this->parseDomainInfo(answer["result"], info);
  return true;
}

// NOLINTNEXTLINE(readability-identifier-length)
void RemoteBackend::setNotified(domainid_t id, uint32_t serial)
{
  Json query = Json::object{
    {"method", "setNotified"},
    {"parameters", Json::object{{"id", id}, {"serial", static_cast<double>(serial)}}}};

  Json answer;
  if (!this->send(query) || !this->recv(answer)) {
    g_log << Logger::Error << kBackendId << " Failed to execute RPC for RemoteBackend::setNotified(" << id << "," << serial << ")" << endl;
  }
}

bool RemoteBackend::autoPrimaryBackend(const string& ipAddress, const ZoneName& domain, const vector<DNSResourceRecord>& nsset, string* nameserver, string* account, DNSBackend** ddb)
{
  Json::array rrset;

  for (const auto& ns : nsset) {
    rrset.push_back(Json::object{
      {"qtype", ns.qtype.toString()},
      {"qname", ns.qname.toString()},
      {"qclass", QClass::IN.getCode()},
      {"content", ns.content},
      {"ttl", static_cast<int>(ns.ttl)},
      {"auth", ns.auth}});
  }

  Json query = Json::object{
    {"method", "superMasterBackend"},
    {"parameters", Json::object{{"ip", ipAddress}, {"domain", domain.toString()}, {"nsset", rrset}}}};

  *ddb = nullptr;

  Json answer;
  if (!this->send(query) || !this->recv(answer)) {
    return false;
  }

  // we are the backend
  *ddb = this;

  // we allow simple true as well...
  if (answer["result"].is_object()) {
    *account = stringFromJson(answer["result"], "account");
    *nameserver = stringFromJson(answer["result"], "nameserver");
  }

  return true;
}

bool RemoteBackend::createSecondaryDomain(const string& ipAddress, const ZoneName& domain, const string& nameserver, const string& account)
{
  Json query = Json::object{
    {"method", "createSlaveDomain"},
    {"parameters", Json::object{
                     {"ip", ipAddress},
                     {"domain", domain.toString()},
                     {"nameserver", nameserver},
                     {"account", account},
                   }}};

  Json answer;
  return this->send(query) && this->recv(answer);
}

bool RemoteBackend::replaceRRSet(domainid_t domain_id, const DNSName& qname, const QType& qtype, const vector<DNSResourceRecord>& rrset)
{
  Json::array json_rrset;
  for (const auto& rr : rrset) {
    json_rrset.push_back(Json::object{
      {"qtype", rr.qtype.toString()},
      {"qname", rr.qname.toString()},
      {"qclass", QClass::IN.getCode()},
      {"content", rr.content},
      {"ttl", static_cast<int>(rr.ttl)},
      {"auth", rr.auth}});
  }

  Json query = Json::object{
    {"method", "replaceRRSet"},
    {"parameters", Json::object{{"domain_id", domain_id}, {"qname", qname.toString()}, {"qtype", qtype.toString()}, {"trxid", static_cast<double>(d_trxid)}, {"rrset", json_rrset}}}};

  Json answer;
  return this->send(query) && this->recv(answer);
}

bool RemoteBackend::feedRecord(const DNSResourceRecord& rr, const DNSName& ordername, bool /* ordernameIsNSEC3 */)
{
  Json query = Json::object{
    {"method", "feedRecord"},
    {"parameters", Json::object{
                     {"rr", Json::object{{"qtype", rr.qtype.toString()}, {"qname", rr.qname.toString()}, {"qclass", QClass::IN.getCode()}, {"content", rr.content}, {"ttl", static_cast<int>(rr.ttl)}, {"auth", rr.auth}, {"ordername", (ordername.empty() ? Json() : ordername.toString())}}},
                     {"trxid", static_cast<double>(d_trxid)},
                   }}};

  Json answer;
  return this->send(query) && this->recv(answer); // XXX FIXME this API should not return 'true' I think -ahu
}

bool RemoteBackend::feedEnts(domainid_t domain_id, map<DNSName, bool>& nonterm)
{
  Json::array nts;

  for (const auto& t : nonterm) {
    nts.push_back(Json::object{
      {"nonterm", t.first.toString()},
      {"auth", t.second}});
  }

  Json query = Json::object{
    {"method", "feedEnts"},
    {"parameters", Json::object{{"domain_id", domain_id}, {"trxid", static_cast<double>(d_trxid)}, {"nonterm", nts}}},
  };

  Json answer;
  return this->send(query) && this->recv(answer);
}

bool RemoteBackend::feedEnts3(domainid_t domain_id, const DNSName& domain, map<DNSName, bool>& nonterm, const NSEC3PARAMRecordContent& ns3prc, bool narrow)
{
  Json::array nts;

  for (const auto& t : nonterm) {
    nts.push_back(Json::object{
      {"nonterm", t.first.toString()},
      {"auth", t.second}});
  }

  Json query = Json::object{
    {"method", "feedEnts3"},
    {"parameters", Json::object{{"domain_id", domain_id}, {"domain", domain.toString()}, {"times", ns3prc.d_iterations}, {"salt", ns3prc.d_salt}, {"narrow", narrow}, {"trxid", static_cast<double>(d_trxid)}, {"nonterm", nts}}},
  };

  Json answer;
  return this->send(query) && this->recv(answer);
}

bool RemoteBackend::startTransaction(const ZoneName& domain, domainid_t domain_id)
{
  this->d_trxid = time((time_t*)nullptr);

  Json query = Json::object{
    {"method", "startTransaction"},
    {"parameters", Json::object{{"domain", domain.toString()}, {"domain_id", domain_id}, {"trxid", static_cast<double>(d_trxid)}}}};

  Json answer;
  if (!this->send(query) || !this->recv(answer)) {
    d_trxid = -1;
    return false;
  }
  return true;
}

bool RemoteBackend::commitTransaction()
{
  if (d_trxid == -1) {
    return false;
  }

  Json query = Json::object{
    {"method", "commitTransaction"},
    {"parameters", Json::object{{"trxid", static_cast<double>(d_trxid)}}}};

  d_trxid = -1;
  Json answer;
  return this->send(query) && this->recv(answer);
}

bool RemoteBackend::abortTransaction()
{
  if (d_trxid == -1) {
    return false;
  }

  Json query = Json::object{
    {"method", "abortTransaction"},
    {"parameters", Json::object{{"trxid", static_cast<double>(d_trxid)}}}};

  d_trxid = -1;
  Json answer;
  return this->send(query) && this->recv(answer);
}

string RemoteBackend::directBackendCmd(const string& querystr)
{
  Json query = Json::object{
    {"method", "directBackendCmd"},
    {"parameters", Json::object{{"query", querystr}}}};

  Json answer;
  if (!this->send(query) || !this->recv(answer)) {
    return "backend command failed";
  }

  return asString(answer["result"]);
}

bool RemoteBackend::searchRecords(const string& pattern, size_t maxResults, vector<DNSResourceRecord>& result)
{
  const auto intMax = static_cast<decltype(maxResults)>(std::numeric_limits<int>::max());
  if (maxResults > intMax) {
    throw std::out_of_range("Remote backend: length of list of result (" + std::to_string(maxResults) + ") is larger than what the JSON library supports for serialization (" + std::to_string(intMax) + ")");
  }

  Json query = Json::object{
    {"method", "searchRecords"},
    {"parameters", Json::object{{"pattern", pattern}, {"maxResults", static_cast<int>(maxResults)}}}};

  Json answer;
  if (!this->send(query) || !this->recv(answer)) {
    return false;
  }

  if (!answer["result"].is_array()) {
    return false;
  }

  for (const auto& row : answer["result"].array_items()) {
    DNSResourceRecord rr;
    rr.qtype = stringFromJson(row, "qtype");
    rr.qname = DNSName(stringFromJson(row, "qname"));
    rr.qclass = QClass::IN;
    rr.content = stringFromJson(row, "content");
    rr.ttl = row["ttl"].int_value();
    rr.domain_id = static_cast<domainid_t>(intFromJson(row, "domain_id", UnknownDomainID));
    if (d_dnssec) {
      rr.auth = (intFromJson(row, "auth", 1) != 0);
    }
    else {
      rr.auth = 1;
    }
    rr.scopeMask = row["scopeMask"].int_value();
    result.push_back(rr);
  }

  return true;
}

bool RemoteBackend::searchComments(const string& /* pattern */, size_t /* maxResults */, vector<Comment>& /* result */)
{
  // FIXME: Implement Comment API
  return false;
}

void RemoteBackend::getAllDomains(vector<DomainInfo>* domains, bool /* getSerial */, bool include_disabled)
{
  Json query = Json::object{
    {"method", "getAllDomains"},
    {"parameters", Json::object{{"include_disabled", include_disabled}}}};

  Json answer;
  if (!this->send(query) || !this->recv(answer)) {
    return;
  }

  if (!answer["result"].is_array()) {
    return;
  }

  for (const auto& row : answer["result"].array_items()) {
    DomainInfo di;
    this->parseDomainInfo(row, di);
    domains->push_back(di);
  }
}

void RemoteBackend::getUpdatedPrimaries(vector<DomainInfo>& domains, std::unordered_set<DNSName>& /* catalogs */, CatalogHashMap& /* catalogHashes */)
{
  Json query = Json::object{
    {"method", "getUpdatedMasters"},
    {"parameters", Json::object{}},
  };

  Json answer;
  if (!this->send(query) || !this->recv(answer)) {
    return;
  }

  if (!answer["result"].is_array()) {
    return;
  }

  for (const auto& row : answer["result"].array_items()) {
    DomainInfo di;
    this->parseDomainInfo(row, di);
    domains.push_back(di);
  }
}

void RemoteBackend::getUnfreshSecondaryInfos(vector<DomainInfo>* domains)
{
  Json query = Json::object{
    {"method", "getUnfreshSlaveInfos"},
    {"parameters", Json::object{}},
  };

  Json answer;
  if (!this->send(query) || !this->recv(answer)) {
    return;
  }

  if (!answer["result"].is_array()) {
    return;
  }

  for (const auto& row : answer["result"].array_items()) {
    DomainInfo di;
    this->parseDomainInfo(row, di);
    domains->push_back(di);
  }
}

void RemoteBackend::setStale(domainid_t domain_id)
{
  Json query = Json::object{
    {"method", "setStale"},
    {"parameters", Json::object{{"id", domain_id}}}};

  Json answer;
  if (!this->send(query) || !this->recv(answer)) {
    g_log << Logger::Error << kBackendId << " Failed to execute RPC for RemoteBackend::setStale(" << domain_id << ")" << endl;
  }
}

void RemoteBackend::setFresh(domainid_t domain_id)
{
  Json query = Json::object{
    {"method", "setFresh"},
    {"parameters", Json::object{{"id", domain_id}}}};

  Json answer;
  if (!this->send(query) || !this->recv(answer)) {
    g_log << Logger::Error << kBackendId << " Failed to execute RPC for RemoteBackend::setFresh(" << domain_id << ")" << endl;
  }
}

DNSBackend* RemoteBackend::maker()
{
  try {
    return new RemoteBackend();
  }
  catch (...) {
    g_log << Logger::Error << kBackendId << " Unable to instantiate a remotebackend!" << endl;
    return nullptr;
  };
}

class RemoteBackendFactory : public BackendFactory
{
public:
  RemoteBackendFactory() :
    BackendFactory("remote") {}

  void declareArguments(const std::string& suffix = "") override
  {
    declare(suffix, "dnssec", "Enable dnssec support", "no");
    declare(suffix, "connection-string", "Connection string", "");
  }

  DNSBackend* make(const std::string& suffix = "") override
  {
    return new RemoteBackend(suffix);
  }
};

class RemoteLoader
{
public:
  RemoteLoader();
};

RemoteLoader::RemoteLoader()
{
  BackendMakers().report(std::make_unique<RemoteBackendFactory>());
  g_log << Logger::Info << kBackendId << " This is the remote backend version " VERSION
#ifndef REPRODUCIBLE
        << " (" __DATE__ " " __TIME__ ")"
#endif
        << " reporting" << endl;
}

static RemoteLoader remoteloader;
