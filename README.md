semi_index
==========

This library implements the algorithm described in [*Semi-Indexing
Semi-Structured Data in Tiny Space*](http://www.di.unipi.it/~ottavian/files/semi_index_cikm.pdf).

The aim is to speed-up the processing of large collections of JSON
documents when only a small subset of the values in each document is
needed at each scan. An example is given by MapReduce jobs on
semi-structured logging data, where usually two or three values are
needed but the average document size can be in the hundreds of
kilobytes. Another example is the creation of indexes in document
databases such as CouchDB and MongoDB.

The main idea is, given a collection of JSON documents, to generate an
auxiliary file that encodes the structure of the documents, so that in
the parsing phase the parser can be pointed directly at the positions
where the values are, without having to parse the whole document. Such
an auxiliary file is called a *semi-index*.

This auxiliary file can be create once and used in all the subsequent
scans. By using *succinct data structures* the size of the auxiliary
file can be kept relatively small compared to the size of the
collection.

The reasons for the speed-up are two-fold:

* Since the structure of the document is known, the parser can be
  pointed to the position in the file where the values we are
  interested in are located. This way a considerable amount of I/O can
  be saved. 
* For the same reason, there is no need to perform a full parsing of
  the document into a document tree. This reduces both CPU time and
  memory usage.
  
  
Depending on the characteristics of the documents and how the
semi-index is used, the speedups reported in the paper range between
2x and 12x.
  
Usage example: `json_select`
----------------------------

An example application is provided with the library: `json_select`. 

`json_select` scans a collection of JSON documents given as lines of a
text file, extracts a set of given attributes and outputs them as a
collection of JSON lists.

For example consider the file `example.json`, which is a sample from
Wikipedia edits log.

    $ head -3 example.json 
    {"comment": "content was: '[[Media:Example.og[http://www.example.com link title][http://www.example.com link title]''Italic text'''''Bold text'''jjhkjhkjhkjhkjhjggghg]]'", "logtitle": "Vivian Blaine", "timestamp": "2004-12-23T03:20:32Z", "action": "delete", "params": "", "contributor": {"username": "Slowking Man", "id": 56299}, "type": "delete", "id": 1}
    {"comment": "{{GFDL}} {{cc-by-sa-2.0}}", "logtitle": "File:Mini Christmas tree.png", "timestamp": "2004-12-23T03:24:26Z", "action": "upload", "params": "", "contributor": {"username": "Fredrik", "id": 26675}, "type": "upload", "id": 2}
    {"comment": "content was: 'Daniel Li is an amazing human being.'", "logtitle": "Daniel Li", "timestamp": "2004-12-23T03:27:51Z", "action": "delete", "params": "", "contributor": {"username": "Slowking Man", "id": 56299}, "type": "delete", "id": 3}

If we want to extract the attributes `logtitle` and `contributor.id`
from attribute we can use the following command:

    $ ./json_select naive_parse_stream logtitle,contributor.id < example.json | head -3
    ["Vivian Blaine",56299]
    ["File:Mini Christmas tree.png",26675]
    ["Daniel Li",56299]
    
`naive_parse_stream` works by loading each document and parsing it
with [JsonCpp](http://jsoncpp.sourceforge.net/).

To show the speed-up that can be obtained by using a semi-index it is
better to use a bigger file. We use `wp_history.json` from the
[datasets used in the paper](http://www.di.unipi.it/~ottavian/json_datasets.tar.bz2).

We get the id, title and timestamp of the last contribution from each
page.

    $ sudo ./drop_caches.sh && time ./json_select naive_parse_stream id,title,revision[-1].timestamp < wp_history.json > /dev/null

    real	1m0.408s
    user	0m34.630s
    sys	0m3.260s

`drop_cache.sh` drops all the kernel page caches to ensure that the
file is read from disk each time. The script supports only Linux.

Now we create a semi-index on the file with the following command:

    $ sudo ./drop_caches.sh && time ./json_select si_save wp_history.json.si < wp_history.json > /dev/null
    [ ... DEBUG OUTPUT ... ]
    real	0m37.710s
    user	0m8.890s
    sys	0m2.880s

Note that the time needed to create the semi-index is less then the
time needed for a scan+parse!

For this very low density file the semi-index is negligibly small,
compared to the raw collection:

    2.8G	wp_history.json
    9.7M	wp_history.json.si

For more typical file, the overhead is around 10%.

We can now extract the same attributes as before but taking advantage
of the semi-index.

    $ sudo ./drop_caches.sh && time ./json_select saved_si_parse_mapped wp_history.json wp_history.json.si id,title,revision[-1].timestamp > /dev/null

    real	0m11.282s
    user	0m0.030s
    sys 	0m0.380s

Using the semi-index, the extraction is almost 6 times faster than
normal parsing. Using a compressor that supports random-access on the
JSON file further speedups are possible thanks to the reduced I/O. See
the source code of `json_select` for the details.

How it works
------------

Semi-indexing is a technique that can be applied to most text
serialization formats (such as XML). The generic scheme is described
in full detail in the paper. Here we give a simple explanation of how
it works when applied to JSON.

The JSON semi-index consists of two binary strings, `pos` and
`bp`, which encode respectively the positions of the structural
elements of JSON (i.e. `{}[]:,`) and the structure of the parsing
tree, encoded as a sequence of balanced parentheses.

        {"a": 1, "b": {"l": [1, null], "v": true}}
    pos 110000100100001100001100100000010000100000
    bp  (()()()(()(()())()()))

Note that each `1` in the `pos` string is associated to two
consecutive parentheses in the `bp` string.

One property of `pos` is that it is very sparse, because usually keys
and values are at least a few characters long, hence it is very
compressible. Using an Elias-Fano encoding, a space close to the
information-theoretical optimum can be reached while allowing
efficient random-access and powerful operations such as `select`,
which gives the position of the `i`-th `1` bit in the string.

On the other hand `bp`, which is very small (it is twice as long as
the number of ones in `pos`) can be augmented with data structures
that enable quick tree-like navigation. This structure take only a
negligible amount of space compared to the string. 

Navigation in the JSON object is performed by navigating the tree
represented by `bp` and then retrieving the key/values from the JSON
document by pointing to the the positions obtained from `pos`.

How to build the code
---------------------

### Dependencies ###

The following dependencies have to be installed to compile the library.

* CMake >= 2.6, for the build system
* zlib
* Boost >= 1.42

Also, the library `succinct` has to be downloaded as a git submodule,
so the following two commands have to be executed *before running
cmake*:

    $ git submodule init
    $ git submodule update

`json_select` and the performance tests also depend on MongoDB and
JsonCpp, but their sources are included in the source tree so they
don't need to be installed.

### Supported systems ###

The code has been developed and tested mainly on Linux, but it has
been tested also on Mac OS X and Windows 7.

The code has been tested only on x86-64. Compiling it on 32bit
architectures would probably require some work.

### Building on Unix ###

The project uses CMake. To build it on Unix systems it should be
sufficient to do the following:

    $ cmake . -DCMAKE_BUILD_TYPE=Release
    $ make

It is also advised to perform a `make test`, which runs the unit tests.

### Building on Windows ###

On Windows, Boost and zlib are not installed in default locations, so
it is necessary to set some environment variables to allow the build
system to find them.

* For Boost `BOOST_ROOT` must be set to the directory which contains
  the `boost` include directory.
* For zlib `CMAKE_PREFIX_PATH` must be set to the directory that
  contains `zlib.h`
* Both the directories that contain the Boost and zlib DLLs must be
  added to `PATH` so that the executables find them

Once the env variables are set, the quickest way to build the code is
by using NMake (instead of the default Visual Studio). Run the
following commands in a Visual Studio x64 Command Prompt:

    $ cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release .
    $ nmake
    $ nmake test

