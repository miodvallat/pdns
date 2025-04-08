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

#include "pdns/misc.hh"
#include "auth-zonecache.hh"
#include "logger.hh"
#include "statbag.hh"
#include "arguments.hh"
#include "cachecleaner.hh"
extern StatBag S;

AuthZoneCache::AuthZoneCache(size_t mapsCount) :
  d_maps(mapsCount)
{
  S.declare("zone-cache-hit", "Number of zone cache hits");
  S.declare("zone-cache-miss", "Number of zone cache misses");
  S.declare("zone-cache-size", "Number of entries in the zone cache", StatType::gauge);

  d_statnumhit = S.getPointer("zone-cache-hit");
  d_statnummiss = S.getPointer("zone-cache-miss");
  d_statnumentries = S.getPointer("zone-cache-size");
}

bool AuthZoneCache::getEntry(const ZoneName& zone, int& zoneId, Netmask* net)
{
  string view;

  try {
    // FIXME: adjust test_auth_zonecache.cc to pass a netmask, then see if anything else crashes
    // just so we know what codepaths also don't pass a net
    // i noticed that AXFR does not crash (but also does not 'view'), perhaps it does not use the zone cache?
    if (net != nullptr) {
      auto* netview = d_nets.lookup(net->getNetwork());
      if (netview != nullptr) {
        view = netview->second;
      }
    }
  }
  catch (...) {
    // this handles the "empty" case, but might hide other errors
  }

  cerr << "view=[" << view << "]";

  string variant;
  if (d_views.count(view) == 1) { // FIXME lock
    cerr<<1;
    auto& viewmap = d_views.at(view);
    if (viewmap.count(DNSName(zone)) == 1) {
      cerr<<2;
      variant = viewmap.at(DNSName(zone));
    }
  }
  cerr << ", variant=[" << variant << "]" << endl;

  ZoneName tagZone(zone.operator const DNSName&(), variant); // FIXME: feels ugly?
  // tagZone.d_tag = tag;

  auto& mc = getMap(tagZone);
  cerr << "looking for " << tagZone << ", hash=" << tagZone.hash() << endl;
  bool found = false;
  {
    auto map = mc.d_map.read_lock();
    auto iter = map->find(tagZone);
    if (iter != map->end()) {
      found = true;
      zoneId = iter->second.zoneId;
      cerr << "found with zoneId=" << zoneId << endl;
    }
  }

  if (found) {
    (*d_statnumhit)++;
  }
  else {
    (*d_statnummiss)++;
  }
  return found;
}

bool AuthZoneCache::isEnabled() const
{
  return d_refreshinterval > 0;
}

void AuthZoneCache::clear()
{
  purgeLockedCollectionsVector(d_maps);
}

void AuthZoneCache::replace(const vector<std::tuple<ZoneName, int>>& zone_indices)
{
  if (!d_refreshinterval)
    return;

  size_t count = zone_indices.size();
  vector<cmap_t> newMaps(d_maps.size());

  // build new maps
  for (const auto& [zone, id] : zone_indices) {
    cerr << "inserting zone=" << zone << ", id=" << id << endl;
    CacheValue val;
    val.zoneId = id;
    auto& mc = newMaps[getMapIndex(zone)];
    auto iter = mc.find(zone);
    if (iter != mc.end()) {
      iter->second = val;
    }
    else {
      mc.emplace(zone, val);
    }
  }

  {
    // process zone updates done while data collection for replace() was already in progress.
    auto pending = d_pending.lock();
    assert(pending->d_replacePending); // make sure we never forget to call setReplacePending()
    for (const auto& [zone, id, insert] : pending->d_pendingUpdates) {
      CacheValue val;
      val.zoneId = id;
      auto& mc = newMaps[getMapIndex(zone)];
      auto iter = mc.find(zone);
      if (iter != mc.end()) {
        if (insert) {
          iter->second = val;
        }
        else {
          mc.erase(iter);
          count--;
        }
      }
      else if (insert) {
        mc.emplace(zone, val);
        count++;
      }
    }

    for (size_t mapIndex = 0; mapIndex < d_maps.size(); mapIndex++) {
      auto& mc = d_maps[mapIndex];
      auto map = mc.d_map.write_lock();
      *map = std::move(newMaps[mapIndex]);
    }

    pending->d_pendingUpdates.clear();
    pending->d_replacePending = false;

    d_statnumentries->store(count);
  }
}

void AuthZoneCache::replace(NetmaskTree<string> nettree)
{
  // FIXME: lock

  d_nets.swap(nettree);
}

void AuthZoneCache::replace(ViewsMap viewsmap)
{
  // FIXME: lock

  d_views.swap(viewsmap);
}



void AuthZoneCache::add(const ZoneName& zone, const int zoneId)
{
  if (!d_refreshinterval)
    return;

  {
    auto pending = d_pending.lock();
    if (pending->d_replacePending) {
      pending->d_pendingUpdates.emplace_back(zone, zoneId, true);
    }
  }

  CacheValue val;
  val.zoneId = zoneId;

  int mapIndex = getMapIndex(zone);
  {
    auto& mc = d_maps[mapIndex];
    auto map = mc.d_map.write_lock();
    auto iter = map->find(zone);
    if (iter != map->end()) {
      iter->second = val;
    }
    else {
      map->emplace(zone, val);
      (*d_statnumentries)++;
    }
  }
}

void AuthZoneCache::remove(const ZoneName& zone)
{
  if (!d_refreshinterval)
    return;

  {
    auto pending = d_pending.lock();
    if (pending->d_replacePending) {
      pending->d_pendingUpdates.emplace_back(zone, -1, false);
    }
  }

  int mapIndex = getMapIndex(zone);
  {
    auto& mc = d_maps[mapIndex];
    auto map = mc.d_map.write_lock();
    if (map->erase(zone)) {
      (*d_statnumentries)--;
    }
  }
}

void AuthZoneCache::setReplacePending()
{
  if (!d_refreshinterval)
    return;

  {
    auto pending = d_pending.lock();
    pending->d_replacePending = true;
    pending->d_pendingUpdates.clear();
  }
}
