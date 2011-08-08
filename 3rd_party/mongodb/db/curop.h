// @file curop.h

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

#include "namespace-inl.h"
#include "client.h"
#include "../bson/util/atomic_int.h"
#include "../util/concurrency/spin_lock.h"
#include "../util/time_support.h"
#include "db.h"
#include "../scripting/engine.h"

namespace mongo { 

    /* lifespan is different than CurOp because of recursives with DBDirectClient */
    class OpDebug {
    public:
        StringBuilder str;
        void reset() { str.reset(); }
    };
    
    /**
     * stores a copy of a bson obj in a fixed size buffer
     * if its too big for the buffer, says "too big"
     * useful for keeping a copy around indefinitely without wasting a lot of space or doing malloc
     */
    class CachedBSONObj {
    public:
        enum { TOO_BIG_SENTINEL = 1 } ;
        static BSONObj _tooBig; // { $msg : "query not recording (too large)" }

        CachedBSONObj(){
            _size = (int*)_buf;
            reset();
        }
        
        void reset( int sz = 0 ) { _size[0] = sz; }
        
        void set( const BSONObj& o ){
            _lock.lock();
            try {
                int sz = o.objsize();
                
                if ( sz > (int) sizeof(_buf) ) { 
                    reset(TOO_BIG_SENTINEL);
                }
                else {
                    memcpy(_buf, o.objdata(), sz );
                }
                
                _lock.unlock();
            }
            catch ( ... ){
                _lock.unlock();
                throw;
            }
            
        }
        
        int size() const { return *_size; }
        bool have() const { return size() > 0; }

        BSONObj get(){
            _lock.lock();            
            BSONObj o;
            try {
                o = _get();
                _lock.unlock();
            }
            catch ( ... ){
                _lock.unlock();
                throw;
            }            
            return o;            
        }

        void append( BSONObjBuilder& b , const StringData& name ){
            _lock.lock();
            try {
                BSONObj temp = _get();
                b.append( name , temp );
                _lock.unlock();
            }
            catch ( ... ){
                _lock.unlock();
                throw;
            }
        }
        
    private:
        /** you have to be locked when you call this */
        BSONObj _get(){
            int sz = size();
            if ( sz == 0 )
                return BSONObj();
            if ( sz == TOO_BIG_SENTINEL )
                return _tooBig;
            return BSONObj( _buf ).copy();
        }

        SpinLock _lock;
        int * _size;
        char _buf[512];
    };

    /* Current operation (for the current Client).
       an embedded member of Client class, and typically used from within the mutex there. 
    */
    class CurOp : boost::noncopyable {
    public:
        CurOp( Client * client , CurOp * wrapped = 0 );
        ~CurOp();

        bool haveQuery() const { return _query.have(); }
        BSONObj query(){ return _query.get();  }

        void ensureStarted(){
            if ( _start == 0 )
                _start = _checkpoint = curTimeMicros64();            
        }
        void enter( Client::Context * context ){
            ensureStarted();
            setNS( context->ns() );
            if ( context->_db && context->_db->profile > _dbprofile )
                _dbprofile = context->_db->profile;
        }

        void leave( Client::Context * context ){
            unsigned long long now = curTimeMicros64();
            Top::global.record( _ns , _op , _lockType , now - _checkpoint , _command );
            _checkpoint = now;
        }

        void reset(){
            _reset();
            _start = _checkpoint = 0;
            _active = true;
            _opNum = _nextOpNum++;
            _ns[0] = '?'; // just in case not set later
            _debug.reset();
            _query.reset();
        }
        
        void reset( const SockAddr & remote, int op ) {
            reset();
            _remote = remote;
            _op = op;
        }
        
        void markCommand() { _command = true; }

        void waitingForLock( int type ){
            _waitingForLock = true;
            if ( type > 0 )
                _lockType = 1;
            else
                _lockType = -1;
        }
        void gotLock()             { _waitingForLock = false; }
        OpDebug& debug()           { return _debug; }        
        int profileLevel() const   { return _dbprofile; }
        const char * getNS() const { return _ns; }

        bool shouldDBProfile( int ms ) const {
            if ( _dbprofile <= 0 )
                return false;
            
            return _dbprofile >= 2 || ms >= cmdLine.slowMS;
        }
        
        AtomicUInt opNum() const { return _opNum; }

        /** if this op is running */
        bool active() const { return _active; }
        
        int getLockType() const { return _lockType; }
        bool isWaitingForLock() const { return _waitingForLock; } 
        int getOp() const { return _op; }
                
        /** micros */
        unsigned long long startTime() {
            ensureStarted();
            return _start;
        }

        void done() {
            _active = false;
            _end = curTimeMicros64();
        }
        
        unsigned long long totalTimeMicros() {
            massert( 12601 , "CurOp not marked done yet" , ! _active );
            return _end - startTime();
        }

        int totalTimeMillis() { return (int) (totalTimeMicros() / 1000); }

        int elapsedMillis() {
            unsigned long long total = curTimeMicros64() - startTime();
            return (int) (total / 1000);
        }

        int elapsedSeconds() { return elapsedMillis() / 1000; }

        void setQuery(const BSONObj& query) { _query.set( query ); }

        Client * getClient() const { return _client; }

        BSONObj info() { 
            if( ! cc().getAuthenticationInfo()->isAuthorized("admin") ) { 
                BSONObjBuilder b;
                b.append("err", "unauthorized");
                return b.obj();
            }
            return infoNoauth();
        }
        
        BSONObj infoNoauth();

        string getRemoteString( bool includePort = true ) { return _remote.toString(includePort); }

        ProgressMeter& setMessage( const char * msg , unsigned long long progressMeterTotal = 0 , int secondsBetween = 3 ) {
            if ( progressMeterTotal ){
                if ( _progressMeter.isActive() ){
                    cout << "about to assert, old _message: " << _message << " new message:" << msg << endl;
                    assert( ! _progressMeter.isActive() );
                }
                _progressMeter.reset( progressMeterTotal , secondsBetween );
            }
            else {
                _progressMeter.finished();
            }
            
            _message = msg;
            
            return _progressMeter;
        }
        
        string getMessage() const { return _message.toString(); }
        ProgressMeter& getProgressMeter() { return _progressMeter; }
        CurOp *parent() const { return _wrapped; }
        void kill() { _killed = true; }
        bool killed() const { return _killed; }
        void setNS(const char *ns) { 
            strncpy(_ns, ns, Namespace::MaxNsLen); 
            _ns[Namespace::MaxNsLen] = 0;
        }
        friend class Client;

    private:
        static AtomicUInt _nextOpNum;
        Client * _client;
        CurOp * _wrapped;
        unsigned long long _start;
        unsigned long long _checkpoint;
        unsigned long long _end;
        bool _active;
        int _op;
        bool _command;
        int _lockType; // see concurrency.h for values
        bool _waitingForLock;
        int _dbprofile; // 0=off, 1=slow, 2=all
        AtomicUInt _opNum;
        char _ns[Namespace::MaxNsLen+2];
        struct SockAddr _remote;
        CachedBSONObj _query;
        OpDebug _debug;
        ThreadSafeString _message;
        ProgressMeter _progressMeter;
        volatile bool _killed;

        void _reset(){
            _command = false;
            _lockType = 0;
            _dbprofile = 0;
            _end = 0;
            _waitingForLock = false;
            _message = "";
            _progressMeter.finished();
            _killed = false;
        }
    };

    /* _globalKill: we are shutting down
       otherwise kill attribute set on specified CurOp
       this class does not handle races between interruptJs and the checkForInterrupt functions - those must be
       handled by the client of this class
    */
    extern class KillCurrentOp { 
    public:
        void killAll();
        void kill(AtomicUInt i);

        /** @return true if global interrupt and should terminate the operation */
        bool globalInterruptCheck() const { return _globalKill; }
        
        void checkForInterrupt( bool heedMutex = true ) {
            if ( heedMutex && dbMutex.isWriteLocked() )
                return;
            if( _globalKill )
                uasserted(11600,"interrupted at shutdown");
            if( cc().curop()->killed() )
                uasserted(11601,"interrupted");
        }
        
        /** @return "" if not interrupted.  otherwise, you should stop. */
        const char *checkForInterruptNoAssert( bool heedMutex = true ) {
            if ( heedMutex && dbMutex.isWriteLocked() )
                return "";
            if( _globalKill )
                return "interrupted at shutdown";
            if( cc().curop()->killed() )
                return "interrupted";
            return "";
        }
        
    private:
        void interruptJs( AtomicUInt *op );
        volatile bool _globalKill;
    } killCurrentOp;

}
