// @file 

/*
 *    Copyright (C) 2010 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <map>
#include "../../client/dbclient.h"

namespace mongo { 

    /** here we keep a single connection (with reconnect) for a set of hosts, 
        one each, and allow one user at a time per host.  if in use already for that 
        host, we block.  so this is an easy way to keep a 1-deep pool of connections
        that many threads can share.

        thread-safe.

        Example:
        {
            ScopedConn c("foo.acme.com:9999");
            c->runCommand(...);
        }

        throws exception on connect error (but fine to try again later with a new
        scopedconn object for same host).
    */
    class ScopedConn { 
    public:
        /** throws assertions if connect failure etc. */
        ScopedConn(string hostport);
        ~ScopedConn();

        /* If we were to run a query and not exhaust the cursor, future use of the connection would be problematic.
           So here what we do is wrapper known safe methods and not allow cursor-style queries at all.  This makes 
           ScopedConn limited in functionality but very safe.  More non-cursor wrappers can be added here if needed.
           */

        bool runCommand(const string &dbname, const BSONObj& cmd, BSONObj &info, int options=0) {
            return conn()->runCommand(dbname, cmd, info, options);
        }
        unsigned long long count(const string &ns) { 
            return conn()->count(ns); 
        }
        BSONObj findOne(const string &ns, const Query& q, const BSONObj *fieldsToReturn = 0, int queryOptions = 0) { 
            return conn()->findOne(ns, q, fieldsToReturn, queryOptions);
        }
        void setTimeout(double to) {
            conn()->setSoTimeout(to);
        }

    private:
        auto_ptr<scoped_lock> connLock;
        static mongo::mutex mapMutex;
        struct X { 
            mongo::mutex z;
            DBClientConnection cc;
            X() : z("X"), cc(/*reconnect*/ true, 0, /*timeout*/ 10.0) { 
                cc._logLevel = 2;
            }
        } *x;
        typedef map<string,ScopedConn::X*> M;
        static M& _map;
        DBClientConnection* conn() { return &x->cc; }
    };

    inline ScopedConn::ScopedConn(string hostport) {
        bool first = false;
        {
            scoped_lock lk(mapMutex);
            x = _map[hostport];
            if( x == 0 ) {
                x = _map[hostport] = new X();
                first = true;
                connLock.reset( new scoped_lock(x->z) );
            }
        }
        if( !first ) { 
            connLock.reset( new scoped_lock(x->z) );
            return;
        }

        // we already locked above...
        string err;
        x->cc.connect(hostport, err);
    }

    inline ScopedConn::~ScopedConn() { 
        // conLock releases...
    }

    /*inline DBClientConnection* ScopedConn::operator->() { 
        return &x->cc; 
    }*/

}
