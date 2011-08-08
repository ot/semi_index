// mr.h

/**
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

#include "pch.h"

namespace mongo {
    
    namespace mr {

        typedef vector<BSONObj> BSONList;

        class State;

        // ------------  function interfaces -----------

        class Mapper : boost::noncopyable {
        public:
            virtual ~Mapper(){}
            virtual void init( State * state ) = 0;

            virtual void map( const BSONObj& o ) = 0;
        };
        
        class Finalizer : boost::noncopyable {
        public:
            virtual ~Finalizer(){}
            virtual void init( State * state ) = 0;
            
            /**
             * this takes a tuple and returns a tuple
             */
            virtual BSONObj finalize( const BSONObj& tuple ) = 0;
        };
        
        class Reducer : boost::noncopyable {
        public:
            virtual ~Reducer(){}
            virtual void init( State * state ) = 0;
            
            virtual BSONObj reduce( const BSONList& tuples ) = 0;
            /** this means its a fianl reduce, even if there is no finalizer */
            virtual BSONObj reduce( const BSONList& tuples , Finalizer * finalizer ) = 0;
        };
        
        // ------------  js function implementations -----------        
        
        /**
         * used as a holder for Scope and ScriptingFunction
         * visitor like pattern as Scope is gotten from first access
         */
        class JSFunction : boost::noncopyable {
        public:
            /**
             * @param type (map|reduce|finalzie)
             */
            JSFunction( string type , const BSONElement& e );
            virtual ~JSFunction(){}
            
            virtual void init( State * state );

            Scope * scope() const { return _scope; }
            ScriptingFunction func() const { return _func; }

        private:
            string _type;
            string _code; // actual javascript code
            BSONObj _wantedScope; // this is for CodeWScope 
            
            Scope * _scope; // this is not owned by us, and might be shared
            ScriptingFunction _func;
        };

        class JSMapper : public Mapper {
        public:
            JSMapper( const BSONElement & code ) : _func( "map" , code ){}
            virtual void map( const BSONObj& o );
            virtual void init( State * state );
            
        private:
            JSFunction _func;
            BSONObj _params;
        };
        
        class JSReducer : public Reducer {
        public:
            JSReducer( const BSONElement& code ) : _func( "reduce" , code ){}
            virtual void init( State * state ){ _func.init( state ); }

            virtual BSONObj reduce( const BSONList& tuples );
            virtual BSONObj reduce( const BSONList& tuples , Finalizer * finalizer );

        private:

            /**
             * result in "return"
             * @param key OUT 
             * @param endSizeEstimate OUT
            */
            void _reduce( const BSONList& values , BSONObj& key , int& endSizeEstimate );
            
            JSFunction _func;

        };
        
        class JSFinalizer : public Finalizer  {
        public:
            JSFinalizer( const BSONElement& code ) : _func( "finalize" , code ){}
            virtual BSONObj finalize( const BSONObj& o );
            virtual void init( State * state ){ _func.init( state ); }
        private:
            JSFunction _func;

        };

        // -----------------
        

        class TupleKeyCmp {
        public:
            TupleKeyCmp(){}
            bool operator()( const BSONObj &l, const BSONObj &r ) const {
                return l.firstElement().woCompare( r.firstElement() ) < 0;
            }
        };
        
        typedef map< BSONObj,BSONList,TupleKeyCmp > InMemory; // from key to list of tuples

        /**
         * holds map/reduce config information
         */
        class Config {
        public:
            Config( const string& _dbname , const BSONObj& cmdObj );

            string dbname;
            string ns;
            
            // options
            bool verbose;            

            // query options
            
            BSONObj filter;
            BSONObj sort;
            long long limit;

            // functions
            
            scoped_ptr<Mapper> mapper;
            scoped_ptr<Reducer> reducer;
            scoped_ptr<Finalizer> finalizer;
            
            BSONObj mapParams;
            BSONObj scopeSetup;
            
            // output tables
            string incLong;
            
            string tempShort;
            string tempLong;
            
            string finalShort;
            string finalLong;

            enum { REPLACE , // atomically replace the collection
                   MERGE ,  // merge keys, override dups
                   REDUCE , // merge keys, reduce dups
                   INMEMORY // only store in memory, limited in size
            } outType;
            
            static AtomicUInt JOB_NUMBER;
        }; // end MRsetup
        
        /**
         * stores information about intermediate map reduce state
         * controls flow of data from map->reduce->finalize->output
         */
        class State {
        public:
            State( const Config& c );
            ~State();

            void init();
            
            // ---- prep  -----
            bool sourceExists();

            long long incomingDocuments();

            // ---- map stage ---- 
            
            /**
             * stages on in in-memory storage
             */
            void emit( const BSONObj& a );

            /**
             * if size is big, run a reduce
             * if its still big, dump to temp collection
             */
            void checkSize();

            /**
             * run reduce on _temp
             */
            void reduceInMemory();

            /**
             * transfers in memory storage to temp collection
             */
            void dumpToInc();

            // ------ reduce stage -----------

            void prepTempCollection();            
            
            void finalReduce( BSONList& values );
            
            void finalReduce( CurOp * op , ProgressMeterHolder& pm );
            
            // ------- cleanup/data positioning ----------
            
            /**
               @return number objects in collection
             */
            long long renameIfNeeded();
            
            /**
             * if INMEMORY will append
             * may also append stats or anything else it likes
             */
            void appendResults( BSONObjBuilder& b );

            // -------- util ------------
            
            /**
             * inserts with correct replication semantics
             */
            void insert( const string& ns , BSONObj& o );
            
            // ------ simple accessors -----

            /** State maintains ownership, do no use past State lifetime */
            Scope* scope() { return _scope.get(); }
            
            const Config& config() { return _config; }

            long long numEmits() const { return _numEmits; }

        protected:

            void _insertToInc( BSONObj& o );
            static void _add( InMemory* im , const BSONObj& a , long& size );

            scoped_ptr<Scope> _scope;
            const Config& _config;
            bool _onDisk; // if the end result of this map reduce is disk or not

            DBDirectClient _db;

            scoped_ptr<InMemory> _temp;
            long _size; // bytes in _temp
            
            long long _numEmits;
        };

        BSONObj fast_emit( const BSONObj& args );

    } // end mr namespace
}


