/* clientcursor.h */

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

/* Cursor -- and its derived classes -- are our internal cursors.

   ClientCursor is a wrapper that represents a cursorid from our database
   application's perspective.
*/

#pragma once

#include "../pch.h"
#include "cursor.h"
#include "jsobj.h"
#include "../util/message.h"
#include "../util/background.h"
#include "diskloc.h"
#include "dbhelpers.h"
#include "matcher.h"
#include "../client/dbclient.h"
#include "projection.h"

namespace mongo {

    typedef long long CursorId; /* passed to the client so it can send back on getMore */
    class Cursor; /* internal server cursor base class */
    class ClientCursor;
    class ParsedQuery;

    struct ByLocKey {
        
        ByLocKey( const DiskLoc & l , const CursorId& i ) : loc(l), id(i){}
        
        static ByLocKey min( const DiskLoc& l ){ return ByLocKey( l , numeric_limits<long long>::min() ); }
        static ByLocKey max( const DiskLoc& l ){ return ByLocKey( l , numeric_limits<long long>::max() ); }

        bool operator<( const ByLocKey &other ) const {
            int x = loc.compare( other.loc );
            if ( x )
                return x < 0;
            return id < other.id;
        }

        DiskLoc loc;
        CursorId id;

    };

    /* todo: make this map be per connection.  this will prevent cursor hijacking security attacks perhaps.
     *       ERH: 9/2010 this may not work since some drivers send getMore over a different connection
    */
    typedef map<CursorId, ClientCursor*> CCById;
    typedef map<ByLocKey, ClientCursor*> CCByLoc;

    extern BSONObj id_obj;

    class ClientCursor {
        friend class CmdCursorInfo;
    public:
        static void assertNoCursors();

        /* use this to assure we don't in the background time out cursor while it is under use.
           if you are using noTimeout() already, there is no risk anyway.
           Further, this mechanism guards against two getMore requests on the same cursor executing
           at the same time - which might be bad.  That should never happen, but if a client driver
           had a bug, it could (or perhaps some sort of attack situation).
        */
        class Pointer : boost::noncopyable { 
            ClientCursor *_c;
        public:
            ClientCursor * c() { return _c; }
            void release() {
                if( _c ) {
                    assert( _c->_pinValue >= 100 );
                    _c->_pinValue -= 100;
                    _c = 0;
                }
            }
            ~Pointer() { release(); }
            Pointer(long long cursorid) {
                recursive_scoped_lock lock(ccmutex);
                _c = ClientCursor::find_inlock(cursorid, true);
                if( _c ) {
                    if( _c->_pinValue >= 100 ) {
                        _c = 0;
                        uasserted(12051, "clientcursor already in use? driver problem?");
                    }
                    _c->_pinValue += 100;
                }
            }
        }; 
        
        // This object assures safe and reliable cleanup of the ClientCursor.
        // The implementation assumes that there will be no duplicate ids among cursors
        // (which is assured if cursors must last longer than 1 second).
        class CleanupPointer : boost::noncopyable {
        public:
            CleanupPointer() : _c( 0 ), _id( -1 ) {}
            void reset( ClientCursor *c = 0 ) {
                if ( c == _c )
                    return;
                if ( _c ) {
                    // be careful in case cursor was deleted by someone else
                    ClientCursor::erase( _id );
                }                
                if ( c ) {
                    _c = c;
                    _id = c->_cursorid;
                } else {
                    _c = 0;
                    _id = -1;
                }
            }
            ~CleanupPointer() {
                DESTRUCTOR_GUARD ( reset(); );
            }
            operator bool() { return _c; }
            ClientCursor * operator-> () { return _c; }
        private:
            ClientCursor *_c;
            CursorId _id;
        };

        ClientCursor(int queryOptions, const shared_ptr<Cursor>& c, const string& ns, BSONObj query = BSONObj() );

        ~ClientCursor();

        // ***************  basic accessors *******************
        
        CursorId cursorid() const { return _cursorid; }
        string ns() const { return _ns; }
        Database * db() const { return _db; }
        const BSONObj& query() const { return _query; }
        int queryOptions() const { return _queryOptions; }
        
        DiskLoc lastLoc() const { return _lastLoc; }

        /* Get rid of cursors for namespaces that begin with nsprefix.
           Used by drop, dropIndexes, dropDatabase.
        */
        static void invalidate(const char *nsPrefix);

        /**
         * @param microsToSleep -1 : ask client 
         *                     >=0 : sleep for that amount
         * do a dbtemprelease 
         * note: caller should check matcher.docMatcher().atomic() first and not yield if atomic - 
         *       we don't do herein as this->matcher (above) is only initialized for true queries/getmore.
         *       (ie not set for remote/update)
         * @return if the cursor is still valid. 
         *         if false is returned, then this ClientCursor should be considered deleted - 
         *         in fact, the whole database could be gone.
         */
        bool yield( int microsToSleep = -1 );

        /**
         * @return same as yield()
         */
        bool yieldSometimes();
        
        static int yieldSuggest();
        static void staticYield( int micros );
        
        struct YieldData { CursorId _id; bool _doingDeletes; };
        bool prepareToYield( YieldData &data );
        static bool recoverFromYield( const YieldData &data );

        struct YieldLock : boost::noncopyable {
            explicit YieldLock( ptr<ClientCursor> cc )
                : _canYield(cc->_c->supportYields()) {
                if ( _canYield ){
                    cc->prepareToYield( _data );
                    _unlock.reset(new dbtempreleasecond());
                }
            }
            ~YieldLock(){
                if ( _unlock ){
                    log( LL_WARNING ) << "ClientCursor::YieldLock not closed properly" << endl;
                    relock();
                }
            }
            bool stillOk(){
                if ( ! _canYield )
                    return true;
                relock();
                return ClientCursor::recoverFromYield( _data );
            }
            void relock(){
                _unlock.reset();
            }
        private:
            const bool _canYield;
            YieldData _data;            
            scoped_ptr<dbtempreleasecond> _unlock;
        };

        // --- some pass through helpers for Cursor ---
        
        Cursor* c() const { return _c.get(); }
        int pos() const { return _pos; }
        
        void incPos( int n ) { _pos += n; } // TODO: this is bad
        void setPos( int n ) { _pos = n; } // TODO : this is bad too
        
        BSONObj indexKeyPattern() { return _c->indexKeyPattern();  }
        bool modifiedKeys() const { return _c->modifiedKeys(); }
        bool isMultiKey() const { return _c->isMultiKey(); }

        bool ok() { return _c->ok(); }
        bool advance(){ return _c->advance(); }
        BSONObj current() { return _c->current(); }
        DiskLoc currLoc() { return _c->currLoc(); }
        BSONObj currKey() const { return _c->currKey(); }
        

        /**
         * same as BSONObj::getFieldsDotted
         * if it can be retrieved from key, it is
         * @return if this was retrieved from key
         */
        bool getFieldsDotted( const string& name, BSONElementSet &ret );

        bool currentIsDup() { return _c->getsetdup( _c->currLoc() ); }
        
        bool currentMatches(){
            if ( ! _c->matcher() )
                return true;
            return _c->matcher()->matchesCurrent( _c.get() );
        }

    private:
        void setLastLoc_inlock(DiskLoc);

        static ClientCursor* find_inlock(CursorId id, bool warn = true) {
            CCById::iterator it = clientCursorsById.find(id);
            if ( it == clientCursorsById.end() ) {
                if ( warn )
                    OCCASIONALLY out() << "ClientCursor::find(): cursor not found in map " << id << " (ok after a drop)\n";
                return 0;
            }
            return it->second;
        }
    public:
        static ClientCursor* find(CursorId id, bool warn = true) { 
            recursive_scoped_lock lock(ccmutex);
            ClientCursor *c = find_inlock(id, warn);
			// if this asserts, your code was not thread safe - you either need to set no timeout 
			// for the cursor or keep a ClientCursor::Pointer in scope for it.
            massert( 12521, "internal error: use of an unlocked ClientCursor", c == 0 || c->_pinValue ); 
            return c;
        }

        static bool erase(CursorId id) {
            recursive_scoped_lock lock(ccmutex);
            ClientCursor *cc = find_inlock(id);
            if ( cc ) {
                assert( cc->_pinValue < 100 ); // you can't still have an active ClientCursor::Pointer
                delete cc;
                return true;
            }
            return false;
        }

        /**
         * @return number of cursors found
         */
        static int erase( int n , long long * ids );

        /* call when cursor's location changes so that we can update the
           cursorsbylocation map.  if you are locked and internally iterating, only
           need to call when you are ready to "unlock".
           */
        void updateLocation();

        void mayUpgradeStorage() {
            /* if ( !ids_.get() )
                return;
            stringstream ss;
            ss << ns << "." << cursorid;
            ids_->mayUpgradeStorage( ss.str() );*/
        }

        /**
         * @param millis amount of idle passed time since last call
         */
        bool shouldTimeout( unsigned millis );

        void storeOpForSlave( DiskLoc last );
        void updateSlaveLocation( CurOp& curop );
        
        unsigned idleTime() const { return _idleAgeMillis; }

        void setDoingDeletes( bool doingDeletes ) {_doingDeletes = doingDeletes; }

        void slaveReadTill( const OpTime& t ) { _slaveReadTill = t; }
        
    public: // static methods

        static void idleTimeReport(unsigned millis);
        
        static void appendStats( BSONObjBuilder& result );
        static unsigned numCursors() { return clientCursorsById.size(); }
        static void informAboutToDeleteBucket(const DiskLoc& b);
        static void aboutToDelete(const DiskLoc& dl);
        static void find( const string& ns , set<CursorId>& all );


    private: // methods
        
        // cursors normally timeout after an inactivy period to prevent excess memory use
        // setting this prevents timeout of the cursor in question.
        void noTimeout() { _pinValue++; }
        
        CCByLoc& byLoc() { return _db->ccByLoc; }

    private:

        CursorId _cursorid;
        
        const string _ns;
        Database * _db;

        const shared_ptr<Cursor> _c;
        map<string,int> _indexedFields;  // map from indexed field to offset in key object
        int _pos;                        // # objects into the cursor so far 
        
        const BSONObj _query;            // used for logging diags only; optional in constructor
        int _queryOptions;        // see enum QueryOptions dbclient.h
        
        OpTime _slaveReadTill;

        DiskLoc _lastLoc;                        // use getter and setter not this (important)
        unsigned _idleAgeMillis;                 // how long has the cursor been around, relative to server idle time

        /* 0 = normal
           1 = no timeout allowed
           100 = in use (pinned) -- see Pointer class
        */
        unsigned _pinValue;

        bool _doingDeletes;
        ElapsedTracker _yieldSometimesTracker;

    public:
        shared_ptr<ParsedQuery> pq;
        shared_ptr<Projection> fields; // which fields query wants returned
        Message originalMessage; // this is effectively an auto ptr for data the matcher points to



    private: // static members

        static CCById clientCursorsById;
        static long long numberTimedOut;
        static boost::recursive_mutex ccmutex;   // must use this for all statics above!        
        static CursorId allocCursorId_inlock();        

    };

    class ClientCursorMonitor : public BackgroundJob {
    public:
        string name() const { return "ClientCursorMonitor"; }
        void run();
    };

    extern ClientCursorMonitor clientCursorMonitor;
    
} // namespace mongo
