// btreecursor.cpp

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
#include "btree.h"
#include "pdfile.h"
#include "jsobj.h"
#include "curop-inl.h"

namespace mongo {

    extern int otherTraceLevel;

    BtreeCursor::BtreeCursor( NamespaceDetails *_d, int _idxNo, const IndexDetails &_id, 
                              const BSONObj &_startKey, const BSONObj &_endKey, bool endKeyInclusive, int _direction ) :
            d(_d), idxNo(_idxNo), 
            startKey( _startKey ),
            endKey( _endKey ),
            _endKeyInclusive( endKeyInclusive ),
            _multikey( d->isMultikey( idxNo ) ),
            indexDetails( _id ),
            _order( _id.keyPattern() ),
            _ordering( Ordering::make( _order ) ),
            _direction( _direction ),
            _spec( _id.getSpec() ),
            _independentFieldRanges( false ),
            _nscanned( 0 )
    {
        audit();
        init();
        dassert( _dups.size() == 0 );
    }

    BtreeCursor::BtreeCursor( NamespaceDetails *_d, int _idxNo, const IndexDetails& _id, const shared_ptr< FieldRangeVector > &_bounds, int _direction )
        :
            d(_d), idxNo(_idxNo), 
            _endKeyInclusive( true ),
            _multikey( d->isMultikey( idxNo ) ),
            indexDetails( _id ),
            _order( _id.keyPattern() ),
            _ordering( Ordering::make( _order ) ),
            _direction( _direction ),
            _bounds( ( assert( _bounds.get() ), _bounds ) ),
            _boundsIterator( new FieldRangeVector::Iterator( *_bounds  ) ),
            _spec( _id.getSpec() ),
            _independentFieldRanges( true ),
            _nscanned( 0 )
    {
        massert( 13384, "BtreeCursor FieldRangeVector constructor doesn't accept special indexes", !_spec.getType() );
        audit();
        startKey = _bounds->startKey();
        _boundsIterator->advance( startKey ); // handles initialization
        _boundsIterator->prepDive();
        pair< DiskLoc, int > noBestParent;
        bucket = indexDetails.head;
        keyOfs = 0;
        indexDetails.head.btree()->customLocate( bucket, keyOfs, startKey, 0, false, _boundsIterator->cmp(), _boundsIterator->inc(), _ordering, _direction, noBestParent );
        skipAndCheck();
        dassert( _dups.size() == 0 );
    }

    void BtreeCursor::audit() {
        dassert( d->idxNo((IndexDetails&) indexDetails) == idxNo );

        if ( otherTraceLevel >= 12 ) {
            if ( otherTraceLevel >= 200 ) {
                out() << "::BtreeCursor() qtl>200.  validating entire index." << endl;
                indexDetails.head.btree()->fullValidate(indexDetails.head, _order);
            }
            else {
                out() << "BTreeCursor(). dumping head bucket" << endl;
                indexDetails.head.btree()->dump();
            }
        }
    }

    void BtreeCursor::init() {
        if ( _spec.getType() ){
            startKey = _spec.getType()->fixKey( startKey );
            endKey = _spec.getType()->fixKey( endKey );
        }
        bool found;
        bucket = indexDetails.head.btree()->
            locate(indexDetails, indexDetails.head, startKey, _ordering, keyOfs, found, _direction > 0 ? minDiskLoc : maxDiskLoc, _direction);
        if ( ok() ) {
            _nscanned = 1;
        }        
        skipUnusedKeys( false );
        checkEnd();
    }
    
    void BtreeCursor::skipAndCheck() {
        skipUnusedKeys( true );
        while( 1 ) {
            if ( !skipOutOfRangeKeysAndCheckEnd() ) {
                break;
            }
            while( skipOutOfRangeKeysAndCheckEnd() );
            if ( !skipUnusedKeys( true ) ) {
                break;
            }
        }
    }
    
    bool BtreeCursor::skipOutOfRangeKeysAndCheckEnd() {
        if ( !ok() ) {
            return false;
        }
        int ret = _boundsIterator->advance( currKeyNode().key );
        if ( ret == -2 ) {
            bucket = DiskLoc();
            return false;
        } else if ( ret == -1 ) {
            ++_nscanned;
            return false;
        }
        ++_nscanned;
        advanceTo( currKeyNode().key, ret, _boundsIterator->after(), _boundsIterator->cmp(), _boundsIterator->inc() );
        return true;
    }
    
    /* skip unused keys. */
    bool BtreeCursor::skipUnusedKeys( bool mayJump ) {
        int u = 0;
        while ( 1 ) {
            if ( !ok() )
                break;
            const BtreeBucket *b = bucket.btree();
            const _KeyNode& kn = b->k(keyOfs);
            if ( kn.isUsed() )
                break;
            bucket = b->advance(bucket, keyOfs, _direction, "skipUnusedKeys");
            u++;
            //don't include unused keys in nscanned
            //++_nscanned;
            if ( mayJump && ( u % 10 == 0 ) ) {
                skipOutOfRangeKeysAndCheckEnd();
            }
        }
        if ( u > 10 )
            OCCASIONALLY log() << "btree unused skipped:" << u << '\n';
        return u;
    }

    // Return a value in the set {-1, 0, 1} to represent the sign of parameter i.
    int sgn( int i ) {
        if ( i == 0 )
            return 0;
        return i > 0 ? 1 : -1;
    }

    // Check if the current key is beyond endKey.
    void BtreeCursor::checkEnd() {
        if ( bucket.isNull() )
            return;
        if ( !endKey.isEmpty() ) {
            int cmp = sgn( endKey.woCompare( currKey(), _order ) );
            if ( ( cmp != 0 && cmp != _direction ) ||
                ( cmp == 0 && !_endKeyInclusive ) )
                bucket = DiskLoc();
        }
    }
    
    void BtreeCursor::advanceTo( const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive) {
        bucket.btree()->advanceTo( bucket, keyOfs, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, _ordering, _direction );
    }
    
    bool BtreeCursor::advance() {
        killCurrentOp.checkForInterrupt();
        if ( bucket.isNull() )
            return false;

        bucket = bucket.btree()->advance(bucket, keyOfs, _direction, "BtreeCursor::advance");
        
        if ( !_independentFieldRanges ) {
            skipUnusedKeys( false );
            checkEnd();
            if ( ok() ) {
                ++_nscanned;
            }
        } else {
            skipAndCheck();
        }
        return ok();
    }

    void BtreeCursor::noteLocation() {
        if ( !eof() ) {
            BSONObj o = bucket.btree()->keyAt(keyOfs).copy();
            keyAtKeyOfs = o;
            locAtKeyOfs = bucket.btree()->k(keyOfs).recordLoc;
        }
    }

    /* Since the last noteLocation(), our key may have moved around, and that old cached
       information may thus be stale and wrong (although often it is right).  We check
       that here; if we have moved, we have to search back for where we were at.

       i.e., after operations on the index, the BtreeCursor's cached location info may
       be invalid.  This function ensures validity, so you should call it before using
       the cursor if other writers have used the database since the last noteLocation
       call.
    */
    void BtreeCursor::checkLocation() {
        if ( eof() )
            return;

        _multikey = d->isMultikey(idxNo);

        if ( keyOfs >= 0 ) {
            const BtreeBucket *b = bucket.btree();

            assert( !keyAtKeyOfs.isEmpty() );

            // Note keyAt() returns an empty BSONObj if keyOfs is now out of range,
            // which is possible as keys may have been deleted.
            int x = 0;
            while( 1 ) {
                if ( b->keyAt(keyOfs).woEqual(keyAtKeyOfs) &&
                    b->k(keyOfs).recordLoc == locAtKeyOfs ) {
                        if ( !b->k(keyOfs).isUsed() ) {
                            /* we were deleted but still exist as an unused
                            marker key. advance.
                            */
                            skipUnusedKeys( false );
                        }
                        return;
                }

                /* we check one key earlier too, in case a key was just deleted.  this is 
                   important so that multi updates are reasonably fast.
                   */
                if( keyOfs == 0 || x++ )
                    break;
                keyOfs--;
            }
        }

        /* normally we don't get to here.  when we do, old position is no longer
            valid and we must refind where we left off (which is expensive)
        */

        bool found;

        /* TODO: Switch to keep indexdetails and do idx.head! */
        bucket = indexDetails.head.btree()->locate(indexDetails, indexDetails.head, keyAtKeyOfs, _ordering, keyOfs, found, locAtKeyOfs, _direction);
        RARELY log() << "  key seems to have moved in the index, refinding. found:" << found << endl;
        if ( ! bucket.isNull() )
            skipUnusedKeys( false );

    }

    /* ----------------------------------------------------------------------------- */

    struct BtreeCursorUnitTest {
        BtreeCursorUnitTest() {
            assert( minDiskLoc.compare(maxDiskLoc) < 0 );
        }
    } btut;

} // namespace mongo
