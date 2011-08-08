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
#include "../cmdline.h"
#include "../commands.h"
#include "health.h"
#include "rs.h"
#include "rs_config.h"
#include "../dbwebserver.h"
#include "../../util/mongoutils/html.h"
#include "../../client/dbclient.h"

using namespace bson;

namespace mongo { 

    void checkMembersUpForConfigChange(const ReplSetConfig& cfg, bool initial);

    /* commands in other files:
         replSetHeartbeat - health.cpp
         replSetInitiate  - rs_mod.cpp
    */

    bool replSetBlind = false;
    unsigned replSetForceInitialSyncFailure = 0;

    class CmdReplSetTest : public ReplSetCommand {
    public:
        virtual void help( stringstream &help ) const {
            help << "Just for regression tests.\n";
        }
        CmdReplSetTest() : ReplSetCommand("replSetTest") { }
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            log() << "replSet replSetTest command received: " << cmdObj.toString() << rsLog;
            if( cmdObj.hasElement("forceInitialSyncFailure") ) {
                replSetForceInitialSyncFailure = (unsigned) cmdObj["forceInitialSyncFailure"].Number();
                return true;
            }

            // may not need this, but if removed check all tests still work:
            if( !check(errmsg, result) ) 
                return false;

            if( cmdObj.hasElement("blind") ) {
                replSetBlind = cmdObj.getBoolField("blind");
                return true;
            }
            return false;
        }
    } cmdReplSetTest;

    /** get rollback id */
    class CmdReplSetGetRBID : public ReplSetCommand {
    public:
        /* todo: ideally this should only change on rollbacks NOT on mongod restarts also. fix... */
        int rbid;
        virtual void help( stringstream &help ) const {
            help << "internal";
        }
        CmdReplSetGetRBID() : ReplSetCommand("replSetGetRBID") { 
            rbid = (int) curTimeMillis();
        }
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) ) 
                return false;
            result.append("rbid",rbid);
            return true;
        }
    } cmdReplSetRBID;

    /** we increment the rollback id on every rollback event. */
    void incRBID() { 
        cmdReplSetRBID.rbid++;
    }

    /** helper to get rollback id from another server. */
    int getRBID(DBClientConnection *c) { 
        bo info;
        c->simpleCommand("admin", &info, "replSetGetRBID");
        return info["rbid"].numberInt();
    } 

    class CmdReplSetGetStatus : public ReplSetCommand {
    public:
        virtual void help( stringstream &help ) const {
            help << "Report status of a replica set from the POV of this server\n";
            help << "{ replSetGetStatus : 1 }";
            help << "\nhttp://www.mongodb.org/display/DOCS/Replica+Set+Commands";
        }
        CmdReplSetGetStatus() : ReplSetCommand("replSetGetStatus", true) { }
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) ) 
                return false;
            theReplSet->summarizeStatus(result);
            return true;
        }
    } cmdReplSetGetStatus;

    class CmdReplSetReconfig : public ReplSetCommand {
        RWLock mutex; /* we don't need rw but we wanted try capability. :-( */
    public:
        virtual void help( stringstream &help ) const {
            help << "Adjust configuration of a replica set\n";
            help << "{ replSetReconfig : config_object }";
            help << "\nhttp://www.mongodb.org/display/DOCS/Replica+Set+Commands";
        }
        CmdReplSetReconfig() : ReplSetCommand("replSetReconfig"), mutex("rsreconfig") { }
        virtual bool run(const string& a, BSONObj& b, string& errmsg, BSONObjBuilder& c, bool d) {
            try { 
                rwlock_try_write lk(mutex);
                return _run(a,b,errmsg,c,d);
            }
            catch(rwlock_try_write::exception&) { }
            errmsg = "a replSetReconfig is already in progress";
            return false;
        }
    private:
        bool _run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) ) 
                return false;
            if( !theReplSet->box.getState().primary() ) { 
                errmsg = "replSetReconfig command must be sent to the current replica set primary.";
                return false;
            }

            {
                // just make sure we can get a write lock before doing anything else.  we'll reacquire one 
                // later.  of course it could be stuck then, but this check lowers the risk if weird things 
                // are up - we probably don't want a change to apply 30 minutes after the initial attempt.
                time_t t = time(0);
                writelock lk("");
                if( time(0)-t > 20 ) {
                    errmsg = "took a long time to get write lock, so not initiating.  Initiate when server less busy?";
                    return false;
                }
            }

            if( cmdObj["replSetReconfig"].type() != Object ) {
                errmsg = "no configuration specified";
                return false;
            }

            /** TODO
                Support changes when a majority, but not all, members of a set are up.
                Determine what changes should not be allowed as they would cause erroneous states.
                What should be possible when a majority is not up?
                */
            try {
                ReplSetConfig newConfig(cmdObj["replSetReconfig"].Obj());

                log() << "replSet replSetReconfig config object parses ok, " << newConfig.members.size() << " members specified" << rsLog;

                if( !ReplSetConfig::legalChange(theReplSet->getConfig(), newConfig, errmsg) ) { 
                    return false;
                }

                checkMembersUpForConfigChange(newConfig,false);

                log() << "replSet replSetReconfig [2]" << rsLog;

                theReplSet->haveNewConfig(newConfig, true);
                ReplSet::startupStatusMsg = "replSetReconfig'd";
            }
            catch( DBException& e ) { 
                log() << "replSet replSetReconfig exception: " << e.what() << rsLog;
                throw;
            }

            return true;
        }
    } cmdReplSetReconfig;

    class CmdReplSetFreeze : public ReplSetCommand {
    public:
        virtual void help( stringstream &help ) const {
            help << "{ replSetFreeze : <seconds> }";
            help << "'freeze' state of member to the extent we can do that.  What this really means is that\n";
            help << "this node will not attempt to become primary until the time period specified expires.\n";
            help << "You can call again with {replSetFreeze:0} to unfreeze sooner.\n";
            help << "A process restart unfreezes the member also.\n";
            help << "\nhttp://www.mongodb.org/display/DOCS/Replica+Set+Commands";
        }

        CmdReplSetFreeze() : ReplSetCommand("replSetFreeze") { }
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) )
                return false;
            int secs = (int) cmdObj.firstElement().numberInt();
            if( theReplSet->freeze(secs) ) {
                if( secs == 0 )
                    result.append("info","unfreezing");
            }
            if( secs == 1 ) 
                result.append("warning", "you really want to freeze for only 1 second?");
            return true;
        }
    } cmdReplSetFreeze;

    class CmdReplSetStepDown: public ReplSetCommand {
    public:
        virtual void help( stringstream &help ) const {
            help << "{ replSetStepDown : <seconds> }\n";
            help << "Step down as primary.  Will not try to reelect self for the specified time period (1 minute if no numeric secs value specified).\n";
            help << "(If another member with same priority takes over in the meantime, it will stay primary.)\n";
            help << "http://www.mongodb.org/display/DOCS/Replica+Set+Commands";
        }

        CmdReplSetStepDown() : ReplSetCommand("replSetStepDown") { }
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) )
                return false;
            if( !theReplSet->box.getState().primary() ) {
                errmsg = "not primary so can't step down";
                return false;
            }
            int secs = (int) cmdObj.firstElement().numberInt();
            if( secs == 0 )
                secs = 60;
            return theReplSet->stepDown(secs);
        }
    } cmdReplSetStepDown;

    using namespace bson;
    using namespace mongoutils::html;
    extern void fillRsLog(stringstream&);

    class ReplSetHandler : public DbWebHandler {
    public:
        ReplSetHandler() : DbWebHandler( "_replSet" , 1 , true ){}

        virtual bool handles( const string& url ) const {
            return startsWith( url , "/_replSet" );
        }

        virtual void handle( const char *rq, string url, BSONObj params, 
                             string& responseMsg, int& responseCode,
                             vector<string>& headers,  const SockAddr &from ){
            
            if( url == "/_replSetOplog" ) {
                responseMsg = _replSetOplog(params);
            } else
                responseMsg = _replSet();
            responseCode = 200;
        }

        string _replSetOplog(bo parms) { 
            int _id = (int) str::toUnsigned( parms["_id"].String() );

            stringstream s;
            string t = "Replication oplog";
            s << start(t);
            s << p(t);

            if( theReplSet == 0 ) { 
                if( cmdLine._replSet.empty() ) 
                    s << p("Not using --replSet");
                else  {
                    s << p("Still starting up, or else set is not yet " + a("http://www.mongodb.org/display/DOCS/Replica+Set+Configuration#InitialSetup", "", "initiated") 
                           + ".<br>" + ReplSet::startupStatusMsg);
                }
            }
            else {
                try {
                    theReplSet->getOplogDiagsAsHtml(_id, s);
                }
                catch(std::exception& e) { 
                    s << "error querying oplog: " << e.what() << '\n'; 
                }
            }

            s << _end();
            return s.str();
        }

        /* /_replSet show replica set status in html format */
        string _replSet() { 
            stringstream s;
            s << start("Replica Set Status " + prettyHostName());
            s << p( a("/", "back", "Home") + " | " + 
                    a("/local/system.replset/?html=1", "", "View Replset Config") + " | " +
                    a("/replSetGetStatus?text=1", "", "replSetGetStatus") + " | " +
                    a("http://www.mongodb.org/display/DOCS/Replica+Sets", "", "Docs")
                  );

            if( theReplSet == 0 ) { 
                if( cmdLine._replSet.empty() ) 
                    s << p("Not using --replSet");
                else  {
                    s << p("Still starting up, or else set is not yet " + a("http://www.mongodb.org/display/DOCS/Replica+Set+Configuration#InitialSetup", "", "initiated") 
                           + ".<br>" + ReplSet::startupStatusMsg);
                }
            }
            else {
                try {
                    theReplSet->summarizeAsHtml(s);
                }
                catch(...) { s << "error summarizing replset status\n"; }
            }
            s << p("Recent replset log activity:");
            fillRsLog(s);
            s << _end();
            return s.str();
        }



    } replSetHandler;

}
