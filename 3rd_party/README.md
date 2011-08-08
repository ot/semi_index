3rd-party dependencies
======================

Some of the dependencies have complicated build scripts or they need
modifications to be used as libraries, so they were included in the
repository and made part of the build system. Custom `CMakeFiles.txt`
have been written for them.


jsoncpp
-------

* The files were copied from `jsoncpp-src-0.5.0`.


mongodb (for bson)
------------------

* The files were copied from `mongodb-src-r1.7.4`.
* Some changes made to support big BSON objects.
* Some dummy functions implemented in a new file `dummy.cpp` to reduce dependencies.
* Also, fixed a bug in `text.cpp` that prevented compilation on Windows.
