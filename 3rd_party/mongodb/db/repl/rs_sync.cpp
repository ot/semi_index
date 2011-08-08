/**
*    Copyright (C) 2008 10gen Inc.
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

#include "pch.h"
#include "../client.h"
#include "../../client/dbclient.h"
#include "rs.h"
#include "../repl.h"
#include "connections.h"
namespace mongo {

    using namespace bson;
    extern unsigned replSetForceInitialSyncFailure;

    /* apply the log op that is in param o */
    void ReplSetImpl::syncApply(const BSONObj &o) {
        char db[MaxDatabaseNameLen];
        const char *ns = o.getStringField("ns");
        nsToDatabase(ns, db);

        if ( *ns == '.' || *ns == 0 ) {
		    if( *o.getStringField("op") == 'n' )
			    return;
            log() << "replSet skipping bad op in oplog: " << o.toString() << endl;
            return;
        }

        Client::Context ctx(ns);
        ctx.getClient()->curop()->reset();

        /* todo : if this asserts, do we want to ignore or not? */
        applyOperation_inlock(o);
    }

    /* initial oplog application, during initial sync, after cloning. 
       @return false on failure.  
       this method returns an error and doesn't throw exceptions (i think).
    */
    bool ReplSetImpl::initialSyncOplogApplication(
        const Member *source,
        OpTime applyGTE,
        OpTime minValid)
    { 
        if( source == 0 ) return false;

        const string hn = source->h().toString();
        OpTime ts;
        try {
            OplogReader r;
            if( !r.connect(hn) ) { 
                log() << "replSet initial sync error can't connect to " << hn << " to read " << rsoplog << rsLog;
                return false;
            }

            {
                BSONObjBuilder q;
                q.appendDate("$gte", applyGTE.asDate());
                BSONObjBuilder query;
                query.append("ts", q.done());
                BSONObj queryObj = query.done();
                r.query(rsoplog, queryObj);
            }
            assert( r.haveCursor() );

            /* we lock outside the loop to avoid the overhead of locking on every operation.  server isn't usable yet anyway! */
            writelock lk("");

            {
                if( !r.more() ) { 
                    sethbmsg("replSet initial sync error reading remote oplog");
                    log() << "replSet initial sync error remote oplog (" << rsoplog << ") on host " << hn << " is empty?" << rsLog;
                    return false;
                }
                bo op = r.next();
                OpTime t = op["ts"]._opTime();
                r.putBack(op);

                if( op.firstElement().fieldName() == string("$err") ) { 
                    log() << "replSet initial sync error querying " << rsoplog << " on " << hn << " : " << op.toString() << rsLog;
                    return false;
                }

                uassert( 13508 , str::stream() << "no 'ts' in first op in oplog: " << op , !t.isNull() );
                if( t > applyGTE ) {
                    sethbmsg(str::stream() << "error " << hn << " oplog wrapped during initial sync");
                    log() << "replSet initial sync expected first optime of " << applyGTE << rsLog;
                    log() << "replSet initial sync but received a first optime of " << t << " from " << hn << rsLog;
                    return false;
                }
            }

            // todo : use exhaust
            unsigned long long n = 0;
            while( 1 ) { 

                if( !r.more() )
                    break;
                BSONObj o = r.nextSafe(); /* note we might get "not master" at some point */
                {
                    ts = o["ts"]._opTime();

                    /* if we have become primary, we dont' want to apply things from elsewhere
                        anymore. assumePrimary is in the db lock so we are safe as long as 
                        we check after we locked above. */
                    if( (source->state() != MemberState::RS_PRIMARY &&
                         source->state() != MemberState::RS_SECONDARY) ||
                        replSetForceInitialSyncFailure ) {
                        
                        int f = replSetForceInitialSyncFailure;
                        if( f > 0 ) {
                            replSetForceInitialSyncFailure = f-1;
                            log() << "replSet test code invoked, replSetForceInitialSyncFailure" << rsLog;
                            throw DBException("forced error",0);
                        }
                        log() << "replSet we are now primary" << rsLog;
                        throw DBException("primary changed",0);
                    }

                    if( ts >= applyGTE ) {
                        // optimes before we started copying need not be applied.
                        syncApply(o);
                    }
                    _logOpObjRS(o);   /* with repl sets we write the ops to our oplog too */
                }
                if( ++n % 100000 == 0 ) { 
                    // simple progress metering
                    log() << "replSet initialSyncOplogApplication " << n << rsLog;
                }
            }
        }
        catch(DBException& e) { 
            if( ts <= minValid ) {
                // didn't make it far enough
                log() << "replSet initial sync failing, error applying oplog " << e.toString() << rsLog;
                return false;
            }
        }
        return true;
    }

    /* should be in RECOVERING state on arrival here.  
       readlocks
       @return true if transitioned to SECONDARY
    */
    bool ReplSetImpl::tryToGoLiveAsASecondary(OpTime& /*out*/ minvalid) { 
        bool golive = false;			
        {
            readlock lk("local.replset.minvalid");
            BSONObj mv;
            if( Helpers::getSingleton("local.replset.minvalid", mv) ) { 
                minvalid = mv["ts"]._opTime();
                if( minvalid <= lastOpTimeWritten ) { 
                    golive=true;
                }
            }
            else 
                golive = true; /* must have been the original member */
        }
        if( golive ) {
            sethbmsg("");
            changeState(MemberState::RS_SECONDARY);
        }
        return golive;
    }

    /* tail the primary's oplog.  ok to return, will be re-called. */
    void ReplSetImpl::syncTail() { 
        // todo : locking vis a vis the mgr...

        const Member *primary = box.getPrimary();
        if( primary == 0 ) return;
        string hn = primary->h().toString();
        OplogReader r;
        if( !r.connect(primary->h().toString()) ) { 
            log(2) << "replSet can't connect to " << hn << " to read operations" << rsLog;
            return;
        }

        /* first make sure we are not hopelessly out of sync by being very stale. */
        {
            BSONObj remoteOldestOp = r.findOne(rsoplog, Query());
            OpTime ts = remoteOldestOp["ts"]._opTime();
            DEV log() << "replSet remoteOldestOp:    " << ts.toStringLong() << rsLog;
            else log(3) << "replSet remoteOldestOp: " << ts.toStringLong() << rsLog;
            DEV { 
                // debugging sync1.js...
                log() << "replSet lastOpTimeWritten: " << lastOpTimeWritten.toStringLong() << rsLog;
                log() << "replSet our state: " << state().toString() << rsLog;
            }
            if( lastOpTimeWritten < ts ) { 
                log() << "replSet error RS102 too stale to catch up, at least from primary: " << hn << rsLog;
                log() << "replSet our last optime : " << lastOpTimeWritten.toStringLong() << rsLog;
                log() << "replSet oldest at " << hn << " : " << ts.toStringLong() << rsLog;
                log() << "replSet See http://www.mongodb.org/display/DOCS/Resyncing+a+Very+Stale+Replica+Set+Member" << rsLog;
                sethbmsg("error RS102 too stale to catch up");
                changeState(MemberState::RS_RECOVERING);
                sleepsecs(120);
                return;
            }
        }

        r.tailingQueryGTE(rsoplog, lastOpTimeWritten);
        assert( r.haveCursor() );

        uassert(1000, "replSet source for syncing doesn't seem to be await capable -- is it an older version of mongodb?", r.awaitCapable() );

        {
            if( !r.more() ) {
                /* maybe we are ahead and need to roll back? */
                try {
                    bo theirLastOp = r.getLastOp(rsoplog);
                    if( theirLastOp.isEmpty() ) {
                        log() << "replSet error empty query result from " << hn << " oplog" << rsLog;
                        sleepsecs(2);
                        return;
                    }
                    OpTime theirTS = theirLastOp["ts"]._opTime();
                    if( theirTS < lastOpTimeWritten ) { 
                        log() << "replSet we are ahead of the primary, will try to roll back" << rsLog;
                        syncRollback(r);
                        return;
                    }
                    /* we're not ahead?  maybe our new query got fresher data.  best to come back and try again */
                    log() << "replSet syncTail condition 1" << rsLog;
                    sleepsecs(1);
                }
                catch(DBException& e) { 
                    log() << "replSet error querying " << hn << ' ' << e.toString() << rsLog;
                    sleepsecs(2);
                }
                return;
                /*
                log() << "replSet syncTail error querying oplog >= " << lastOpTimeWritten.toString() << " from " << hn << rsLog;
                try {
                    log() << "replSet " << hn << " last op: " << r.getLastOp(rsoplog).toString() << rsLog;
                }
                catch(...) { }
                sleepsecs(1);
                return;*/
            }

            BSONObj o = r.nextSafe();
            OpTime ts = o["ts"]._opTime();
            long long h = o["h"].numberLong();
            if( ts != lastOpTimeWritten || h != lastH ) { 
                log() << "replSet our last op time written: " << lastOpTimeWritten.toStringPretty() << endl;
                log() << "replset primary's GTE: " << ts.toStringPretty() << endl;
                syncRollback(r);
                return;
            }
        }

        /* we have now checked if we need to rollback and we either don't have to or did it. */
        {
            OpTime minvalid;
            tryToGoLiveAsASecondary(minvalid);
        }

        while( 1 ) {
            while( 1 ) {
                if( !r.moreInCurrentBatch() ) { 
                    /* we need to occasionally check some things. between 
                       batches is probably a good time. */

                    /* perhaps we should check this earlier? but not before the rollback checks. */
                    if( state().recovering() ) { 
                        /* can we go to RS_SECONDARY state?  we can if not too old and if minvalid achieved */
                        OpTime minvalid;
                        bool golive = ReplSetImpl::tryToGoLiveAsASecondary(minvalid);
                        if( golive ) {
                            ;
                        }
                        else { 
                            sethbmsg(str::stream() << "still syncing, not yet to minValid optime" << minvalid.toString());
                        }

                        /* todo: too stale capability */
                    }

                    if( box.getPrimary() != primary ) 
                        return;
                }
                if( !r.more() )
                    break;
                { 
                    BSONObj o = r.nextSafe(); /* note we might get "not master" at some point */

                    int sd = myConfig().slaveDelay;
                    // ignore slaveDelay if the box is still initializing. once
                    // it becomes secondary we can worry about it.
                    if( sd && box.getState().secondary() ) { 
                        const OpTime ts = o["ts"]._opTime();
                        long long a = ts.getSecs();
                        long long b = time(0);
                        long long lag = b - a;
                        long long sleeptime = sd - lag;
                        if( sleeptime > 0 ) {
                            uassert(12000, "rs slaveDelay differential too big check clocks and systems", sleeptime < 0x40000000);
                            log() << "replSet temp slavedelay sleep:" << sleeptime << rsLog;
                            if( sleeptime < 60 ) {
                                sleepsecs((int) sleeptime);
                            }
                            else {
                                // sleep(hours) would prevent reconfigs from taking effect & such!
                                long long waitUntil = b + sleeptime;
                                while( 1 ) {
                                    sleepsecs(6);
                                    if( time(0) >= waitUntil )
                                        break;
                                    if( box.getPrimary() != primary )
                                        break;
                                    if( myConfig().slaveDelay != sd ) // reconf
                                        break;
                                }
                            }
                        }
                        
                    }

                    {
                        writelock lk("");

                        /* if we have become primary, we dont' want to apply things from elsewhere
                           anymore. assumePrimary is in the db lock so we are safe as long as 
                           we check after we locked above. */
                        if( box.getPrimary() != primary ) {
                            if( box.getState().primary() )
                                log(0) << "replSet stopping syncTail we are now primary" << rsLog;
                            return;
                        }

                        syncApply(o);
                        _logOpObjRS(o);   /* with repl sets we write the ops to our oplog too: */                   
                    }
                }
            }
            r.tailCheck();
            if( !r.haveCursor() ) {
                log(1) << "replSet end syncTail pass with " << hn << rsLog;
                // TODO : reuse our connection to the primary.
                return;
            }
            if( box.getPrimary() != primary )
                return;
            // looping back is ok because this is a tailable cursor
        }
    }

    void ReplSetImpl::_syncThread() {
        StateBox::SP sp = box.get();
        if( sp.state.primary() ) {
            sleepsecs(1);
            return;
        }
        if( sp.state.fatal() ) { 
            sleepsecs(5);
            return;
        }

        /* later, we can sync from up secondaries if we want. tbd. */
        if( sp.primary == 0 ) {
            return;
        }

        /* do we have anything at all? */
        if( lastOpTimeWritten.isNull() ) {
            syncDoInitialSync();
            return; // _syncThread will be recalled, starts from top again in case sync failed.
        }

        /* we have some data.  continue tailing. */
        syncTail();
    }

    void ReplSetImpl::syncThread() {
        /* test here was to force a receive timeout
        ScopedConn c("localhost");
        bo info;
        try {
            log() << "this is temp" << endl;
            c.runCommand("admin", BSON("sleep"<<120), info);
            log() << info.toString() << endl;
            c.runCommand("admin", BSON("sleep"<<120), info);
            log() << "temp" << endl;
        }
        catch( DBException& e ) { 
            log() << e.toString() << endl;
            c.runCommand("admin", BSON("sleep"<<120), info);
            log() << "temp" << endl;
        }
        */

        while( 1 ) {
            if( myConfig().arbiterOnly )
                return;
            
            try {
                _syncThread();
            }
            catch(DBException& e) { 
                sethbmsg("syncThread: " + e.toString());
                sleepsecs(10);
            }
            catch(...) { 
                sethbmsg("unexpected exception in syncThread()");
                // TODO : SET NOT SECONDARY here?
                sleepsecs(60);
            }
            sleepsecs(1);

            /* normally msgCheckNewState gets called periodically, but in a single node repl set there 
               are no heartbeat threads, so we do it here to be sure.  this is relevant if the singleton 
               member has done a stepDown() and needs to come back up. 
               */
            OCCASIONALLY mgr->send( boost::bind(&Manager::msgCheckNewState, theReplSet->mgr) );
        }
    }

    void startSyncThread() {
        static int n;
        if( n != 0 ) {
            log() << "replSet ERROR : more than one sync thread?" << rsLog;
            assert( n == 0 );
        }
        n++;

        Client::initThread("replica set sync");
        cc().iAmSyncThread();
        theReplSet->syncThread();
        cc().shutdown();
    }

}
