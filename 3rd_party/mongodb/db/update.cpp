// update.cpp

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
#include "query.h"
#include "pdfile.h"
#include "jsobjmanipulator.h"
#include "queryoptimizer.h"
#include "repl.h"
#include "update.h"
#include "btree.h"

//#define DEBUGUPDATE(x) cout << x << endl;
#define DEBUGUPDATE(x)

namespace mongo {

    const char* Mod::modNames[] = { "$inc", "$set", "$push", "$pushAll", "$pull", "$pullAll" , "$pop", "$unset" ,
                                    "$bitand" , "$bitor" , "$bit" , "$addToSet", "$rename", "$rename" };
    unsigned Mod::modNamesNum = sizeof(Mod::modNames)/sizeof(char*);

    bool Mod::_pullElementMatch( BSONElement& toMatch ) const {
        
        if ( elt.type() != Object ){
            // if elt isn't an object, then comparison will work
            return toMatch.woCompare( elt , false ) == 0;
        }
        
        if ( matcherOnPrimitive )
            return matcher->matches( toMatch.wrap( "" ) );

        if ( toMatch.type() != Object ){
            // looking for an object, so this can't match
            return false;
        }
        
        // now we have an object on both sides
        return matcher->matches( toMatch.embeddedObject() );
    }

    template< class Builder >
    void Mod::appendIncremented( Builder& bb , const BSONElement& in, ModState& ms ) const {
        BSONType a = in.type();
        BSONType b = elt.type();
        
        if ( a == NumberDouble || b == NumberDouble ){
            ms.incType = NumberDouble;
            ms.incdouble = elt.numberDouble() + in.numberDouble();
        }
        else if ( a == NumberLong || b == NumberLong ){
            ms.incType = NumberLong;
            ms.inclong = elt.numberLong() + in.numberLong();
        }
        else {
            int x = elt.numberInt() + in.numberInt();
            if ( x < 0 && elt.numberInt() > 0 && in.numberInt() > 0 ){
                // overflow
                ms.incType = NumberLong;
                ms.inclong = elt.numberLong() + in.numberLong();
            }
            else {
                ms.incType = NumberInt;
                ms.incint = elt.numberInt() + in.numberInt();
            }
        }
        
        ms.appendIncValue( bb , false );
    }

    template< class Builder >
    void appendUnset( Builder &b ) {
    }
    
    template<>
    void appendUnset( BSONArrayBuilder &b ) {
        b.appendNull();
    }
    
    template< class Builder >
    void Mod::apply( Builder& b , BSONElement in , ModState& ms ) const {
        if ( ms.dontApply ) {
            return;
        }
        
        switch ( op ){
        
        case INC: {
            appendIncremented( b , in , ms );
            break;
        }
            
        case SET: {
            _checkForAppending( elt );
            b.appendAs( elt , shortFieldName );
            break;
        }

        case UNSET: {
            appendUnset( b );
            break;
        }
            
        case PUSH: {
            uassert( 10131 ,  "$push can only be applied to an array" , in.type() == Array );
            BSONObjBuilder bb( b.subarrayStart( shortFieldName ) );
            BSONObjIterator i( in.embeddedObject() );
            int n=0;
            while ( i.more() ){
                bb.append( i.next() );
                n++;
            }

            ms.pushStartSize = n;

            bb.appendAs( elt ,  bb.numStr( n ) );
            bb.done();
            break;
        }
            
        case ADDTOSET: {
            uassert( 12592 ,  "$addToSet can only be applied to an array" , in.type() == Array );
            BSONObjBuilder bb( b.subarrayStart( shortFieldName ) );
            
            BSONObjIterator i( in.embeddedObject() );
            int n=0;            

            if ( isEach() ){
                
                BSONElementSet toadd;
                parseEach( toadd );
                
                while ( i.more() ){
                    BSONElement cur = i.next();
                    bb.append( cur );
                    n++;           
                    toadd.erase( cur );
                }
                
                {
                    BSONObjIterator i( getEach() );
                    while ( i.more() ){
                        BSONElement e = i.next();
                        if ( toadd.count(e) ){
                            bb.appendAs( e , BSONObjBuilder::numStr( n++ ) );
                            toadd.erase( e );
                        }
                    }
                }

            }
            else {

                bool found = false;

                while ( i.more() ){
                    BSONElement cur = i.next();
                    bb.append( cur );
                    n++;
                    if ( elt.woCompare( cur , false ) == 0 )
                        found = true;
                }
                
                if ( ! found )
                    bb.appendAs( elt ,  bb.numStr( n ) );
                
            }
            
            bb.done();
            break;
        }


            
        case PUSH_ALL: {
            uassert( 10132 ,  "$pushAll can only be applied to an array" , in.type() == Array );
            uassert( 10133 ,  "$pushAll has to be passed an array" , elt.type() );

            BSONObjBuilder bb( b.subarrayStart( shortFieldName ) );
            
            BSONObjIterator i( in.embeddedObject() );
            int n=0;
            while ( i.more() ){
                bb.append( i.next() );
                n++;
            }

            ms.pushStartSize = n;

            i = BSONObjIterator( elt.embeddedObject() );
            while ( i.more() ){
                bb.appendAs( i.next() , bb.numStr( n++ ) );
            }

            bb.done();
            break;
        }
            
        case PULL:
        case PULL_ALL: {
            uassert( 10134 ,  "$pull/$pullAll can only be applied to an array" , in.type() == Array );
            BSONObjBuilder bb( b.subarrayStart( shortFieldName ) );
                        
            int n = 0;

            BSONObjIterator i( in.embeddedObject() );
            while ( i.more() ){
                BSONElement e = i.next();
                bool allowed = true;

                if ( op == PULL ){
                    allowed = ! _pullElementMatch( e );
                }
                else {
                    BSONObjIterator j( elt.embeddedObject() );
                    while( j.more() ) {
                        BSONElement arrJ = j.next();
                        if ( e.woCompare( arrJ, false ) == 0 ){
                            allowed = false;
                            break;
                        }
                    }
                }

                if ( allowed )
                    bb.appendAs( e , bb.numStr( n++ ) );
            }
            
            bb.done();
            break;
        }

        case POP: {
            uassert( 10135 ,  "$pop can only be applied to an array" , in.type() == Array );
            BSONObjBuilder bb( b.subarrayStart( shortFieldName ) );
                        
            int n = 0;

            BSONObjIterator i( in.embeddedObject() );
            if ( elt.isNumber() && elt.number() < 0 ){
                // pop from front
                if ( i.more() ){
                    i.next();
                    n++;
                }

                while( i.more() ) {
                    bb.appendAs( i.next() , bb.numStr( n - 1 ) );
                    n++;
                }
            }
            else {
                // pop from back
                while( i.more() ) {
                    n++;
                    BSONElement arrI = i.next();
                    if ( i.more() ){
                        bb.append( arrI );
                    }
                }
            }

            ms.pushStartSize = n;
            assert( ms.pushStartSize == in.embeddedObject().nFields() );
            bb.done();
            break;
        }

        case BIT: {
            uassert( 10136 ,  "$bit needs an array" , elt.type() == Object );
            uassert( 10137 ,  "$bit can only be applied to numbers" , in.isNumber() );
            uassert( 10138 ,  "$bit can't use a double" , in.type() != NumberDouble );
            
            int x = in.numberInt();
            long long y = in.numberLong();

            BSONObjIterator it( elt.embeddedObject() );
            while ( it.more() ){
                BSONElement e = it.next();
                uassert( 10139 ,  "$bit field must be number" , e.isNumber() );
                if ( strcmp( e.fieldName() , "and" ) == 0 ){
                    switch( in.type() ){
                    case NumberInt: x = x&e.numberInt(); break;
                    case NumberLong: y = y&e.numberLong(); break;
                    default: assert( 0 );
                    }
                }
                else if ( strcmp( e.fieldName() , "or" ) == 0 ){
                    switch( in.type() ){
                    case NumberInt: x = x|e.numberInt(); break;
                    case NumberLong: y = y|e.numberLong(); break;
                    default: assert( 0 );
                    }
                }

                else {
                    throw UserException( 9016, (string)"unknown bit mod:" + e.fieldName() );
                }
            }
            
            switch( in.type() ){
            case NumberInt: b.append( shortFieldName , x ); break;
            case NumberLong: b.append( shortFieldName , y ); break;
            default: assert( 0 );
            }

            break;
        }

        case RENAME_FROM: {
            break;
        }

        case RENAME_TO: {
            ms.handleRename( b, shortFieldName );
            break;
        }
                
        default:
            stringstream ss;
            ss << "Mod::apply can't handle type: " << op;
            throw UserException( 9017, ss.str() );
        }
    }

    // -1 inside a non-object (non-object could be array)
    // 0 missing
    // 1 found
    int validRenamePath( BSONObj obj, const char *path ) {
        while( const char *p = strchr( path, '.' ) ) {
            string left( path, p - path );
            BSONElement e = obj.getField( left );
            if ( e.eoo() ) {
                return 0;
            }
            if ( e.type() != Object ) {
                return -1;
            }
            obj = e.embeddedObject();
            path = p + 1;
        }
        return !obj.getField( path ).eoo();
    }
    
    auto_ptr<ModSetState> ModSet::prepare(const BSONObj &obj) const {
        DEBUGUPDATE( "\t start prepare" );
        ModSetState * mss = new ModSetState( obj );
        
        
        // Perform this check first, so that we don't leave a partially modified object on uassert.
        for ( ModHolder::const_iterator i = _mods.begin(); i != _mods.end(); ++i ) {
            DEBUGUPDATE( "\t\t prepare : " << i->first );
            ModState& ms = mss->_mods[i->first];

            const Mod& m = i->second;
            BSONElement e = obj.getFieldDotted(m.fieldName);
            
            ms.m = &m;
            ms.old = e;

            if ( m.op == Mod::RENAME_FROM ) {
                int source = validRenamePath( obj, m.fieldName );
                uassert( 13489, "$rename source field invalid", source != -1 );
                if ( source != 1 ) {
                    ms.dontApply = true;
                }
                continue;
            }
            
            if ( m.op == Mod::RENAME_TO ) {
                int source = validRenamePath( obj, m.renameFrom() );
                if ( source == 1 ) {
                    int target = validRenamePath( obj, m.fieldName );
                    uassert( 13490, "$rename target field invalid", target != -1 );
                    ms.newVal = obj.getFieldDotted( m.renameFrom() );
                    mss->amIInPlacePossible( false );
                } else {
                    ms.dontApply = true;
                }
                continue;                
            }
            
            if ( e.eoo() ) {
                mss->amIInPlacePossible( m.op == Mod::UNSET );
                continue;
            } 
            
            switch( m.op ) {
            case Mod::INC:
                uassert( 10140 ,  "Cannot apply $inc modifier to non-number", e.isNumber() || e.eoo() );
                if ( mss->amIInPlacePossible( e.isNumber() ) ){
                    // check more typing info here
                    if ( m.elt.type() != e.type() ){
                        // if i'm incrememnting with a double, then the storage has to be a double
                        mss->amIInPlacePossible( m.elt.type() != NumberDouble ); 
                    }
                    
                    // check for overflow
                    if ( e.type() == NumberInt && e.numberLong() + m.elt.numberLong() > numeric_limits<int>::max() ){
                        mss->amIInPlacePossible( false );
                    }
                }
                break;

            case Mod::SET:
                mss->amIInPlacePossible( m.elt.type() == e.type() &&
                                         m.elt.valuesize() == e.valuesize() );
                break;
            
            case Mod::PUSH:
            case Mod::PUSH_ALL:
                uassert( 10141 ,  "Cannot apply $push/$pushAll modifier to non-array", e.type() == Array || e.eoo() );
                mss->amIInPlacePossible( false );
                break;

            case Mod::PULL:
            case Mod::PULL_ALL: {
                uassert( 10142 ,  "Cannot apply $pull/$pullAll modifier to non-array", e.type() == Array || e.eoo() );
                BSONObjIterator i( e.embeddedObject() );
                while( mss->_inPlacePossible && i.more() ) {
                    BSONElement arrI = i.next();
                    if ( m.op == Mod::PULL ) {
                        mss->amIInPlacePossible( ! m._pullElementMatch( arrI ) );
                    } 
                    else if ( m.op == Mod::PULL_ALL ) {
                        BSONObjIterator j( m.elt.embeddedObject() );
                        while( mss->_inPlacePossible && j.moreWithEOO() ) {
                            BSONElement arrJ = j.next();
                            if ( arrJ.eoo() )
                                break;
                            mss->amIInPlacePossible( arrI.woCompare( arrJ, false ) );
                        }
                    }
                }
                break;
            }

            case Mod::POP: {
                uassert( 10143 ,  "Cannot apply $pop modifier to non-array", e.type() == Array || e.eoo() );
                mss->amIInPlacePossible( e.embeddedObject().isEmpty() );
                break;
            }
                
            case Mod::ADDTOSET: {
                uassert( 12591 ,  "Cannot apply $addToSet modifier to non-array", e.type() == Array || e.eoo() );
                
                BSONObjIterator i( e.embeddedObject() );
                if ( m.isEach() ){
                    BSONElementSet toadd;
                    m.parseEach( toadd );
                    while( i.more() ) {
                        BSONElement arrI = i.next();
                        toadd.erase( arrI );
                    }
                    mss->amIInPlacePossible( toadd.size() == 0 );
                }
                else {
                    bool found = false;
                    while( i.more() ) {
                        BSONElement arrI = i.next();
                        if ( arrI.woCompare( m.elt , false ) == 0 ){
                            found = true;
                            break;
                        }
                    }
                    mss->amIInPlacePossible( found );
                }
                break;
            }
                
            default:
                // mods we don't know about shouldn't be done in place
                mss->amIInPlacePossible( false );
            }
        }

        DEBUGUPDATE( "\t mss\n" << mss->toString() << "\t--" );
        
        return auto_ptr<ModSetState>( mss );
    }

    void ModState::appendForOpLog( BSONObjBuilder& b ) const {
        if ( dontApply ) {
            return;
        }
        
        if ( incType ){
            DEBUGUPDATE( "\t\t\t\t\t appendForOpLog inc fieldname: " << m->fieldName << " short:" << m->shortFieldName );
            BSONObjBuilder bb( b.subobjStart( "$set" ) );
            appendIncValue( bb , true );
            bb.done();
            return;
        }

        if ( m->op == Mod::RENAME_FROM ) {
            DEBUGUPDATE( "\t\t\t\t\t appendForOpLog RENAME_FROM fielName:" << m->fieldName );
            BSONObjBuilder bb( b.subobjStart( "$unset" ) );
            bb.append( m->fieldName, 1 );
            bb.done();
            return;
        }
        
        if ( m->op == Mod::RENAME_TO ) {
            DEBUGUPDATE( "\t\t\t\t\t appendForOpLog RENAME_TO fielName:" << m->fieldName );
            BSONObjBuilder bb( b.subobjStart( "$set" ) );
            bb.appendAs( newVal, m->fieldName );
            return;
        }
        
        const char * name = fixedOpName ? fixedOpName : Mod::modNames[op()];

        DEBUGUPDATE( "\t\t\t\t\t appendForOpLog name:" << name << " fixed: " << fixed << " fn: " << m->fieldName );

        BSONObjBuilder bb( b.subobjStart( name ) );
        if ( fixed ) {
            bb.appendAs( *fixed , m->fieldName );
        } else {
            bb.appendAs( m->elt , m->fieldName );
        }
        bb.done();
    }

    string ModState::toString() const {
        stringstream ss;
        if ( fixedOpName )
            ss << " fixedOpName: " << fixedOpName;
        if ( fixed )
            ss << " fixed: " << fixed;
        return ss.str();
    }
    
    template< class Builder >
    void ModState::handleRename( Builder &newObjBuilder, const char *shortFieldName ) {
        newObjBuilder.appendAs( newVal , shortFieldName );
        BSONObjBuilder b;
        b.appendAs( newVal, shortFieldName );
        assert( _objData.isEmpty() );
        _objData = b.obj();
        newVal = _objData.firstElement();            
    }    

    void ModSetState::applyModsInPlace( bool isOnDisk ) {
        // TODO i think this assert means that we can get rid of the isOnDisk param
        //      and just use isOwned as the determination
        DEV assert( isOnDisk == ! _obj.isOwned() );

        for ( ModStateHolder::iterator i = _mods.begin(); i != _mods.end(); ++i ) {
            ModState& m = i->second;  
            
            if ( m.dontApply ) {
                continue;
            }
            
            switch ( m.m->op ){
            case Mod::UNSET:
            case Mod::PULL:
            case Mod::PULL_ALL:
            case Mod::ADDTOSET:
            case Mod::RENAME_FROM:
            case Mod::RENAME_TO:
                // this should have been handled by prepare
                break;
            // [dm] the BSONElementManipulator statements below are for replication (correct?)
            case Mod::INC:
                if ( isOnDisk )
                    m.m->IncrementMe( m.old );
                else
                    m.m->incrementMe( m.old );
                m.fixedOpName = "$set";
                m.fixed = &(m.old);
                break;
            case Mod::SET:
                if ( isOnDisk )
                    BSONElementManipulator( m.old ).ReplaceTypeAndValue( m.m->elt );
                else
                    BSONElementManipulator( m.old ).replaceTypeAndValue( m.m->elt );
                break;
            default:
                uassert( 13478 ,  "can't apply mod in place - shouldn't have gotten here" , 0 );
            }
        }
    }

    void ModSet::extractFields( map< string, BSONElement > &fields, const BSONElement &top, const string &base ) {
        if ( top.type() != Object ) {
            fields[ base + top.fieldName() ] = top;
            return;
        }
        BSONObjIterator i( top.embeddedObject() );
        bool empty = true;
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            extractFields( fields, e, base + top.fieldName() + "." );
            empty = false;
        }
        if ( empty )
            fields[ base + top.fieldName() ] = top;            
    }
    
    template< class Builder >
    void ModSetState::_appendNewFromMods( const string& root , ModState& m , Builder& b , set<string>& onedownseen ){
        const char * temp = m.fieldName();
        temp += root.size();
        const char * dot = strchr( temp , '.' );
        if ( dot ){
            string nr( m.fieldName() , 0 , 1 + ( dot - m.fieldName() ) );
            string nf( temp , 0 , dot - temp );
            if ( onedownseen.count( nf ) )
                return;
            onedownseen.insert( nf );
            BSONObjBuilder bb ( b.subobjStart( nf ) );
            createNewFromMods( nr , bb , BSONObj() ); // don't infer an array from name
            bb.done();
        }
        else {
            appendNewFromMod( m , b );
        }
        
    }
    
    template< class Builder >
    void ModSetState::createNewFromMods( const string& root , Builder& b , const BSONObj &obj ){
        DEBUGUPDATE( "\t\t createNewFromMods root: " << root );
        BSONObjIteratorSorted es( obj );
        BSONElement e = es.next();
        
        ModStateHolder::iterator m = _mods.lower_bound( root );
        StringBuilder buf(root.size() + 2 );
        buf << root << (char)255;
        ModStateHolder::iterator mend = _mods.lower_bound( buf.str() );
        
        set<string> onedownseen;
        
        while ( e.type() && m != mend ){
            string field = root + e.fieldName();
            FieldCompareResult cmp = compareDottedFieldNames( m->second.m->fieldName , field );

            DEBUGUPDATE( "\t\t\t field:" << field << "\t mod:" << m->second.m->fieldName << "\t cmp:" << cmp << "\t short: " << e.fieldName() );
            
            switch ( cmp ){
                
            case LEFT_SUBFIELD: { // Mod is embeddeed under this element
                uassert( 10145 ,  str::stream() << "LEFT_SUBFIELD only supports Object: " << field << " not: " << e.type() , e.type() == Object || e.type() == Array );
                if ( onedownseen.count( e.fieldName() ) == 0 ){
                    onedownseen.insert( e.fieldName() );
                    if ( e.type() == Object ) {
                        BSONObjBuilder bb( b.subobjStart( e.fieldName() ) );
                        stringstream nr; nr << root << e.fieldName() << ".";
                        createNewFromMods( nr.str() , bb , e.embeddedObject() );
                        bb.done();                        
                    } else {
                        BSONArrayBuilder ba( b.subarrayStart( e.fieldName() ) );
                        stringstream nr; nr << root << e.fieldName() << ".";
                        createNewFromMods( nr.str() , ba , e.embeddedObject() );
                        ba.done();
                    }
                    // inc both as we handled both
                    e = es.next();
                    m++;
                }
                else {
                    // this is a very weird case
                    // have seen it in production, but can't reproduce
                    // this assert prevents an inf. loop
                    // but likely isn't the correct solution
                    assert(0);
                }
                continue;
            }
            case LEFT_BEFORE: // Mod on a field that doesn't exist
                DEBUGUPDATE( "\t\t\t\t creating new field for: " << m->second.m->fieldName );
                _appendNewFromMods( root , m->second , b , onedownseen );
                m++;
                continue;
            case SAME:
                DEBUGUPDATE( "\t\t\t\t applying mod on: " << m->second.m->fieldName );
                m->second.apply( b , e );
                e = es.next();
                m++;
                continue;
            case RIGHT_BEFORE: // field that doesn't have a MOD
                DEBUGUPDATE( "\t\t\t\t just copying" );
                b.append( e ); // if array, ignore field name
                e = es.next();
                continue;
            case RIGHT_SUBFIELD:
                massert( 10399 ,  "ModSet::createNewFromMods - RIGHT_SUBFIELD should be impossible" , 0 ); 
                break;
            default:
                massert( 10400 ,  "unhandled case" , 0 );
            }
        }
        
        // finished looping the mods, just adding the rest of the elements
        while ( e.type() ){
            DEBUGUPDATE( "\t\t\t copying: " << e.fieldName() );
            b.append( e );  // if array, ignore field name
            e = es.next();
        }
        
        // do mods that don't have fields already
        for ( ; m != mend; m++ ){
            DEBUGUPDATE( "\t\t\t\t appending from mod at end: " << m->second.m->fieldName );
            _appendNewFromMods( root , m->second , b , onedownseen );
        }
    }

    BSONObj ModSetState::createNewFromMods() {
        BSONObjBuilder b( (int)(_obj.objsize() * 1.1) );
        createNewFromMods( "" , b , _obj );
        return _newFromMods = b.obj();
    }

    string ModSetState::toString() const {
        stringstream ss;
        for ( ModStateHolder::const_iterator i=_mods.begin(); i!=_mods.end(); ++i ){
            ss << "\t\t" << i->first << "\t" << i->second.toString() << "\n";
        }
        return ss.str();
    }

    BSONObj ModSet::createNewFromQuery( const BSONObj& query ){
        BSONObj newObj;

        {
            BSONObjBuilder bb;
            EmbeddedBuilder eb( &bb );
            BSONObjIteratorSorted i( query );
            while ( i.more() ){
                BSONElement e = i.next();
                if ( e.fieldName()[0] == '$' ) // for $atomic and anything else we add
                    continue;

                if ( e.type() == Object && e.embeddedObject().firstElement().fieldName()[0] == '$' ){
                    // this means this is a $gt type filter, so don't make part of the new object
                    continue;
                }

                eb.appendAs( e , e.fieldName() );
            }
            eb.done();
            newObj = bb.obj();
        }
        
        auto_ptr<ModSetState> mss = prepare( newObj );

        if ( mss->canApplyInPlace() )
            mss->applyModsInPlace( false );
        else
            newObj = mss->createNewFromMods();
        
        return newObj;
    }
    
    /* get special operations like $inc
       { $inc: { a:1, b:1 } }
       { $set: { a:77 } }
       { $push: { a:55 } }
       { $pushAll: { a:[77,88] } }
       { $pull: { a:66 } }
       { $pullAll : { a:[99,1010] } }
       NOTE: MODIFIES source from object!
    */
    ModSet::ModSet(
        const BSONObj &from , 
        const set<string>& idxKeys,
        const set<string> *backgroundKeys)
        : _isIndexed(0) , _hasDynamicArray( false ) {
        
        BSONObjIterator it(from);
        
        while ( it.more() ) {
            BSONElement e = it.next();
            const char *fn = e.fieldName();
            
            uassert( 10147 ,  "Invalid modifier specified" + string( fn ), e.type() == Object );
            BSONObj j = e.embeddedObject();
            DEBUGUPDATE( "\t" << j );
            
            BSONObjIterator jt(j);
            Mod::Op op = opFromStr( fn );

            while ( jt.more() ) {
                BSONElement f = jt.next(); // x:44

                const char * fieldName = f.fieldName();

                uassert( 10148 ,  "Mod on _id not allowed", strcmp( fieldName, "_id" ) != 0 );
                uassert( 10149 ,  "Invalid mod field name, may not end in a period", fieldName[ strlen( fieldName ) - 1 ] != '.' );
                uassert( 10150 ,  "Field name duplication not allowed with modifiers", ! haveModForField( fieldName ) );
                uassert( 10151 ,  "have conflicting mods in update" , ! haveConflictingMod( fieldName ) );
                uassert( 10152 ,  "Modifier $inc allowed for numbers only", f.isNumber() || op != Mod::INC );
                uassert( 10153 ,  "Modifier $pushAll/pullAll allowed for arrays only", f.type() == Array || ( op != Mod::PUSH_ALL && op != Mod::PULL_ALL ) );
                
                if ( op == Mod::RENAME_TO ) {
                    uassert( 13494, "$rename target must be a string", f.type() == String );
                    const char *target = f.valuestr();
                    uassert( 13495, "$rename source must differ from target", strcmp( fieldName, target ) != 0 );
                    uassert( 13496, "invalid mod field name, source may not be empty", fieldName[0] );
                    uassert( 13479, "invalid mod field name, target may not be empty", target[0] );
                    uassert( 13480, "invalid mod field name, source may not begin or end in period", fieldName[0] != '.' && fieldName[ strlen( fieldName ) - 1 ] != '.' );
                    uassert( 13481, "invalid mod field name, target may not begin or end in period", target[0] != '.' && target[ strlen( target ) - 1 ] != '.' );
                    uassert( 13482, "$rename affecting _id not allowed", !( fieldName[0] == '_' && fieldName[1] == 'i' && fieldName[2] == 'd' && ( !fieldName[3] || fieldName[3] == '.' ) ) );
                    uassert( 13483, "$rename affecting _id not allowed", !( target[0] == '_' && target[1] == 'i' && target[2] == 'd' && ( !target[3] || target[3] == '.' ) ) );
                    uassert( 13484, "field name duplication not allowed with $rename target", !haveModForField( target ) );
                    uassert( 13485, "conflicting mods not allowed with $rename target", !haveConflictingMod( target ) );
                    uassert( 13486, "$rename target may not be a parent of source", !( strncmp( fieldName, target, strlen( target ) ) == 0 && fieldName[ strlen( target ) ] == '.' ) );
                    uassert( 13487, "$rename source may not be dynamic array", strstr( fieldName , ".$" ) == 0 );
                    uassert( 13488, "$rename target may not be dynamic array", strstr( target , ".$" ) == 0 );

                    Mod from;
                    from.init( Mod::RENAME_FROM, f );
                    from.setFieldName( fieldName );
                    updateIsIndexed( from, idxKeys, backgroundKeys );
                    _mods[ from.fieldName ] = from;

                    Mod to;
                    to.init( Mod::RENAME_TO, f );
                    to.setFieldName( target );
                    updateIsIndexed( to, idxKeys, backgroundKeys );
                    _mods[ to.fieldName ] = to;
                    
                    DEBUGUPDATE( "\t\t " << fieldName << "\t" << from.fieldName << "\t" << to.fieldName );
                    continue;
                }
                
                _hasDynamicArray = _hasDynamicArray || strstr( fieldName , ".$" ) > 0;
                
                Mod m;
                m.init( op , f );
                m.setFieldName( f.fieldName() );
                updateIsIndexed( m, idxKeys, backgroundKeys );
                _mods[m.fieldName] = m;

                DEBUGUPDATE( "\t\t " << fieldName << "\t" << m.fieldName << "\t" << _hasDynamicArray );
            }
        }

    }

    ModSet * ModSet::fixDynamicArray( const char * elemMatchKey ) const {
        ModSet * n = new ModSet();
        n->_isIndexed = _isIndexed;
        n->_hasDynamicArray = _hasDynamicArray;
        for ( ModHolder::const_iterator i=_mods.begin(); i!=_mods.end(); i++ ){
            string s = i->first;
            size_t idx = s.find( ".$" );
            if ( idx == string::npos ){
                n->_mods[s] = i->second;
                continue;
            }
            StringBuilder buf(s.size()+strlen(elemMatchKey));
            buf << s.substr(0,idx+1) << elemMatchKey << s.substr(idx+2);
            string fixed = buf.str();
            DEBUGUPDATE( "fixed dynamic: " << s << " -->> " << fixed );
            n->_mods[fixed] = i->second;
            ModHolder::iterator temp = n->_mods.find( fixed );
            temp->second.setFieldName( temp->first.c_str() );
        }
        return n;
    }
    
    void checkNoMods( BSONObj o ) {
        BSONObjIterator i( o );
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            uassert( 10154 ,  "Modifiers and non-modifiers cannot be mixed", e.fieldName()[ 0 ] != '$' );
        }
    }
    
    class UpdateOp : public MultiCursor::CursorOp {
    public:
        UpdateOp( bool hasPositionalField ) : _nscanned(), _hasPositionalField( hasPositionalField ){}
        virtual void _init() {
            _c = qp().newCursor();
            if ( ! _c->ok() ) {
                setComplete();
            }
        }
        virtual bool prepareToYield() {
            if ( ! _cc ) {
                _cc.reset( new ClientCursor( QueryOption_NoCursorTimeout , _c , qp().ns() ) );
            }
            return _cc->prepareToYield( _yieldData );
        }        
        virtual void recoverFromYield() {
            if ( !ClientCursor::recoverFromYield( _yieldData ) ) {
                _c.reset();
                _cc.reset();
                massert( 13339, "cursor dropped during update", false );
            }
        }     
        virtual long long nscanned() {
            assert( _c.get() );
            return _c->nscanned();
        }
        virtual void next() {
            if ( ! _c->ok() ) {
                setComplete();
                return;
            }
            _nscanned++;
            if ( matcher()->matches(_c->currKey(), _c->currLoc(), &_details ) ) {
                setComplete();
                return;
            }
            _c->advance();
        }

        virtual bool mayRecordPlan() const { return false; }
        virtual QueryOp *_createChild() const {
            return new UpdateOp( _hasPositionalField );
        }
        // already scanned to the first match, so return _c
        virtual shared_ptr< Cursor > newCursor() const { return _c; }
        virtual bool alwaysUseRecord() const { return _hasPositionalField; }
    private:
        shared_ptr< Cursor > _c;
        long long _nscanned;
        bool _hasPositionalField;
        MatchDetails _details;
        ClientCursor::CleanupPointer _cc;
        ClientCursor::YieldData _yieldData;
    };

    static void checkTooLarge(const BSONObj& newObj) {
        uassert( 12522 , "$ operator made object too large" , newObj.objsize() <= BSONObjMaxUserSize );
    }

    /* note: this is only (as-is) called for 

             - not multi
             - not mods is indexed
             - not upsert
    */
    static UpdateResult _updateById(bool isOperatorUpdate, int idIdxNo, ModSet *mods, int profile, NamespaceDetails *d, 
                                    NamespaceDetailsTransient *nsdt,
                                    bool god, const char *ns, 
                                    const BSONObj& updateobj, BSONObj patternOrig, bool logop, OpDebug& debug) 
    {
        DiskLoc loc;
        {
            IndexDetails& i = d->idx(idIdxNo);
            BSONObj key = i.getKeyFromQuery( patternOrig );
            loc = i.head.btree()->findSingle(i, i.head, key);
            if( loc.isNull() ) { 
                // no upsert support in _updateById yet, so we are done.
                return UpdateResult(0, 0, 0);
            }
        }

        Record *r = loc.rec();
                
        /* look for $inc etc.  note as listed here, all fields to inc must be this type, you can't set some
           regular ones at the moment. */
        if ( isOperatorUpdate ) {                   
            const BSONObj& onDisk = loc.obj();                    
            auto_ptr<ModSetState> mss = mods->prepare( onDisk );
                    
            if( mss->canApplyInPlace() ) {
                mss->applyModsInPlace(true);
                DEBUGUPDATE( "\t\t\t updateById doing in place update" );
                /*if ( profile )
                    ss << " fastmod "; */
            } 
            else {
                BSONObj newObj = mss->createNewFromMods();
                checkTooLarge(newObj);
                assert(nsdt);
                DiskLoc newLoc = theDataFileMgr.updateRecord(ns, d, nsdt, r, loc , newObj.objdata(), newObj.objsize(), debug);                        
            }
                    
            if ( logop ) {
                DEV assert( mods->size() );
                 
                BSONObj pattern = patternOrig;
                if ( mss->haveArrayDepMod() ) {
                    BSONObjBuilder patternBuilder;
                    patternBuilder.appendElements( pattern );
                    mss->appendSizeSpecForArrayDepMods( patternBuilder );
                    pattern = patternBuilder.obj();                        
                }
                        
                if( mss->needOpLogRewrite() ) {
                    DEBUGUPDATE( "\t rewrite update: " << mss->getOpLogRewrite() );
                    logOp("u", ns, mss->getOpLogRewrite() , &pattern );
                }
                else {
                    logOp("u", ns, updateobj, &pattern );
                }
            }
            return UpdateResult( 1 , 1 , 1);
        } // end $operator update
                
        // regular update
        BSONElementManipulator::lookForTimestamps( updateobj );
        checkNoMods( updateobj );
        assert(nsdt);
        theDataFileMgr.updateRecord(ns, d, nsdt, r, loc , updateobj.objdata(), updateobj.objsize(), debug );
        if ( logop ) {
            logOp("u", ns, updateobj, &patternOrig );
        }
        return UpdateResult( 1 , 0 , 1 );
    }
 
    UpdateResult _updateObjects(bool god, const char *ns, const BSONObj& updateobj, BSONObj patternOrig, bool upsert, bool multi, bool logop , OpDebug& debug, RemoveSaver* rs ) {
        DEBUGUPDATE( "update: " << ns << " update: " << updateobj << " query: " << patternOrig << " upsert: " << upsert << " multi: " << multi );
        Client& client = cc();
        int profile = client.database()->profile;
        StringBuilder& ss = debug.str;

        if ( logLevel > 2 )
            ss << " update: " << updateobj.toString();
        
        /* idea with these here it to make them loop invariant for multi updates, and thus be a bit faster for that case */
        /* NOTE: when yield() is added herein, these must be refreshed after each call to yield! */
        NamespaceDetails *d = nsdetails(ns); // can be null if an upsert...
        NamespaceDetailsTransient *nsdt = &NamespaceDetailsTransient::get_w(ns);
        /* end note */
        
        auto_ptr<ModSet> mods;
        bool isOperatorUpdate = updateobj.firstElement().fieldName()[0] == '$';
        int modsIsIndexed = false; // really the # of indexes
        if ( isOperatorUpdate ){
            if( d && d->backgroundIndexBuildInProgress ) { 
                set<string> bgKeys;
                d->backgroundIdx().keyPattern().getFieldNames(bgKeys);
                mods.reset( new ModSet(updateobj, nsdt->indexKeys(), &bgKeys) );
            }
            else {
                mods.reset( new ModSet(updateobj, nsdt->indexKeys()) );
            }
            modsIsIndexed = mods->isIndexed();
        }

        if( !upsert && !multi && isSimpleIdQuery(patternOrig) && d && !modsIsIndexed ) {
            int idxNo = d->findIdIndex();
            if( idxNo >= 0 ) {
                ss << " byid ";
                return _updateById(isOperatorUpdate, idxNo, mods.get(), profile, d, nsdt, god, ns, updateobj, patternOrig, logop, debug);
            }
        }

        set<DiskLoc> seenObjects;
        
        int numModded = 0;
        long long nscanned = 0;
        MatchDetails details;
        shared_ptr< MultiCursor::CursorOp > opPtr( new UpdateOp( mods.get() && mods->hasDynamicArray() ) );
        shared_ptr< MultiCursor > c( new MultiCursor( ns, patternOrig, BSONObj(), opPtr, true ) );
        
        auto_ptr<ClientCursor> cc;
            
        while ( c->ok() ) {
            nscanned++;

            bool atomic = c->matcher()->docMatcher().atomic();
                
            // May have already matched in UpdateOp, but do again to get details set correctly
            if ( ! c->matcher()->matches( c->currKey(), c->currLoc(), &details ) ){
                c->advance();
                    
                if ( nscanned % 256 == 0 && ! atomic ){
                    if ( cc.get() == 0 ) {
                        shared_ptr< Cursor > cPtr = c;
                        cc.reset( new ClientCursor( QueryOption_NoCursorTimeout , cPtr , ns ) );
                    }
                    if ( ! cc->yield() ){
                        cc.release();
                        // TODO should we assert or something?
                        break;
                    }
                    if ( !c->ok() ) {
                        break;
                    }
                }
                continue;
            }
            
            Record *r = c->_current();
            DiskLoc loc = c->currLoc();
                
            // TODO Maybe this is unnecessary since we have seenObjects
            if ( c->getsetdup( loc ) ){
                c->advance();
                continue;
            }
                
            BSONObj js(r);
                
            BSONObj pattern = patternOrig;
                
            if ( logop ) {
                BSONObjBuilder idPattern;
                BSONElement id;
                // NOTE: If the matching object lacks an id, we'll log
                // with the original pattern.  This isn't replay-safe.
                // It might make sense to suppress the log instead
                // if there's no id.
                if ( js.getObjectID( id ) ) {
                    idPattern.append( id );
                    pattern = idPattern.obj();
                }
                else {
                    uassert( 10157 ,  "multi-update requires all modified objects to have an _id" , ! multi );
                }
            }
                
            if ( profile )
                ss << " nscanned:" << nscanned;
                
            /* look for $inc etc.  note as listed here, all fields to inc must be this type, you can't set some
                regular ones at the moment. */
            if ( isOperatorUpdate ) {
                    
                if ( multi ){
                    c->advance(); // go to next record in case this one moves
                    if ( seenObjects.count( loc ) )
                        continue;
                }
                    
                const BSONObj& onDisk = loc.obj();
                    
                ModSet * useMods = mods.get();
                bool forceRewrite = false;
                    
                auto_ptr<ModSet> mymodset;
                if ( details.elemMatchKey && mods->hasDynamicArray() ){
                    useMods = mods->fixDynamicArray( details.elemMatchKey );
                    mymodset.reset( useMods );
                    forceRewrite = true;
                }
                    
                auto_ptr<ModSetState> mss = useMods->prepare( onDisk );
                    
                bool indexHack = multi && ( modsIsIndexed || ! mss->canApplyInPlace() );
                    
                if ( indexHack ){
                    if ( cc.get() )
                        cc->updateLocation();
                    else
                        c->noteLocation();
                }
                    
                if ( modsIsIndexed <= 0 && mss->canApplyInPlace() ){
                    mss->applyModsInPlace( true );// const_cast<BSONObj&>(onDisk) );
                    
                    DEBUGUPDATE( "\t\t\t doing in place update" );
                    if ( profile )
                        ss << " fastmod ";
                    
                    if ( modsIsIndexed ){
                        seenObjects.insert( loc );
                    }
                } 
                else {
                    if ( rs )
                        rs->goingToDelete( onDisk );

                    BSONObj newObj = mss->createNewFromMods();
                    checkTooLarge(newObj);
                    DiskLoc newLoc = theDataFileMgr.updateRecord(ns, d, nsdt, r, loc , newObj.objdata(), newObj.objsize(), debug);
                    if ( newLoc != loc || modsIsIndexed ) {
                        // object moved, need to make sure we don' get again
                        seenObjects.insert( newLoc );
                    }
                        
                }
                    
                if ( logop ) {
                    DEV assert( mods->size() );
                        
                    if ( mss->haveArrayDepMod() ) {
                        BSONObjBuilder patternBuilder;
                        patternBuilder.appendElements( pattern );
                        mss->appendSizeSpecForArrayDepMods( patternBuilder );
                        pattern = patternBuilder.obj();                        
                    }
                        
                    if ( forceRewrite || mss->needOpLogRewrite() ){
                        DEBUGUPDATE( "\t rewrite update: " << mss->getOpLogRewrite() );
                        logOp("u", ns, mss->getOpLogRewrite() , &pattern );
                    }
                    else {
                        logOp("u", ns, updateobj, &pattern );
                    }
                }
                numModded++;
                if ( ! multi )
                    return UpdateResult( 1 , 1 , numModded );
                if ( indexHack )
                    c->checkLocation();
                    
                if ( nscanned % 64 == 0 && ! atomic ){
                    if ( cc.get() == 0 ) {
                        shared_ptr< Cursor > cPtr = c;
                        cc.reset( new ClientCursor( QueryOption_NoCursorTimeout , cPtr , ns ) );
                    }
                    if ( ! cc->yield() ){
                        cc.release();
                        break;
                    }
                    if ( !c->ok() ) {
                        break;
                    }
                }
                
                continue;
            }
                
            uassert( 10158 ,  "multi update only works with $ operators" , ! multi );
                
            BSONElementManipulator::lookForTimestamps( updateobj );
            checkNoMods( updateobj );
            theDataFileMgr.updateRecord(ns, d, nsdt, r, loc , updateobj.objdata(), updateobj.objsize(), debug, god);
            if ( logop ) {
                DEV if( god ) log() << "REALLY??" << endl; // god doesn't get logged, this would be bad.
                logOp("u", ns, updateobj, &pattern );
            }
            return UpdateResult( 1 , 0 , 1 );
        }
        
        if ( numModded )
            return UpdateResult( 1 , 1 , numModded );

        
        if ( profile )
            ss << " nscanned:" << nscanned;
        
        if ( upsert ) {
            if ( updateobj.firstElement().fieldName()[0] == '$' ) {
                /* upsert of an $inc. build a default */
                BSONObj newObj = mods->createNewFromQuery( patternOrig );
                if ( profile )
                    ss << " fastmodinsert ";
                theDataFileMgr.insertWithObjMod(ns, newObj, god);
                if ( logop )
                    logOp( "i", ns, newObj );
                
                return UpdateResult( 0 , 1 , 1 , newObj );
            }
            uassert( 10159 ,  "multi update only works with $ operators" , ! multi );
            checkNoMods( updateobj );
            if ( profile )
                ss << " upsert ";
            BSONObj no = updateobj;
            theDataFileMgr.insertWithObjMod(ns, no, god);
            if ( logop )
                logOp( "i", ns, no );
            return UpdateResult( 0 , 0 , 1 , no );
        }
        return UpdateResult( 0 , 0 , 0 );
    }
 
    UpdateResult updateObjects(const char *ns, const BSONObj& updateobj, BSONObj patternOrig, bool upsert, bool multi, bool logop , OpDebug& debug ) {
        uassert( 10155 , "cannot update reserved $ collection", strchr(ns, '$') == 0 );
        if ( strstr(ns, ".system.") ) {
            /* dm: it's very important that system.indexes is never updated as IndexDetails has pointers into it */
            uassert( 10156 , "cannot update system collection", legalClientSystemNS( ns , true ) );
        }
        return _updateObjects(false, ns, updateobj, patternOrig, upsert, multi, logop, debug);
    }
   
}
