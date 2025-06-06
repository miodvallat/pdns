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
#ifndef BOOST_TEST_DYN_LINK
#define BOOST_TEST_DYN_LINK
#endif

#define BOOST_TEST_MAIN
#define BOOST_TEST_MODULE unit

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <boost/test/unit_test.hpp>
#include <boost/assign/list_of.hpp>

#include <boost/tuple/tuple.hpp>
#include "pdns/namespaces.hh"
#include "pdns/dns.hh"
#include "pdns/dnsbackend.hh"
#include "pdns/dnspacket.hh"
#include "pdns/pdnsexception.hh"
#include "pdns/logger.hh"
#include "pdns/arguments.hh"
#include "pdns/dnsrecords.hh"
#include "pdns/json.hh"
#include "pdns/statbag.hh"
#include "pdns/auth-packetcache.hh"
#include "pdns/auth-querycache.hh"
#include "pdns/auth-zonecache.hh"

StatBag S;
AuthPacketCache PC;
AuthQueryCache QC;
AuthZoneCache g_zoneCache;
ArgvMap& arg()
{
  static ArgvMap arg;
  return arg;
};

class RemoteLoader
{
public:
  RemoteLoader();
};

std::unique_ptr<DNSBackend> backendUnderTest;

struct RemotebackendSetup
{
  RemotebackendSetup()
  {
    backendUnderTest = nullptr;
    try {
      // setup minimum arguments
      ::arg().set("module-dir") = "./.libs";
      auto loader = std::make_unique<RemoteLoader>();
      BackendMakers().launch("remote");
      // then get us a instance of it
      ::arg().set("remote-connection-string") = "unix:path=/tmp/remotebackend.sock";
      ::arg().set("remote-dnssec") = "yes";
      backendUnderTest = std::move(BackendMakers().all()[0]);
      reportAllTypes();
    }
    catch (PDNSException& ex) {
      BOOST_TEST_MESSAGE("Cannot start remotebackend: " << ex.reason);
    };
  }
  ~RemotebackendSetup() = default;
};

BOOST_GLOBAL_FIXTURE(RemotebackendSetup);
