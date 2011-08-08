// client.cpp

/**
*    Copyright (C) 2009 10gen Inc.
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

/* Client represents a connection to the database (the server-side) and corresponds 
   to an open socket (or logical connection if pooling on sockets) from a client.
*/

#include "pch.h"
#include "db.h"
#include "client.h"
#include "curop-inl.h"
#include "json.h"
#include "security.h"
#include "commands.h"
#include "instance.h"
#include "../s/d_logic.h"
#include "dbwebserver.h"
#include "../util/mongoutils/html.h"
#include "../util/mongoutils/checksum.h"

namespace mongo {

    Client* Client::syncThread;
    mongo::mutex Client::clientsMutex("clientsMutex");
    set<Client*> Client::clients; // always be in clientsMutex when manipulating this
    boost::thread_specific_ptr<Client> currentClient;

    /* each thread which does db operations has a Client object in TLS.  
       call this when your thread starts. 
    */
    Client& Client::initThread(const char *desc, MessagingPort *mp) {
        setThreadName(desc);
        assert( currentClient.get() == 0 );
        Client *c = new Client(desc, mp);
        currentClient.reset(c);
        mongo::lastError.initThread();
        return *c;
    }

    Client::Client(const char *desc, MessagingPort *p) : 
      _context(0),
      _shutdown(false),
      _desc(desc),
      _god(0),
      _lastOp(0), 
      _mp(p)
    {
        _curOp = new CurOp( this );
        scoped_lock bl(clientsMutex);
        clients.insert(this);
    }

    Client::~Client() { 
        _god = 0;

        if ( _context )
            error() << "Client::~Client _context should be null but is not; client:" << _desc << endl;

        if ( ! _shutdown ) {
            error() << "Client::shutdown not called: " << _desc << endl;
        }
        
        scoped_lock bl(clientsMutex);
        if ( ! _shutdown )
            clients.erase(this);
        delete _curOp;
    }
    
    bool Client::shutdown(){
        _shutdown = true;
        if ( inShutdown() )
            return false;
        {
            scoped_lock bl(clientsMutex);
            clients.erase(this);
            if ( isSyncThread() ) {
                syncThread = 0;
            }
        }

        return false;
    }

    BSONObj CachedBSONObj::_tooBig = fromjson("{\"$msg\":\"query not recording (too large)\"}");
    AtomicUInt CurOp::_nextOpNum;
    
    Client::Context::Context( string ns , Database * db, bool doauth )
        : _client( currentClient.get() ) , _oldContext( _client->_context ) , 
          _path( dbpath ) , _lock(0) , _justCreated(false) {
        assert( db && db->isOk() );
        _ns = ns;
        _db = db;
        _client->_context = this;
        if ( doauth )
            _auth();
    }

    void Client::Context::_finishInit( bool doauth ){
        int lockState = dbMutex.getState();
        assert( lockState );
        
        _db = dbHolder.get( _ns , _path );
        if ( _db ){
            _justCreated = false;
        }
        else if ( dbMutex.getState() > 0 ){
            // already in a write lock
            _db = dbHolder.getOrCreate( _ns , _path , _justCreated );
            assert( _db );
        }
        else if ( dbMutex.getState() < -1 ){
            // nested read lock :(
            assert( _lock );
            _lock->releaseAndWriteLock();
            _db = dbHolder.getOrCreate( _ns , _path , _justCreated );
            assert( _db );
        }
        else {
            // we have a read lock, but need to get a write lock for a bit
            // we need to be in a write lock since we're going to create the DB object
            // to do that, we're going to unlock, then get a write lock
            // this is so that if this is the first query and its long doesn't block db
            // we just have to check that the db wasn't closed in the interim where we unlock
            for ( int x=0; x<2; x++ ){
                {                     
                    dbtemprelease unlock;
                    writelock lk( _ns );
                    dbHolder.getOrCreate( _ns , _path , _justCreated );
                }
                
                _db = dbHolder.get( _ns , _path );
                
                if ( _db )
                    break;
                
                log() << "db was closed on us right after we opened it: " << _ns << endl;
            }
            
            uassert( 13005 , "can't create db, keeps getting closed" , _db );
        }
        
        _client->_context = this;
        _client->_curOp->enter( this );
        if ( doauth )
            _auth( lockState );

        switch ( _client->_curOp->getOp() ){
        case dbGetMore: // getMore's are special and should be handled else where
        case dbUpdate: // update & delete check shard version in instance.cpp, so don't check here as well
        case dbDelete: 
            break;
        default: {
            string errmsg;
            if ( ! shardVersionOk( _ns , lockState > 0 , errmsg ) ){
                ostringstream os;
                os << "[" << _ns << "] shard version not ok in Client::Context: " << errmsg;
                msgassertedNoTrace( StaleConfigInContextCode , os.str().c_str() );
            }
        }
        }
    }
    
    void Client::Context::_auth( int lockState ){
        if ( _client->_ai.isAuthorizedForLock( _db->name , lockState ) )
            return;

        // before we assert, do a little cleanup
        _client->_context = _oldContext; // note: _oldContext may be null
        
        stringstream ss;
        ss << "unauthorized db:" << _db->name << " lock type:" << lockState << " client:" << _client->clientAddress();
        uasserted( 10057 , ss.str() );
    }

    Client::Context::~Context() {
        DEV assert( _client == currentClient.get() );
        _client->_curOp->leave( this );
        _client->_context = _oldContext; // note: _oldContext may be null
    }

    string Client::clientAddress(bool includePort) const {
        if( _curOp )
            return _curOp->getRemoteString(includePort);
        return "";
    }

    string Client::toString() const {
        stringstream ss;
        if ( _curOp )
            ss << _curOp->infoNoauth().jsonString();
        return ss.str();
    }

    string sayClientState(){
        Client* c = currentClient.get();
        if ( !c )
            return "no client";
        return c->toString();
    }
    
    Client* curopWaitingForLock( int type ){
        Client * c = currentClient.get();
        assert( c );
        CurOp * co = c->curop();
        if ( co ){
            co->waitingForLock( type );
        }
        return c;
    }
    void curopGotLock(Client *c){
        assert(c);
        CurOp * co = c->curop();
        if ( co ) 
            co->gotLock();
    }

    void KillCurrentOp::interruptJs( AtomicUInt *op ) {
        if ( !globalScriptEngine )
            return;
        if ( !op ) {
            globalScriptEngine->interruptAll();
        } else {
            globalScriptEngine->interrupt( *op );
        }
    }

    void KillCurrentOp::killAll() {
        _globalKill = true;
        interruptJs( 0 );
    }

    void KillCurrentOp::kill(AtomicUInt i) {
        bool found = false;
        {
            scoped_lock l( Client::clientsMutex );
            for( set< Client* >::const_iterator j = Client::clients.begin(); !found && j != Client::clients.end(); ++j ) {
                for( CurOp *k = ( *j )->curop(); !found && k; k = k->parent() ) {
                    if ( k->opNum() == i ) {
                        k->kill();
                        for( CurOp *l = ( *j )->curop(); l != k; l = l->parent() ) {
                            l->kill();
                        }
                        found = true;
                    }                        
                }
            }
        }
        if ( found ) {
            interruptJs( &i );
        }
    }
        
    CurOp::~CurOp(){
        if ( _wrapped ){
            scoped_lock bl(Client::clientsMutex);
            _client->_curOp = _wrapped;
        }
        _client = 0;
    }

    BSONObj CurOp::infoNoauth() {
        BSONObjBuilder b;
        b.append("opid", _opNum);
        bool a = _active && _start;
        b.append("active", a);
        if ( _lockType )
            b.append("lockType" , _lockType > 0 ? "write" : "read"  );
        b.append("waitingForLock" , _waitingForLock );
        
        if( a ){
            b.append("secs_running", elapsedSeconds() );
        }
        
        b.append( "op" , opToString( _op ) );
        
        b.append("ns", _ns);
        
        _query.append( b , "query" );

        // b.append("inLock",  ??
        stringstream clientStr;
        clientStr << _remote.toString();
        b.append("client", clientStr.str());

        if ( _client )
            b.append( "desc" , _client->desc() );
        
        if ( ! _message.empty() ){
            if ( _progressMeter.isActive() ){
                StringBuilder buf(128);
                buf << _message.toString() << " " << _progressMeter.toString();
                b.append( "msg" , buf.str() );
            }
            else {
                b.append( "msg" , _message.toString() );
            }
        }

        return b.obj();
    }

    void Client::gotHandshake( const BSONObj& o ){
        BSONObjIterator i(o);

        {
            BSONElement id = i.next();
            assert( id.type() );
            _remoteId = id.wrap( "_id" );
        }
        
        BSONObjBuilder b;
        while ( i.more() )
            b.append( i.next() );
        _handshake = b.obj();
    }

    class HandshakeCmd : public Command {
    public:
        void help(stringstream& h) const { h << "internal"; }
        HandshakeCmd() : Command( "handshake" ){}
        virtual LockType locktype() const { return NONE; } 
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return false; }
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            Client& c = cc();
            c.gotHandshake( cmdObj );
            return 1;
        }        

    } handshakeCmd;

    class ClientListPlugin : public WebStatusPlugin {
    public:
        ClientListPlugin() : WebStatusPlugin( "clients" , 20 ){}
        virtual void init(){}
        
        virtual void run( stringstream& ss ){
            using namespace mongoutils::html;

            ss << "\n<table border=1 cellpadding=2 cellspacing=0>";
            ss << "<tr align='left'>"
               << th( a("", "Connections to the database, both internal and external.", "Client") )
               << th( a("http://www.mongodb.org/display/DOCS/Viewing+and+Terminating+Current+Operation", "", "OpId") )
               << "<th>Active</th>" 
               << "<th>LockType</th>"
               << "<th>Waiting</th>"
               << "<th>SecsRunning</th>"
               << "<th>Op</th>"
               << th( a("http://www.mongodb.org/display/DOCS/Developer+FAQ#DeveloperFAQ-What%27sa%22namespace%22%3F", "", "Namespace") )
               << "<th>Query</th>"
               << "<th>client</th>"
               << "<th>msg</th>"
               << "<th>progress</th>"

               << "</tr>\n";
            {
                scoped_lock bl(Client::clientsMutex);
                for( set<Client*>::iterator i = Client::clients.begin(); i != Client::clients.end(); i++ ) { 
                    Client *c = *i;
                    CurOp& co = *(c->curop());
                    ss << "<tr><td>" << c->desc() << "</td>";
                    
                    tablecell( ss , co.opNum() );
                    tablecell( ss , co.active() );
                    {
                        int lt = co.getLockType();
                        if( lt == -1 ) tablecell(ss, "R");
                        else if( lt == 1 ) tablecell(ss, "W");
                        else
                            tablecell( ss ,  lt);
                    }
                    tablecell( ss , co.isWaitingForLock() );
                    if ( co.active() )
                        tablecell( ss , co.elapsedSeconds() );
                    else
                        tablecell( ss , "" );
                    tablecell( ss , co.getOp() );
                    tablecell( ss , co.getNS() );
                    if ( co.haveQuery() ){
                        tablecell( ss , co.query() );
                    }
                    else
                        tablecell( ss , "" );
                    tablecell( ss , co.getRemoteString() );

                    tablecell( ss , co.getMessage() );
                    tablecell( ss , co.getProgressMeter().toString() );


                    ss << "</tr>\n";
                }
            }
            ss << "</table>\n";

        }
        
    } clientListPlugin;

    int Client::recommendedYieldMicros( int * writers , int * readers ){
        int num = 0;
        int w = 0;
        int r = 0;
        {
            scoped_lock bl(clientsMutex);
            for ( set<Client*>::iterator i=clients.begin(); i!=clients.end(); ++i ){
                Client* c = *i;
                if ( c->curop()->isWaitingForLock() ){
                    num++;
                    if ( c->curop()->getLockType() > 0 )
                        w++;
                    else
                        r++;
                }
            }
        }
        
        if ( writers )
            *writers = w;
        if ( readers )
            *readers = r;
        
        int time = r * 100;
        time += r * 500;
        
        time = min( time , 1000000 );

        // there has been a kill request for this op - we should yield to allow the op to stop
        if ( killCurrentOp.checkForInterruptNoAssert( false ) ) {
            return 100;
        }
        
        return time;
    }

    int Client::getActiveClientCount( int& writers, int& readers ){
        writers = 0;
        readers = 0;
        
        scoped_lock bl(clientsMutex);
        for ( set<Client*>::iterator i=clients.begin(); i!=clients.end(); ++i ){
            Client* c = *i;
            if ( ! c->curop()->active() )
                continue;
            
            int l = c->curop()->getLockType();
            if ( l > 0 )
                writers++;
            else if ( l < 0 )
                readers++;
            
        }

        return writers + readers;
    }
}
