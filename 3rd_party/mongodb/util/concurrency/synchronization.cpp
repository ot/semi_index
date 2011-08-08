// synchronization.cpp

/*    Copyright 2010 10gen Inc.
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
#include "synchronization.h"

namespace mongo {

    Notification::Notification() : _mutex ( "Notification" ) , _notified( false ) { }

    Notification::~Notification(){ }

    void Notification::waitToBeNotified(){
        scoped_lock lock( _mutex );
        while ( ! _notified )
            _condition.wait( lock.boost() );
    }

    void Notification::notifyOne(){
        scoped_lock lock( _mutex );
        assert( !_notified );
        _notified = true;
        _condition.notify_one();
    }

    NotifyAll::NotifyAll() : _mutex("NotifyAll"), _counter(0) { }
    
    void NotifyAll::wait() {
        scoped_lock lock( _mutex );
        unsigned long long old = _counter;
        while( old == _counter ) {
            _condition.wait( lock.boost() );
        }
    }

    void NotifyAll::notifyAll() {
        scoped_lock lock( _mutex );
        ++_counter;
        _condition.notify_all();
    }

} // namespace mongo
