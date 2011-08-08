// query.h

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

#pragma once

#include "../pch.h"
#include "../util/message.h"
#include "dbmessage.h"
#include "jsobj.h"
#include "diskloc.h"
#include "projection.h"

/* db request message format

   unsigned opid;         // arbitary; will be echoed back
   byte operation;
   int options;

   then for:

   dbInsert:
      string collection;
      a series of JSObjects
   dbDelete:
      string collection;
	  int flags=0; // 1=DeleteSingle
      JSObject query;
   dbUpdate:
      string collection;
	  int flags; // 1=upsert
      JSObject query;
	  JSObject objectToUpdate;
        objectToUpdate may include { $inc: <field> } or { $set: ... }, see struct Mod.
   dbQuery:
      string collection;
	  int nToSkip;
	  int nToReturn; // how many you want back as the beginning of the cursor data (0=no limit)            
                     // greater than zero is simply a hint on how many objects to send back per "cursor batch".
                     // a negative number indicates a hard limit.
      JSObject query;
	  [JSObject fieldsToReturn]
   dbGetMore:
	  string collection; // redundant, might use for security.
      int nToReturn;
      int64 cursorID;
   dbKillCursors=2007:
      int n;
	  int64 cursorIDs[n];

   Note that on Update, there is only one object, which is different
   from insert where you can pass a list of objects to insert in the db.
   Note that the update field layout is very similar layout to Query.
*/

// struct QueryOptions, QueryResult, QueryResultFlags in:
#include "../client/dbclient.h"

namespace mongo {

    extern const int MaxBytesToReturnToClientAtOnce;

    // for an existing query (ie a ClientCursor), send back additional information.
    struct GetMoreWaitException { };

    QueryResult* processGetMore(const char *ns, int ntoreturn, long long cursorid , CurOp& op, int pass, bool& exhaust);
    
    struct UpdateResult {
        bool existing; // if existing objects were modified
        bool mod;      // was this a $ mod
        long long num; // how many objects touched
        OID upserted;  // if something was upserted, the new _id of the object

        UpdateResult( bool e, bool m, unsigned long long n , const BSONObj& upsertedObject = BSONObj() )
            : existing(e) , mod(m), num(n){
            upserted.clear();

            BSONElement id = upsertedObject["_id"];
            if ( ! e && n == 1 && id.type() == jstOID ){
                upserted = id.OID();
            }
        }
        
    };

    class RemoveSaver;
    
    /* returns true if an existing object was updated, false if no existing object was found.
       multi - update multiple objects - mostly useful with things like $set
       god - allow access to system namespaces
    */
    UpdateResult updateObjects(const char *ns, const BSONObj& updateobj, BSONObj pattern, bool upsert, bool multi , bool logop , OpDebug& debug );
    UpdateResult _updateObjects(bool god, const char *ns, const BSONObj& updateobj, BSONObj pattern, 
                                bool upsert, bool multi , bool logop , OpDebug& debug , RemoveSaver * rs = 0 );

    // If justOne is true, deletedId is set to the id of the deleted object.
    long long deleteObjects(const char *ns, BSONObj pattern, bool justOne, bool logop = false, bool god=false, RemoveSaver * rs=0);

    long long runCount(const char *ns, const BSONObj& cmd, string& err);

    const char * runQuery(Message& m, QueryMessage& q, CurOp& curop, Message &result);
    
    /* This is for languages whose "objects" are not well ordered (JSON is well ordered).
       [ { a : ... } , { b : ... } ] -> { a : ..., b : ... }
    */
    inline BSONObj transformOrderFromArrayFormat(BSONObj order) {
        /* note: this is slow, but that is ok as order will have very few pieces */
        BSONObjBuilder b;
        char p[2] = "0";

        while ( 1 ) {
            BSONObj j = order.getObjectField(p);
            if ( j.isEmpty() )
                break;
            BSONElement e = j.firstElement();
            uassert( 10102 , "bad order array", !e.eoo());
            uassert( 10103 , "bad order array [2]", e.isNumber());
            b.append(e);
            (*p)++;
            uassert( 10104 , "too many ordering elements", *p <= '9');
        }

        return b.obj();
    }

    /**
     * this represents a total user query
     * includes fields from the query message, both possible query levels
     * parses everything up front
     */
    class ParsedQuery {
    public:
        ParsedQuery( QueryMessage& qm )
            : _ns( qm.ns ) , _ntoskip( qm.ntoskip ) , _ntoreturn( qm.ntoreturn ) , _options( qm.queryOptions ){
            init( qm.query );
            initFields( qm.fields );
        }
        ParsedQuery( const char* ns , int ntoskip , int ntoreturn , int queryoptions , const BSONObj& query , const BSONObj& fields )
            : _ns( ns ) , _ntoskip( ntoskip ) , _ntoreturn( ntoreturn ) , _options( queryoptions ){
            init( query );
            initFields( fields );
        }
        
        ~ParsedQuery(){}

        const char * ns() const { return _ns; }
        bool isLocalDB() const { return strncmp(_ns, "local.", 6) == 0; }

        const BSONObj& getFilter() const { return _filter; }
        Projection* getFields() const { return _fields.get(); }
        shared_ptr<Projection> getFieldPtr() const { return _fields; }

        int getSkip() const { return _ntoskip; }
        int getNumToReturn() const { return _ntoreturn; }
        bool wantMore() const { return _wantMore; }
        int getOptions() const { return _options; }
        bool hasOption( int x ) const { return x & _options; }

        
        bool isExplain() const { return _explain; }
        bool isSnapshot() const { return _snapshot; }
        bool returnKey() const { return _returnKey; }
        bool showDiskLoc() const { return _showDiskLoc; }

        const BSONObj& getMin() const { return _min; }
        const BSONObj& getMax() const { return _max; }
        const BSONObj& getOrder() const { return _order; }
        const BSONElement& getHint() const { return _hint; }
        int getMaxScan() const { return _maxScan; }
        
        bool couldBeCommand() const {
            /* we assume you are using findOne() for running a cmd... */
            return _ntoreturn == 1 && strstr( _ns , ".$cmd" );
        }

        bool hasIndexSpecifier() const {
            return ! _hint.eoo() || ! _min.isEmpty() || ! _max.isEmpty();
        }

        /* if ntoreturn is zero, we return up to 101 objects.  on the subsequent getmore, there
           is only a size limit.  The idea is that on a find() where one doesn't use much results,
           we don't return much, but once getmore kicks in, we start pushing significant quantities.
           
           The n limit (vs. size) is important when someone fetches only one small field from big
           objects, which causes massive scanning server-side.
        */
        bool enoughForFirstBatch( int n , int len ) const {
            if ( _ntoreturn == 0 )
                return ( len > 1024 * 1024 ) || n >= 101;
            return n >= _ntoreturn || len > MaxBytesToReturnToClientAtOnce;
        }

        bool enough( int n ) const {
            if ( _ntoreturn == 0 )
                return false;
            return n >= _ntoreturn;
        }
        
    private:
        void init( const BSONObj& q ){
            _reset();
            uassert( 10105 , "bad skip value in query", _ntoskip >= 0);
            
            if ( _ntoreturn < 0 ){
                /* _ntoreturn greater than zero is simply a hint on how many objects to send back per 
                   "cursor batch".
                   A negative number indicates a hard limit.
                */
                _wantMore = false;
                _ntoreturn = -_ntoreturn;
            }

            
            BSONElement e = q["query"];
            if ( ! e.isABSONObj() )
                e = q["$query"];
            
            if ( e.isABSONObj() ){
                _filter = e.embeddedObject();
                _initTop( q );
            }
            else {
                _filter = q;
            }
        }

        void _reset(){
            _wantMore = true;
            _explain = false;
            _snapshot = false;
            _returnKey = false;
            _showDiskLoc = false;
            _maxScan = 0;
        }

        void _initTop( const BSONObj& top ){
            BSONObjIterator i( top );
            while ( i.more() ){
                BSONElement e = i.next();
                const char * name = e.fieldName();

                if ( strcmp( "$orderby" , name ) == 0 ||
                     strcmp( "orderby" , name ) == 0 ){
                    if ( e.type() == Object ) {
                        _order = e.embeddedObject();
                    } else if ( e.type() == Array ) {
                        _order = transformOrderFromArrayFormat( _order );
                    } else {
                        uassert(13513, "sort must be an object or array", 0);
                    }
                }
                else if ( strcmp( "$explain" , name ) == 0 )
                    _explain = e.trueValue();
                else if ( strcmp( "$snapshot" , name ) == 0 )
                    _snapshot = e.trueValue();
                else if ( strcmp( "$min" , name ) == 0 )
                    _min = e.embeddedObject();
                else if ( strcmp( "$max" , name ) == 0 )
                    _max = e.embeddedObject();
                else if ( strcmp( "$hint" , name ) == 0 )
                    _hint = e;
                else if ( strcmp( "$returnKey" , name ) == 0 )
                    _returnKey = e.trueValue();
                else if ( strcmp( "$maxScan" , name ) == 0 )
                    _maxScan = e.numberInt();
                else if ( strcmp( "$showDiskLoc" , name ) == 0 )
                    _showDiskLoc = e.trueValue();
                

            }

            if ( _snapshot ){
                uassert( 12001 , "E12001 can't sort with $snapshot", _order.isEmpty() );
                uassert( 12002 , "E12002 can't use hint with $snapshot", _hint.eoo() );
            }
            
        }

        void initFields( const BSONObj& fields ){
            if ( fields.isEmpty() )
                return;
            _fields.reset( new Projection() );
            _fields->init( fields );
        }

        ParsedQuery( const ParsedQuery& other ){
            assert(0);
        }

        const char* _ns;
        int _ntoskip;
        int _ntoreturn;
        int _options;
        
        BSONObj _filter;
        shared_ptr< Projection > _fields;
        
        bool _wantMore;

        bool _explain;
        bool _snapshot;
        bool _returnKey;
        bool _showDiskLoc;
        BSONObj _min;
        BSONObj _max;
        BSONElement _hint;
        BSONObj _order;
        int _maxScan;
    };
    

} // namespace mongo

#include "clientcursor.h"
