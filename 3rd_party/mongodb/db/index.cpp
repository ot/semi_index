// index.cpp

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
#include "namespace-inl.h"
#include "index.h"
#include "btree.h"
#include "query.h"
#include "background.h"
#include "repl/rs.h"

namespace mongo {

    int removeFromSysIndexes(const char *ns, const char *idxName) { 
        string system_indexes = cc().database()->name + ".system.indexes";
        BSONObjBuilder b;
        b.append("ns", ns);
        b.append("name", idxName); // e.g.: { name: "ts_1", ns: "foo.coll" }
        BSONObj cond = b.done();
        return (int) deleteObjects(system_indexes.c_str(), cond, false, false, true);
    }

    /* this is just an attempt to clean up old orphaned stuff on a delete all indexes 
       call. repair database is the clean solution, but this gives one a lighter weight 
       partial option.  see dropIndexes()
    */
    void assureSysIndexesEmptied(const char *ns, IndexDetails *idIndex) { 
        string system_indexes = cc().database()->name + ".system.indexes";
        BSONObjBuilder b;
        b.append("ns", ns);
        if( idIndex ) { 
            b.append("name", BSON( "$ne" << idIndex->indexName().c_str() ));
        }
        BSONObj cond = b.done();
        int n = (int) deleteObjects(system_indexes.c_str(), cond, false, false, true);
        if( n ) { 
            log() << "info: assureSysIndexesEmptied cleaned up " << n << " entries" << endl;
        }
    }

    int IndexDetails::keyPatternOffset( const string& key ) const {
        BSONObjIterator i( keyPattern() );
        int n = 0;
        while ( i.more() ){
            BSONElement e = i.next();
            if ( key == e.fieldName() )
                return n;
            n++;
        }
        return -1;
    }

    const IndexSpec& IndexDetails::getSpec() const {
        scoped_lock lk(NamespaceDetailsTransient::_qcMutex);
        return NamespaceDetailsTransient::get_inlock( info.obj()["ns"].valuestr() ).getIndexSpec( this );
    }

    /* delete this index.  does NOT clean up the system catalog
       (system.indexes or system.namespaces) -- only NamespaceIndex.
    */
    void IndexDetails::kill_idx() {
        string ns = indexNamespace(); // e.g. foo.coll.$ts_1
        try {

            string pns = parentNS(); // note we need a copy, as parentNS() won't work after the drop() below 
        
            // clean up parent namespace index cache
            NamespaceDetailsTransient::get_w( pns.c_str() ).deletedIndex();

            string name = indexName();

            /* important to catch exception here so we can finish cleanup below. */
            try { 
                dropNS(ns.c_str());
            }
            catch(DBException& ) { 
                log(2) << "IndexDetails::kill(): couldn't drop ns " << ns << endl;
            }
            head.setInvalid();
            info.setInvalid();

            // clean up in system.indexes.  we do this last on purpose.
            int n = removeFromSysIndexes(pns.c_str(), name.c_str());
            wassert( n == 1 );
            
        } catch ( DBException &e ) {
            log() << "exception in kill_idx: " << e << ", ns: " << ns << endl;
        }
    }
    
    void IndexDetails::getKeysFromObject( const BSONObj& obj, BSONObjSetDefaultOrder& keys) const {
        getSpec().getKeys( obj, keys );
    }

    void setDifference(BSONObjSetDefaultOrder &l, BSONObjSetDefaultOrder &r, vector<BSONObj*> &diff) {
        BSONObjSetDefaultOrder::iterator i = l.begin();
        BSONObjSetDefaultOrder::iterator j = r.begin();
        while ( 1 ) {
            if ( i == l.end() )
                break;
            while ( j != r.end() && j->woCompare( *i ) < 0 )
                j++;
            if ( j == r.end() || i->woCompare(*j) != 0  ) {
                const BSONObj *jo = &*i;
                diff.push_back( (BSONObj *) jo );
            }
            i++;
        }
    }

    void getIndexChanges(vector<IndexChanges>& v, NamespaceDetails& d, BSONObj newObj, BSONObj oldObj, bool &changedId) { 
        int z = d.nIndexesBeingBuilt();
        v.resize(z);
        NamespaceDetails::IndexIterator i = d.ii();
        for( int i = 0; i < z; i++ ) {
            IndexDetails& idx = d.idx(i);
            BSONObj idxKey = idx.info.obj().getObjectField("key"); // eg { ts : 1 }
            IndexChanges& ch = v[i];
            idx.getKeysFromObject(oldObj, ch.oldkeys);
            idx.getKeysFromObject(newObj, ch.newkeys);
            if( ch.newkeys.size() > 1 ) 
                d.setIndexIsMultikey(i);
            setDifference(ch.oldkeys, ch.newkeys, ch.removed);
            setDifference(ch.newkeys, ch.oldkeys, ch.added);
            if ( ch.removed.size() > 0 && ch.added.size() > 0 && idx.isIdIndex() ) {
                changedId = true;
            }
        }
    }

    void dupCheck(vector<IndexChanges>& v, NamespaceDetails& d, DiskLoc curObjLoc) {
        int z = d.nIndexesBeingBuilt();
        for( int i = 0; i < z; i++ ) {
            IndexDetails& idx = d.idx(i);
            v[i].dupCheck(idx, curObjLoc);
        }
    }

    // should be { <something> : <simpletype[1|-1]>, .keyp.. } 
    static bool validKeyPattern(BSONObj kp) { 
        BSONObjIterator i(kp);
        while( i.moreWithEOO() ) { 
            BSONElement e = i.next();
            if( e.type() == Object || e.type() == Array ) 
                return false;
        }
        return true;
    }

    /* Prepare to build an index.  Does not actually build it (except for a special _id case).
       - We validate that the params are good
       - That the index does not already exist
       - Creates the source collection if it DNE

       example of 'io':
         { ns : 'test.foo', name : 'z', key : { z : 1 } }

       throws DBException

       @param sourceNS - source NS we are indexing
       @param sourceCollection - its details ptr
       @return true if ok to continue.  when false we stop/fail silently (index already exists)
    */
    bool prepareToBuildIndex(const BSONObj& io, bool god, string& sourceNS, NamespaceDetails *&sourceCollection, BSONObj& fixedIndexObject ) {
        sourceCollection = 0;

        // logical name of the index.  todo: get rid of the name, we don't need it!
        const char *name = io.getStringField("name"); 
        uassert(12523, "no index name specified", *name);

        // the collection for which we are building an index
        sourceNS = io.getStringField("ns");  
        uassert(10096, "invalid ns to index", sourceNS.find( '.' ) != string::npos);
        uassert(10097, "bad table to index name on add index attempt", 
            cc().database()->name == nsToDatabase(sourceNS.c_str()));


        BSONObj key = io.getObjectField("key");
        uassert(12524, "index key pattern too large", key.objsize() <= 2048);
        if( !validKeyPattern(key) ) {
            string s = string("bad index key pattern ") + key.toString();
            uasserted(10098 , s.c_str());
        }

        if ( sourceNS.empty() || key.isEmpty() ) {
            log(2) << "bad add index attempt name:" << (name?name:"") << "\n  ns:" <<
                sourceNS << "\n  idxobj:" << io.toString() << endl;
            string s = "bad add index attempt " + sourceNS + " key:" + key.toString();
            uasserted(12504, s);
        }

        sourceCollection = nsdetails(sourceNS.c_str());
        if( sourceCollection == 0 ) {
            // try to create it
            string err;
            if ( !userCreateNS(sourceNS.c_str(), BSONObj(), err, false) ) {
                problem() << "ERROR: failed to create collection while adding its index. " << sourceNS << endl;
                return false;
            }
            sourceCollection = nsdetails(sourceNS.c_str());
            tlog() << "info: creating collection " << sourceNS << " on add index" << endl;
            assert( sourceCollection );
        }

        if ( sourceCollection->findIndexByName(name) >= 0 ) {
            // index already exists.
            return false;
        }
        if( sourceCollection->findIndexByKeyPattern(key) >= 0 ) {
            log(2) << "index already exists with diff name " << name << ' ' << key.toString() << endl;
            return false;
        }

        if ( sourceCollection->nIndexes >= NamespaceDetails::NIndexesMax ) {
            stringstream ss;
            ss << "add index fails, too many indexes for " << sourceNS << " key:" << key.toString();
            string s = ss.str();
            log() << s << '\n';
            uasserted(12505,s);
        }

        /* we can't build a new index for the ns if a build is already in progress in the background - 
           EVEN IF this is a foreground build.
           */
        uassert(12588, "cannot add index with a background operation in progress", 
            !BackgroundOperation::inProgForNs(sourceNS.c_str()));

        /* this is because we want key patterns like { _id : 1 } and { _id : <someobjid> } to 
           all be treated as the same pattern.
        */
        if ( IndexDetails::isIdIndexPattern(key) ) {
            if( !god ) {
                ensureHaveIdIndex( sourceNS.c_str() );
                return false;
            }
        }
        else { 
            /* is buildIndexes:false set for this replica set member? 
               if so we don't build any indexes except _id
            */
            if( theReplSet && !theReplSet->buildIndexes() ) 
                return false;
        }
        
        string pluginName = IndexPlugin::findPluginName( key );
        IndexPlugin * plugin = pluginName.size() ? IndexPlugin::get( pluginName ) : 0;
        
        if ( plugin ){
            fixedIndexObject = plugin->adjustIndexSpec( io );
        }
        else if ( io["v"].eoo() ) { 
            // add "v" if it doesn't exist
            // if it does - leave whatever value was there
            // this is for testing and replication
            BSONObjBuilder b( io.objsize() + 32 );
            b.appendElements( io );
            b.append( "v" , 0 );
            fixedIndexObject = b.obj();
        }

        return true;
    }


    void IndexSpec::reset( const IndexDetails * details ){
        _details = details;
        reset( details->info );
    }

    void IndexSpec::reset( const DiskLoc& loc ){
        info = loc.obj();
        keyPattern = info["key"].embeddedObjectUserCheck();
        if ( keyPattern.objsize() == 0 ) {
            out() << info.toString() << endl;
            assert(false);
        }
        _init();
    }

}
