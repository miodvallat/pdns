dnsdist Overview
================

dnsdist is a highly DNS-, DoS- and abuse-aware loadbalancer.
Its goal in life is to route traffic to the best server, delivering top performance to legitimate users while shunting or blocking abusive traffic.

dnsdist is dynamic, its configuration language is `Lua <http://lua.org>`_ and it can be changed at runtime, and its statistics can be queried from a console-like interface or an HTTP API.

A configuration to balance DNS queries to several backend servers:

.. code-block:: lua

   newServer({address="2620:fe::fe", qps=1})
   newServer({address="2620:fe::9", qps=1})
   newServer({address="9.9.9.9", qps=1})
   newServer({address="2001:db8::1", qps=10})
   newServer({address="[2001:db8::2]:5300", name="dns1", qps=10})
   newServer("192.0.2.1")
   setServerPolicy(firstAvailable) -- first server within its QPS limit

Running dnsdist
---------------

If you have not worked with dnsdist before, here are some resources to get you going:

* :doc:`Install dnsdist <install>`.
* To get a feeling for how it works, see the :doc:`Quickstart Guide <quickstart>`.
* :doc:`running`
* The :doc:`rules-actions` page covers how to apply policies to traffic
* There are several :doc:`guides/index` about the different features and options
* :doc:`advanced/index` describes some of the more advanced features
* :doc:`reference/index` has all the configuration and object information

Questions, requests or comments?
--------------------------------

There are several ways to reach us:

* The `dnsdist mailing-list <https://mailman.powerdns.com/mailman/listinfo/dnsdist>`_
* #powerdns on `irc.oftc.net <irc://irc.oftc.net/#powerdns>`_

The Open-Xchange/PowerDNS company can provide help or support you in private as well.
Please `contact PowerDNS <https://www.powerdns.com/contact-us>`__.

This documentation is also available as a `PDF document <dnsdist.pdf>`_.
