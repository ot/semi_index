// mmap.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"
#include "mmap.h"
#include "processinfo.h"
#include "concurrency/rwlock.h"

namespace mongo {

    set<MongoFile*> MongoFile::mmfiles;

    /* Create. Must not exist. 
    @param zero fill file with zeros when true
    */
    void* MemoryMappedFile::create(string filename, unsigned long long len, bool zero) {
        uassert( 13468, string("can't create file already exists ") + filename, !exists(filename) );
        void *p = map(filename.c_str(), len);
        if( p && zero ) {
            size_t sz = (size_t) len;
            assert( len == sz );
            memset(p, 0, sz);
        }
        return p;
    }

    /*static*/ void MemoryMappedFile::updateLength( const char *filename, unsigned long long &length ) {
        if ( !boost::filesystem::exists( filename ) )
            return;
        // make sure we map full length if preexisting file.
        boost::uintmax_t l = boost::filesystem::file_size( filename );
        length = l;
    }

    void* MemoryMappedFile::map(const char *filename) {
        unsigned long long l = boost::filesystem::file_size( filename );
        return map( filename , l );
    }
    void* MemoryMappedFile::mapWithOptions(const char *filename, int options) {
        unsigned long long l = boost::filesystem::file_size( filename );
        return map( filename , l, options );
    }

    void printMemInfo( const char * where ){
        cout << "mem info: ";
        if ( where ) 
            cout << where << " "; 
        ProcessInfo pi;
        if ( ! pi.supported() ){
            cout << " not supported" << endl;
            return;
        }
        
        cout << "vsize: " << pi.getVirtualMemorySize() << " resident: " << pi.getResidentSize() << " mapped: " << ( MemoryMappedFile::totalMappedLength() / ( 1024 * 1024 ) ) << endl;
    }

    /* --- MongoFile -------------------------------------------------
       this is the administrative stuff 
    */

    RWLock MongoFile::mmmutex("rw:mmmutex");

    void MongoFile::destroyed() {
        rwlock lk( mmmutex , true );
        mmfiles.erase(this);
    }

    /*static*/
    void MongoFile::closeAllFiles( stringstream &message ) {
        static int closingAllFiles = 0;
        if ( closingAllFiles ) {
            message << "warning closingAllFiles=" << closingAllFiles << endl;
            return;
        }
        ++closingAllFiles;

        rwlock lk( mmmutex , true );

        ProgressMeter pm( mmfiles.size() , 2 , 1 );
        for ( set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ ){
            (*i)->close();
            pm.hit();
        }
        message << "closeAllFiles() finished";
        --closingAllFiles;
    }

    /*static*/ long long MongoFile::totalMappedLength(){
        unsigned long long total = 0;
        
        rwlock lk( mmmutex , false );
        for ( set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ )
            total += (*i)->length();

        return total;
    }

    void nullFunc() { }

    // callback notifications
    void (*MongoFile::notifyPreFlush)() = nullFunc;
    void (*MongoFile::notifyPostFlush)() = nullFunc;

    /*static*/ int MongoFile::flushAll( bool sync ){
        notifyPreFlush();
        int x = _flushAll(sync);
        notifyPostFlush();
        return x;
    }

    /*static*/ int MongoFile::_flushAll( bool sync ){
        if ( ! sync ){
            int num = 0;
            rwlock lk( mmmutex , false );
            for ( set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ ){
                num++;
                MongoFile * mmf = *i;
                if ( ! mmf )
                    continue;
                
                mmf->flush( sync );
            }
            return num;
        }
        
        // want to do it sync
        set<MongoFile*> seen;
        while ( true ){
            auto_ptr<Flushable> f;
            {
                rwlock lk( mmmutex , false );
                for ( set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ ){
                    MongoFile * mmf = *i;
                    if ( ! mmf )
                        continue;
                    if ( seen.count( mmf ) )
                        continue;
                    f.reset( mmf->prepareFlush() );
                    seen.insert( mmf );
                    break;
                }
            }
            if ( ! f.get() )
                break;
            
            f->flush();
        }
        return seen.size();
    }

    void MongoFile::created(){
        rwlock lk( mmmutex , true );
        mmfiles.insert(this);
    }

#if defined(_DEBUG) && !defined(_TESTINTENT)

    void MongoFile::markAllWritable() {
        rwlock lk( mmmutex , false );
        for ( set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ ){
            MongoFile * mmf = *i;
            if (mmf) mmf->_lock();
        }
    }

    void MongoFile::unmarkAllWritable() {
        rwlock lk( mmmutex , false );
        for ( set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ ){
            MongoFile * mmf = *i;
            if (mmf) mmf->_unlock();
        }
    }
#endif

} // namespace mongo
