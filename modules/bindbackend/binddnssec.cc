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
#include "bindbackend2.hh"
#include "pdns/arguments.hh"
#include "pdns/dnsrecords.hh"

#ifndef HAVE_SQLITE3

void Bind2Backend::setupDNSSEC()
{
  if (!getArg("dnssec-db").empty()) {
    throw runtime_error("bind-dnssec-db requires building PowerDNS with SQLite3");
  }
}

unsigned int Bind2Backend::getCapabilities()
{
  unsigned int caps = CAP_LIST | CAP_SEARCH;
  if (d_hybrid) {
    caps |= CAP_DNSSEC;
  }
  return caps;
}

bool Bind2Backend::getNSEC3PARAM(const ZoneName& /* name */, NSEC3PARAMRecordContent* /* ns3p */)
{
  return false;
}

bool Bind2Backend::getNSEC3PARAMuncached(const ZoneName& /* name */, NSEC3PARAMRecordContent* /* ns3p */)
{
  return false;
}

bool Bind2Backend::getAllDomainMetadata(const ZoneName& /* name */, std::map<std::string, std::vector<std::string>>& /* meta */)
{
  return false;
}

bool Bind2Backend::getDomainMetadata(const ZoneName& /* name */, const std::string& /* kind */, std::vector<std::string>& /* meta */)
{
  return false;
}

bool Bind2Backend::setDomainMetadata(const ZoneName& /* name */, const std::string& /* kind */, const std::vector<std::string>& /* meta */)
{
  return false;
}

bool Bind2Backend::getDomainKeys(const ZoneName& /* name */, std::vector<KeyData>& /* keys */)
{
  return false;
}

bool Bind2Backend::removeDomainKey(const ZoneName& /* name */, unsigned int /* id */)
{
  return false;
}

bool Bind2Backend::addDomainKey(const ZoneName& /* name */, const KeyData& /* key */, int64_t& /* id */)
{
  return false;
}

bool Bind2Backend::activateDomainKey(const ZoneName& /* name */, unsigned int /* id */)
{
  return false;
}

bool Bind2Backend::deactivateDomainKey(const ZoneName& /* name */, unsigned int /* id */)
{
  return false;
}

bool Bind2Backend::publishDomainKey(const ZoneName& /* name */, unsigned int /* id */)
{
  return false;
}

bool Bind2Backend::unpublishDomainKey(const ZoneName& /* name */, unsigned int /* id */)
{
  return false;
}

bool Bind2Backend::getTSIGKey(const DNSName& /* name */, DNSName& /* algorithm */, string& /* content */)
{
  return false;
}

bool Bind2Backend::setTSIGKey(const DNSName& /* name */, const DNSName& /* algorithm */, const string& /* content */)
{
  return false;
}

bool Bind2Backend::deleteTSIGKey(const DNSName& /* name */)
{
  return false;
}

bool Bind2Backend::getTSIGKeys(std::vector<struct TSIGKey>& /* keys */)
{
  return false;
}

void Bind2Backend::setupStatements()
{
}

void Bind2Backend::freeStatements()
{
}

#else

#include "pdns/logger.hh"
#include "pdns/ssqlite3.hh"

#define ASSERT_ROW_COLUMNS(query, row, num)                                                                                                \
  {                                                                                                                                        \
    if (row.size() != num) {                                                                                                               \
      throw PDNSException(std::string(query) + " returned wrong number of columns, expected " #num ", got " + std::to_string(row.size())); \
    }                                                                                                                                      \
  }

void Bind2Backend::setupDNSSEC()
{
  if (getArg("dnssec-db").empty() || d_hybrid)
    return;
  try {
    d_dnssecdb = std::make_shared<SSQLite3>(getArg("dnssec-db"), getArg("dnssec-db-journal-mode"));
    setupStatements();
  }
  catch (SSqlException& se) {
    // this error is meant to kill the server dead - it makes no sense to continue..
    throw runtime_error("Error opening DNSSEC database in BIND backend: " + se.txtReason());
  }

  d_dnssecdb->setLog(::arg().mustDo("query-logging"));
}

void Bind2Backend::setupStatements()
{
  d_getAllDomainMetadataQuery_stmt = d_dnssecdb->prepare("select kind, content from domainmetadata where domain=:domain", 1);
  d_getDomainMetadataQuery_stmt = d_dnssecdb->prepare("select content from domainmetadata where domain=:domain and kind=:kind", 2);
  d_deleteDomainMetadataQuery_stmt = d_dnssecdb->prepare("delete from domainmetadata where domain=:domain and kind=:kind", 2);
  d_insertDomainMetadataQuery_stmt = d_dnssecdb->prepare("insert into domainmetadata (domain, kind, content) values (:domain,:kind,:content)", 3);
  d_getDomainKeysQuery_stmt = d_dnssecdb->prepare("select id,flags, active, published, content from cryptokeys where domain=:domain", 1);
  d_deleteDomainKeyQuery_stmt = d_dnssecdb->prepare("delete from cryptokeys where domain=:domain and id=:key_id", 2);
  d_insertDomainKeyQuery_stmt = d_dnssecdb->prepare("insert into cryptokeys (domain, flags, active, published, content) values (:domain, :flags, :active, :published, :content)", 5);
  d_GetLastInsertedKeyIdQuery_stmt = d_dnssecdb->prepare("select last_insert_rowid()", 0);
  d_activateDomainKeyQuery_stmt = d_dnssecdb->prepare("update cryptokeys set active=1 where domain=:domain and id=:key_id", 2);
  d_deactivateDomainKeyQuery_stmt = d_dnssecdb->prepare("update cryptokeys set active=0 where domain=:domain and id=:key_id", 2);
  d_publishDomainKeyQuery_stmt = d_dnssecdb->prepare("update cryptokeys set published=1 where domain=:domain and id=:key_id", 2);
  d_unpublishDomainKeyQuery_stmt = d_dnssecdb->prepare("update cryptokeys set published=0 where domain=:domain and id=:key_id", 2);
  d_getTSIGKeyQuery_stmt = d_dnssecdb->prepare("select algorithm, secret from tsigkeys where name=:key_name", 1);
  d_setTSIGKeyQuery_stmt = d_dnssecdb->prepare("replace into tsigkeys (name,algorithm,secret) values(:key_name, :algorithm, :content)", 3);
  d_deleteTSIGKeyQuery_stmt = d_dnssecdb->prepare("delete from tsigkeys where name=:key_name", 1);
  d_getTSIGKeysQuery_stmt = d_dnssecdb->prepare("select name,algorithm,secret from tsigkeys", 0);
}

void Bind2Backend::freeStatements()
{
  d_getAllDomainMetadataQuery_stmt.reset();
  d_getDomainMetadataQuery_stmt.reset();
  d_deleteDomainMetadataQuery_stmt.reset();
  d_insertDomainMetadataQuery_stmt.reset();
  d_getDomainKeysQuery_stmt.reset();
  d_deleteDomainKeyQuery_stmt.reset();
  d_insertDomainKeyQuery_stmt.reset();
  d_GetLastInsertedKeyIdQuery_stmt.reset();
  d_activateDomainKeyQuery_stmt.reset();
  d_deactivateDomainKeyQuery_stmt.reset();
  d_publishDomainKeyQuery_stmt.reset();
  d_unpublishDomainKeyQuery_stmt.reset();
  d_getTSIGKeyQuery_stmt.reset();
  d_setTSIGKeyQuery_stmt.reset();
  d_deleteTSIGKeyQuery_stmt.reset();
  d_getTSIGKeysQuery_stmt.reset();
}

unsigned int Bind2Backend::getCapabilities()
{
  unsigned int caps = CAP_LIST | CAP_SEARCH;
  if (d_dnssecdb || d_hybrid) {
    caps |= CAP_DNSSEC;
  }
  return caps;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool Bind2Backend::getNSEC3PARAM(const ZoneName& name, NSEC3PARAMRecordContent* ns3p)
{
  BB2DomainInfo bbd;
  if (!safeGetBBDomainInfo(name, &bbd))
    return false;

  if (ns3p != nullptr) {
    *ns3p = bbd.d_nsec3param;
  }

  return bbd.d_nsec3zone;
}

bool Bind2Backend::getNSEC3PARAMuncached(const ZoneName& name, NSEC3PARAMRecordContent* ns3p)
{
  if (!d_dnssecdb || d_hybrid)
    return false;

  string value;
  vector<string> meta;
  getDomainMetadata(name, "NSEC3PARAM", meta);
  if (!meta.empty())
    value = *meta.begin();
  else
    return false; // No NSEC3 zone

  static int maxNSEC3Iterations = ::arg().asNum("max-nsec3-iterations");
  if (ns3p) {
    auto tmp = std::dynamic_pointer_cast<NSEC3PARAMRecordContent>(DNSRecordContent::make(QType::NSEC3PARAM, 1, value));
    *ns3p = *tmp;

    if (ns3p->d_iterations > maxNSEC3Iterations) {
      ns3p->d_iterations = maxNSEC3Iterations;
      g_log << Logger::Error << "Number of NSEC3 iterations for zone '" << name << "' is above 'max-nsec3-iterations'. Value adjusted to: " << maxNSEC3Iterations << endl;
    }

    if (ns3p->d_algorithm != 1) {
      g_log << Logger::Error << "Invalid hash algorithm for NSEC3: '" << std::to_string(ns3p->d_algorithm) << "', setting to 1 for zone '" << name << "'." << endl;
      ns3p->d_algorithm = 1;
    }
  }

  return true;
}

bool Bind2Backend::getAllDomainMetadata(const ZoneName& name, std::map<std::string, std::vector<std::string>>& meta)
{
  if (!d_dnssecdb || d_hybrid)
    return false;

  try {
    d_getAllDomainMetadataQuery_stmt->bind("domain", name)->execute();

    SSqlStatement::row_t row;
    while (d_getAllDomainMetadataQuery_stmt->hasNextRow()) {
      d_getAllDomainMetadataQuery_stmt->nextRow(row);
      meta[row[0]].push_back(row[1]);
    }

    d_getAllDomainMetadataQuery_stmt->reset();
  }
  catch (SSqlException& se) {
    throw PDNSException("Error accessing DNSSEC database in BIND backend, getAllDomainMetadata(): " + se.txtReason());
  }
  return true;
}

bool Bind2Backend::getDomainMetadata(const ZoneName& name, const std::string& kind, std::vector<std::string>& meta)
{
  if (!d_dnssecdb || d_hybrid)
    return false;

  try {
    d_getDomainMetadataQuery_stmt->bind("domain", name)->bind("kind", kind)->execute();

    SSqlStatement::row_t row;
    while (d_getDomainMetadataQuery_stmt->hasNextRow()) {
      d_getDomainMetadataQuery_stmt->nextRow(row);
      meta.push_back(row[0]);
    }

    d_getDomainMetadataQuery_stmt->reset();
  }
  catch (SSqlException& se) {
    throw PDNSException("Error accessing DNSSEC database in BIND backend, getDomainMetadata(): " + se.txtReason());
  }
  return true;
}

bool Bind2Backend::setDomainMetadata(const ZoneName& name, const std::string& kind, const std::vector<std::string>& meta)
{
  if (!d_dnssecdb || d_hybrid)
    return false;

  try {
    d_deleteDomainMetadataQuery_stmt->bind("domain", name)->bind("kind", kind)->execute()->reset();
    if (!meta.empty()) {
      for (const auto& value : meta) {
        d_insertDomainMetadataQuery_stmt->bind("domain", name)->bind("kind", kind)->bind("content", value)->execute()->reset();
      }
    }
  }
  catch (SSqlException& se) {
    throw PDNSException("Error accessing DNSSEC database in BIND backend, setDomainMetadata(): " + se.txtReason());
  }
  return true;
}

bool Bind2Backend::getDomainKeys(const ZoneName& name, std::vector<KeyData>& keys)
{
  if (!d_dnssecdb || d_hybrid)
    return false;

  try {
    d_getDomainKeysQuery_stmt->bind("domain", name)->execute();

    KeyData kd;
    SSqlStatement::row_t row;
    while (d_getDomainKeysQuery_stmt->hasNextRow()) {
      d_getDomainKeysQuery_stmt->nextRow(row);
      pdns::checked_stoi_into(kd.id, row[0]);
      pdns::checked_stoi_into(kd.flags, row[1]);
      kd.active = (row[2] == "1");
      kd.published = (row[3] == "1");
      kd.content = row[4];
      keys.push_back(kd);
    }

    d_getDomainKeysQuery_stmt->reset();
  }
  catch (SSqlException& se) {
    throw PDNSException("Error accessing DNSSEC database in BIND backend, getDomainKeys(): " + se.txtReason());
  }
  return true;
}

bool Bind2Backend::removeDomainKey(const ZoneName& name, unsigned int keyId)
{
  if (!d_dnssecdb || d_hybrid)
    return false;

  try {
    d_deleteDomainKeyQuery_stmt->bind("domain", name)->bind("key_id", keyId)->execute()->reset();
  }
  catch (SSqlException& se) {
    throw PDNSException("Error accessing DNSSEC database in BIND backend, removeDomainKeys(): " + se.txtReason());
  }
  return true;
}

bool Bind2Backend::addDomainKey(const ZoneName& name, const KeyData& key, int64_t& keyId)
{
  if (!d_dnssecdb || d_hybrid)
    return false;

  try {
    d_insertDomainKeyQuery_stmt->bind("domain", name)->bind("flags", key.flags)->bind("active", key.active)->bind("published", key.published)->bind("content", key.content)->execute()->reset();
  }
  catch (SSqlException& se) {
    throw PDNSException("Error accessing DNSSEC database in BIND backend, addDomainKey(): " + se.txtReason());
  }

  try {
    d_GetLastInsertedKeyIdQuery_stmt->execute();
    if (!d_GetLastInsertedKeyIdQuery_stmt->hasNextRow()) {
      keyId = -2;
      return true;
    }
    SSqlStatement::row_t row;
    d_GetLastInsertedKeyIdQuery_stmt->nextRow(row);
    ASSERT_ROW_COLUMNS("get-last-inserted-key-id-query", row, 1);
    keyId = std::stoi(row[0]);
    d_GetLastInsertedKeyIdQuery_stmt->reset();
    if (keyId == 0) {
      // No insert took place, report as error.
      keyId = -1;
    }
    return true;
  }
  catch (SSqlException& e) {
    keyId = -2;
    return true;
  }
}

bool Bind2Backend::activateDomainKey(const ZoneName& name, unsigned int keyId)
{
  if (!d_dnssecdb || d_hybrid)
    return false;

  try {
    d_activateDomainKeyQuery_stmt->bind("domain", name)->bind("key_id", keyId)->execute()->reset();
  }
  catch (SSqlException& se) {
    throw PDNSException("Error accessing DNSSEC database in BIND backend, activateDomainKey(): " + se.txtReason());
  }
  return true;
}

bool Bind2Backend::deactivateDomainKey(const ZoneName& name, unsigned int keyId)
{
  if (!d_dnssecdb || d_hybrid)
    return false;

  try {
    d_deactivateDomainKeyQuery_stmt->bind("domain", name)->bind("key_id", keyId)->execute()->reset();
  }
  catch (SSqlException& se) {
    throw PDNSException("Error accessing DNSSEC database in BIND backend, deactivateDomainKey(): " + se.txtReason());
  }
  return true;
}

bool Bind2Backend::publishDomainKey(const ZoneName& name, unsigned int keyId)
{
  if (!d_dnssecdb || d_hybrid)
    return false;

  try {
    d_publishDomainKeyQuery_stmt->bind("domain", name)->bind("key_id", keyId)->execute()->reset();
  }
  catch (SSqlException& se) {
    throw PDNSException("Error accessing DNSSEC database in BIND backend, publishDomainKey(): " + se.txtReason());
  }
  return true;
}

bool Bind2Backend::unpublishDomainKey(const ZoneName& name, unsigned int keyId)
{
  if (!d_dnssecdb || d_hybrid)
    return false;

  try {
    d_unpublishDomainKeyQuery_stmt->bind("domain", name)->bind("key_id", keyId)->execute()->reset();
  }
  catch (SSqlException& se) {
    throw PDNSException("Error accessing DNSSEC database in BIND backend, unpublishDomainKey(): " + se.txtReason());
  }
  return true;
}

bool Bind2Backend::getTSIGKey(const DNSName& name, DNSName& algorithm, string& content)
{
  if (!d_dnssecdb || d_hybrid)
    return false;

  try {
    d_getTSIGKeyQuery_stmt->bind("key_name", name)->execute();

    SSqlStatement::row_t row;
    while (d_getTSIGKeyQuery_stmt->hasNextRow()) {
      d_getTSIGKeyQuery_stmt->nextRow(row);
      if (row.size() >= 2 && (algorithm.empty() || algorithm == DNSName(row[0]))) {
        algorithm = DNSName(row[0]);
        content = row[1];
      }
    }

    d_getTSIGKeyQuery_stmt->reset();
  }
  catch (SSqlException& e) {
    throw PDNSException("Error accessing DNSSEC database in BIND backend, getTSIGKey(): " + e.txtReason());
  }
  return true;
}

bool Bind2Backend::setTSIGKey(const DNSName& name, const DNSName& algorithm, const string& content)
{
  if (!d_dnssecdb || d_hybrid)
    return false;

  try {
    d_setTSIGKeyQuery_stmt->bind("key_name", name)->bind("algorithm", algorithm)->bind("content", content)->execute()->reset();
  }
  catch (SSqlException& e) {
    throw PDNSException("Error accessing DNSSEC database in BIND backend, setTSIGKey(): " + e.txtReason());
  }
  return true;
}

bool Bind2Backend::deleteTSIGKey(const DNSName& name)
{
  if (!d_dnssecdb || d_hybrid)
    return false;

  try {
    d_deleteTSIGKeyQuery_stmt->bind("key_name", name)->execute()->reset();
  }
  catch (SSqlException& e) {
    throw PDNSException("Error accessing DNSSEC database in BIND backend, deleteTSIGKey(): " + e.txtReason());
  }
  return true;
}

bool Bind2Backend::getTSIGKeys(std::vector<struct TSIGKey>& keys)
{
  if (!d_dnssecdb || d_hybrid)
    return false;

  try {
    d_getTSIGKeysQuery_stmt->execute();

    SSqlStatement::row_t row;
    while (d_getTSIGKeysQuery_stmt->hasNextRow()) {
      d_getTSIGKeysQuery_stmt->nextRow(row);
      struct TSIGKey key;
      key.name = DNSName(row[0]);
      key.algorithm = DNSName(row[1]);
      key.key = row[2];
      keys.push_back(key);
    }

    d_getTSIGKeysQuery_stmt->reset();
  }
  catch (SSqlException& e) {
    throw PDNSException("Error accessing DNSSEC database in BIND backend, getTSIGKeys(): " + e.txtReason());
  }
  return true;
}

#endif
