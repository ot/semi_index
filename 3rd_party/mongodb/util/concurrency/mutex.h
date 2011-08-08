// @file mutex.h

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

#pragma once

#include <map>
#include <set>

#include "../heapcheck.h"

namespace mongo { 

    class mutex;

    inline boost::xtime incxtimemillis( long long s ){
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        xt.sec += (int)( s / 1000 );
        xt.nsec += (int)(( s % 1000 ) * 1000000);
        if ( xt.nsec >= 1000000000 ) {
            xt.nsec -= 1000000000;
            xt.sec++;
        }        
        return xt;
    }

    /** only used on _DEBUG builds.
        MutexDebugger checks that we always acquire locks for multiple mutexes in a consistant (acyclic) order.
        If we were inconsistent we could deadlock.
    */
    class MutexDebugger { 
        typedef const char * mid; // mid = mutex ID
        typedef map<mid,int> Preceeding;
        map< mid, int > maxNest;
        boost::thread_specific_ptr< Preceeding > us;
        map< mid, set<mid> > followers;
        boost::mutex &x;
        unsigned magic;

        void aBreakPoint() { } // for debugging
    public:
        // set these to create an assert that
        //   b must never be locked before a
        //   so 
        //     a.lock(); b.lock(); is fine
        //     b.lock(); alone is fine too
        //   only checked on _DEBUG builds.
        string a,b;
        
        /** outputs some diagnostic info on mutexes (on _DEBUG builds) */
        void programEnding();

        MutexDebugger();

        void entering(mid m) {
            if( this == 0 ) return;
            assert( magic == 0x12345678 );

            Preceeding *_preceeding = us.get();
            if( _preceeding == 0 )
                us.reset( _preceeding = new Preceeding() );
            Preceeding &preceeding = *_preceeding;

            if( a == m ) { 
                aBreakPoint();
                if( preceeding[b.c_str()] ) {
                    cout << "****** MutexDebugger error! warning " << b << " was locked before " << a << endl;
                    assert(false);
                }
            }

            preceeding[m]++;
            if( preceeding[m] > 1 ) { 
                // recursive re-locking.
                if( preceeding[m] > maxNest[m] )
                    maxNest[m] = preceeding[m];
                return;
            }

            bool failed = false;
            string err;
            {
                boost::mutex::scoped_lock lk(x);
                followers[m];
                for( Preceeding::iterator i = preceeding.begin(); i != preceeding.end(); i++ ) { 
                    if( m != i->first && i->second > 0 ) {
                        followers[i->first].insert(m);
                        if( followers[m].count(i->first) != 0 ){
                            failed = true;
                            stringstream ss;
                            mid bad = i->first;
                            ss << "mutex problem" <<
                                "\n  when locking " << m <<
                                "\n  " << bad << " was already locked and should not be."
                                "\n  set a and b above to debug.\n";
                            stringstream q;
                            for( Preceeding::iterator i = preceeding.begin(); i != preceeding.end(); i++ ) { 
                                if( i->first != m && i->first != bad && i->second > 0 )
                                    q << "  " << i->first << '\n';
                            }
                            string also = q.str();
                            if( !also.empty() )
                                ss << "also locked before " << m << " in this thread (no particular order):\n" << also;
                            err = ss.str();
                            break;
                        }
                    }
                }
            }
            if( failed ) {
                cout << err << endl;
                assert( 0 );
            }
        }
        void leaving(mid m) { 
            if( this == 0 ) return; // still in startup pre-main()
            Preceeding& preceeding = *us.get();
            preceeding[m]--;
            if( preceeding[m] < 0 ) {
                cout << "ERROR: lock count for " << m << " is " << preceeding[m] << endl;
                assert( preceeding[m] >= 0 );
            }
        }
    };
    extern MutexDebugger &mutexDebugger;
    
    // If you create a local static instance of this class, that instance will be destroyed
    // before all global static objects are destroyed, so _destroyingStatics will be set
    // to true before the global static variables are destroyed.
    class StaticObserver : boost::noncopyable {
    public:
        static bool _destroyingStatics;
        ~StaticObserver() { _destroyingStatics = true; }
    };

    // On pthread systems, it is an error to destroy a mutex while held.  Static global
    // mutexes may be held upon shutdown in our implementation, and this way we avoid
    // destroying them.
    class mutex : boost::noncopyable {
    public:
#if defined(_DEBUG)
        const char * const _name;
#endif

#if defined(_DEBUG)
        mutex(const char *name) 
           : _name(name) 
#else
        mutex(const char *) 
#endif
        { 
            _m = new boost::timed_mutex(); 
            IGNORE_OBJECT( _m  );   // Turn-off heap checking on _m
        }
        ~mutex() {
            if( !StaticObserver::_destroyingStatics ) {
                UNIGNORE_OBJECT( _m );
                delete _m;
            }
        }
        
        class try_lock : boost::noncopyable {
        public:
            try_lock( mongo::mutex &m , int millis = 0 ) 
                : _l( m.boost() , incxtimemillis( millis ) ) , 
#if BOOST_VERSION >= 103500
                  ok( _l.owns_lock() ) 
#else
                  ok( _l.locked() )
#endif
            {
            }

            ~try_lock() { 
            }
            
        private:
            boost::timed_mutex::scoped_timed_lock _l;

        public:
            const bool ok;
        };


        class scoped_lock : boost::noncopyable {
#if defined(_DEBUG)
            mongo::mutex *mut;
#endif
        public:
            scoped_lock( mongo::mutex &m ) : _l( m.boost() ) {
#if defined(_DEBUG)
                mut = &m;
                mutexDebugger.entering(mut->_name);
#endif
            }
            ~scoped_lock() { 
#if defined(_DEBUG)
                mutexDebugger.leaving(mut->_name);
#endif
            }
            boost::timed_mutex::scoped_lock &boost() { return _l; }
        private:
            boost::timed_mutex::scoped_lock _l;
        };


    private:

        boost::timed_mutex &boost() { return *_m; }
        boost::timed_mutex *_m;
    };
    
    typedef mutex::scoped_lock scoped_lock;
    typedef boost::recursive_mutex::scoped_lock recursive_scoped_lock;

}
