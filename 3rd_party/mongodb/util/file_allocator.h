// @file file_allocator.h

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

#include "../pch.h"
#include <fcntl.h>
#include <errno.h>

#if defined(__freebsd__) || defined(__openbsd__)
#include <sys/stat.h>
#endif

#include "timer.h"
#include "mongoutils/str.h"
using namespace mongoutils;

#ifndef O_NOATIME
#define O_NOATIME (0)
#endif

namespace mongo {

    /* Handles allocation of contiguous files on disk.  Allocation may be
       requested asynchronously or synchronously.
       */
    class FileAllocator {
        /* The public functions may not be called concurrently.  The allocation
           functions may be called multiple times per file, but only the first
           size specified per file will be used.
        */
    public:
#if !defined(_WIN32)
        FileAllocator() : pendingMutex_("FileAllocator"), failed_() {}
#endif
        void start() {
#if !defined(_WIN32)
            Runner r( *this );
            boost::thread t( r );
#endif
        }
        // May be called if file exists. If file exists, or its allocation has
        // been requested, size is updated to match existing file size.
        void requestAllocation( const string &name, long &size ) {
            /* Some of the system calls in the file allocator don't work in win, 
               so no win support - 32 or 64 bit.  Plus we don't seem to need preallocation 
               on windows anyway as we don't have to pre-zero the file there.
            */
#if !defined(_WIN32)
            scoped_lock lk( pendingMutex_ );
            if ( failed_ )
                return;
            long oldSize = prevSize( name );
            if ( oldSize != -1 ) {
                size = oldSize;
                return;
            }
            pending_.push_back( name );
            pendingSize_[ name ] = size;
            pendingUpdated_.notify_all();
#endif
        }
        // Returns when file has been allocated.  If file exists, size is
        // updated to match existing file size.
        void allocateAsap( const string &name, unsigned long long &size ) {
#if !defined(_WIN32)
            scoped_lock lk( pendingMutex_ );
            long oldSize = prevSize( name );
            if ( oldSize != -1 ) {
                size = oldSize;
                if ( !inProgress( name ) )
                    return;
            }
            checkFailure();
            pendingSize_[ name ] = size;
            if ( pending_.size() == 0 )
                pending_.push_back( name );
            else if ( pending_.front() != name ) {
                pending_.remove( name );
                list< string >::iterator i = pending_.begin();
                ++i;
                pending_.insert( i, name );
            }
            pendingUpdated_.notify_all();
            while( inProgress( name ) ) {
                checkFailure();
                pendingUpdated_.wait( lk.boost() );
            }
#endif
        }

        void waitUntilFinished() const {
#if !defined(_WIN32)
            if ( failed_ )
                return;
            scoped_lock lk( pendingMutex_ );
            while( pending_.size() != 0 )
                pendingUpdated_.wait( lk.boost() );
#endif
        }
        
        static void ensureLength(int fd , long size) {

#if defined(_WIN32)
            // we don't zero on windows
            // TODO : we should to avoid fragmentation
#else

#if defined(__linux__) 
            int ret = posix_fallocate(fd,0,size);
            if ( ret == 0 )
                return;
            
            log() << "FileAllocator: posix_fallocate failed: " << errnoWithDescription( ret ) << " falling back" << endl;
#endif
            
            off_t filelen = lseek(fd, 0, SEEK_END);
            if ( filelen < size ) {
                if (filelen != 0) {
                    stringstream ss;
                    ss << "failure creating new datafile; lseek failed for fd " << fd << " with errno: " << errnoWithDescription();
                    uassert( 10440 ,  ss.str(), filelen == 0 );
                }
                // Check for end of disk.
				
                uassert( 10441 ,  str::stream() << "Unable to allocate new file of size " << size << ' ' << errnoWithDescription(),
                         size - 1 == lseek(fd, size - 1, SEEK_SET) );
                uassert( 10442 ,  str::stream() << "Unable to allocate new file of size " << size << ' ' << errnoWithDescription(),
                         1 == write(fd, "", 1) );
                lseek(fd, 0, SEEK_SET);
                
                const long z = 256 * 1024;
                const boost::scoped_array<char> buf_holder (new char[z]);
                char* buf = buf_holder.get();
                memset(buf, 0, z);
                long left = size;
                while ( left > 0 ) {
                    long towrite = left;
                    if ( towrite > z )
                        towrite = z;
                    
                    int written = write( fd , buf , towrite );
                    uassert( 10443 , errnoWithPrefix("FileAllocator: file write failed" ), written > 0 );
                    left -= written;
                }
            }
#endif
        }
        
    private:
#if !defined(_WIN32)
        void checkFailure() {
            if (failed_) {
                // we want to log the problem (diskfull.js expects it) but we do not want to dump a stack tracke
                msgassertedNoTrace( 12520, "new file allocation failure" );
            }
        }
        
        // caller must hold pendingMutex_ lock.  Returns size if allocated or 
        // allocation requested, -1 otherwise.
        long prevSize( const string &name ) const {
            if ( pendingSize_.count( name ) > 0 )
                return pendingSize_[ name ];
            if ( boost::filesystem::exists( name ) )
                return boost::filesystem::file_size( name );
            return -1;
        }
         
        // caller must hold pendingMutex_ lock.
        bool inProgress( const string &name ) const {
            for( list< string >::const_iterator i = pending_.begin(); i != pending_.end(); ++i )
                if ( *i == name )
                    return true;
            return false;
        }

        mutable mongo::mutex pendingMutex_;
        mutable boost::condition pendingUpdated_;
        list< string > pending_;
        mutable map< string, long > pendingSize_;
        bool failed_;
        
        struct Runner {
            Runner( FileAllocator &allocator ) : a_( allocator ) {}
            FileAllocator &a_;
            void operator()() {
                while( 1 ) {
                    {
                        scoped_lock lk( a_.pendingMutex_ );
                        if ( a_.pending_.size() == 0 )
                            a_.pendingUpdated_.wait( lk.boost() );
                    }
                    while( 1 ) {
                        string name;
                        long size;
                        {
                            scoped_lock lk( a_.pendingMutex_ );
                            if ( a_.pending_.size() == 0 )
                                break;
                            name = a_.pending_.front();
                            size = a_.pendingSize_[ name ];
                        }
                        try {
                            log() << "allocating new datafile " << name << ", filling with zeroes..." << endl;
                            long fd = open(name.c_str(), O_CREAT | O_RDWR | O_NOATIME, S_IRUSR | S_IWUSR);
                            if ( fd <= 0 ) {
                                stringstream ss;
                                ss << "FileAllocator: couldn't open " << name << ' ' << errnoWithDescription();
                                uassert( 10439 ,  ss.str(), fd <= 0 );
                            }

#if defined(POSIX_FADV_DONTNEED)
                            if( posix_fadvise(fd, 0, size, POSIX_FADV_DONTNEED) ) { 
                                log() << "warning: posix_fadvise fails " << name << ' ' << errnoWithDescription() << endl;
                            }
#endif
                            
                            Timer t;
                            
                            /* make sure the file is the full desired length */
                            ensureLength( fd , size );

                            log() << "done allocating datafile " << name << ", " 
                                  << "size: " << size/1024/1024 << "MB, "
                                  << " took " << ((double)t.millis())/1000.0 << " secs" 
                                  << endl;

                            close( fd );
                            
                        } catch ( ... ) {
                            log() << "error failed to allocate new file: " << name
								  << " size: " << size << ' ' << errnoWithDescription() << endl;
                            try {
                                BOOST_CHECK_EXCEPTION( boost::filesystem::remove( name ) );
                            } catch ( ... ) {
                            }
                            scoped_lock lk( a_.pendingMutex_ );
                            a_.failed_ = true;
                            // not erasing from pending
                            a_.pendingUpdated_.notify_all();
                            return; // no more allocation
                        }
                        
                        {
                            scoped_lock lk( a_.pendingMutex_ );
                            a_.pendingSize_.erase( name );
                            a_.pending_.pop_front();
                            a_.pendingUpdated_.notify_all();
                        }
                    }
                }
            }
        };
#endif    
    };
    
    FileAllocator &theFileAllocator();
} // namespace mongo
