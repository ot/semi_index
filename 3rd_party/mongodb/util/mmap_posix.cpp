// mmap_posix.cpp

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
#include "file_allocator.h"
#include "../db/concurrency.h"

#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace mongo {

    MemoryMappedFile::MemoryMappedFile() {
        fd = 0;
        maphandle = 0;
        len = 0;
        created();
    }
    
    void MemoryMappedFile::close() {
        for( vector<void*>::iterator i = views.begin(); i != views.end(); i++ ) {
            munmap(*i,len);
        }

        if ( fd )
            ::close(fd);
        fd = 0;
    }

#ifndef O_NOATIME
#define O_NOATIME (0)
#endif

    void* MemoryMappedFile::map(const char *filename, unsigned long long &length, int options) {
        // length may be updated by callee.
        _filename = filename;
        theFileAllocator().allocateAsap( filename, length );
        len = length;

        massert( 10446 , str::stream() << "mmap: can't map area of size 0 file: " << filename, length > 0 );
        
        fd = open(filename, O_RDWR | O_NOATIME);
        if ( fd <= 0 ) {
            log() << "couldn't open " << filename << ' ' << errnoWithDescription() << endl;
            return 0;
        }

        unsigned long long filelen = lseek(fd, 0, SEEK_END);
		uassert(10447,  str::stream() << "map file alloc failed, wanted: " << length << " filelen: " << filelen << ' ' << sizeof(size_t), filelen == length );
        lseek( fd, 0, SEEK_SET );
        
        void * view = mmap(NULL, length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if ( view == MAP_FAILED ) {
            error() << "  mmap() failed for " << filename << " len:" << length << " " << errnoWithDescription() << endl;
            if ( errno == ENOMEM ) {
			  if( sizeof(void*) == 4 )
                error() << "mmap failed with out of memory. You are using a 32-bit build and probably need to upgrade to 64" << endl;
			  else
                error() << "mmap failed with out of memory. (64 bit build)" << endl;
            }
            return 0;
        }


#if defined(__sunos__)
#warning madvise not supported on solaris yet
#else
        if ( options & SEQUENTIAL ){
            if ( madvise( view , length , MADV_SEQUENTIAL ) ){
                warning() << "map: madvise failed for " << filename << ' ' << errnoWithDescription() << endl;
            }
        }
#endif

        DEV if (! dbMutex.info().isLocked()){
            _unlock();
        }

        views.push_back( view );

        return view;
    }

    void* MemoryMappedFile::createReadOnlyMap() {
        void * x = mmap( /*start*/0 , len , PROT_READ , MAP_SHARED , fd , 0 );        
	    if( x == MAP_FAILED ) {
	        if ( errno == ENOMEM ) {
	            if( sizeof(void*) == 4 )
		        error() << "mmap ro failed with out of memory. You are using a 32-bit build and probably need to upgrade to 64" << endl;
		    else
  		        error() << "mmap ro failed with out of memory. (64 bit build)" << endl;
	        }
	        return 0;
	    }
	    return x;
    }
    
    void* MemoryMappedFile::createPrivateMap() {
        void * x = mmap( /*start*/0 , len , PROT_READ|PROT_WRITE , MAP_PRIVATE , fd , 0 );        
        if( x == MAP_FAILED ) {
            if ( errno == ENOMEM ) {
                if( sizeof(void*) == 4 ) {
                    error() << "mmap private failed with out of memory. You are using a 32-bit build and probably need to upgrade to 64" << endl;
                } else {
                    error() << "mmap private failed with out of memory. (64 bit build)" << endl;
                }
            }
            return 0;
        }
        else { 
            views.push_back(x);
        }
        return x;
    }

    void* MemoryMappedFile::remapPrivateView(void *oldPrivateAddr) {
        assert( munmap(oldPrivateAddr,len) == 0 );
        void * x = mmap( oldPrivateAddr, len , PROT_READ|PROT_WRITE , MAP_PRIVATE|MAP_FIXED , fd , 0 );        
        if( x == MAP_FAILED ) {
            int err = errno;
            remove(views.begin(), views.end(), oldPrivateAddr);
            massert(13601, str::stream() << "Couldn't remap private view: " << errnoWithDescription(err), false);
        }
        assert( x == oldPrivateAddr );
        return x;
    }
    
    void MemoryMappedFile::flush(bool sync) {
        if ( views.empty() || fd == 0 )
            return;
        if ( msync(views[0], len, sync ? MS_SYNC : MS_ASYNC) )
            problem() << "msync " << errnoWithDescription() << endl;
    }
    
    class PosixFlushable : public MemoryMappedFile::Flushable {
    public:
        PosixFlushable( void * view , HANDLE fd , long len )
            : _view( view ) , _fd( fd ) , _len(len){
        }

        void flush(){
            if ( _view && _fd )
                if ( msync(_view, _len, MS_SYNC ) )
                    problem() << "msync " << errnoWithDescription() << endl;
            
        }

        void * _view;
        HANDLE _fd;
        long _len;
    };

    MemoryMappedFile::Flushable * MemoryMappedFile::prepareFlush(){
        return new PosixFlushable( views.empty() ? 0 : views[0] , fd , len );
    }

    void MemoryMappedFile::_lock() {
        if (! views.empty() ) assert(mprotect(views[0], len, PROT_READ | PROT_WRITE) == 0);
    }

    void MemoryMappedFile::_unlock() {
        if (! views.empty() ) assert(mprotect(views[0], len, PROT_READ) == 0);
    }

} // namespace mongo

