#   JABC: JavaScriptCore bindings

<img align=right width="25%" src="./logo.jpg"/>

There is quite a selection of JavaScript environments these days:
[node.js][n], [Deno][d], [Bun][b], [Bare][r], 
and so on. My immediate goal was to write extensions and scripts
for a revision control system, but all contenders turned out 
overweight, or worse. The top issue with node-like
environments is *bloat*. What should be a simple scripting language
gradually became an elephantine monstrosity. Can we try to work
around the underlying forces that cause bloat? Why does it happen,
again and again? My working hypothesis is that

  - layered architecture and
  - over-engineering, combined with
  - the market forces, cause bloat.

 The layering works like this:

 1. the JavaScript world,
 2. the JS VM world (V8, JavaScriptCore),
 3. the node.js/deno/etc bindings world,
 4. the system-library layer (incl. `libuv`),
 5. the POSIX layer (also Windows).

Each of the layers tends to have its own programming language,
build system, memory management theory, I/O abstraction layer, and
package/dependency management. Each layer is a separate universe.
We will try to compact that into three layers:

 1. the JavaScript world,
 2. the system-library layer (incl libv8/libjavascriptcore),
 3. the POSIX layer.

Essentially, the bindings become the glue in between the layers.
In the library layer, there are:

  - libabc for low-level system things:
     1. buffers, 
     2. mmap, 
     3. network,
     4. files,
     5. poll loops,
     6. etc.
  - libdog for revision control related things:
     1. tokenizers, 
     2. git compatibility toolkit,
     3. diff and merge algorithms,
     4. the tokenized hunk format.

On top of that, all the revision control machinery is being built.


[7]: http://github.com/gritzko/k7
[n]: https://nodejs.org/en
[d]: https://deno.com/
[b]: https://bun.com/
[r]: https://bare.pears.com/
