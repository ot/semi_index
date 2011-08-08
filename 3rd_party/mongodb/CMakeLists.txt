
include_directories(.)

set(MONGODBLIB_SOURCES 
  bson/oid.cpp
  util/base64.cpp
  util/md5.c
  util/assert_util.cpp
  util/log.cpp
  util/logfile.cpp
  util/text.cpp
  util/util.cpp
  db/json.cpp
  db/jsobj.cpp
  db/nonce.cpp
  db/lasterror.cpp
  dummy.cpp
  )
add_library(mongodblib STATIC ${MONGODBLIB_SOURCES})
