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

#include "auth-querycache.hh"
#include "logger.hh"
#include "statbag.hh"
#include "cachecleaner.hh"
extern StatBag S;

const unsigned int AuthQueryCache::s_mincleaninterval, AuthQueryCache::s_maxcleaninterval;

AuthQueryCache::AuthQueryCache(size_t mapsCount): d_maps(mapsCount), d_lastclean(time(nullptr))
{
  S.declare("query-cache-hit","Number of hits on the query cache");
  S.declare("query-cache-miss","Number of misses on the query cache");
  S.declare("query-cache-size", "Number of entries in the query cache", StatType::gauge);
  S.declare("deferred-cache-inserts","Amount of cache inserts that were deferred because of maintenance");
  S.declare("deferred-cache-lookup","Amount of cache lookups that were deferred because of maintenance");

  d_statnumhit=S.getPointer("query-cache-hit");
  d_statnummiss=S.getPointer("query-cache-miss");
  d_statnumentries=S.getPointer("query-cache-size");
}

void AuthQueryCache::MapCombo::reserve(size_t numberOfEntries)
{
#if BOOST_VERSION >= 105600
  d_map.write_lock()->get<HashTag>().reserve(numberOfEntries);
#endif /* BOOST_VERSION >= 105600 */
}

// called from ueberbackend
bool AuthQueryCache::getEntry(const DNSName &qname, const QType& qtype, vector<DNSZoneRecord>& value, domainid_t zoneID)
{
  cleanupIfNeeded();

  time_t now = time(nullptr);
  uint16_t qt = qtype.getCode();
  auto& mc = getMap(qname);
  {
    auto map = mc.d_map.try_read_lock();
    if (!map.owns_lock()) {
      S.inc("deferred-cache-lookup");
      return false;
    }

    return getEntryLocked(*map, qname, qt, value, zoneID, now);
  }
}

void AuthQueryCache::insert(const DNSName &qname, const QType& qtype, vector<DNSZoneRecord>&& value, uint32_t ttl, domainid_t zoneID)
{
  cleanupIfNeeded();

  if(!ttl)
    return;

  time_t now = time(nullptr);
  CacheEntry val;
  val.ttd = now + ttl;
  val.qname = qname;
  val.qtype = qtype.getCode();
  val.drs = std::move(value);
  val.zoneID = zoneID;

  auto& mc = getMap(val.qname);

  {
    auto map = mc.d_map.try_write_lock();
    if (!map.owns_lock()) {
      S.inc("deferred-cache-inserts"); 
      return;
    }

    bool inserted;
    cmap_t::iterator place;
    std::tie(place, inserted) = map->insert(val);

    if (!inserted) {
      map->replace(place, std::move(val));
      moveCacheItemToBack<SequencedTag>(*map, place);
    }
    else {
      if (*d_statnumentries >= d_maxEntries) {
        /* remove the least recently inserted or replaced entry */
        auto& sidx = map->get<SequencedTag>();
        sidx.pop_front();
      }
      else {
        (*d_statnumentries)++;
      }
    }
  }
}

bool AuthQueryCache::getEntryLocked(const cmap_t& map, const DNSName &qname, uint16_t qtype, vector<DNSZoneRecord>& value, domainid_t zoneID, time_t now)
{
  auto& idx = boost::multi_index::get<HashTag>(map);
  auto iter = idx.find(std::tie(qname, qtype, zoneID));

  if (iter == idx.end()) {
    (*d_statnummiss)++;
    return false;
  }

  if (iter->ttd < now) {
    (*d_statnummiss)++;
    return false;
  }

  value = iter->drs;
  (*d_statnumhit)++;
  return true;
}

map<char,uint64_t> AuthQueryCache::getCounts()
{
  uint64_t queryCacheEntries=0, negQueryCacheEntries=0;

  for(auto& mc : d_maps) {
    auto map = mc.d_map.read_lock();
    
    for(const auto & iter : *map) {
      if(iter.drs.empty())
        negQueryCacheEntries++;
      else
        queryCacheEntries++;
    }
  }
  map<char,uint64_t> ret;

  ret['!']=negQueryCacheEntries;
  ret['Q']=queryCacheEntries;
  return ret;
}

/* clears the entire cache. */
uint64_t AuthQueryCache::purge()
{
  d_statnumentries->store(0);

  return purgeLockedCollectionsVector(d_maps);
}

uint64_t AuthQueryCache::purgeExact(const DNSName& qname)
{
  auto& mc = getMap(qname);
  uint64_t delcount = purgeExactLockedCollection<NameTag>(mc, qname);

  *d_statnumentries -= delcount;

  return delcount;
}

/* purges entries from the querycache. If match ends on a $, it is treated as a suffix */
uint64_t AuthQueryCache::purge(const string &match)
{
  uint64_t delcount = 0;

  if(boost::ends_with(match, "$")) {
    delcount = purgeLockedCollectionsVector<NameTag>(d_maps, match);
    *d_statnumentries -= delcount;
  }
  else {
    delcount = purgeExact(DNSName(match));
  }

  return delcount;
}

void AuthQueryCache::cleanup()
{
  uint64_t totErased = pruneLockedCollectionsVector<SequencedTag>(d_maps);
  *d_statnumentries -= totErased;

  DLOG(g_log<<"Done with cache clean, cacheSize: "<<*d_statnumentries<<", totErased"<<totErased<<endl);
}

/* the logic:
   after d_nextclean operations, we clean. We also adjust the cleaninterval
   a bit so we slowly move it to a value where we clean roughly every 30 seconds.

   If d_nextclean has reached its maximum value, we also test if we were called
   within 30 seconds, and if so, we skip cleaning. This means that under high load,
   we will not clean more often than every 30 seconds anyhow.
*/

void AuthQueryCache::cleanupIfNeeded()
{
  if (d_ops++ == d_nextclean) {
    time_t now = time(nullptr);
    int timediff = max((int)(now - d_lastclean), 1);

    DLOG(g_log<<"cleaninterval: "<<d_cleaninterval<<", timediff: "<<timediff<<endl);

    if (d_cleaninterval == s_maxcleaninterval && timediff < 30) {
      d_cleanskipped = true;
      d_nextclean += d_cleaninterval;

      DLOG(g_log<<"cleaning skipped, timediff: "<<timediff<<endl);

      return;
    }

    if(!d_cleanskipped) {
      d_cleaninterval=(int)(0.6*d_cleaninterval)+(0.4*d_cleaninterval*(30.0/timediff));
      d_cleaninterval=std::max(d_cleaninterval, s_mincleaninterval);
      d_cleaninterval=std::min(d_cleaninterval, s_maxcleaninterval);

      DLOG(g_log<<"new cleaninterval: "<<d_cleaninterval<<endl);
    } else {
      d_cleanskipped = false;
    }

    d_nextclean += d_cleaninterval;
    d_lastclean=now;
    cleanup();
  }
}
