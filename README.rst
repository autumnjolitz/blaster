--------
Blaster
--------

A small attempt at doing an http echo server with libmill

There is a contrib directory used for storing https://github.com/nodejs/http-parser/ so we
can just compile. Not mine.


Build directions
-----------------

Acquire libmill. On OSX, you can use ```brew install --HEAD libmill```

Compile using ```gcc  -DDEBUG=1 -o hello hello.c contrib/http_parser.c -lmill```

