// index.h

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
#include "diskloc.h"
#include "jsobj.h"
#include "indexkey.h"

namespace mongo {

	/* Details about a particular index. There is one of these effectively for each object in 
	   system.namespaces (although this also includes the head pointer, which is not in that 
	   collection).

       ** MemoryMapped Record ** (i.e., this is on disk data)
	 */
    class IndexDetails {
    public:
        /**
         * btree head disk location
         * TODO We should make this variable private, since btree operations
         * may change its value and we don't want clients to rely on an old
         * value.  If we create a btree class, we can provide a btree object
         * to clients instead of 'head'.
         */
        DiskLoc head;

        /* Location of index info object. Format:

             { name:"nameofindex", ns:"parentnsname", key: {keypattobject}
               [, unique: <bool>, background: <bool>] 
             }

           This object is in the system.indexes collection.  Note that since we
           have a pointer to the object here, the object in system.indexes MUST NEVER MOVE.
        */
        DiskLoc info;

        /* extract key value from the query object
           e.g., if key() == { x : 1 },
                 { x : 70, y : 3 } -> { x : 70 }
        */
        BSONObj getKeyFromQuery(const BSONObj& query) const {
            BSONObj k = keyPattern();
            BSONObj res = query.extractFieldsUnDotted(k);
            return res;
        }

        /* pull out the relevant key objects from obj, so we
           can index them.  Note that the set is multiple elements
           only when it's a "multikey" array.
           keys will be left empty if key not found in the object.
        */
        void getKeysFromObject( const BSONObj& obj, BSONObjSetDefaultOrder& keys) const;

        /* get the key pattern for this object.
           e.g., { lastname:1, firstname:1 }
        */
        BSONObj keyPattern() const {
            return info.obj().getObjectField("key");
        }

        /**
         * @return offset into keyPattern for key
                   -1 if doesn't exist
         */
        int keyPatternOffset( const string& key ) const;
        bool inKeyPattern( const string& key ) const { return keyPatternOffset( key ) >= 0; }
        
        /* true if the specified key is in the index */
        bool hasKey(const BSONObj& key);
        bool wouldCreateDup(const BSONObj& key, DiskLoc self);

        // returns name of this index's storage area
        // database.table.$index
        string indexNamespace() const {
            BSONObj io = info.obj();
            string s;
            s.reserve(Namespace::MaxNsLen);
            s = io.getStringField("ns");
            assert( !s.empty() );
            s += ".$";
            s += io.getStringField("name");
            return s;
        }

        string indexName() const { // e.g. "ts_1"
            BSONObj io = info.obj();
            return io.getStringField("name");
        }

        static bool isIdIndexPattern( const BSONObj &pattern ) {
            BSONObjIterator i(pattern);
            BSONElement e = i.next();
            if( strcmp(e.fieldName(), "_id") != 0 ) return false;
            return i.next().eoo();            
        }
        
        /* returns true if this is the _id index. */
        bool isIdIndex() const { 
            return isIdIndexPattern( keyPattern() );
        }

        /* gets not our namespace name (indexNamespace for that),
           but the collection we index, its name.
           */
        string parentNS() const {
            BSONObj io = info.obj();
            return io.getStringField("ns");
        }

        bool unique() const { 
            BSONObj io = info.obj();
            return io["unique"].trueValue() || 
                /* temp: can we juse make unique:true always be there for _id and get rid of this? */
                isIdIndex();
        }

        /* if set, when building index, if any duplicates, drop the duplicating object */
        bool dropDups() const {
            return info.obj().getBoolField( "dropDups" );
        }

        /* delete this index.  does NOT clean up the system catalog
           (system.indexes or system.namespaces) -- only NamespaceIndex.
        */
        void kill_idx();
        
        const IndexSpec& getSpec() const;

        string toString() const {
            return info.obj().toString();
        }
    };

    struct IndexChanges/*on an update*/ {
        BSONObjSetDefaultOrder oldkeys;
        BSONObjSetDefaultOrder newkeys;
        vector<BSONObj*> removed; // these keys were removed as part of the change
        vector<BSONObj*> added;   // these keys were added as part of the change

        /** @curObjLoc - the object we want to add's location.  if it is already in the 
                         index, that is allowed here (for bg indexing case).
        */
        void dupCheck(IndexDetails& idx, DiskLoc curObjLoc) {
            if( added.empty() || !idx.unique() )
                return;
            for( vector<BSONObj*>::iterator i = added.begin(); i != added.end(); i++ ) {
                bool dup = idx.wouldCreateDup(**i, curObjLoc);
                uassert( 11001 , "E11001 duplicate key on update", !dup);
            }
        }
    };

    class NamespaceDetails;
    // changedId should be initialized to false
    void getIndexChanges(vector<IndexChanges>& v, NamespaceDetails& d, BSONObj newObj, BSONObj oldObj, bool &cangedId);
    void dupCheck(vector<IndexChanges>& v, NamespaceDetails& d, DiskLoc curObjLoc);
} // namespace mongo
